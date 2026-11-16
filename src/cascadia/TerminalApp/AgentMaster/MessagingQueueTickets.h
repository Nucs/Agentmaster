// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — QUEUE MANAGEMENT from a session's Telegram topic (MESSAGING.md §21.57). The user's request (2026-11-16): "I want on
// telegram, queued messages should maybe get a Steer button and Delete button, what else is necessary for queue management
// lifecycle on telegram" — then the decisions of §21.57.3: a TICKET under each waiting message, the number's keycap on its line and
// on its buttons; ⚡ Steer, 🗑 Delete, ⏫ Move Up, 🔗 Merge (after an approved preview, the second text on a NEW line); ↩ Undo; a
// reply of ❌ removes, a reply of ➕ queues a message again; a message deleted in Telegram is left out at its send; and LEFT-OVER
// messages (their session closed or parked while they waited) follow the runner like the PC's own queue.
//
// It WRAPS the one queue (SessionInfo::queue): every control is an operation the PC's Prompt Queue already has (Send now, Delete,
// Move up) or a plain queueing; the ticket is a VIEW of the queue, rebuilt from it at every tick, so a change made at the PC shows
// in Telegram and the other way round.
//
// This header is the pure half (no Telegram call, no service, no registry): the button data, the keycaps, why a prompt waits, the
// ticket's text and buttons for each phase of its life, the merge's text, the ❌ / ➕ reply reading, an edit's reading, the /queue
// list, and the tickets' state-file form. The m5 suite pins every rule (tests_messaging_queue.cpp); the service
// (Messaging.Queue.cpp) posts, edits and deletes the tickets and runs the controls.

#pragma once

#include <algorithm> // std::find, std::any_of
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Json.h"
#include "MessagingFromQueue.h" // IsTopicPrompt (a prompt typed in the topic), TurnInFlightState (through MessagingSessionControl.h)
#include "MessagingModels.h" // RemoteCommand / RemoteKind (an edit's reading), WithTopicTitleRowAfterHeader (the ticket's title row)
#include "SessionModels.h" // QueuedPrompt, RemotePromptPart, SessionInfo, SessionState, PromptStatus, AutorunnerMode
#include "TelegramFormat.h" // TgEscapeHtml, TgFirstLine, TgClockText

namespace Agentmaster::Msg
{
    // ===== the button data =============================================================================================

    /// <summary>
    /// The prefix of every queue button's data: "pq:&lt;verb&gt;:&lt;id&gt;[:&lt;arg&gt;]" ("phone queue"). Unique among the bot's button
    /// families — "q:" is the question card's (§21.30), "c:" the card's, "t:" a token's, "tm:" a menu card's, "dl:" a download's,
    /// "nt:" a new thread's, "s:" the Search topic's. STATELESS (no token, no expiry): a prompt can wait for hours in a dormant tab
    /// or an Error, and a tap acts on the queue as it is NOW, answering "already sent" when the prompt is gone.
    /// </summary>
    inline constexpr std::wstring_view kQueueCallbackPrefix = L"pq:";

    /// <summary>⚡ Steer: the prompt goes NOW — /steer's path for a prompt that already waits (§21.33's decision and watch).</summary>
    inline constexpr std::wstring_view kQueueVerbSteer = L"steer";
    /// <summary>🗑 Delete: the prompt leaves the queue; its ✍ comes off; ↩ Undo for kQueueUndoWindowMs.</summary>
    inline constexpr std::wstring_view kQueueVerbDelete = L"del";
    /// <summary>⏫ Move Up: the prompt swaps places with the waiting prompt above it (the PC's Move up).</summary>
    inline constexpr std::wstring_view kQueueVerbMoveUp = L"up";
    /// <summary>🔗 Merge ↑: the ticket turns into the merge's PREVIEW (the user approves it first, §21.57.3 decision 2).</summary>
    inline constexpr std::wstring_view kQueueVerbMerge = L"merge";
    /// <summary>✅ Merge on the preview: "pq:mok:&lt;lower id&gt;:&lt;the upper id's first 8&gt;" — the merge is done only into the prompt the preview showed.</summary>
    inline constexpr std::wstring_view kQueueVerbMergeYes = L"mok";
    /// <summary>✕ Cancel on the preview: the ticket goes back to its queue line.</summary>
    inline constexpr std::wstring_view kQueueVerbMergeNo = L"mno";
    /// <summary>↩ Undo on a removed ticket: the prompt goes back where it was (within kQueueUndoWindowMs).</summary>
    inline constexpr std::wstring_view kQueueVerbUndo = L"undo";
    /// <summary>🔁 Retry on a failed prompt's notice: its words are queued again as a new prompt.</summary>
    inline constexpr std::wstring_view kQueueVerbRetry = L"retry";
    /// <summary>🗑 Dismiss on a failed prompt's notice: the buttons go, the notice stays.</summary>
    inline constexpr std::wstring_view kQueueVerbDismiss = L"dismiss";
    /// <summary>The /queue list's ✖: removes the prompt and edits the list in place (its numbers stay right).</summary>
    inline constexpr std::wstring_view kQueueVerbListRemove = L"x";
    /// <summary>The /queue list's ▶ Send next now: "pq:next:&lt;SESSION id&gt;" (the one verb whose id is the session's, not a prompt's).</summary>
    inline constexpr std::wstring_view kQueueVerbListNext = L"next";

    // ===== timing and marks =============================================================================================

    /// <summary>How long a removed prompt's ticket offers ↩ Undo (two minutes; then one edit takes the button off).</summary>
    inline constexpr int64_t kQueueUndoWindowMs = 2 * 60 * 1000;

    /// <summary>
    /// The age past which a ticket is EDITED instead of deleted when it ends (§21.51: Telegram lets a bot delete its own message only
    /// within 48 hours; one hour of margin, so a delete that waits in the outbox for a while is never refused).
    /// </summary>
    inline constexpr int64_t kTicketDeleteMaxAgeMs = 47ll * 3600 * 1000;

    /// <summary>
    /// How long the ⚡ the delete check put on a message may stay while the prompt is Pending again (its send was declined and
    /// rolled back): after this the reaction sync puts the ✍ back (MessagingService::_syncReactions).
    /// </summary>
    inline constexpr int64_t kSendingMarkRevertMs = 20 * 1000;

    /// <summary>The longest the delete check's Telegram call may take before the prompt is sent anyway (§21.57.6).</summary>
    inline constexpr int64_t kPreSendCheckTimeoutMs = 3000;

    /// <summary>
    /// How long a message the delete check found ALIVE is taken as alive without a new call: a declined send that is retried a beat
    /// later costs no second call (the user deleting the message within these seconds is the rare case, and the next ticks see it).
    /// </summary>
    inline constexpr int64_t kPreSendCheckReuseMs = 20 * 1000;

    /// <summary>How many characters of the merged text the merge's preview quotes (a longer one ends in "…").</summary>
    inline constexpr size_t kMergePreviewMaxChars = 700;

