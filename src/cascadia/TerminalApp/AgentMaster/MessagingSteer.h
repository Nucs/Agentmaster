// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — /steer: a message sent NOW from a session's Telegram topic, into a running turn too (MESSAGING.md §21.33). The
// user's request (2026-10-19): "Add support for /steer which is like send-now".
//
// What a message typed in a topic does by itself is D8, the user's rule of 2026-10-16: it is QUEUED, never a steer — it waits in
// the session's queue and goes out when the turn ends. /steer is the explicit other way: "/steer also run the tests" types its
// words into the session's input box AT ONCE and submits them — the Manager's Send now, through the one send seam
// (SessionRegistry::SubmitPrompt: the delivery gate, the draft swap that keeps the user's unsent draft at the PC, the verified
// placement, the Enter rescue), with a NEW prompt instead of the first queued one. What the session does with the words is Claude
// Code's own handling of a message submitted while it works: it HOLDS the message, the running turn takes it in at its next step
// (a queued_command attachment in the transcript, "absorbed mid-turn"), and a turn that ends first starts the next one with it. A
// session at rest simply starts a turn with the words.
//
// The words never go where a send is impossible or harmful (DecideSteer): a closed session has nothing to type into; a tab whose
// claude is not started drops what is written into it; a tab whose claude ENDED may hold the shell that hosted it, where typed
// words would RUN as a command; and a MENU on the terminal (a question, a permission prompt, a picker) takes Enter as a pick.
// Those keep the words in the queue — at its FRONT, ahead of everything that waits — and say why. A send the hosting window
// still declines (its pre-flight found a menu that opened in between, another delivery held the box) puts the words back as the
// first queued prompt; the service tells the user once (MessagingService::_sweepSteers), and the runner sends them at the
// session's next ready moment like any message of the topic (D30: whatever the runner's mode).
//
// The status is, as for every message of a topic, a REACTION on the user's own message: ⚡ while the words are being sent, 👨‍💻
// once claude took them (their echo; MessagingService::_syncReactions), ✍ while they wait in the queue.
//
// This header is the pure half (no service, no Windows, no Telegram call): the command word, the decision, the texts, the prompt
// and its place in the queue, and the watch's verdict. The m5 suite pins every rule (tests_messaging_steer.cpp); the service
// (Messaging.Inbound.cpp: MessagingService::_steerRemotePrompt and _sweepSteers) sends, reacts and replies.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Scheduler.h" // kAnswersQuestionOk (a steer that comes back to the queue still passes the question guard)
#include "SessionModels.h" // QueuedPrompt, PromptStatus, PromptGate, PromptOrigin, SessionState, InputBoxState (PendingInput.h)
#include "TelegramFormat.h" // TgFirstLine (the prompt's label)

namespace Agentmaster::Msg
{
    // ===== the command and its constants ================================================================================

    /// <summary>
    /// The command word (§21.33): "/steer &lt;message&gt;". It is in kAgentmasterCommands, so a session's topic never queues it for
    /// Claude Code — which has no /steer of its own (none in the 2.1.291 binary's command table), so the name hides nothing of it,
    /// and "/cc /steer …" still forces the word through. The 🔎 Search topic refuses it as a session's command (it has no session).
    /// </summary>
    inline constexpr std::wstring_view kSteerCommand = L"steer";

    /// <summary>
    /// How long the service watches a steer it sent (MessagingService::_sweepSteers, ClassifySteerWatch): until claude took the
    /// words (their echo), until the hosting window put them back in the queue (a declined delivery: the user is told once), or
    /// this long. Past it the delivery's own safety nets own the prompt: the Enter rescue, and the lost-send verdict that fails it
    /// with a 👎 and a line in the topic (MessagingService::_syncReactions).
    /// </summary>
    inline constexpr int64_t kSteerWatchMaxMs = 10 * 60 * 1000;

    /// <summary>
    /// The reaction a steer's message shows while its words are being sent: "⚡", one of the reactions Telegram lets a bot set,
    /// written WITHOUT a variation selector (Telegram refuses a reaction that is not exactly one of its list). A message whose
    /// words wait in the queue shows "✍" (kSteerQueuedReaction), one whose words claude took "👨‍💻" (_syncReactions).
    /// </summary>
    inline constexpr std::wstring_view kSteerSendingReaction = L"\x26A1";

    /// <summary>The reaction of a message whose words wait in the queue ("✍"; MessagingService::_queueRemotePrompt sets the same).</summary>
    inline constexpr std::wstring_view kSteerQueuedReaction = L"\x270D";

