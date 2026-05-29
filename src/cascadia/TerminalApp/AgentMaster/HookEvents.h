// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — Claude Code hook events + the hook-driven session state machine.
//
// State is authoritative from Claude Code hooks, NEVER screen-scraping (DESIGN §7-8,
// HOOKS.md). This header is pure C++ (no WinRT) so the state machine — the single most
// correctness-critical piece — can be unit-tested standalone.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "SessionModels.h"

namespace Agentmaster
{
    // The Claude Code hook lifecycle events Agentmaster consumes (HOOKS.md).
    // High-frequency PreToolUse/PostToolUse are intentionally NOT consumed in M5: the
    // session is already `Running` between UserPromptSubmit and Stop, and hooking every
    // tool call would spawn a forwarder per tool (latency). They can be added later for
    // activity heartbeats or PreToolUse-based auto-approval.
    enum class HookEvent
    {
        Unknown,
        SessionStart,
        UserPromptSubmit,
        PreToolUse,
        PostToolUse,
        Notification,
        Stop,
        SubagentStop,
        SessionEnd,
    };

    inline HookEvent ParseHookEvent(std::wstring_view name) noexcept
    {
        if (name == L"SessionStart")
        {
            return HookEvent::SessionStart;
        }
        if (name == L"UserPromptSubmit")
        {
            return HookEvent::UserPromptSubmit;
        }
        if (name == L"PreToolUse")
        {
            return HookEvent::PreToolUse;
        }
        if (name == L"PostToolUse")
        {
            return HookEvent::PostToolUse;
        }
        if (name == L"Notification")
        {
            return HookEvent::Notification;
        }
        if (name == L"Stop")
        {
            return HookEvent::Stop;
        }
        if (name == L"SubagentStop")
        {
            return HookEvent::SubagentStop;
        }
        if (name == L"SessionEnd")
        {
            return HookEvent::SessionEnd;
        }
        return HookEvent::Unknown;
    }

    inline const wchar_t* HookEventName(HookEvent e) noexcept
    {
        switch (e)
        {
        case HookEvent::SessionStart:
            return L"SessionStart";
        case HookEvent::UserPromptSubmit:
            return L"UserPromptSubmit";
        case HookEvent::PreToolUse:
            return L"PreToolUse";
        case HookEvent::PostToolUse:
            return L"PostToolUse";
        case HookEvent::Notification:
            return L"Notification";
        case HookEvent::Stop:
            return L"Stop";
        case HookEvent::SubagentStop:
            return L"SubagentStop";
        case HookEvent::SessionEnd:
            return L"SessionEnd";
        default:
            return L"Unknown";
        }
    }

    // One authoritative hook report, correlated to a session by `sessionId`
    // (== CCMGR_SESSION_ID == Claude's own --session-id). See HOOKS.md.
    struct HookMessage
    {
        std::wstring sessionId;
        std::wstring cwd;
        HookEvent event{ HookEvent::Unknown };
        int64_t ts{ 0 };
        // Best-effort, set only on Stop: did the agent's last message end in a question?
        // Feeds the question-guard so Autopilot does not auto-answer a clarifying question.
        bool lastMessageIsQuestion{ false };
        // Notification specifically requesting tool permission (vs. an idle notification).
        // Drives NeedsApproval / ApprovalPolicy rather than the prompt queue.
        bool permissionRequest{ false };
        std::wstring tool; // associated tool name, when applicable
        // The hosting terminal's WT_SESSION GUID (plain, no braces), echoed by the forwarder
        // from $env:WT_SESSION. Lets the app correlate a session we did NOT launch (a
        // hand-typed `claude` in a `+` tab) back to its ConPTY connection so it can bind a
        // stdin injector — i.e. ADOPT it into full observe+control. Empty when unavailable.
        std::wstring tabToken;
    };

    // The hook-driven state machine (DESIGN §7). PURE — depends only on the current
    // state and the incoming event. This encodes Correctness Rule #1: "waiting" is three
    // distinct states, and only a clean `Stop` produces WaitingForInput (ready for the
    // next queued prompt). A `Notification(permission)` produces NeedsApproval, which the
    // ApprovalPolicy handles — never the prompt queue. A `Stop` whose last message is a
    // question is STILL WaitingForInput here; it is the scheduler's question-guard (M7),
    // reading `lastMessageIsQuestion`, that Holds the next prompt.
    inline SessionState NextSessionState(SessionState current, const HookMessage& m) noexcept
    {
        switch (m.event)
        {
        case HookEvent::SessionStart:
            return SessionState::Idle;
        case HookEvent::UserPromptSubmit:
            return SessionState::Running;
        case HookEvent::PreToolUse:
        case HookEvent::PostToolUse:
            return SessionState::Running;
        case HookEvent::Notification:
            // Only a permission request is a blocking "needs you" state; a plain idle
            // notification doesn't change the lifecycle state.
            return m.permissionRequest ? SessionState::NeedsApproval : current;
        case HookEvent::Stop:
            return SessionState::WaitingForInput;
        case HookEvent::SubagentStop:
            return current; // informational only
        case HookEvent::SessionEnd:
            return SessionState::Done;
        case HookEvent::Unknown:
        default:
            return current;
        }
    }
}