    /// <summary>
    /// The reaction on a message whose prompt was MERGED into the one above (🤝, one of the reactions a bot may set — the Bot API's
    /// ReactionTypeEmoji list; written without a variation selector, the list's own spelling).
    /// </summary>
    inline constexpr std::wstring_view kMergedReaction = L"\U0001F91D";

    // ===== keycaps ======================================================================================================

    /// <summary>
    /// A waiting prompt's place as a NUMBER EMOJI (the user's "a number emoji on both"): 1️⃣ … 9️⃣ (the digit, U+FE0F, U+20E3), 🔟,
    /// then "#11", "#12", … — the same number /queue shows and /unqueue n takes. A bot cannot REACT with a digit (Telegram's reaction
    /// list has none), so the number goes on the ticket's line and on its buttons.
    /// </summary>
    /// <param name="n">The 1-based place (0 reads as "#0").</param>
    /// <returns>The keycap text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring QueueKeycap(size_t n)
    {
        if (n >= 1 && n <= 9)
        {
            std::wstring s(1, static_cast<wchar_t>(L'0' + n));
            s += L"\xFE0F\x20E3";
            return s;
        }
        if (n == 10)
        {
            return L"\U0001F51F";
        }
        return L"#" + std::to_wstring(n);
    }

    // ===== button data ==================================================================================================

    /// <summary>A queue button's data, read back (ParseQueueCallback).</summary>
    struct QueueCallback
    {
        std::wstring verb; // one of the kQueueVerb* words
        std::wstring id; // the prompt's id (the session's for kQueueVerbListNext)
        std::wstring arg; // the merge's upper-prompt prefix (kQueueVerbMergeYes); "" otherwise
    };

    /// <summary>
    /// A queue button's data: "pq:&lt;verb&gt;:&lt;id&gt;" or "pq:&lt;verb&gt;:&lt;id&gt;:&lt;arg&gt;". The prompt and session ids are GUIDs (36
    /// characters), so the longest form ("pq:mok:" + 36 + ":" + 8 = 52 bytes) stays within Telegram's 64-byte callback_data.
    /// </summary>
    /// <param name="verb">The verb (kQueueVerb*).</param>
    /// <param name="id">The prompt's id (the session's for kQueueVerbListNext).</param>
    /// <param name="arg">An extra argument ("" = none).</param>
    /// <returns>The button data.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring EncodeQueueCallback(std::wstring_view verb, std::wstring_view id, std::wstring_view arg = {})
    {
        std::wstring out{ kQueueCallbackPrefix };
        out.append(verb);
        out += L':';
        out.append(id);
        if (!arg.empty())
        {
            out += L':';
            out.append(arg);
        }
        return out;
    }

    /// <summary>
    /// Reads a queue button's data back. Anything that is not "pq:" + a verb + ":" + a non-empty id answers nullopt, so a damaged or
    /// foreign button is told apart from a real one (the caller then answers the tap with nothing).
    /// </summary>
    /// <param name="data">The button's callback_data.</param>
    /// <returns>The verb, the id and the argument; nullopt when the data is not a queue button's.</returns>
    /// <exception cref="std::bad_alloc">If the parts cannot be copied.</exception>
    inline std::optional<QueueCallback> ParseQueueCallback(std::wstring_view data)
    {
        if (!data.starts_with(kQueueCallbackPrefix))
        {
            return std::nullopt;
        }
        data.remove_prefix(kQueueCallbackPrefix.size());
        const size_t verbEnd = data.find(L':');
        if (verbEnd == std::wstring_view::npos || verbEnd == 0)
        {
            return std::nullopt;
        }
        QueueCallback cb;
        cb.verb = std::wstring{ data.substr(0, verbEnd) };
        std::wstring_view rest = data.substr(verbEnd + 1);
        const size_t idEnd = rest.find(L':');
        cb.id = std::wstring{ idEnd == std::wstring_view::npos ? rest : rest.substr(0, idEnd) };
        if (idEnd != std::wstring_view::npos)
        {
            cb.arg = std::wstring{ rest.substr(idEnd + 1) };
        }
        if (cb.id.empty())
        {
            return std::nullopt;
        }
        return cb;
    }

    // ===== a prompt's Telegram messages =================================================================================

    /// <summary>
    /// The Telegram message a remote prompt came from — QueuedPrompt::remoteSource "telegram:&lt;message id&gt;" — or 0 for a prompt the
    /// PC or the runner queued (or a source that does not parse).
    /// </summary>
    /// <param name="q">The queued prompt.</param>
    /// <returns>The message id, or 0.</returns>
    inline int64_t TopicMessageIdOf(const QueuedPrompt& q) noexcept
    {
        if (!IsTopicPrompt(q))
        {
            return 0;
        }
        int64_t v = 0;
        for (size_t i = 9; i < q.remoteSource.size(); ++i)
        {
            const wchar_t c = q.remoteSource[i];
            if (c < L'0' || c > L'9')
            {
                break;
            }
            v = v * 10 + (c - L'0');
            if (v > (int64_t{ 1 } << 53))
            {
                return 0; // not a Telegram message id (they fit a double exactly): a damaged source
            }
        }
        return v;
    }

    /// <summary>
    /// The PARTS of a remote prompt: the merged ones it records (QueuedPrompt::remoteParts), else ONE part made of its message
    /// (remoteSource) and its text — so every caller handles a merged and a plain prompt alike.
    /// </summary>
    /// <param name="q">The queued prompt (a remote one; another prompt gives one part with message id 0).</param>
    /// <returns>The parts, in order.</returns>
    /// <exception cref="std::bad_alloc">If the parts cannot be copied.</exception>
    inline std::vector<RemotePromptPart> PromptPartsOf(const QueuedPrompt& q)
    {
        if (!q.remoteParts.empty())
        {
            return q.remoteParts;
        }
        return { RemotePromptPart{ TopicMessageIdOf(q), q.text } };
    }

    /// <summary>
    /// Every Telegram message a remote prompt stands for — its own (remoteSource) and every merged part's — each once, in order, ids
    /// above 0 only: the reactions go to each, and the delete check probes each.
    /// </summary>
    /// <param name="q">The queued prompt.</param>
    /// <returns>The message ids (empty for a prompt that is not remote).</returns>
    /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
    inline std::vector<int64_t> PromptMessageIds(const QueuedPrompt& q)
    {
        std::vector<int64_t> ids;
        const auto add = [&ids](int64_t id) {
            if (id > 0 && std::find(ids.begin(), ids.end(), id) == ids.end())
            {
                ids.push_back(id);
            }
        };
        add(TopicMessageIdOf(q));
        for (const auto& part : q.remoteParts)
        {
            add(part.messageId);
        }
        return ids;
    }