    // ===== the decision =============================================================================================

    /// <summary>What a /steer does (DecideSteer), by the facts of the session it is sent to.</summary>
    enum class SteerAction
    {
        Usage, // no words after the command: the usage line, nothing else
        Closed, // the session is closed (no open tab): nothing to type into, and nothing is queued
        Send, // typed and submitted NOW (SessionRegistry::SubmitPrompt, the Manager's Send now)
        QueueDormant, // the tab's claude is not started (a restored or parked tab): the tab is started, the words wait FIRST
        QueueEnded, // claude ended in the tab (Done): the words wait FIRST, for a restart (/reactivate)
        QueueMenu, // a menu is open on the session's terminal (a question, a permission prompt, a picker): the words wait FIRST
    };

    /// <summary>
    /// Decides what a /steer does (§21.33). Only a fact that makes a send impossible or harmful keeps the words in the queue — at
    /// its FRONT, so they still go before everything that waits there:
    ///   - a CLOSED session has no input to type into (Closed: nothing is queued; the reply points to the 🔎 Search topic);
    ///   - a tab whose claude is NOT STARTED drops what is written into it (DELIVERY.md §17.2): the tab is started, and the words
    ///     go once it is up (QueueDormant). An adopted session is never "not started": its tab is the user's own running shell;
    ///   - claude ENDED in the tab (Done): the tab may hold the shell that hosted it, where typed words would RUN as a command
    ///     (QueueEnded; /reactivate starts it again);
    ///   - the last read of the session's box saw a MENU (a question, a permission prompt, a picker): Enter would pick an option
    ///     (QueueMenu), whatever the state says.
    /// Every other state is sent at once: Running above all (that is the steer), Waiting for you and Idle (a turn starts), Error
    /// (the words are the retry), and Needs approval whose box reads as a plain one (a stale question nobody answers any more).
    /// The hosting window's own pre-flight reads the box again before it types, and puts the words back in the queue when a menu
    /// opened in between (MessagingService::_sweepSteers then tells the user).
    /// </summary>
    /// <param name="words">The words after /steer (ParseRemoteCommand's args: trimmed, every line).</param>
    /// <param name="live">The session has an open tab (SessionInfo::live).</param>
    /// <param name="started">Its claude was started this run (SessionInfo::started).</param>
    /// <param name="external">An adopted session (SessionInfo::external): its tab is the user's own running shell.</param>
    /// <param name="state">The session's state.</param>
    /// <param name="boxState">The last read of its input box (SessionInfo::pendingBoxState).</param>
    /// <returns>The action.</returns>
    inline SteerAction DecideSteer(std::wstring_view words, bool live, bool started, bool external, SessionState state, InputBoxState boxState) noexcept
    {
        if (words.find_first_not_of(L" \t\r\n") == std::wstring_view::npos)
        {
            return SteerAction::Usage;
        }
        if (!live)
        {
            return SteerAction::Closed;
        }
        if (!started && !external)
        {
            return SteerAction::QueueDormant;
        }
        if (state == SessionState::Done)
        {
            return SteerAction::QueueEnded;
        }
        if (boxState == InputBoxState::MenuOpen)
        {
            return SteerAction::QueueMenu;
        }
        return SteerAction::Send;
    }

    /// <summary>
    /// The bot's answer to a /steer that was NOT sent at once (DecideSteer), as a reply to the command message. "" for Send: its
    /// status is a reaction on the user's own message (⚡ sending, 👨‍💻 claude took it), as for every message of a topic.
    /// </summary>
    /// <param name="action">What the /steer did.</param>
    /// <returns>Telegram HTML, or "" for SteerAction::Send.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring SteerReplyHtml(SteerAction action)
    {
        switch (action)
        {
        case SteerAction::Usage:
            return L"Usage: /steer &lt;message&gt; \x2014 sends it to the session NOW, into a running turn too, like Send now at the PC. "
                   L"A plain message waits in the queue until the turn ends.";
        case SteerAction::Closed:
            return L"This session is closed. Resume it from the \U0001F50E Search topic.";
        case SteerAction::QueueDormant:
            return L"The tab's claude is not running: it starts now, and your message goes first once it is up.";
        case SteerAction::QueueEnded:
            return L"Claude has ended in this tab: your message waits first in the queue. /reactivate starts the session again, and then it goes out.";
        case SteerAction::QueueMenu:
            return L"A question or a menu is open in the session, and typing would answer it: your message waits first in the queue and goes out once it is closed.";
        case SteerAction::Send:
        default:
            return {};
        }
    }

