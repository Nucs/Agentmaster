// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — a topic message that WAITED in the queue, and the line that says when it went (MESSAGING.md §21.44). The user's
// request (2026-10-27): "When user sends a message on telegram which gets queued as we are actively running then the message will
// not be chronologically ordered which is fine since we queue up msgs. but if the queues msg gets dequeued and sent, we need to
// write a message with title mentioning that this message sent from queue".
//
// What happens:
//   - A message typed in a session's topic is a queued prompt (D8, §7.1). While a turn is in flight it WAITS: the turn's
//     remarks, its reply, a question all land in the topic BELOW the message, so the message sits above the turn that kept it
//     waiting. The one funnel every queued topic message takes (MessagingService::_queueRemotePrompt) records that moment on
//     the prompt (QueuedPrompt::remoteQueuedBusyUnixMs) when RemotePromptWait says it will wait.
//   - When the queue finally sends it and its words reach the transcript (the mirror's prompt item), the bot posts the user's
//     line where the turn really starts: "👤 You · 14:03 · 📱 from the phone · 📤 sent from the queue · ⏳ running" (PromptSource::
//     PhoneQueued, MessagingModels.h), the session's title row, the words. That line is the TURN'S PROMPT from then on: its
//     status is an edit of it (⏳ → ✅ …), the turn's reply quotes it, the next turn's line replies to it (§21.12, §21.17.3), and it
//     replies to the previous turn's prompt like every turn line. A message that went at a ready moment (an idle session: the
//     5 s remote settle, a dormant tab's start) keeps today's form: the user's own message is the prompt and its status is a
//     reaction — nothing was posted between it and its turn, so there is nothing to put right.
//   - The user's own message gets 🫡 (kSentFromQueueReaction) instead of the turn's status reactions: it was sent, and its turn
//     shows its status on the line below. Set by the mirror when it posts the line (the one moment that is known to be this
//     run's pickup, after a restart too); `_syncReactions` shows no 👨‍💻 for such a message and sets 🫡 itself only when the line
//     never came (SentFromQueueReactionOverdue).
//
// This header is the pure half (no Telegram call, no service, no registry): when a message waits, the words of why for the log,
// the mirror's test at the echo, the reaction and its fallback. The m5 suite pins every rule (tests_messaging.cpp, §21.44).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "MessagingSessionControl.h" // TurnInFlightState (§21.41): Running and Needs approval are a turn in flight
#include "SessionModels.h" // SessionInfo, QueuedPrompt, PromptStatus

namespace Agentmaster::Msg
{
    // ===== the queueing: will this message wait? ======================================================================

    /// <summary>
    /// Why a message of the topic that goes into the queue NOW will wait for a turn before it is sent (§21.44): what is ahead of
    /// it. Anything but None means the topic will show other posts between the message and its own turn, so its send gets the
    /// "📤 sent from the queue" line.
    /// </summary>
    enum class RemoteWait
    {
        None, // nothing is ahead of it: it goes at the session's next ready moment (after the 5 s remote settle)
        TurnInFlight, // the session's turn is in flight (Running, Needs approval): it goes when that turn ends
        DeliveryInFlight, // a prompt was just sent and has not been picked up yet: its turn starts first
        EarlierMessage, // an earlier message of the topic waits ahead of it in the queue: it goes, and its turn runs, first
    };

    /// <summary>
    /// Will a message of the topic that is queued NOW wait for a turn (§21.44)? Read inside the registry update that inserts it,
    /// so the facts are the queue and state of that very moment.
    /// </summary>
    /// <param name="s">The session as the insertion sees it (its state and queue, BEFORE the message is inserted).</param>
    /// <param name="insertAt">The index the message will take (D30: before the runner's own planned prompts, after earlier remote
    /// ones); every prompt at a lower index is ahead of it. A value past the end means "at the end".</param>
    /// <returns>The first reason found, in this order: the turn in flight, a delivery in flight, an earlier waiting prompt; None
    /// when nothing is ahead.</returns>
    /// <remarks>
    /// What it does NOT see, on purpose: a session in Error or Done also holds the message (neither state is ready, Rule #1), but
    /// no turn is running there and the message may wait for a restart that never comes — the user sees why on the card. A turn
    /// typed at the PC AFTER the message was queued and before it went is not foreseeable here either; such a message keeps
    /// today's form (MESSAGING.md §21.44's limits). A legacy Held prompt counts as waiting work (the scheduler makes it Pending
    /// again, and it stays ahead in the queue's order).
    /// </remarks>
    inline RemoteWait RemotePromptWait(const SessionInfo& s, size_t insertAt) noexcept
    {
        if (TurnInFlightState(s.state))
        {
            return RemoteWait::TurnInFlight;
        }
        const size_t ahead = insertAt < s.queue.size() ? insertAt : s.queue.size();
        bool earlier = false;
        for (size_t i = 0; i < s.queue.size(); ++i)
        {
            const QueuedPrompt& q = s.queue[i];
            if (q.status == PromptStatus::Sent && !q.echoed)
            {
                // Mark-Sent comes before the injection and the echo after it (Rule #4): a turn is about to start with it. A
                // Typed record is its own echo (echoed from birth) and a reloaded Sent prompt loads echoed, so neither counts.
                return RemoteWait::DeliveryInFlight;
            }
            if (i < ahead && (q.status == PromptStatus::Pending || q.status == PromptStatus::Held))
            {
                earlier = true; // keep looking: a delivery in flight further down is the better reason for the log
            }
        }
        return earlier ? RemoteWait::EarlierMessage : RemoteWait::None;
    }