    /// <summary>Does the remote prompt stand for this Telegram message (its own, or one of its merged parts')?</summary>
    /// <param name="q">The queued prompt.</param>
    /// <param name="messageId">The message (0 or less never matches).</param>
    /// <returns>True when the message is one of the prompt's.</returns>
    inline bool PromptHasMessage(const QueuedPrompt& q, int64_t messageId) noexcept
    {
        if (messageId <= 0)
        {
            return false;
        }
        if (TopicMessageIdOf(q) == messageId)
        {
            return true;
        }
        return std::any_of(q.remoteParts.begin(), q.remoteParts.end(), [messageId](const RemotePromptPart& p) { return p.messageId == messageId; });
    }

    /// <summary>
    /// The text of a merged prompt: its parts' texts in order, each on a NEW LINE after the one before (the user's rule: "The merge
    /// is at a new line at the bottom. no just appending"). One part gives its own text.
    /// </summary>
    /// <param name="parts">The parts.</param>
    /// <returns>The prompt's text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring MergedRemoteText(const std::vector<RemotePromptPart>& parts)
    {
        std::wstring out;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0)
            {
                out += L'\n';
            }
            out += parts[i].text;
        }
        return out;
    }

    // ===== where a prompt stands in the queue ===========================================================================

    /// <summary>Is the prompt still WAITING (Pending, or a legacy Held that the scheduler makes Pending again)?</summary>
    /// <param name="q">The queued prompt.</param>
    /// <returns>True while it waits.</returns>
    inline bool PromptWaits(const QueuedPrompt& q) noexcept
    {
        return q.status == PromptStatus::Pending || q.status == PromptStatus::Held;
    }

    /// <summary>
    /// A prompt's 1-based place among the session's WAITING prompts (the number /queue shows and /unqueue n takes); 0 when it is not
    /// in the queue or no longer waits.
    /// </summary>
    /// <param name="s">The session.</param>
    /// <param name="promptId">The prompt's id.</param>
    /// <returns>The place, or 0.</returns>
    inline size_t WaitingPosition(const SessionInfo& s, std::wstring_view promptId) noexcept
    {
        size_t n = 0;
        for (const auto& q : s.queue)
        {
            if (!PromptWaits(q))
            {
                continue;
            }
            ++n;
            if (q.id == promptId)
            {
                return n;
            }
        }
        return 0;
    }

    /// <summary>The waiting prompt right ABOVE this one in the queue's order (what ⏫ Move Up swaps with, what 🔗 Merge ↑ merges into).</summary>
    /// <param name="s">The session.</param>
    /// <param name="promptId">The prompt's id.</param>
    /// <returns>The prompt above (a pointer into s.queue), or nullptr when the prompt is first, gone, or no longer waits.</returns>
    inline const QueuedPrompt* WaitingPromptAbove(const SessionInfo& s, std::wstring_view promptId) noexcept
    {
        const QueuedPrompt* above = nullptr;
        for (const auto& q : s.queue)
        {
            if (!PromptWaits(q))
            {
                continue;
            }
            if (q.id == promptId)
            {
                return above;
            }
            above = &q;
        }
        return nullptr;
    }

    /// <summary>
    /// Where a FRESH phone prompt goes in the queue (D30 + §21.57.8): before the first PENDING prompt that is not a fresh phone
    /// prompt — the runner's own planned prompts and every LEFT-OVER one —, after the fresh phone prompts that already wait. So a
    /// left-over prompt held by a Paused runner never blocks a new message (NextPromptIsRemote reads the FIRST pending prompt).
    /// </summary>
    /// <param name="queue">The session's queue.</param>
    /// <returns>The index to insert at; queue.size() when every pending prompt is a fresh phone prompt (or none waits).</returns>
    inline size_t FreshRemoteInsertIndex(const std::vector<QueuedPrompt>& queue) noexcept
    {
        for (size_t i = 0; i < queue.size(); ++i)
        {
            if (queue[i].status == PromptStatus::Pending && !IsFreshRemotePrompt(queue[i]))
            {
                return i;
            }
        }
        return queue.size();
    }

    /// <summary>Is a prompt of this session SENT and not yet picked up (its turn has not started)? The same test RemotePromptWait makes.</summary>
    /// <param name="s">The session.</param>
    /// <returns>True while a delivery is in flight.</returns>
    inline bool DeliveryInFlightOf(const SessionInfo& s) noexcept
    {
        return std::any_of(s.queue.begin(), s.queue.end(), [](const QueuedPrompt& q) { return q.status == PromptStatus::Sent && !q.echoed; });
    }

    // ===== why a prompt waits ===========================================================================================

    /// <summary>Why a waiting prompt of the topic has not gone yet (DecideTicketWhy) — the ticket's line says it.</summary>
    enum class TicketWhy
    {
        None, // nothing holds it: it goes at the session's next ready moment (no ticket is POSTED for it)
        ClosedConversation, // its session is closed (a /clear or /resume moved the tab on, or the tab was closed): it waits there
        Question, // a question or permission prompt is open at the PC (Needs approval)
        Error, // a LEFT-OVER prompt in a session in Error (a fresh one is the Error's retry and goes, MESSAGING.md §21.5)
        Ended, // claude ended in the tab (Done): nothing is sent until it starts again
        TabStarting, // the tab's claude is not running yet; a fresh prompt started it, and it goes once the tab is up
        TabParked, // a LEFT-OVER prompt in a parked tab: nothing starts the tab by itself
        RunnerPaused, // a LEFT-OVER prompt while the runner is Paused (or Pause all is on): ⚡ Steer sends it
        After, // another waiting prompt goes first
        RunningTurn, // the first waiting prompt, and a turn runs
        DeliveryInFlight, // the first waiting prompt, and a prompt just sent has not started its turn
        RunnerSettle, // the first waiting LEFT-OVER prompt, runner Auto: it waits out the runner's 40 s settle
    };

    /// <summary>The facts DecideTicketWhy reads (TicketWhyFactsFor builds them from a session).</summary>
    struct TicketWhyFacts
    {
        bool live{ true }; // the session has an open tab (SessionInfo::live)
        bool started{ true }; // its claude was started this run (SessionInfo::started)
        bool external{ false }; // an adopted session (its tab is the user's own running shell: never "not started")
        SessionState state{ SessionState::Idle }; // the session's state
        bool leftOver{ false }; // the prompt is left over (QueuedPrompt::remoteLeftOver)
        bool runnerAuto{ true }; // the session's runner is Auto
        bool globalPause{ false }; // Pause all is on
        size_t position{ 1 }; // the prompt's 1-based place among the waiting prompts
        bool deliveryInFlight{ false }; // a prompt was sent and has not started its turn (DeliveryInFlightOf)
    };

    /// <summary>The facts of one waiting prompt in its session, for DecideTicketWhy.</summary>
    /// <param name="s">The session.</param>
    /// <param name="q">The prompt (one of s.queue's).</param>
    /// <param name="position">Its 1-based place among the waiting prompts (WaitingPosition).</param>
    /// <param name="globalPause">Pause all is on (Scheduler::GlobalPaused).</param>
    /// <returns>The facts.</returns>
    inline TicketWhyFacts TicketWhyFactsFor(const SessionInfo& s, const QueuedPrompt& q, size_t position, bool globalPause) noexcept
    {
        TicketWhyFacts f;
        f.live = s.live;
        f.started = s.started;
        f.external = s.external;
        f.state = s.state;
        f.leftOver = q.remoteLeftOver;
        f.runnerAuto = s.autorunner.mode == AutorunnerMode::Full;
        f.globalPause = globalPause;
        f.position = position;
        f.deliveryInFlight = DeliveryInFlightOf(s);
        return f;
    }

    /// <summary>
    /// Why a waiting prompt of the topic has not gone (§21.57.4). The SESSION's holds come first (a closed conversation, an open
    /// question, an Error or an ended claude, a tab that is not running, a Paused runner for a left-over prompt) — they hold every
    /// prompt, so they say more than a place in the line; then the place ("after 1️⃣"); then, for the first prompt, the turn or the
    /// delivery in flight, and a left-over prompt's settle. A fresh prompt in Error is not held by the Error: it is the Error's
    /// retry (MESSAGING.md §21.5) and goes at the next ready moment.
    /// </summary>
    /// <param name="f">The facts.</param>
    /// <returns>The reason; None when nothing holds it.</returns>
    inline TicketWhy DecideTicketWhy(const TicketWhyFacts& f) noexcept
    {
        if (!f.live)
        {
            return TicketWhy::ClosedConversation;
        }
        if (f.state == SessionState::NeedsApproval)
        {
            return TicketWhy::Question;
        }
        if (f.state == SessionState::Error && f.leftOver)
        {
            return TicketWhy::Error;
        }
        if (f.state == SessionState::Done)
        {
            return TicketWhy::Ended;
        }
        if (!f.started && !f.external)
        {
            return f.leftOver ? TicketWhy::TabParked : TicketWhy::TabStarting;
        }
        if (f.leftOver && (!f.runnerAuto || f.globalPause))
        {
            return TicketWhy::RunnerPaused;
        }
        if (f.position > 1)
        {
            return TicketWhy::After;
        }
        if (f.state == SessionState::Running)
        {
            return TicketWhy::RunningTurn;
        }
        if (f.deliveryInFlight)
        {
            return TicketWhy::DeliveryInFlight;
        }
        if (f.leftOver)
        {
            return TicketWhy::RunnerSettle;
        }
        return TicketWhy::None;
    }

    /// <summary>The why in the ticket's words (HTML-safe: no markup characters but the keycap's).</summary>
    /// <param name="why">The reason.</param>
    /// <param name="aboveKeycap">The keycap of the prompt above (QueueKeycap): the After reason names it.</param>
    /// <returns>The words.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring TicketWhyWords(TicketWhy why, std::wstring_view aboveKeycap)
    {
        switch (why)
        {
        case TicketWhy::ClosedConversation:
            return L"waits in a closed conversation: it goes if that conversation is resumed with its runner on";
        case TicketWhy::Question:
            return L"waits: a question is open at the PC \x2014 answer it first";
        case TicketWhy::Error:
            return L"waits: the session is in Error \x2014 \U0001F504 Restart Session on the card";
        case TicketWhy::Ended:
            return L"waits: claude ended in the tab \x2014 \x25B6 Reactivate on the card";
        case TicketWhy::TabStarting:
            return L"waits for the tab to start";
        case TicketWhy::TabParked:
            return L"waits until the tab is active again (\x25B6 Reactivate on the card)";
        case TicketWhy::RunnerPaused:
            return L"waits for the runner (Paused): \x26A1 Steer sends it now";
        case TicketWhy::After:
            return L"after " + std::wstring{ aboveKeycap };
        case TicketWhy::RunningTurn:
            return L"waits for the running turn";
        case TicketWhy::DeliveryInFlight:
            return L"waits for the prompt just sent";
        case TicketWhy::RunnerSettle:
            return L"goes once the session has been ready for 40 s";
        case TicketWhy::None:
        default:
            return L"goes in a moment";
        }
    }

    // ===== the ticket ===================================================================================================

    /// <summary>What a ticket is (persisted as a number: never renumber).</summary>
    enum class TicketKind : int
    {
        Reply = 0, // the bot's REPLY under the user's own message (a typed message, a voice note): it quotes the message
        OwnPost = 1, // the bot's own POST that IS the prompt's message (a ➕ re-queue, a left-over prompt re-posted in a new topic)
    };

    /// <summary>Where a ticket stands in its life (persisted as a number: never renumber).</summary>
    enum class TicketPhase : int
    {
        Live = 0, // the prompt waits: the line (keycap, why) and the buttons
        MergePreview = 1, // 🔗 Merge ↑ was tapped: the merged text and ✅ Merge / ✕ Cancel
        Removed = 2, // the prompt left the queue (🗑, ❌, /unqueue, the PC's Delete): ↩ Undo while the window is open
        Merged = 3, // merged into the prompt above: "🔗 Merged into 1️⃣"
        Failed = 4, // the send failed (a lost send, a refused delivery): 🔁 Retry · 🗑 Dismiss
        Requeued = 5, // 🔁 Retry queued the words again (the new prompt has its own ticket when it waits)
        Dismissed = 6, // 🗑 Dismiss took the failure's buttons off
        Sent = 7, // picked up: a reply ticket is deleted (edited when too old), an own post keeps its words and says "sent"
        Gone = 8, // the message was deleted in Telegram, or the session's record is gone: deleted (edited when too old)
    };

    /// <summary>Everything one render of a ticket depends on (BuildTicketView).</summary>
    struct TicketViewFacts
    {
        TicketKind kind{ TicketKind::Reply }; // a reply under the user's message, or the bot's own post
        std::wstring baseHtml; // OwnPost: the post's own text (its header line, then the words; no title row) — "" for Reply
        std::wstring titleRow; // the topic's title row, classic (_titleRowClassic): line 2, under the first line ("" = none)
        TicketPhase phase{ TicketPhase::Live }; // where the ticket stands
        std::wstring promptId; // the prompt (its buttons' data)
        size_t position{ 1 }; // Live / MergePreview: the prompt's place (its keycap)
        TicketWhy why{ TicketWhy::None }; // Live: why it waits
        size_t partCount{ 1 }; // Live: how many messages the prompt is made of (2+ after a merge)
        int64_t editedAtMs{ 0 }; // Live: when the user last edited its message (0 = never)
        bool canSteer{ true }; // Live: ⚡ Steer is offered (the session is open)
        bool canMoveUp{ false }; // Live: ⏫ Move Up is offered (a waiting prompt is above it)
        bool canMerge{ false }; // Live: 🔗 Merge ↑ is offered (the prompt above is the topic's too)
        std::wstring mergeUpperId; // MergePreview: the prompt above, as the preview showed it
        std::wstring mergedText; // MergePreview: the text the merge would make (MergedRemoteText)
        size_t mergedIntoPosition{ 0 }; // Merged: the place of the prompt it went into, when it merged
        bool undoOpen{ false }; // Removed: the ↩ Undo window is still open
        bool runnerAuto{ true }; // Failed: the runner's mode (the notice says it)
        int64_t sentAtMs{ 0 }; // Sent: when it was picked up (the "📤 sent HH:MM" of an own post or an old reply)
        int64_t ageMs{ 0 }; // how old the ticket's message is (past kTicketDeleteMaxAgeMs it is edited, never deleted)
        int64_t nowMs{ 0 }; // the clock (the times' "same day" test)
    };

    /// <summary>One render of a ticket: delete it, or show this text with these buttons.</summary>
    struct TicketView
    {
        bool remove{ false }; // delete the ticket's message (a reply that ended: it served its purpose)
        std::wstring html; // the message's text (Telegram HTML, the title row in place) when not removed
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows; // the buttons: rows of (label, callback data)
    };

    /// <summary>
    /// A ticket's text and buttons for its phase (§21.57.4). The first line is the ticket's own ("1️⃣ Queued · after …"), the topic's
    /// title row goes under it (§21.54: an EDIT adds no row, so every render carries it); an own post keeps its header and words on
    /// top and the ticket's line under them. Every button carries the prompt's keycap (the user's "a number emoji on both").
    /// </summary>
    /// <param name="f">The facts.</param>
    /// <returns>The render.</returns>
    /// <exception cref="std::bad_alloc">If the text or the buttons cannot be allocated.</exception>
    inline TicketView BuildTicketView(const TicketViewFacts& f)
    {
        TicketView v;
        const bool ownPost = f.kind == TicketKind::OwnPost;
        const bool deletable = f.ageMs < kTicketDeleteMaxAgeMs;
        const std::wstring key = QueueKeycap(f.position);
        std::wstring line;
        switch (f.phase)
        {
        case TicketPhase::Live:
        {
            line = key + L" <b>Queued</b> \x00B7 " + TicketWhyWords(f.why, f.position > 1 ? QueueKeycap(f.position - 1) : std::wstring{});
            if (f.partCount >= 2)
            {
                line += L" \x00B7 \U0001F517 " + std::to_wstring(f.partCount) + L" messages";
            }
            if (f.editedAtMs > 0)
            {
                line += L" \x00B7 \x270F\xFE0F edited " + TgClockText(f.editedAtMs, f.nowMs);
            }
            std::vector<std::pair<std::wstring, std::wstring>> first;
            if (f.canSteer)
            {
                first.emplace_back(key + L" \x26A1 Steer", EncodeQueueCallback(kQueueVerbSteer, f.promptId));
            }
            first.emplace_back(key + L" \U0001F5D1 Delete", EncodeQueueCallback(kQueueVerbDelete, f.promptId));
            v.rows.push_back(std::move(first));
            std::vector<std::pair<std::wstring, std::wstring>> second;
            if (f.canMoveUp)
            {
                second.emplace_back(key + L" \x23EB Move Up", EncodeQueueCallback(kQueueVerbMoveUp, f.promptId));
            }
            if (f.canMerge)
            {
                second.emplace_back(key + L" \U0001F517 Merge \x2191", EncodeQueueCallback(kQueueVerbMerge, f.promptId));
            }
            if (!second.empty())
            {
                v.rows.push_back(std::move(second));
            }
            break;
        }
        case TicketPhase::MergePreview:
        {
            std::wstring merged = f.mergedText;
            if (merged.size() > kMergePreviewMaxChars)
            {
                merged = merged.substr(0, kMergePreviewMaxChars) + L"\x2026";
            }
            const std::wstring upperKey = QueueKeycap(f.position > 1 ? f.position - 1 : 1);
            line = L"\U0001F517 <b>Merge " + key + L" into " + upperKey + L"?</b> It becomes:\n<blockquote>" + TgEscapeHtml(merged) + L"</blockquote>";
            std::wstring upperPrefix = f.mergeUpperId.substr(0, 8);
            v.rows.push_back({ { L"\x2705 Merge", EncodeQueueCallback(kQueueVerbMergeYes, f.promptId, upperPrefix) },
                               { L"\x2715 Cancel", EncodeQueueCallback(kQueueVerbMergeNo, f.promptId) } });
            break;
        }
        case TicketPhase::Removed:
            line = L"\U0001F5D1 <b>Removed from the queue</b>";
            if (f.undoOpen)
            {
                v.rows.push_back({ { L"\x21A9 Undo", EncodeQueueCallback(kQueueVerbUndo, f.promptId) } });
            }
            break;
        case TicketPhase::Merged:
            line = L"\U0001F517 <b>Merged into " + QueueKeycap(f.mergedIntoPosition > 0 ? f.mergedIntoPosition : 1) + L"</b>";
            break;
        case TicketPhase::Failed:
            line = std::wstring{ L"\U0001F44E <b>Not delivered</b> \x2014 the prompt never became a message (a lost send or a refused delivery); " } +
                   (f.runnerAuto ? L"the runner stays <b>Auto</b>." : L"the runner is <b>Paused</b>.");
            v.rows.push_back({ { L"\U0001F501 Retry", EncodeQueueCallback(kQueueVerbRetry, f.promptId) },
                               { L"\U0001F5D1 Dismiss", EncodeQueueCallback(kQueueVerbDismiss, f.promptId) } });
            break;
        case TicketPhase::Requeued:
            line = L"\U0001F501 <b>Queued again</b> as a new message";
            break;
        case TicketPhase::Dismissed:
            line = L"\U0001F44E <b>Not delivered</b>";
            break;
        case TicketPhase::Sent:
            if (!ownPost && deletable)
            {
                v.remove = true; // the "📤 sent from the queue" line and the 🫡 take over (§21.44)
                return v;
            }
            line = L"\U0001F4E4 <b>Sent</b> " + TgClockText(f.sentAtMs > 0 ? f.sentAtMs : f.nowMs, f.nowMs);
            break;
        case TicketPhase::Gone:
        default:
            if (deletable || ownPost)
            {
                v.remove = true;
                return v;
            }
            line = L"\x2716 <b>No longer in the queue</b>";
            break;
        }
        if (ownPost)
        {
            // The post's header line first, the title row under it (§21.54), its words, then the ticket's line.
            v.html = WithTopicTitleRowAfterHeader(f.baseHtml, f.titleRow) + L"\n\n" + line;
        }
        else
        {
            v.html = WithTopicTitleRowAfterHeader(line, f.titleRow);
        }
        return v;
    }

    /// <summary>
    /// A render's fingerprint (FNV-1a over the text and every button's label and data): the service edits a ticket only when this
    /// changes, so an unchanged queue costs no Telegram call.
    /// </summary>
    /// <param name="v">The render.</param>
    /// <returns>The hash (never 0 for a non-removed render, so 0 can mean "nothing shown yet").</returns>
    inline uint32_t TicketViewHash(const TicketView& v) noexcept
    {
        uint32_t h = 2166136261u;
        const auto mix = [&h](std::wstring_view s) noexcept {
            for (const wchar_t c : s)
            {
                h ^= static_cast<uint32_t>(c);
                h *= 16777619u;
            }
            h ^= 0x1Fu; // a separator, so ("ab","c") and ("a","bc") differ
            h *= 16777619u;
        };
        mix(v.remove ? std::wstring_view{ L"remove" } : std::wstring_view{ v.html });
        for (const auto& row : v.rows)
        {
            for (const auto& [label, data] : row)
            {
                mix(label);
                mix(data);
            }
            mix(L"|");
        }
        return h == 0 ? 1u : h;
    }

    // ===== the ❌ and ➕ replies ==========================================================================================

    /// <summary>What a REPLY of the user means to the queue (ClassifyQueueReply, §21.57.7).</summary>
    enum class QueueReplyKind
    {
        None, // an ordinary message (a new prompt, as before)
        Remove, // only an X: the replied-to waiting message is removed (🗑 Delete, with its Undo)
        Again, // only a plus: the replied-to message's prompt is queued again, posted below as the user's message
    };

    /// <summary>
    /// Reads a reply's text as a queue gesture. The user's REACTIONS never reach a bot in a private chat (§21.57.2), so the X the
    /// user asked for ("X emoji (any) on message/voice recording") is a REPLY made of an X alone: ❌ ✖ ❎ 🚫 ⛔ 🗑 × or the letter x
    /// (any case); and ➕ + ✚ ＋ alone is the plus. Spaces around it and emoji variation selectors (U+FE0F) are ignored; anything
    /// longer is no gesture (a real message that starts with "x" stays a message).
    /// </summary>
    /// <param name="text">The reply's text.</param>
    /// <returns>The gesture, or None.</returns>
    inline QueueReplyKind ClassifyQueueReply(std::wstring_view text) noexcept
    {
        // Strip spaces and variation selectors into a small fixed buffer: a gesture is at most two UTF-16 units (a surrogate pair),
        // so anything longer than four units after stripping is no gesture at all.
        wchar_t buf[5]{};
        size_t n = 0;
        for (const wchar_t c : text)
        {
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\xFE0F' || c == L'\x00A0')
            {
                continue;
            }
            if (n >= 4)
            {
                return QueueReplyKind::None;
            }
            buf[n++] = c;
        }
        const std::wstring_view t{ buf, n };
        if (t == L"x" || t == L"X" || t == L"\x274C" || t == L"\x2716" || t == L"\x274E" || t == L"\x26D4" || t == L"\x00D7" ||
            t == L"\U0001F6AB" || t == L"\U0001F5D1")
        {
            return QueueReplyKind::Remove;
        }
        if (t == L"+" || t == L"\x2795" || t == L"\x271A" || t == L"\xFF0B")
        {
            return QueueReplyKind::Again;
        }
        return QueueReplyKind::None;
    }

    /// <summary>
    /// The words of a bot's "👤 You" prompt line (§21.12, §21.44) as its quoted text shows them — what a ➕ reply to such a line
    /// queues again. The line's text is "👤 You · 14:02 · … ", then the session's title row, then the words: the first line is
    /// dropped, and the second too when it is the title row (it starts with one of the color squares, §21.13, or the ⤴ lift).
    /// A text that is not such a line comes back as it is.
    /// </summary>
    /// <param name="lineText">The quoted message's plain text (reply_to_message.text).</param>
    /// <returns>The words (trimmed of the line breaks around them).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring PromptWordsFromBotLine(std::wstring_view lineText)
    {
        constexpr std::wstring_view kYou = L"\U0001F464 You";
        if (!lineText.starts_with(kYou))
        {
            return std::wstring{ lineText };
        }
        size_t nl = lineText.find(L'\n');
        if (nl == std::wstring_view::npos)
        {
            return {};
        }
        std::wstring_view rest = lineText.substr(nl + 1);
        // The title row: a color square (🟥🟧🟨🟩🟦🟪🟫 are U+1F7E5..U+1F7EB, a surrogate pair D83D DFE5..DFEB; ⬛ ⬜ are U+2B1B / U+2B1C)
        // or the ⤴ lift (U+2934) leads it.
        const bool squareRow = rest.size() >= 2 && rest[0] == L'\xD83D' && rest[1] >= L'\xDFE5' && rest[1] <= L'\xDFEB';
        const bool otherRow = !rest.empty() && (rest[0] == L'\x2B1B' || rest[0] == L'\x2B1C' || rest[0] == L'\x2934');
        if (squareRow || otherRow)
        {
            nl = rest.find(L'\n');
            rest = nl == std::wstring_view::npos ? std::wstring_view{} : rest.substr(nl + 1);
        }
        while (!rest.empty() && (rest.front() == L'\n' || rest.front() == L'\r'))
        {
            rest.remove_prefix(1);
        }
        while (!rest.empty() && (rest.back() == L'\n' || rest.back() == L'\r'))
        {
            rest.remove_suffix(1);
        }
        return std::wstring{ rest };
    }

    /// <summary>The bot's post that stands for a message queued AGAIN by a ➕ reply: "👤 You · 14:20 · ➕ again", then the words.</summary>
    /// <param name="nowMs">When it was queued (its clock).</param>
    /// <param name="words">The words (escaped here).</param>
    /// <returns>Telegram HTML without the title row (the ticket render puts it under the first line).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring AgainPostHtml(int64_t nowMs, std::wstring_view words)
    {
        return L"\U0001F464 You \x00B7 " + TgClockText(nowMs, nowMs) + L" \x00B7 \x2795 again\n" + TgEscapeHtml(words);
    }

    /// <summary>
    /// The bot's post that stands for a LEFT-OVER prompt whose message went with an older topic (a closed tab's topic is deleted
    /// with its messages; a /recreate makes a new one): "📱 from the phone · waiting from before this topic", then the words.
    /// </summary>
    /// <param name="words">The words (escaped here).</param>
    /// <returns>Telegram HTML without the title row.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring RepostedPromptHtml(std::wstring_view words)
    {
        return L"\U0001F4F1 from the phone \x00B7 waiting from before this topic\n" + TgEscapeHtml(words);
    }

    // ===== an edit of a waiting message ===============================================================================

    /// <summary>What an EDIT of a waiting message does to its prompt (DecideQueuedEdit).</summary>
    enum class QueuedEditOutcome
    {
        Replace, // the prompt takes the new words (the text in QueuedEditDecision::text)
        RefuseNoRecord, // the edit is a Claude Code command that leaves no record: refused as a typed one is
        RefuseLeavesTab, // a command that would take the conversation out of its tab: refused as a typed one is
        RefuseCommand, // one of Agentmaster's own commands (/steer, /queue, /btw, …): an edit cannot become one
        RefuseAddDir, // /add-dir: its dialog is answered from the topic only for a message SENT as /add-dir (its watch)
        RefuseEmpty, // nothing left to send
    };

    /// <summary>An edit's reading: what to do and with which words.</summary>
    struct QueuedEditDecision
    {
        QueuedEditOutcome outcome{ QueuedEditOutcome::Replace };
        std::wstring text; // Replace: the words as they go to the session (a /cc /x edit gives "/x")
        std::wstring name; // a refused command's name (without its '/')
    };

    /// <summary>
    /// Reads the new text of an edited waiting message like a TYPED message of the topic (the bug of §21.57.1: the edit used to
    /// write the raw text into the prompt, so an edit into /exit, /resume or /config skipped the refusal tables of §7.2 and was
    /// typed into claude). A refused edit keeps the prompt's words as they were.
    /// </summary>
    /// <param name="cmd">ParseRemoteCommand of the new text.</param>
    /// <returns>The decision.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be copied.</exception>
    inline QueuedEditDecision DecideQueuedEdit(const RemoteCommand& cmd)
    {
        QueuedEditDecision d;
        d.name = cmd.name;
        switch (cmd.kind)
        {
        case RemoteKind::RefusedNoRecord:
            d.outcome = QueuedEditOutcome::RefuseNoRecord;
            return d;
        case RemoteKind::RefusedLeavesTab:
            d.outcome = QueuedEditOutcome::RefuseLeavesTab;
            return d;
        case RemoteKind::AgentmasterCommand:
            d.outcome = QueuedEditOutcome::RefuseCommand;
            return d;
        case RemoteKind::ClaudeCommand:
            if (cmd.name == L"add-dir")
            {
                d.outcome = QueuedEditOutcome::RefuseAddDir;
                return d;
            }
            break;
        case RemoteKind::Text:
        default:
            break;
        }
        if (cmd.text.find_first_not_of(L" \t\r\n") == std::wstring::npos)
        {
            d.outcome = QueuedEditOutcome::RefuseEmpty;
            return d;
        }
        d.outcome = QueuedEditOutcome::Replace;
        d.text = cmd.text;
        return d;
    }

    /// <summary>The bot's answer to a REFUSED edit, as a reply to the edited message ("" for Replace: an accepted edit is silent).</summary>
    /// <param name="d">The decision.</param>
    /// <returns>Telegram HTML, or "".</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring QueuedEditRefusalHtml(const QueuedEditDecision& d)
    {
        const std::wstring keeps = L" The queued message keeps its words.";
        const std::wstring name = L"<code>/" + TgEscapeHtml(d.name) + L"</code>";
        switch (d.outcome)
        {
        case QueuedEditOutcome::RefuseNoRecord:
            return name + L" leaves no record in the session (you would not see it when the session is reopened) \x2014 run it at the PC." + keeps;
        case QueuedEditOutcome::RefuseLeavesTab:
            return name + L" would take the conversation out of its tab \x2014 use /forksession or /newsession here, or run it at the PC." + keeps;
        case QueuedEditOutcome::RefuseCommand:
            return L"An edit cannot turn a queued message into " + name + L" \x2014 send it as a new message." + keeps;
        case QueuedEditOutcome::RefuseAddDir:
            return L"An edit cannot turn a queued message into <code>/add-dir</code> (its dialog comes here only for a message sent as /add-dir) \x2014 send it as a new message." + keeps;
        case QueuedEditOutcome::RefuseEmpty:
            return L"The edit left the message empty." + keeps + L" \U0001F5D1 Delete on its ticket takes it out.";
        case QueuedEditOutcome::Replace:
        default:
            return {};
        }
    }

    // ===== the /queue list ===============================================================================================

    /// <summary>One line of the /queue list (BuildQueueListHtml).</summary>
    struct QueueListItem
    {
        std::wstring promptId; // its ✖'s data
        std::wstring text; // the prompt's words (the list shows the first line)
        bool fromPhone{ false }; // typed in the topic (📱), else the runner's (⚙)
        std::wstring why; // the phone prompt's why in words ("" = none shown)
    };

    /// <summary>
    /// The /queue list (§21.57.5): every waiting prompt with its keycap — the phone's marked 📱 with why it waits, the runner's ⚙ —
    /// or "empty". The list is LIVE: its own buttons edit it in place, so its numbers stay right.
    /// </summary>
    /// <param name="items">The waiting prompts, in the queue's order.</param>
    /// <returns>Telegram HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildQueueListHtml(const std::vector<QueueListItem>& items)
    {
        std::wstring html = L"<b>\U0001F4E5 Queue</b> \x00B7 " + std::to_wstring(items.size()) + L" waiting";
        if (items.empty())
        {
            return html + L"\nempty";
        }
        for (size_t i = 0; i < items.size(); ++i)
        {
            const auto& it = items[i];
            html += L"\n" + QueueKeycap(i + 1) + L" " + (it.fromPhone ? L"\U0001F4F1 " : L"\x2699 ") + TgEscapeHtml(TgFirstLine(it.text, 120));
            if (!it.why.empty())
            {
                html += L" \x2014 <i>" + TgEscapeHtml(it.why) + L"</i>";
            }
        }
        return html;
    }

    /// <summary>
    /// The /queue list's buttons: a ✖ per prompt (the first eight, four to a row, each with its keycap) and ▶ Send next now — all
    /// stateless "pq:" buttons, so a list from hours ago still works on the queue as it is.
    /// </summary>
    /// <param name="items">The waiting prompts, in the queue's order.</param>
    /// <param name="sessionId">The session (▶ Send next now's data).</param>
    /// <returns>The rows of (label, data); empty when nothing waits.</returns>
    /// <exception cref="std::bad_alloc">If the buttons cannot be allocated.</exception>
    inline std::vector<std::vector<std::pair<std::wstring, std::wstring>>> BuildQueueListButtons(const std::vector<QueueListItem>& items, std::wstring_view sessionId)
    {
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
        if (items.empty())
        {
            return rows;
        }
        std::vector<std::pair<std::wstring, std::wstring>> row;
        for (size_t i = 0; i < items.size() && i < 8; ++i)
        {
            row.emplace_back(L"\x2716 " + QueueKeycap(i + 1), EncodeQueueCallback(kQueueVerbListRemove, items[i].promptId));
            if (row.size() == 4)
            {
                rows.push_back(std::move(row));
                row.clear();
            }
        }
        if (!row.empty())
        {
            rows.push_back(std::move(row));
        }
        rows.push_back({ { L"\x25B6 Send next now", EncodeQueueCallback(kQueueVerbListNext, sessionId) } });
        return rows;
    }

    // ===== the tickets' record (state.json) ==============================================================================

    /// <summary>
    /// One ticket as the service remembers it (MessagingService::_tickets, keyed by the prompt's id), persisted in state.json's
    /// "queueTickets" so a restart still edits and deletes the tickets of the run before. The Undo snapshot is NOT persisted
    /// (a restart within the two minutes loses Undo; the ticket then says "Removed" without the button).
    /// </summary>
    struct QueueTicket
    {
        std::wstring promptId; // the prompt (the map's key, repeated so a record stands alone)
        std::wstring sessionId; // the session whose queue holds (or held) the prompt
        int64_t threadId{ 0 }; // the topic the ticket sits in
        int64_t messageId{ 0 }; // the ticket's message; 0 = not posted yet; -1 = its post is in flight
        int64_t replyTo{ 0 }; // Reply: the user's message it quotes
        TicketKind kind{ TicketKind::Reply }; // a reply, or the bot's own post
        std::wstring baseHtml; // OwnPost: the post's own text (header + words; no title row)
        TicketPhase phase{ TicketPhase::Live }; // where it stands
        uint32_t hash{ 0 }; // TicketViewHash of what it shows (0 = nothing yet)
        int64_t postedMs{ 0 }; // when its message was posted (the 48-hour rule)
        int64_t undoUntilMs{ 0 }; // Removed: when the ↩ Undo window closes
        int64_t editedAtMs{ 0 }; // when the user last edited the prompt's message (the line says so)
        std::wstring mergeUpperId; // MergePreview: the prompt above, as the preview showed it
        size_t mergedIntoPosition{ 0 }; // Merged: the place of the prompt it went into
        bool runnerAuto{ true }; // Failed: the runner's mode when it failed
        int64_t sentAtMs{ 0 }; // Sent: when it was picked up
    };

    /// <summary>The tickets as state.json keeps them (an array; an empty map gives a Null value, so no key is written).</summary>
    /// <param name="tickets">The tickets by prompt id.</param>
    /// <returns>The JSON array, or a Null value when there is none.</returns>
    /// <exception cref="std::bad_alloc">If the document cannot be allocated.</exception>
    inline json::Value QueueTicketsToJson(const std::map<std::wstring, QueueTicket>& tickets)
    {
        if (tickets.empty())
        {
            return json::Value{};
        }
        auto arr = json::Value::MkArr();
        for (const auto& [id, t] : tickets)
        {
            auto o = json::Value::MkObj();
            o.Set(L"promptId", json::Value::MkStr(id));
            o.Set(L"sessionId", json::Value::MkStr(t.sessionId));
            o.Set(L"threadId", json::Value::MkNum(static_cast<double>(t.threadId)));
            // A post still in flight (-1) is written as 0: after a restart its answer never comes, and the ticket is posted again.
            o.Set(L"messageId", json::Value::MkNum(static_cast<double>(t.messageId > 0 ? t.messageId : 0)));
            o.Set(L"replyTo", json::Value::MkNum(static_cast<double>(t.replyTo)));
            o.Set(L"kind", json::Value::MkNum(static_cast<double>(static_cast<int>(t.kind))));
            if (!t.baseHtml.empty())
            {
                o.Set(L"baseHtml", json::Value::MkStr(t.baseHtml));
            }
            o.Set(L"phase", json::Value::MkNum(static_cast<double>(static_cast<int>(t.phase))));
            o.Set(L"hash", json::Value::MkNum(static_cast<double>(t.hash)));
            o.Set(L"postedMs", json::Value::MkNum(static_cast<double>(t.postedMs)));
            if (t.undoUntilMs > 0)
            {
                o.Set(L"undoUntilMs", json::Value::MkNum(static_cast<double>(t.undoUntilMs)));
            }
            if (t.editedAtMs > 0)
            {
                o.Set(L"editedAtMs", json::Value::MkNum(static_cast<double>(t.editedAtMs)));
            }
            if (!t.mergeUpperId.empty())
            {
                o.Set(L"mergeUpperId", json::Value::MkStr(t.mergeUpperId));
            }
            if (t.mergedIntoPosition > 0)
            {
                o.Set(L"mergedIntoPosition", json::Value::MkNum(static_cast<double>(t.mergedIntoPosition)));
            }
            o.Set(L"runnerAuto", json::Value::MkBool(t.runnerAuto));
            if (t.sentAtMs > 0)
            {
                o.Set(L"sentAtMs", json::Value::MkNum(static_cast<double>(t.sentAtMs)));
            }
            arr.Push(std::move(o));
        }
        return arr;
    }

    /// <summary>
    /// Reads the tickets back (QueueTicketsToJson's form). A record without a prompt or a session id is dropped; an unknown kind or
    /// phase number (a newer build's) reads as a Reply / Gone, so the ticket is cleaned up rather than misread.
    /// </summary>
    /// <param name="v">state.json's "queueTickets" value (anything but an array gives none).</param>
    /// <param name="out">Receives the tickets (cleared first).</param>
    /// <exception cref="std::bad_alloc">If the records cannot be allocated.</exception>
    inline void QueueTicketsFromJson(const json::Value& v, std::map<std::wstring, QueueTicket>& out)
    {
        out.clear();
        if (v.type != json::Value::Type::Arr)
        {
            return;
        }
        for (const auto& o : v.arr)
        {
            if (o.type != json::Value::Type::Obj)
            {
                continue;
            }
            QueueTicket t;
            t.promptId = o.StrAt(L"promptId");
            t.sessionId = o.StrAt(L"sessionId");
            if (t.promptId.empty() || t.sessionId.empty())
            {
                continue;
            }
            t.threadId = o.I64At(L"threadId", 0);
            t.messageId = (std::max)(int64_t{ 0 }, o.I64At(L"messageId", 0));
            t.replyTo = (std::max)(int64_t{ 0 }, o.I64At(L"replyTo", 0));
            const int64_t kind = o.I64At(L"kind", 0);
            t.kind = kind == static_cast<int>(TicketKind::OwnPost) ? TicketKind::OwnPost : TicketKind::Reply;
            t.baseHtml = o.StrAt(L"baseHtml");
            const int64_t phase = o.I64At(L"phase", 0);
            t.phase = phase >= 0 && phase <= static_cast<int>(TicketPhase::Gone) ? static_cast<TicketPhase>(phase) : TicketPhase::Gone;
            t.hash = static_cast<uint32_t>(o.I64At(L"hash", 0));
            t.postedMs = o.I64At(L"postedMs", 0);
            t.undoUntilMs = o.I64At(L"undoUntilMs", 0);
            t.editedAtMs = o.I64At(L"editedAtMs", 0);
            t.mergeUpperId = o.StrAt(L"mergeUpperId");
            t.mergedIntoPosition = static_cast<size_t>((std::max)(int64_t{ 0 }, o.I64At(L"mergedIntoPosition", 0)));
            t.runnerAuto = o.BoolAt(L"runnerAuto", true);
            t.sentAtMs = o.I64At(L"sentAtMs", 0);
            out[t.promptId] = std::move(t);
        }
    }
}