    // ===== a steer that came back to the queue ===========================================================================

    /// <summary>Why a steer that was SENT came back to the queue (SteerDeclinedNoticeHtml).</summary>
    enum class SteerDecline
    {
        BoxHeld, // declined at once: another delivery or a box clear holds the session's input box (the delivery gate)
        Dormant, // the hosting window found the tab's claude not started
        Menu, // its pre-flight read a menu on the terminal (the box state it recorded)
        NoBox, // its pre-flight found no input box on screen (the box state it recorded)
        Other, // anything else the window declined for (a draft it could not move aside first, a swap already in flight)
    };

    /// <summary>
    /// Reads why the hosting window put a steer back in the queue, from the facts its decline left behind: a window that finds the
    /// tab's claude not started, or a box it cannot type into, records that (SessionInfo::started, the box state its pre-flight
    /// read — SessionRegistry::SetPendingBoxState); any other decline leaves no fact, and reads as Other.
    /// </summary>
    /// <param name="boxState">The session's box state now (SessionInfo::pendingBoxState).</param>
    /// <param name="started">Its claude was started this run (SessionInfo::started).</param>
    /// <param name="external">An adopted session (SessionInfo::external): never "not started".</param>
    /// <returns>The reason.</returns>
    inline SteerDecline SteerDeclineFor(InputBoxState boxState, bool started, bool external) noexcept
    {
        if (!started && !external)
        {
            return SteerDecline::Dormant;
        }
        if (boxState == InputBoxState::MenuOpen)
        {
            return SteerDecline::Menu;
        }
        if (boxState == InputBoxState::NoBox)
        {
            return SteerDecline::NoBox;
        }
        return SteerDecline::Other;
    }

    /// <summary>
    /// The one line the bot posts when a steer it SENT came back to the queue (declined at once by the delivery gate, or later by
    /// the hosting window): the words are now the first prompt that waits, and the runner sends them at the session's next ready
    /// moment, as it sends every message of the topic (D30: whatever the runner's mode).
    /// </summary>
    /// <param name="why">The reason (SteerDeclineFor, or BoxHeld for a delivery gate that was closed to it).</param>
    /// <returns>Telegram HTML, one line.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring SteerDeclinedNoticeHtml(SteerDecline why)
    {
        const wchar_t* reason = L"the session's input box could not take it at once";
        switch (why)
        {
        case SteerDecline::BoxHeld:
            reason = L"another delivery is typing into the session's input box";
            break;
        case SteerDecline::Dormant:
            reason = L"the tab's claude is not running";
            break;
        case SteerDecline::Menu:
            reason = L"a question or a menu is open in the session";
            break;
        case SteerDecline::NoBox:
            reason = L"the session's input box is not on screen";
            break;
        case SteerDecline::Other:
        default:
            break;
        }
        return std::wstring{ L"\x21A9 Not sent now: " } + reason + L". Your message waits first in the queue and goes out when the session is ready.";
    }

    // ===== the prompt and its place ===================================================================================

    /// <summary>
    /// Where a steer goes in the session's queue: right before the first prompt that still waits (Pending; the legacy Held too) —
    /// after the history of what was sent, ahead of every queued prompt, the topic's own included —, so the queue stays in the
    /// order things go out, and words that come back to the queue (a declined delivery) are the very next ones sent.
    /// </summary>
    /// <param name="queue">The session's queue.</param>
    /// <returns>The index to insert at; queue.size() when nothing waits.</returns>
    inline size_t SteerInsertIndex(const std::vector<QueuedPrompt>& queue) noexcept
    {
        for (size_t i = 0; i < queue.size(); ++i)
        {
            if (queue[i].status == PromptStatus::Pending || queue[i].status == PromptStatus::Held)
            {
                return i;
            }
        }
        return queue.size();
    }