    /// <summary>The reason in a log line's words ("" for None): "[msg] … queued from topic (…; waits: the turn is in flight)".</summary>
    /// <param name="w">The reason.</param>
    /// <returns>A static string.</returns>
    inline const wchar_t* RemoteWaitWords(RemoteWait w) noexcept
    {
        switch (w)
        {
        case RemoteWait::TurnInFlight:
            return L"the turn is in flight";
        case RemoteWait::DeliveryInFlight:
            return L"a prompt just sent has not started its turn";
        case RemoteWait::EarlierMessage:
            return L"an earlier message waits ahead of it";
        case RemoteWait::None:
        default:
            return L"";
        }
    }

    // ===== the echo: the mirror's test, and the user's message =======================================================

    /// <summary>
    /// The reaction the user's own message gets once the queue sent it and the mirror posted its "📤 sent from the queue" line:
    /// 🫡 (U+1FAE1, one of the reactions a bot may set — the Bot API's ReactionTypeEmoji list, checked 2026-10-27; written
    /// without a variation selector, the list's own spelling). Final: the turn's status shows on the line below, so the message
    /// never changes again.
    /// </summary>
    inline constexpr std::wstring_view kSentFromQueueReaction = L"\U0001FAE1";

    /// <summary>
    /// How long after its send a message that waited may go without its line before `_syncReactions` sets 🫡 itself (two
    /// minutes): the mirror reads the transcript every tick, so a line that has not come by then never will (its transcript
    /// prompt matched another queue entry, or the read failed) — and the message must not keep its ✍ ("queued") forever.
    /// </summary>
    inline constexpr int64_t kSentFromQueueReactionFallbackMs = 2 * 60 * 1000;

    /// <summary>Is the prompt a message typed in the session's Telegram topic (QueuedPrompt::remoteSource "telegram:&lt;id&gt;")?</summary>
    /// <param name="q">The queued prompt.</param>
    /// <returns>True for a topic message; false for every prompt the PC or the runner queued.</returns>
    inline bool IsTopicPrompt(const QueuedPrompt& q) noexcept
    {
        return std::wstring_view{ q.remoteSource }.starts_with(L"telegram:");
    }

    /// <summary>Did the topic message wait in the queue (§21.44: QueuedPrompt::remoteQueuedBusyUnixMs set when it was queued)?</summary>
    /// <param name="q">The queued prompt.</param>
    /// <returns>True for a topic message queued while the session was busy; false for any other prompt.</returns>
    inline bool PromptWaitedInQueue(const QueuedPrompt& q) noexcept
    {
        return q.remoteQueuedBusyUnixMs > 0 && IsTopicPrompt(q);
    }

    /// <summary>
    /// The mirror's test at a prompt's echo (§21.44): is the queue entry that a transcript prompt matched a topic message that
    /// WAITED and that the queue SENT? Then the bot posts the "📤 sent from the queue" line, and it becomes the turn's prompt.
    /// </summary>
    /// <param name="q">The queue entry the transcript prompt matched (MessagingService::_applyItems' text match).</param>
    /// <returns>True when the line is due.</returns>
    /// <remarks>
    /// A Pending match is NOT one: the queue has not sent it, so the transcript prompt is someone else's with the same words (the
    /// same "yes" typed at the PC while the phone's "yes" waits) — it keeps today's handling, and the waiting message gets its
    /// own line when it goes.
    /// </remarks>
    inline bool PromptWentFromQueue(const QueuedPrompt& q) noexcept
    {
        return PromptWaitedInQueue(q) && q.status == PromptStatus::Sent;
    }

    /// <summary>
    /// `_syncReactions`' fallback (§21.44): a message that waited, was sent and picked up, and still shows ✍ two minutes after its
    /// send (kSentFromQueueReactionFallbackMs) — the mirror never posted its line — gets 🫡 from the reaction sync instead.
    /// </summary>
    /// <param name="q">The queued prompt.</param>
    /// <param name="nowMs">The wall clock (unix ms).</param>
    /// <returns>True when the fallback is due. A clock that stepped back (now before the send) reads as not due: the next ticks
    /// decide.</returns>
    inline bool SentFromQueueReactionOverdue(const QueuedPrompt& q, int64_t nowMs) noexcept
    {
        return PromptWentFromQueue(q) && q.echoed && q.sentAtUnixMs > 0 && nowMs - q.sentAtUnixMs >= kSentFromQueueReactionFallbackMs;
    }
}
