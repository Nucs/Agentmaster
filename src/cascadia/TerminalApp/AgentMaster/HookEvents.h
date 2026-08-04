// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
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
        // Feeds the question-guard so Autorunner does not auto-answer a clarifying question.
        bool lastMessageIsQuestion{ false };
        // Notification specifically requesting tool permission (vs. an idle notification).
        // Drives NeedsApproval / ApprovalPolicy rather than the prompt queue.
        bool permissionRequest{ false };
        std::wstring tool; // associated tool name, when applicable
        // The submitted prompt body, set ONLY on UserPromptSubmit (escaped on the wire). Lets
        // the registry record EVERY message a session received — including ones the human
        // typed straight into the ConPTY, not just ones we queued — into the Auto Testing.
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
        // Engine-internal (never on the wire): set ONLY by the SessionScanner's recon-subagent
        // promotion (ShouldSynthesizeRunningFromExternalWork). The carrier PostToolUse reports
        // EXTERNAL work — a subagent/teammate side-file write or the busy/shell heartbeat — not a
        // turn of THIS conversation, so IsApiTurnEvidence must not let it stamp lastTurnUnixMs: a
        // teammate's/subagent's API call runs in its OWN context and never re-warms the LEAD
        // conversation's prefix cache (the ⚡ hint would otherwise light off every promotion).
        bool externalWorkActivity{ false };
    };

    // Agentmaster (⚡ server-cache hint, SessionModels.h ServerCacheStillWarm) — PURE + unit-tested.
    // Does this hook event EVIDENCE a real API request just made for this conversation? That — and
    // only that — is what writes/refreshes Claude's server-side prompt cache, so only these events
    // may stamp SessionInfo::lastTurnUnixMs (the hook-side half of the Triage-Board ⚡ hint).
    // Deliberately EXCLUDED:
    //   • SessionStart — fires at launch / `--resume` (restore, Sessions-page resume, ADOPT) /
    //     an in-session /clear or /compact. NONE of those send an API request (a resume doesn't
    //     touch the API until the next prompt), yet each stamps the decay anchor "now" — the exact
    //     "⚡ right after adopting" false positive.
    //   • SessionEnd — claude exiting; its last turn's Stop already counted.
    //   • A synthesized QUIESCENT Stop — reconciliation-TIMED, not turn-timed: recon-stop fires ≥2s
    //     after quiescence, the presence-idle release 5–30s late, and recon-error-release on a
    //     double-ESC rewind that involves NO API call at all. A REAL Stop (the turn's clean end,
    //     cache just re-written) counts.
    //   • SubagentStop — a SUBAGENT'S/TEAMMATE'S turn just ended, but that turn ran in the
    //     subagent's OWN context: it never touches the LEAD conversation's prefix cache, so it is
    //     not evidence THIS session's cache is warm. In-process TEAMMATES (Claude Code teams) fire
    //     SubagentStop minutes–tens-of-minutes after the lead's last real turn (measured: firings
    //     correlate to teammate deliveries to the second, 3–25 min post-turn; 1109 SubagentStops
    //     over a 44-day teams-heavy log), which kept ⚡ lit on a long-cold lead; and during a classic
    //     >cacheMinutes Task wait the lead's cache GENUINELY expires — the old mid-turn stamp was
    //     masking a true expiry, not preventing a false one. (The lead's own next request — the
    //     tool_result turn / the wake turn's UserPromptSubmit — re-lights it honestly.)
    //   • A synthesized external-work PostToolUse (HookMessage::externalWorkActivity — the
    //     scanner's recon-subagent promotion): the observed activity is a side-file write / the
    //     busy-or-shell heartbeat — work OUTSIDE this conversation; same reasoning as SubagentStop.
    // INCLUDED: UserPromptSubmit (the request fires on submit), Pre-/PostToolUse for REAL parent-turn
    // activity (not registered as wire hooks today, but the scanner's recon-run/recon-resume synths
    // ride UserPromptSubmit/PostToolUse for genuine parent-turn work and still count), and
    // Notification (a permission ask / idle nudge arrives mid- or moments-after-turn; the scanner's
    // recon-error synth rides Notification too — the failed request WAS an API attempt moments ago).
    inline bool IsApiTurnEvidence(const HookMessage& m) noexcept
    {
        switch (m.event)
        {
        case HookEvent::UserPromptSubmit:
        case HookEvent::PreToolUse:
        case HookEvent::Notification:
            return true;
        case HookEvent::PostToolUse:
            return !m.externalWorkActivity; // real parent-turn activity counts; the recon-subagent external-work synth does not
        case HookEvent::Stop:
            return !m.quiescentStop; // a real turn-end counts; a reconciliation-timed synth does not
        case HookEvent::SubagentStop: // a subagent/teammate turn — its API call is in ITS OWN context, never the lead's cache
        case HookEvent::SessionStart:
        case HookEvent::SessionEnd:
        case HookEvent::Unknown:
        default:
            return false;
        }
    }

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
            // Agentmaster (Rule #16 / #7): a SessionStart fires at fresh launch, at a `--resume` (crash/
            // window restore, Sessions-page resume, adopt, AND a re-homed/background tab's LAZY claude
            // start on FIRST ACTIVATION — the ConPTY spawns claude only on the control's first non-zero
            // layout), and at an in-session /clear or /compact. A resume/lazy-start RELOADS the
            // conversation but does NOT continue the turn — the transcript tail still reflects the same
            // AT-REST state — so it must NOT clobber an at-rest "needs-you" triage (WaitingForInput /
            // NeedsApproval). That state may have come from a real turn-complete, the persisted
            // crash-restore seed (RestoredSessionState preserves the SAME two states — this mirrors it,
            // because a lazy-start SessionStart IS a resume), or the user's explicit "Move to
            // Waiting-for-you" / "Mark Unread" promote. Preserving it here is what makes those cues
            // SURVIVE opening the tab: the lazy-start SessionStart used to reset them to Idle the instant
            // you focused the tab (the reported "Move to Waiting-for-you then activate the inactive tab
            // resets to Idle/Done" bug — a background/restored tab's claude fires SessionStart on the
            // first visit). A genuine new turn (UserPromptSubmit -> Running) still clears it; every OTHER
            // current state resets to Idle (fresh launch — a new record starts Idle anyway — plus a
            // Running/Error/Done restart and a /clear from idle). Turn identity still resets in
            // NextSessionStateOrdered (turns = {}).
            return (current == SessionState::WaitingForInput || current == SessionState::NeedsApproval)
                       ? current
                       : SessionState::Idle;
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

    // Agentmaster (DELIVERY.md §9 — the duplicate-Stop floor): a NON-quiescent Stop may complete
    // the newest turn ONLY if that turn has demonstrably RUN — its wire ts must be at least this
    // far past the newest UserPromptSubmit's ts. The live incident (b5f766fc, 08:44): a turn's
    // Stop hook fired TWICE; the duplicate's forwarder (pwsh, slow spawn under load) stamped its
    // ts ~3.6s after the turn actually ended — 15ms AFTER the NEXT prompt's UserPromptSubmit ts —
    // so the strict `ts < lastPrompt` staleness test passed it, it read as the new turn's INSTANT
    // completion, state flipped WaitingForInput, and the advance seam delivered the next queued
    // prompt into claude's RUNNING turn (the pasted text was consumed by an AskUserQuestion dialog
    // and lost — never became a message). No real turn completes in 15ms: an API roundtrip alone
    // is ~1s, and the fastest measured trivial turn in the incident corpus is 3.4s; the observed
    // duplicate-stamp skew is <=0.4s. 2000ms sits comfortably between. A genuinely-faster-than-
    // floor turn self-heals: its Stop reads stale (state kept), and the scanner's quiescent Stop —
    // EXEMPT from both checks — reconciles to WaitingForInput ~2.5s later, advance included.
    inline constexpr int64_t kMinRealTurnSpanMs = 2000;

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
        // turnComplete false so no question-guard fires and Autorunner does NOT advance off an error (the
        // scheduler's stopOnError backstop handles the autorunner pause on the Error state itself).
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
            // Agentmaster (DELIVERY_PLAN.md R3 — phantom/duplicate UserPromptSubmit hardening): only a
            // PROMPT-CARRYING UserPromptSubmit is a turn's start for the ACCOUNTING. The live incident
            // (b5f766fc, DELIVERY.md §10): each real hook fired a late EMPTY twin ~0.5-2.6s after it
            // (08:42:28.904 / 08:44:53.902 / 08:50:14.515 — matched NO transcript user message and
            // recorded NO Typed row, so its promptText was provably empty). Each twin's late ts bumped
            // lastPromptUnixMs — which floor-suppressed the next REAL Stop (kMinRealTurnSpanMs read it
            // as too-fast) and spuriously released the pickup guard's "a newer prompt stamp proves the
            // pickup" clause — and a twin landing mid-turn ++queuedPrompts, making the real Stop read
            // as a type-ahead consume (stuck Running until the quiescent synth healed it ~2.5s later).
            // A REAL submit always carries text (claude refuses an empty submit — Enter on an empty box
            // is a no-op — and the AskUserQuestion dialog answer rides PostToolUse, never a UPS: proven
            // live at 08:48:47), so an empty-prompt UPS gets the STATE effect only (-> Running, which
            // self-heals if phantom — the machine's documented property) and touches no accounting.
            // This also covers the scanner's recon-run SYNTH (deliberately empty promptText): its
            // reconciliation-timed stamp was itself a floor-suppression hazard for a hooked session
            // whose real Stop landed within the floor of the late synth time. Known narrow edge: an
            // image-only submit (if claude ever reports it promptless) would skip the type-ahead count;
            // the delivery gate + §9 floor still bound the damage to one quiescent-heal.
            if (!m.promptText.empty())
            {
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
            if (!m.quiescentStop && m.ts != 0 && turns.lastPromptUnixMs != 0 &&
                (m.ts - turns.lastPromptUnixMs) < kMinRealTurnSpanMs)
            {
                // TOO FAST to be the newest turn's real completion (kMinRealTurnSpanMs above): a
                // DUPLICATE Stop of an older turn whose slow forwarder stamped its ts after the
                // newest prompt beats the strict `ts < lastPrompt` test — this floor catches it.
                // Same handling as stale: keep state, suppress the question-bit + advance. The
                // accounting is NOT zeroed (this Stop is bookkeeping noise, not a boundary), and a
                // genuinely sub-floor turn is reconciled by the scanner's exempt quiescent Stop.
                // Placed AFTER the type-ahead branch on purpose: a queued batch's consume must stay
                // floor-free (its Stop legitimately lands close behind the type-ahead prompt).
                out.state = current;
                out.staleStop = true;
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