    /// <summary>
    /// The queue entry of a steer: a remote prompt like every message of the topic (MessagingService::_queueRemotePrompt's fields —
    /// the Autorun origin every delivery-evidence rule keys on, the question guard's pass, the "telegram:&lt;message id&gt;" source
    /// that carries the status to the user's message as a reaction and keeps the mirror from posting the words again) — and, when
    /// it is sent at once, already marked Sent the way the Manager's Send now marks a prompt before the send seam (one attempt,
    /// the send time, no echo yet, no Enter retry, no injection yet).
    /// </summary>
    /// <param name="id">The prompt's id (a fresh one).</param>
    /// <param name="text">The words.</param>
    /// <param name="messageId">The /steer message's id in the topic.</param>
    /// <param name="sendNow">Marked Sent (SteerAction::Send) — else Pending (the words wait first in the queue).</param>
    /// <param name="nowMs">The clock (unix ms): the send time of a prompt sent now.</param>
    /// <returns>The prompt.</returns>
    /// <exception cref="std::bad_alloc">If the prompt cannot be allocated.</exception>
    inline QueuedPrompt BuildSteerPrompt(std::wstring id, std::wstring text, int64_t messageId, bool sendNow, int64_t nowMs)
    {
        QueuedPrompt p;
        p.id = std::move(id);
        p.label = TgFirstLine(text, 56);
        p.text = std::move(text);
        p.gate = PromptGate::OnTurnComplete; // what a prompt that comes back to the queue waits for, like any message of the topic
        p.origin = PromptOrigin::Autorun; // every delivery-evidence rule keys on Autorun (QueuedPrompt::remoteSource)
        p.guardPattern = std::wstring{ kAnswersQuestionOk }; // the user's own words answer an open question themselves
        p.remoteSource = L"telegram:" + std::to_wstring(messageId);
        if (sendNow)
        {
            p.status = PromptStatus::Sent;
            p.attempts = 1;
            p.sentAtUnixMs = nowMs;
            p.echoed = false;
            p.enterRetries = 0;
            p.injectedAtUnixMs = 0;
        }
        else
        {
            p.status = PromptStatus::Pending;
        }
        return p;
    }

    // ===== the watch ====================================================================================================

    /// <summary>
    /// A steer the service SENT and watches until its outcome is known (MessagingService::_steers, keyed by the prompt's id): who
    /// to tell if the words come back to the queue, and since when it watches.
    /// </summary>
    struct SteerWatch
    {
        std::wstring sessionId; // the session the words were sent to
        int64_t messageId{ 0 }; // the /steer message in the topic (the notice quotes it; its reaction turns ✍)
        int64_t threadId{ 0 }; // the topic
        int64_t sinceMs{ 0 }; // when the words were sent (unix ms): the watch ends kSteerWatchMaxMs later
    };

    /// <summary>What a watched steer's prompt shows now (ClassifySteerWatch).</summary>
    enum class SteerOutcome
    {
        Watching, // still Sent, claude has not taken it yet, inside the watch: look again at the next tick
        Taken, // claude took the words (Sent and echoed): the 👨‍💻 reaction says so (_syncReactions); nothing to tell
        BackInQueue, // the hosting window put them back as a waiting prompt (Pending): tell the user once, ✍ on the message
        Resolved, // failed or otherwise resolved: the delivery's own path speaks (a 👎 and its line)
        Gone, // no longer in the queue (removed by the user, a session reset): nothing to tell
        Expired, // still not taken after kSteerWatchMaxMs: the delivery's own safety nets own it now
    };

    /// <summary>
    /// The watch's verdict on a steer's prompt, at one tick of the service. A prompt that is Pending again was put back by a
    /// declined delivery (SessionRegistry::RollbackPromptToPending, or a reclaim of a send that never reached the box); if the
    /// runner already sent it again by the time of the look, it reads Sent and is simply watched on.
    /// </summary>
    /// <param name="prompt">The prompt in the session's queue now (nullptr: it is not there any more).</param>
    /// <param name="sinceMs">When the steer was sent (SteerWatch::sinceMs).</param>
    /// <param name="nowMs">The clock.</param>
    /// <returns>The outcome.</returns>
    inline SteerOutcome ClassifySteerWatch(const QueuedPrompt* prompt, int64_t sinceMs, int64_t nowMs) noexcept
    {
        if (prompt == nullptr)
        {
            return SteerOutcome::Gone;
        }
        switch (prompt->status)
        {
        case PromptStatus::Pending:
        case PromptStatus::Held:
            return SteerOutcome::BackInQueue;
        case PromptStatus::Sent:
            if (prompt->echoed)
            {
                return SteerOutcome::Taken;
            }
            return nowMs - sinceMs >= kSteerWatchMaxMs ? SteerOutcome::Expired : SteerOutcome::Watching;
        case PromptStatus::Failed:
        case PromptStatus::Skipped:
        default:
            return SteerOutcome::Resolved;
        }
    }
}
