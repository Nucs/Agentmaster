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
        // The submitted prompt body, set ONLY on UserPromptSubmit (escaped on the wire). Lets
        // the registry record EVERY message a session received — including ones the human
        // typed straight into the ConPTY, not just ones we queued — into the Flight Plan.
        // Empty for every other event.
        std::wstring promptText;
        // The hosting terminal's WT_SESSION GUID (plain, no braces), echoed by the forwarder
        // from $env:WT_SESSION. Lets the app correlate a session we did NOT launch (a
        // hand-typed `claude` in a `+` tab) back to its ConPTY connection so it can bind a
        // stdin injector — i.e. ADOPT it into full observe+control. Empty when unavailable.
        std::wstring tabToken;
        // Engine-internal (never on the wire): set ONLY by the SessionScanner's missed-Stop
        // reconciliation. That Stop is synthesized from a ≥2s-QUIESCENT transcript whose last
        // assistant message ended the turn, so it is authoritative "the conversation is idle
        // NOW": the ordered machine lands on WaitingForInput unconditionally — it is never
        // stale and never held Running for a type-ahead prompt (already consumed or canceled).
        bool quiescentStop{ false };
        // Engine-internal (never on the wire): set ONLY by the SessionScanner's API-error
        // reconciliation. Claude Code recorded a synthetic assistant message with
        // isApiErrorMessage:true (the turn DIED — "API Error: …", a rate/usage limit, "Prompt is
        // too long", a 4xx/5xx, a dropped connection) and it is still the transcript tail. Drives
        // SessionState::Error through the ONE state machine, OVERRIDING the carrier event's normal
        // mapping (the synthetic line carries a terminal stop_reason that would otherwise read as a
        // clean turn-complete). The session leaves Error on the next real turn event (a
        // UserPromptSubmit -> Running), so no wire hook ever needs this flag.
        bool apiError{ false };
        // Engine-internal (never on the wire): the API-error REASON, carried alongside apiError so
        // OnHookEvent can preserve it onto SessionInfo.errorMessage/errorStatus for the Triage-Board
        // Error card. errorMessage = the synthetic line's text; errorStatus = its apiErrorStatus HTTP
        // code (0 when none). Meaningful only when apiError is set.
        std::wstring errorMessage;
        int errorStatus{ 0 };
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
        // Agentmaster: an API-error turn-ender (a synthetic isApiErrorMessage transcript line the
        // SessionScanner surfaced) -> Error, regardless of the carrier event. Engine-internal: no wire
        // hook ever sets m.apiError, so a real Claude hook can never take this path. The session leaves
        // Error on the next real turn event (e.g. UserPromptSubmit -> Running, in the switch below).
        if (m.apiError)
        {
            return SessionState::Error;
        }
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

    // Agentmaster (event ordering + turn identity). NextSessionState above is PURE on
    // (state, event) — it cannot tell a fresh Stop from a STALE one (the Stop forwarder reads
    // the transcript before connecting, so turn N's Stop can land AFTER turn N+1's
    // UserPromptSubmit and would wrongly demote a Running turn to WaitingForInput), and it
    // cannot see TYPE-AHEAD (Claude Code fires UserPromptSubmit at Enter-time for a prompt
    // queued mid-turn, then runs the queued batch as the next turn with NO further
    // UserPromptSubmit — so that batch turn used to run entirely in "WaitingForInput"). This
    // layer adds the two missing facts via TurnAccounting (SessionModels.h) + the wire `ts`
    // (the hook's FIRE time, stamped by the forwarder BEFORE any slow work):
    //   * stale Stop   — a non-quiescent Stop whose ts predates the newest UserPromptSubmit
    //     ended an OLDER turn: keep the current state; suppress its question-flag + advance.
    //   * type-ahead   — a UserPromptSubmit landing while a turn is in flight (Running /
    //     NeedsApproval) increments queuedPrompts; the next non-quiescent Stop CONSUMES the
    //     batch (queuedPrompts -> 0) and stays Running — Claude immediately processes the
    //     queued prompts as one next turn — instead of declaring turn-complete mid-conversation.
    //   * quiescent Stop — the scanner's synthesized missed-Stop (HookMessage::quiescentStop):
    //     the transcript is provably idle, so it always lands WaitingForInput and zeroes the
    //     accounting (a queued prompt that never produced a turn was consumed or canceled).
    // Self-healing by construction: every applied Stop zeroes queuedPrompts, so a phantom
    // UserPromptSubmit (e.g. a slash command that never starts an API turn) costs at most one
    // wrong-Running interval, which the scanner's quiescence reconciliation then ends. PURE on
    // its inputs (mutates only `turns`); ts==0 (an old forwarder) disables the stale check and
    // falls back to arrival order — exactly the pre-ordering behavior.
    inline constexpr int32_t kMaxQueuedPrompts = 8; // type-ahead cap (collapse-on-Stop bounds drift anyway)

    struct OrderedTransition
    {
        SessionState state{ SessionState::Idle }; // the next state
        bool staleStop{ false }; // Stop predated the newest prompt: bookkeeping-only (no question-flag, no advance)
        bool turnComplete{ false }; // a Stop cleanly completed the LAST outstanding turn (drives question-guard + advance)
    };

    inline OrderedTransition NextSessionStateOrdered(SessionState current, const HookMessage& m, TurnAccounting& turns) noexcept
    {
        OrderedTransition out;
        out.state = NextSessionState(current, m);
        // Agentmaster: an API-error turn-ender ended the turn ABNORMALLY (no clean Stop). out.state is
        // already Error (NextSessionState). Settle the type-ahead accounting like a Stop would (a queued
        // prompt that never produced its turn is now void), but it is NOT a clean turn boundary: leave
        // turnComplete false so no question-guard fires and Autopilot does NOT advance off an error (the
        // scheduler's stopOnError backstop handles the autopilot pause on the Error state itself).
        if (m.apiError)
        {
            turns.queuedPrompts = 0;
            return out;
        }
        switch (m.event)
        {
        case HookEvent::SessionStart:
        case HookEvent::SessionEnd:
            turns = {}; // a (re)start / end settles turn identity
            break;
        case HookEvent::UserPromptSubmit:
            // A prompt submitted while a turn is in flight (Running, or blocked on a permission
            // request) is QUEUED behind it — Claude consumes it as the next turn with no
            // further hook, so remember that the conversation outlives the next Stop.
            if (current == SessionState::Running || current == SessionState::NeedsApproval)
            {
                if (turns.queuedPrompts < kMaxQueuedPrompts)
                {
                    ++turns.queuedPrompts;
                }
            }
            if (m.ts > turns.lastPromptUnixMs)
            {
                turns.lastPromptUnixMs = m.ts; // monotonic: a late-arriving older prompt must not regress it
            }
            break;
        case HookEvent::Stop:
            if (!m.quiescentStop && m.ts != 0 && turns.lastPromptUnixMs != 0 && m.ts < turns.lastPromptUnixMs)
            {
                // STALE: this Stop FIRED before the newest prompt was submitted, so it ends an
                // older turn — the conversation has already moved on. Keep the current state.
                out.state = current;
                out.staleStop = true;
                break;
            }
            if (!m.quiescentStop && turns.queuedPrompts > 0)
            {
                // Type-ahead: prompts are queued behind the turn that just ended; Claude
                // immediately consumes the batch as the next turn (no further UserPromptSubmit),
                // so the session is still mid-conversation — NOT waiting for the user.
                turns.queuedPrompts = 0;
                out.state = SessionState::Running;
                break;
            }
            turns.queuedPrompts = 0;
            out.turnComplete = true; // out.state is already WaitingForInput (the base machine)
            break;
        default:
            break;
        }
        return out;
    }
}
