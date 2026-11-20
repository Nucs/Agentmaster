// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the Telegram service's QUEUE MANAGEMENT (MESSAGING.md §21.57): the TICKET under each waiting phone message (its
// number's keycap, why it waits, ⚡ Steer · 🗑 Delete · ⏫ Move Up · 🔗 Merge ↑), the controls behind those buttons (and ↩ Undo,
// 🔁 Retry, 🗑 Dismiss, the /queue list's ✖ and ▶), the ❌ / ➕ replies, and the check that leaves a message the user deleted in
// Telegram out of its send. The pure half — the texts, the buttons, the decisions — is MessagingQueueTickets.h.
//
// It WRAPS the one queue (SessionInfo::queue): every control is a registry Update of the kind the PC's Prompt Queue makes (remove
// by id, swap with the one above, send now through SessionRegistry::SubmitPrompt) or a plain queueing; a ticket is a VIEW of the
// queue that the tick rebuilds, so a change at the PC shows here at the next tick and a tap here shows in the PC's queue at once.
//
// Threads: _syncTickets runs on the tick; the taps (_onQueueTap) and the replies (_takeQueueReply) on the poll thread; the delete
// check (_remotePreSendCheck) on the SCHEDULER's thread (Scheduler::SetRemotePreSendCheck) or the poll thread. Every record lives
// under `_mtx`, and no Telegram call or outbox item is ever made while it is held (_enqueue takes it; it is not recursive).
// Plain C++ (no WinRT, no PCH).

#include "Messaging.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <unordered_set>

#include "ClaudeSpawn.h" // LogNav, ShortId, LogSwallowedException, NewSessionId
#include "FleetDb.h" // PersistFleetNowDurable — a ticket's ⚡ Steer: the Sent mark is on the disk before the words are typed (Rule #4)
#include "Scheduler.h" // Scheduler::RequestAdvance / GlobalPaused, kAnswersQuestionOk
#include "SessionRegistry.h"

namespace Agentmaster::Msg
{
    namespace
    {
        /// <summary>The wall clock in Unix milliseconds (every time this file stores or compares).</summary>
        /// <returns>The time.</returns>
        int64_t WallMs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /// <summary>The ✍ a waiting phone prompt's messages show (the reaction _queueRemotePrompt sets).</summary>
        constexpr std::wstring_view kQueuedMark = L"\x270D";

        /// <summary>
        /// A beat between a message's queueing and its ticket's post: the queue takes a message at a ready moment within seconds
        /// (the 5 s remote settle), and a ticket posted and deleted again in that time would only flash in the topic.
        /// </summary>
        constexpr int64_t kTicketPostDelayMs = 2500;

        /// <summary>How long a ticket whose post or edit failed waits before the tick tries it again (a tap tries at once).</summary>
        constexpr int64_t kTicketRetryMs = 60 * 1000;

        /// <summary>Is the phase the ticket's LAST (nothing changes after it; its record goes once that render is in)?</summary>
        /// <param name="p">The phase.</param>
        /// <returns>True for Merged, Requeued, Dismissed, Sent and Gone.</returns>
        bool TicketPhaseIsFinal(TicketPhase p) noexcept
        {
            return p == TicketPhase::Merged || p == TicketPhase::Requeued || p == TicketPhase::Dismissed || p == TicketPhase::Sent || p == TicketPhase::Gone;
        }

        /// <summary>The prompt with this id in the session's queue (any status).</summary>
        /// <param name="s">The session.</param>
        /// <param name="id">The prompt's id.</param>
        /// <returns>A pointer into s.queue, or nullptr.</returns>
        const QueuedPrompt* FindPrompt(const SessionInfo& s, std::wstring_view id) noexcept
        {
            const auto it = std::find_if(s.queue.begin(), s.queue.end(), [id](const QueuedPrompt& q) { return q.id == id; });
            return it == s.queue.end() ? nullptr : &*it;
        }

        /// <summary>The message ids as one log word ("123,456"; "-" when there is none).</summary>
        /// <param name="ids">The ids.</param>
        /// <returns>The text.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring IdsText(const std::vector<int64_t>& ids)
        {
            std::wstring out;
            for (const int64_t id : ids)
            {
                out += (out.empty() ? L"" : L",") + std::to_wstring(id);
            }
            return out.empty() ? std::wstring{ L"-" } : out;
        }

        /// <summary>A text without the spaces and line breaks around it.</summary>
        /// <param name="s">The text.</param>
        /// <returns>The trimmed copy.</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        std::wstring Trimmed(std::wstring_view s)
        {
            const size_t a = s.find_first_not_of(L" \t\r\n");
            if (a == std::wstring_view::npos)
            {
                return {};
            }
            const size_t b = s.find_last_not_of(L" \t\r\n");
            return std::wstring{ s.substr(a, b - a + 1) };
        }
    }

    // ===== small helpers ================================================================================================

    std::wstring MessagingService::_sessionOfThread(int64_t threadId)
    {
        if (threadId <= 0)
        {
            return {};
        }
        std::lock_guard lk{ _mtx };
        for (const auto& [sid, t] : _state.topics)
        {
            if (t.threadId == threadId)
            {
                return sid;
            }
        }
        return {};
    }

    void MessagingService::_noteReactedWhere(const std::wstring& promptId, const std::wstring& sessionId, std::vector<int64_t> messageIds)
    {
        if (promptId.empty() || messageIds.empty())
        {
            return;
        }
        std::lock_guard lk{ _mtx };
        auto& where = _reactedWhere[promptId];
        where.sessionId = sessionId;
        where.messageIds = std::move(messageIds);
    }

    void MessagingService::_deleteChatMessage(int64_t messageId, SendPriority prio, const std::wstring& key)
    {
        if (messageId <= 0)
        {
            return;
        }
        OutItem del;
        del.prio = prio;
        del.key = key; // a ticket's: a delete replaces the ticket's edit still waiting in the outbox (the edit would show a dead ticket)
        del.method = L"deleteMessage";
        del.params = json::Value::MkObj();
        del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        _enqueue(std::move(del));
    }

    // ===== the tickets ===================================================================================================

    void MessagingService::_syncTicketsOf(const std::wstring& sessionId)
    {
        const auto s = _session(sessionId);
        if (!s)
        {
            return;
        }
        const int64_t thread = _threadOf(sessionId);
        if (thread <= 0)
        {
            return; // no topic: nothing to show (a closed tab's topic went with its messages)
        }
        _syncTickets(sessionId, thread, *s);
    }

    void MessagingService::_syncTickets(const std::wstring& sessionId, int64_t threadId, const SessionInfo& s)
    {
        if (threadId <= 0)
        {
            return;
        }
        const int64_t now = WallMs();

        // 1. The ➕ posts the send thread has answered (§21.57.7): queued HERE — the queueing belongs to the poll and tick threads,
        //    never the send thread — each as the session's next prompt whose message is the bot's own post (its ticket's base).
        std::vector<AgainPost> again;
        {
            std::lock_guard lk{ _mtx };
            for (auto it = _againPending.begin(); it != _againPending.end();)
            {
                if (it->sessionId == sessionId)
                {
                    again.push_back(std::move(*it));
                    it = _againPending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        for (const auto& a : again)
        {
            std::wstring queuedId;
            if (_queueRemotePrompt(a.sessionId, a.words, a.messageId, a.threadId, false, 0, &queuedId) && !queuedId.empty())
            {
                // The post IS the prompt's message: its ticket edits it (the ticket's line under the words) while it waits, and says
                // "sent" on it when it went. Nothing is shown on it yet (hash 0); a prompt that goes at once leaves it as it is.
                std::lock_guard lk{ _mtx };
                QueueTicket t;
                t.promptId = queuedId;
                t.sessionId = a.sessionId;
                t.threadId = a.threadId;
                t.messageId = a.messageId;
                t.kind = TicketKind::OwnPost;
                t.baseHtml = a.baseHtml;
                t.postedMs = now;
                _tickets[queuedId] = std::move(t);
                ++_ticketGen[queuedId];
                _stateDirty = true;
            }
        }

        // 2. The waiting prompts in the queue's order; the topic's own are the tickets' subjects.
        const bool globalPause = _scheduler ? _scheduler->GlobalPaused() : false;
        struct Want
        {
            QueuedPrompt prompt; // a copy (the snapshot may be replaced while the records are written)
            size_t position{ 0 }; // its keycap
            TicketWhy why{ TicketWhy::None }; // why it waits
            std::wstring nextId; // the waiting prompt after it (Undo's place)
        };
        std::vector<Want> wants;
        {
            std::vector<const QueuedPrompt*> waiting;
            for (const auto& q : s.queue)
            {
                if (PromptWaits(q))
                {
                    waiting.push_back(&q);
                }
            }
            for (size_t i = 0; i < waiting.size(); ++i)
            {
                const QueuedPrompt& q = *waiting[i];
                if (!IsTopicPrompt(q) || PromptMessageIds(q).empty())
                {
                    // The runner's prompt, or one the service wrote itself (a new thread's /add-dir of its extra folders, §21.25.9:
                    // message 0): the /queue list shows it, no ticket does — it has no message here to sit under.
                    continue;
                }
                Want w;
                w.prompt = q;
                w.position = i + 1;
                w.why = DecideTicketWhy(TicketWhyFactsFor(s, q, i + 1, globalPause));
                w.nextId = i + 1 < waiting.size() ? waiting[i + 1]->id : std::wstring{};
                wants.push_back(std::move(w));
            }
        }

        // 3. Under the lock: Undo's copies, the new records, and the tickets to render.
        std::vector<std::wstring> toRender;
        {
            std::lock_guard lk{ _mtx };
            std::unordered_set<std::wstring> wanted;
            for (const auto& w : wants)
            {
                const std::wstring& pid = w.prompt.id;
                wanted.insert(pid);
                _ticketSeen[pid] = TicketSeen{ sessionId, w.prompt, w.nextId };
                if (_ticketSuppressed.count(pid) > 0)
                {
                    continue; // the user deleted this prompt's ticket: never posted again this run
                }
                auto it = _tickets.find(pid);
                if (it != _tickets.end() && it->second.threadId != threadId)
                {
                    // The session's topic changed under the ticket (a closed tab resumed, a /recreate): the old ticket went with the
                    // old topic. A new one is made below, in this topic.
                    _ticketGen.erase(pid);
                    _ticketRetryAtMs.erase(pid);
                    _tickets.erase(it);
                    it = _tickets.end();
                    _stateDirty = true;
                }
                if (it == _tickets.end())
                {
                    if (w.why == TicketWhy::None)
                    {
                        continue; // it goes at the next ready moment: no ticket
                    }
                    if (w.prompt.queuedAtUnixMs > 0 && now - w.prompt.queuedAtUnixMs < kTicketPostDelayMs)
                    {
                        continue; // a beat first (see kTicketPostDelayMs); the next tick looks again
                    }
                    QueueTicket t;
                    t.promptId = pid;
                    t.sessionId = sessionId;
                    t.threadId = threadId;
                    if (w.prompt.remoteThreadId > 0 && w.prompt.remoteThreadId != threadId)
                    {
                        // §21.57.8: its message went with an older topic (a closed tab's topic is deleted with its messages): the
                        // ticket is a POST of its words, which stands for the message from now on (_renderTicket's post moves the
                        // prompt's source to it).
                        t.kind = TicketKind::OwnPost;
                        t.baseHtml = RepostedPromptHtml(w.prompt.text);
                    }
                    else
                    {
                        t.kind = TicketKind::Reply;
                        t.replyTo = TopicMessageIdOf(w.prompt);
                    }
                    _tickets[pid] = std::move(t);
                    ++_ticketGen[pid];
                    _stateDirty = true;
                }
                toRender.push_back(pid);
            }
            for (const auto& [pid, t] : _tickets)
            {
                if (t.threadId == threadId && wanted.count(pid) == 0)
                {
                    toRender.push_back(pid); // ended, removed, failed, merged — or another session's (a /clear's old conversation)
                }
            }
            // Undo's copies: of this session, a prompt that no longer waits and whose ticket left its live phases needs none.
            for (auto it = _ticketSeen.begin(); it != _ticketSeen.end();)
            {
                if (it->second.sessionId == sessionId && wanted.count(it->first) == 0)
                {
                    const auto t = _tickets.find(it->first);
                    if (t == _tickets.end() || (t->second.phase != TicketPhase::Live && t->second.phase != TicketPhase::MergePreview))
                    {
                        it = _ticketSeen.erase(it);
                        continue;
                    }
                }
                ++it;
            }
            // Undo windows long over: their copies go (a late tap answers "too late" either way).
            for (auto it = _queueUndo.begin(); it != _queueUndo.end();)
            {
                if (now - it->second.untilMs > 60 * 1000)
                {
                    it = _queueUndo.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // 4. The renders, outside the lock: this session's tickets from this snapshot, another session's (a /clear's old
        //    conversation keeps its waiting messages, and their tickets sit in this topic) from its own record.
        for (const auto& pid : toRender)
        {
            std::wstring owner;
            {
                std::lock_guard lk{ _mtx };
                const auto it = _tickets.find(pid);
                if (it == _tickets.end())
                {
                    continue;
                }
                owner = it->second.sessionId;
            }
            if (owner == sessionId)
            {
                _renderTicket(pid, &s, false);
            }
            else
            {
                const auto other = _session(owner);
                _renderTicket(pid, other ? &*other : nullptr, false);
            }
        }
    }

    void MessagingService::_renderTicket(const std::wstring& promptId, const SessionInfo* s, bool tapPriority)
    {
        const int64_t now = WallMs();
        // A prompt the caller's snapshot does not hold may have been queued (an Undo, a Retry) after the snapshot was taken: one
        // fresh read decides, so a snapshot that is a beat old never reads a prompt that is back as "removed".
        std::optional<SessionInfo> fresh;
        if (s && !FindPrompt(*s, promptId))
        {
            fresh = _session(s->id);
            if (fresh)
            {
                s = &*fresh;
            }
        }
        const QueuedPrompt* q = s ? FindPrompt(*s, promptId) : nullptr;
        const size_t position = s ? WaitingPosition(*s, promptId) : 0;
        const QueuedPrompt* above = s ? WaitingPromptAbove(*s, promptId) : nullptr;
        const bool globalPause = _scheduler ? _scheduler->GlobalPaused() : false;

        // 1. Under the lock: the record follows the queue (a pickup, a failure, a removal made elsewhere), then a copy to render.
        QueueTicket rec;
        uint64_t gen = 0;
        bool undoOpen = false;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _tickets.find(promptId);
            if (it == _tickets.end())
            {
                return;
            }
            if (!tapPriority)
            {
                if (const auto r = _ticketRetryAtMs.find(promptId); r != _ticketRetryAtMs.end() && now < r->second)
                {
                    return; // a failed post or edit waits out its back-off (a tap tries at once)
                }
            }
            auto& t = it->second;
            if (t.messageId < 0)
            {
                return; // its post is in flight: the answer (its message id) comes first
            }
            if (t.phase == TicketPhase::Live || t.phase == TicketPhase::MergePreview)
            {
                TicketPhase next = t.phase;
                if (!s)
                {
                    next = TicketPhase::Gone; // the session's record is gone
                }
                else if (!q)
                {
                    next = TicketPhase::Removed; // removed elsewhere: the PC's Delete, /unqueue, an older list's ✖
                }
                else if (q->status == PromptStatus::Sent)
                {
                    if (!q->echoed)
                    {
                        return; // sent, its turn not started yet: the ticket stays as it is until the pickup (or a failure)
                    }
                    next = TicketPhase::Sent;
                    t.sentAtMs = q->sentAtUnixMs > 0 ? q->sentAtUnixMs : now;
                }
                else if (q->status == PromptStatus::Failed)
                {
                    next = TicketPhase::Failed;
                    t.runnerAuto = s->autorunner.mode == AutorunnerMode::Full;
                }
                else if (!PromptWaits(*q))
                {
                    next = TicketPhase::Gone; // a legacy Skipped prompt: nothing more to show
                }
                else if (t.phase == TicketPhase::MergePreview && (!above || above->id != t.mergeUpperId || !IsTopicPrompt(*above)))
                {
                    next = TicketPhase::Live; // the queue moved under the preview: back to the line (✅ would refuse anyway)
                }
                if (next == TicketPhase::Removed && _queueUndo.count(promptId) == 0)
                {
                    // A removal made elsewhere gets ↩ Undo too, from the last copy the tick saw.
                    if (const auto seen = _ticketSeen.find(promptId); seen != _ticketSeen.end() && seen->second.sessionId == s->id)
                    {
                        _queueUndo[promptId] = QueueUndo{ s->id, seen->second.prompt, seen->second.nextId, now + kQueueUndoWindowMs };
                        t.undoUntilMs = now + kQueueUndoWindowMs;
                    }
                }
                if (next != t.phase)
                {
                    t.phase = next;
                    ++_ticketGen[promptId];
                    _stateDirty = true;
                }
            }
            rec = t;
            gen = _ticketGen[promptId];
            undoOpen = rec.phase == TicketPhase::Removed && rec.undoUntilMs > now && _queueUndo.count(promptId) > 0;
        }

        // A live ticket whose prompt now goes at once (the turn just ended) is left as it is until the pickup ends it: an edit to
        // "goes in a moment" would cost a call for a message deleted seconds later.
        TicketWhy why = TicketWhy::None;
        if (rec.phase == TicketPhase::Live || rec.phase == TicketPhase::MergePreview)
        {
            if (!q || !s)
            {
                return; // (unreachable: the phase moved above) — nothing sound to show
            }
            why = DecideTicketWhy(TicketWhyFactsFor(*s, *q, position, globalPause));
            if (rec.phase == TicketPhase::Live && why == TicketWhy::None && rec.messageId > 0)
            {
                return;
            }
        }

        // 2. The render.
        TicketViewFacts f;
        f.kind = rec.kind;
        f.baseHtml = rec.baseHtml;
        f.titleRow = _titleRowClassic(rec.threadId);
        f.phase = rec.phase;
        f.promptId = promptId;
        f.position = position > 0 ? position : 1;
        if (rec.phase == TicketPhase::Live && q && s)
        {
            f.why = why;
            f.partCount = (std::max)(size_t{ 1 }, q->remoteParts.size());
            f.editedAtMs = rec.editedAtMs;
            f.canSteer = s->live;
            f.canMoveUp = above != nullptr && s->live;
            f.canMerge = above != nullptr && IsTopicPrompt(*above) && s->live;
        }
        if (rec.phase == TicketPhase::MergePreview && q && above)
        {
            std::vector<RemotePromptPart> parts = PromptPartsOf(*above);
            const std::vector<RemotePromptPart> lower = PromptPartsOf(*q);
            parts.insert(parts.end(), lower.begin(), lower.end());
            f.mergeUpperId = rec.mergeUpperId;
            f.mergedText = MergedRemoteText(parts);
        }
        f.mergedIntoPosition = rec.mergedIntoPosition;
        f.undoOpen = undoOpen;
        f.runnerAuto = rec.runnerAuto;
        f.sentAtMs = rec.sentAtMs;
        f.ageMs = rec.postedMs > 0 ? now - rec.postedMs : 0;
        f.nowMs = now;
        const TicketView view = BuildTicketView(f);
        const uint32_t hash = TicketViewHash(view);

        // 3. The commit: only when the record is still the one rendered (a newer change renders itself).
        enum class Act
        {
            None,
            Post,
            Edit,
            Delete
        };
        Act act = Act::None;
        int64_t messageId = 0;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _tickets.find(promptId);
            if (it == _tickets.end() || _ticketGen[promptId] != gen)
            {
                return;
            }
            auto& t = it->second;
            messageId = t.messageId;
            bool forget = false;
            if (view.remove)
            {
                act = t.messageId > 0 ? Act::Delete : Act::None;
                forget = true;
            }
            else if (t.messageId > 0 && t.hash == hash)
            {
                // It shows this already: an ended ticket's record goes now (its last render is in).
                forget = TicketPhaseIsFinal(t.phase) || (t.phase == TicketPhase::Removed && !undoOpen);
            }
            else if (t.messageId == 0)
            {
                act = Act::Post;
                t.messageId = -1; // in flight: no second post until the answer
                t.hash = hash;
            }
            else
            {
                act = Act::Edit;
                t.hash = hash;
            }
            if (forget)
            {
                _tickets.erase(it);
                _ticketGen.erase(promptId);
                _ticketRetryAtMs.erase(promptId);
            }
            if (act != Act::None || forget)
            {
                _stateDirty = true;
            }
        }

        // 4. The call (outside the lock: _enqueue takes it).
        const json::Value markup = view.rows.empty() ? json::Value{} : _buttons(view.rows);
        const SendPriority prio = tapPriority ? SendPriority::Final : (act == Act::Post ? SendPriority::New : SendPriority::CardEdit);
        const std::wstring key = L"ticket:" + promptId;
        auto self = weak_from_this();
        switch (act)
        {
        case Act::Post:
        {
            const bool repost = rec.kind == TicketKind::OwnPost;
            const std::wstring sessionId = rec.sessionId;
            const int64_t threadId = rec.threadId;
            _sendText(
                rec.threadId,
                view.html,
                true,
                prio,
                L"",
                markup,
                [self, promptId, repost, sessionId, threadId](const TgResult& r) {
                    auto me = self.lock();
                    if (!me)
                    {
                        return;
                    }
                    const int64_t id = r.ok ? r.result.I64At(L"message_id", 0) : 0;
                    {
                        std::lock_guard lk{ me->_mtx };
                        const auto it = me->_tickets.find(promptId);
                        if (it != me->_tickets.end())
                        {
                            if (id > 0)
                            {
                                it->second.messageId = id;
                                it->second.postedMs = WallMs();
                                me->_ticketRetryAtMs.erase(promptId);
                            }
                            else
                            {
                                it->second.messageId = 0;
                                it->second.hash = 0;
                                me->_ticketRetryAtMs[promptId] = WallMs() + kTicketRetryMs;
                            }
                            me->_stateDirty = true;
                        }
                    }
                    if (id > 0 && repost)
                    {
                        // §21.57.8: the RE-POST of a left-over prompt's words stands for its message from now on — the reactions,
                        // an edit's match and the delete check go to it (the old message went with its topic). A merged prompt's
                        // parts become the one post.
                        me->_registry->Update(sessionId, [&](SessionInfo& ss) {
                            for (auto& q : ss.queue)
                            {
                                if (q.id == promptId && PromptWaits(q))
                                {
                                    q.remoteSource = L"telegram:" + std::to_wstring(id);
                                    q.remoteThreadId = threadId;
                                    q.remoteParts.clear();
                                    break;
                                }
                            }
                        });
                        me->_noteReactedWhere(promptId, sessionId, { id });
                    }
                },
                rec.kind == TicketKind::Reply ? rec.replyTo : 0,
                false, // a ticket never makes the sticky card chase it
                nullptr,
                nullptr,
                false); // its title row is in the text (an edit adds none, §21.54)
            break;
        }
        case Act::Edit:
            _editText(messageId, view.html, prio, key, markup, [self, promptId](const TgResult& r) {
                if (r.ok || r.NotModified())
                {
                    return;
                }
                auto me = self.lock();
                if (!me)
                {
                    return;
                }
                std::lock_guard lk{ me->_mtx };
                const auto it = me->_tickets.find(promptId);
                if (it == me->_tickets.end())
                {
                    return;
                }
                if (r.httpStatus == 400 && r.description.find(L"message to edit not found") != std::wstring::npos)
                {
                    // The user deleted the ticket in Telegram: it is not posted again for this prompt this run.
                    me->_ticketSuppressed.insert(promptId);
                    me->_tickets.erase(it);
                    me->_ticketGen.erase(promptId);
                    me->_ticketRetryAtMs.erase(promptId);
                    me->_stateDirty = true;
                    return;
                }
                it->second.hash = 0; // shown unknown: the next pass edits again, after the back-off
                me->_ticketRetryAtMs[promptId] = WallMs() + kTicketRetryMs;
            });
            break;
        case Act::Delete:
            _deleteChatMessage(messageId, prio, key);
            break;
        case Act::None:
        default:
            break;
        }
    }

    void MessagingService::_failTicket(const SessionInfo& s, const QueuedPrompt& q, int64_t threadId)
    {
        if (threadId <= 0)
        {
            return; // no topic: nowhere to say it (the PC's queue shows the failure)
        }
        const bool runnerAuto = s.autorunner.mode == AutorunnerMode::Full;
        std::lock_guard lk{ _mtx };
        const auto it = _tickets.find(q.id);
        if (it == _tickets.end())
        {
            QueueTicket t;
            t.promptId = q.id;
            t.sessionId = s.id;
            t.threadId = threadId;
            t.kind = TicketKind::Reply;
            t.replyTo = TopicMessageIdOf(q);
            t.phase = TicketPhase::Failed;
            t.runnerAuto = runnerAuto;
            _tickets[q.id] = std::move(t);
        }
        else if (it->second.phase == TicketPhase::Live || it->second.phase == TicketPhase::MergePreview)
        {
            it->second.phase = TicketPhase::Failed;
            it->second.runnerAuto = runnerAuto;
        }
        else
        {
            return; // told already (or it ended otherwise)
        }
        // A failure is said even for a prompt whose ticket the user deleted: it is news, not a repost of the same line.
        _ticketSuppressed.erase(q.id);
        ++_ticketGen[q.id];
        _stateDirty = true;
    }

    // ===== the controls ==================================================================================================

    std::wstring MessagingService::_onQueueTap(const std::wstring& data, int64_t threadId, int64_t messageId)
    {
        const auto cb = ParseQueueCallback(data);
        if (!cb)
        {
            return {};
        }
        // The session: the list's ▶ names it; a ticket's record (or an Undo) knows it; else the thread's.
        std::wstring sid;
        if (cb->verb == kQueueVerbListNext)
        {
            sid = cb->id;
        }
        else
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _tickets.find(cb->id); it != _tickets.end())
            {
                sid = it->second.sessionId;
            }
            else if (const auto u = _queueUndo.find(cb->id); u != _queueUndo.end())
            {
                sid = u->second.sessionId;
            }
        }
        if (sid.empty())
        {
            sid = _sessionOfThread(threadId);
        }
        if (sid.empty())
        {
            return L"This topic has no session any more.";
        }
        const auto editList = [&]() {
            // The /queue list is LIVE (§21.57.5): its own buttons edit it in place, so its numbers stay right.
            if (const auto s = _session(sid); s && messageId > 0)
            {
                std::wstring html;
                json::Value markup;
                _queueListParts(*s, html, markup);
                _editText(messageId, WithTopicTitleRowAfterHeader(html, _titleRowClassic(threadId)), SendPriority::Final, L"queue-list:" + std::to_wstring(messageId), markup);
            }
        };
        if (cb->verb == kQueueVerbSteer)
        {
            return _steerQueuedPrompt(sid, cb->id, threadId);
        }
        if (cb->verb == kQueueVerbDelete)
        {
            return _removeQueuedPrompt(sid, cb->id, L"the ticket's Delete");
        }
        if (cb->verb == kQueueVerbMoveUp)
        {
            return _moveQueuedUp(sid, cb->id);
        }
        if (cb->verb == kQueueVerbMerge)
        {
            return _mergeQueued(sid, cb->id, false, {});
        }
        if (cb->verb == kQueueVerbMergeYes)
        {
            return _mergeQueued(sid, cb->id, true, cb->arg);
        }
        if (cb->verb == kQueueVerbMergeNo)
        {
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _tickets.find(cb->id); it != _tickets.end() && it->second.phase == TicketPhase::MergePreview)
                {
                    it->second.phase = TicketPhase::Live;
                    it->second.mergeUpperId.clear();
                    ++_ticketGen[cb->id];
                    _stateDirty = true;
                }
            }
            if (const auto s = _session(sid))
            {
                _renderTicket(cb->id, &*s, true);
            }
            LogNav(L"remote telegram queue merge " + ShortId(sid) + L" -> cancelled");
            return L"Not merged.";
        }
        if (cb->verb == kQueueVerbUndo)
        {
            return _undoQueuedRemove(cb->id);
        }
        if (cb->verb == kQueueVerbRetry)
        {
            return _retryFailedPrompt(sid, cb->id, threadId);
        }
        if (cb->verb == kQueueVerbDismiss)
        {
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _tickets.find(cb->id); it != _tickets.end() && it->second.phase == TicketPhase::Failed)
                {
                    it->second.phase = TicketPhase::Dismissed;
                    ++_ticketGen[cb->id];
                    _stateDirty = true;
                }
            }
            if (const auto s = _session(sid))
            {
                _renderTicket(cb->id, &*s, true);
            }
            LogNav(L"remote telegram queue dismiss " + ShortId(sid) + L" -> done");
            return {};
        }
        if (cb->verb == kQueueVerbListRemove)
        {
            const std::wstring toast = _removeQueuedPrompt(sid, cb->id, L"the /queue list");
            editList();
            return toast;
        }
        if (cb->verb == kQueueVerbListNext)
        {
            _sendnow(sid, threadId);
            editList();
            return {};
        }
        return {};
    }

    std::wstring MessagingService::_steerQueuedPrompt(const std::wstring& sessionId, const std::wstring& promptId, int64_t threadId)
    {
        const auto nav = [&](const wchar_t* outcome) {
            LogNav(L"remote telegram queue steer " + ShortId(sessionId) + L" -> " + outcome);
        };
        const auto s = _session(sessionId);
        const QueuedPrompt* found = s ? FindPrompt(*s, promptId) : nullptr;
        if (!found || !PromptWaits(*found))
        {
            nav(L"refused:gone");
            return L"Already sent, or no longer in the queue.";
        }
        const QueuedPrompt copy = *found;
        const SteerAction action = DecideSteer(copy.text, s->live, s->started, s->external, s->state, s->pendingBoxState);
        if (action == SteerAction::Closed || action == SteerAction::Usage)
        {
            nav(L"refused:closed");
            return L"This session is closed: resume it from the \U0001F50E Search topic.";
        }
        // §21.57.6: the user may have deleted the message since it was queued: a deleted message is never sent.
        if (action == SteerAction::Send && IsTopicPrompt(copy) && !_remotePreSendCheck(sessionId, copy))
        {
            nav(L"left-out:deleted");
            _syncTicketsOf(sessionId);
            return L"That message was deleted in Telegram: it is left out.";
        }
        const bool sendNow = action == SteerAction::Send;
        const int64_t now = WallMs();
        bool moved = false;
        std::wstring text;
        _registry->Update(sessionId, [&](SessionInfo& ss) {
            const auto it = std::find_if(ss.queue.begin(), ss.queue.end(), [&](const QueuedPrompt& x) { return x.id == promptId; });
            if (it == ss.queue.end() || !PromptWaits(*it))
            {
                return;
            }
            QueuedPrompt p = std::move(*it);
            ss.queue.erase(it);
            if (sendNow)
            {
                // Marked Sent the way the Manager's Send now marks a prompt before the send seam (Rule #4).
                p.status = PromptStatus::Sent;
                p.attempts += 1;
                p.sentAtUnixMs = now;
                p.echoed = false;
                p.enterRetries = 0;
                p.injectedAtUnixMs = 0;
            }
            else
            {
                p.status = PromptStatus::Pending; // a legacy Held one too: it waits FIRST now
            }
            text = p.text;
            // FIRST: right before every prompt that still waits (§21.33's place), whatever happens next.
            ss.queue.insert(ss.queue.begin() + static_cast<std::ptrdiff_t>(SteerInsertIndex(ss.queue)), std::move(p));
            moved = true;
        });
        if (!moved)
        {
            nav(L"refused:gone");
            return L"Already sent, or no longer in the queue.";
        }
        const std::vector<int64_t> ids = PromptMessageIds(copy);
        if (!sendNow)
        {
            if (action == SteerAction::QueueDormant && _hooks.sessionAction)
            {
                try
                {
                    _hooks.sessionAction(sessionId, L"start", L"", nullptr);
                }
                catch (...)
                {
                    // Rule #18. Recovery: the message stays first in the queue; it goes when the tab is entered or activated.
                    LogSwallowedException(L"MessagingService::_steerQueuedPrompt (start)");
                }
            }
            if (_scheduler)
            {
                _scheduler->RequestAdvance(sessionId);
            }
            _syncTicketsOf(sessionId);
            switch (action)
            {
            case SteerAction::QueueDormant:
                nav(L"queued:dormant");
                return L"The tab's claude is not running: it starts now, and this message goes first.";
            case SteerAction::QueueEnded:
                nav(L"queued:ended");
                return L"Claude ended in this tab: the message waits first. \x25B6 Reactivate starts the session again.";
            case SteerAction::QueueMenu:
            default:
                nav(L"queued:menu");
                return L"A question or a menu is open in the session: the message waits first and goes once it is closed.";
            }
        }
        {
            std::lock_guard lk{ _mtx };
            _steers[promptId] = SteerWatch{ sessionId, TopicMessageIdOf(copy), threadId, now };
        }
        for (const int64_t id : ids)
        {
            _react(id, kSteerSendingReaction);
        }
        // Rule #4 (SESSIONS_STORE.md §4.4): the Sent mark made above is on the disk before the words are typed (this service's
        // own thread: the FULL commit); a commit that failed types nothing and takes the declined path below.
        const bool saved = PersistFleetNowDurable();
        if (saved && _registry->SubmitPrompt({ sessionId, promptId, text, false }))
        {
            // Accepted: the hosting window types it (its draft swap keeps the user's draft at the PC). Its echo turns the reaction
            // to 👨‍💻 and ends the ticket; a later decline puts it back first in the queue, and _sweepSteers says so.
            _log(L"[msg] " + ShortId(sessionId) + L" queued message steered from its ticket (message " + IdsText(ids) + L", " + std::to_wstring(text.size()) +
                 L" chars): sent now");
            nav(L"sent");
            _syncTicketsOf(sessionId);
            return L"\x26A1 Sent now.";
        }
        // Declined at once: the delivery gate is held (another delivery, or a box clear, owns the input box). Back in the queue,
        // first, and the runner sends it at the next ready moment.
        {
            std::lock_guard lk{ _mtx };
            _steers.erase(promptId);
        }
        _registry->RollbackPromptToPending(sessionId, promptId, false);
        for (const int64_t id : ids)
        {
            _react(id, kSteerQueuedReaction);
        }
        nav(saved ? L"queued:box-held" : L"queued:not-saved");
        _syncTicketsOf(sessionId);
        return saved ? std::wstring{ L"Not sent now: another delivery is typing into the session's input box. It waits first in the queue." } :
                       std::wstring{ L"Not sent now: Agentmaster could not save the send to its session store. It waits first in the queue." };
    }

    std::wstring MessagingService::_removeQueuedPrompt(const std::wstring& sessionId, const std::wstring& promptId, const wchar_t* how)
    {
        const int64_t now = WallMs();
        const int64_t thread = _threadOf(sessionId);
        bool removed = false;
        QueuedPrompt was;
        std::wstring nextId;
        _registry->Update(sessionId, [&](SessionInfo& ss) {
            for (auto it = ss.queue.begin(); it != ss.queue.end(); ++it)
            {
                if (it->id != promptId)
                {
                    continue;
                }
                if (!PromptWaits(*it))
                {
                    return; // sent (or failed) meanwhile: history, never removed
                }
                for (auto jt = std::next(it); jt != ss.queue.end(); ++jt)
                {
                    if (PromptWaits(*jt))
                    {
                        nextId = jt->id;
                        break;
                    }
                }
                was = *it;
                ss.queue.erase(it);
                removed = true;
                return;
            }
        });
        if (!removed)
        {
            LogNav(L"remote telegram queue remove " + ShortId(sessionId) + L" -> refused:gone");
            return L"Already sent, or no longer in the queue.";
        }
        const bool fromPhone = IsTopicPrompt(was);
        const std::vector<int64_t> ids = PromptMessageIds(was);
        {
            std::lock_guard lk{ _mtx };
            _queueUndo[promptId] = QueueUndo{ sessionId, was, nextId, now + kQueueUndoWindowMs };
            if (const auto it = _tickets.find(promptId); it != _tickets.end())
            {
                it->second.phase = TicketPhase::Removed;
                it->second.undoUntilMs = now + kQueueUndoWindowMs;
                it->second.mergeUpperId.clear();
            }
            else if (fromPhone && thread > 0)
            {
                // No ticket yet (it would have gone at once): the removal gets one, so ↩ Undo has a button to sit on.
                QueueTicket t;
                t.promptId = promptId;
                t.sessionId = sessionId;
                t.threadId = thread;
                t.kind = TicketKind::Reply;
                t.replyTo = TopicMessageIdOf(was);
                t.phase = TicketPhase::Removed;
                t.undoUntilMs = now + kQueueUndoWindowMs;
                _tickets[promptId] = std::move(t);
            }
            ++_ticketGen[promptId];
            _ticketSuppressed.erase(promptId);
            _reacted.erase(promptId);
            _reactedWhere.erase(promptId);
            _sendingMarks.erase(promptId);
            _ticketSeen.erase(promptId);
            _stateDirty = true;
        }
        for (const int64_t id : ids)
        {
            _react(id, L""); // §21.57.9 fix 1: the ✍ comes off at once
        }
        _log(L"[msg] " + ShortId(sessionId) + L" queued prompt removed (" + how + L"; message " + IdsText(ids) + L", " + std::to_wstring(was.text.size()) + L" chars)");
        LogNav(std::wstring{ L"remote telegram queue remove " } + ShortId(sessionId) + L" -> removed (" + how + L")");
        _syncTicketsOf(sessionId); // every ticket's number follows, and this one shows "Removed" with ↩ Undo
        return fromPhone ? L"\U0001F5D1 Removed \x2014 \x21A9 Undo puts it back." : L"\U0001F5D1 Removed.";
    }

    std::wstring MessagingService::_undoQueuedRemove(const std::wstring& promptId)
    {
        const int64_t now = WallMs();
        QueueUndo u;
        bool have = false;
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _queueUndo.find(promptId); it != _queueUndo.end())
            {
                u = it->second;
                have = true;
            }
        }
        if (!have || now >= u.untilMs)
        {
            LogNav(L"remote telegram queue undo " + ShortId(have ? u.sessionId : std::wstring{}) + L" -> refused:too-late");
            return L"Too late to undo \x2014 a reply \x2795 to the message queues it again.";
        }
        bool restored = false;
        bool closed = false;
        bool already = false;
        _registry->Update(u.sessionId, [&](SessionInfo& ss) {
            if (!ss.live)
            {
                closed = true;
                return;
            }
            if (FindPrompt(ss, promptId))
            {
                already = true;
                return;
            }
            QueuedPrompt p = u.prompt;
            p.status = PromptStatus::Pending;
            p.echoed = false;
            p.queuedAtUnixMs = 0; // stamped again by the registry: it waits its own (short, for a fresh one) settle
            size_t at = ss.queue.size();
            bool placed = false;
            if (!u.beforeId.empty())
            {
                for (size_t i = 0; i < ss.queue.size(); ++i)
                {
                    if (ss.queue[i].id == u.beforeId && PromptWaits(ss.queue[i]))
                    {
                        at = i; // back where it was: right before the prompt that followed it
                        placed = true;
                        break;
                    }
                }
            }
            if (!placed)
            {
                at = IsFreshRemotePrompt(p) ? FreshRemoteInsertIndex(ss.queue) : ss.queue.size();
            }
            ss.queue.insert(ss.queue.begin() + static_cast<std::ptrdiff_t>(at), std::move(p));
            restored = true;
        });
        if (closed)
        {
            LogNav(L"remote telegram queue undo " + ShortId(u.sessionId) + L" -> refused:closed");
            return L"The session is closed: there is no queue to put it back in.";
        }
        if (already || !restored)
        {
            return L"It is in the queue already.";
        }
        const std::vector<int64_t> ids = PromptMessageIds(u.prompt);
        {
            std::lock_guard lk{ _mtx };
            _queueUndo.erase(promptId);
            if (const auto it = _tickets.find(promptId); it != _tickets.end())
            {
                it->second.phase = TicketPhase::Live;
                it->second.undoUntilMs = 0;
            }
            ++_ticketGen[promptId];
            if (IsTopicPrompt(u.prompt))
            {
                _reacted[promptId] = PromptStatus::Pending;
                _reactedWhere[promptId] = ReactedWhere{ u.sessionId, ids };
            }
            _stateDirty = true;
        }
        for (const int64_t id : ids)
        {
            _react(id, kQueuedMark);
        }
        _log(L"[msg] " + ShortId(u.sessionId) + L" queued prompt put back by Undo (message " + IdsText(ids) + L")");
        LogNav(L"remote telegram queue undo " + ShortId(u.sessionId) + L" -> restored");
        _syncTicketsOf(u.sessionId);
        if (_scheduler)
        {
            _scheduler->RequestAdvance(u.sessionId);
        }
        return L"\x21A9 Back in the queue.";
    }

    std::wstring MessagingService::_moveQueuedUp(const std::wstring& sessionId, const std::wstring& promptId)
    {
        bool moved = false;
        _registry->Update(sessionId, [&](SessionInfo& ss) {
            size_t above = ss.queue.size();
            for (size_t i = 0; i < ss.queue.size(); ++i)
            {
                if (!PromptWaits(ss.queue[i]))
                {
                    continue;
                }
                if (ss.queue[i].id == promptId)
                {
                    if (above < ss.queue.size())
                    {
                        std::swap(ss.queue[i], ss.queue[above]); // the PC's Move up, between the two WAITING prompts
                        moved = true;
                    }
                    return;
                }
                above = i;
            }
        });
        if (!moved)
        {
            LogNav(L"remote telegram queue move-up " + ShortId(sessionId) + L" -> refused");
            return L"It is first already, or no longer in the queue.";
        }
        LogNav(L"remote telegram queue move-up " + ShortId(sessionId) + L" -> moved");
        _syncTicketsOf(sessionId); // both tickets (and any other whose "after" changed) follow
        if (_scheduler)
        {
            _scheduler->RequestAdvance(sessionId);
        }
        return L"\x23EB Moved up.";
    }

    std::wstring MessagingService::_mergeQueued(const std::wstring& sessionId, const std::wstring& promptId, bool confirm, const std::wstring& upperPrefix)
    {
        const int64_t thread = _threadOf(sessionId);
        if (!confirm)
        {
            // The PREVIEW first (the user's rule: "have a user approve the merge with preview").
            const auto s = _session(sessionId);
            const QueuedPrompt* q = s ? FindPrompt(*s, promptId) : nullptr;
            if (!q || !PromptWaits(*q))
            {
                return L"Already sent, or no longer in the queue.";
            }
            const QueuedPrompt* above = WaitingPromptAbove(*s, promptId);
            if (!above || !IsTopicPrompt(*above))
            {
                return L"There is no message of yours above it to merge into.";
            }
            {
                std::lock_guard lk{ _mtx };
                auto it = _tickets.find(promptId);
                if (it == _tickets.end())
                {
                    if (thread <= 0)
                    {
                        return L"This topic has no session any more.";
                    }
                    QueueTicket t;
                    t.promptId = promptId;
                    t.sessionId = sessionId;
                    t.threadId = thread;
                    t.kind = TicketKind::Reply;
                    t.replyTo = TopicMessageIdOf(*q);
                    it = _tickets.emplace(promptId, std::move(t)).first;
                }
                it->second.phase = TicketPhase::MergePreview;
                it->second.mergeUpperId = above->id;
                ++_ticketGen[promptId];
                _stateDirty = true;
            }
            _renderTicket(promptId, &*s, true);
            LogNav(L"remote telegram queue merge " + ShortId(sessionId) + L" -> asked");
            return {};
        }
        // ✅ Merge: into the prompt the preview showed, and only while both still wait one under the other.
        std::wstring upperId;
        size_t upperPosition = 0;
        QueuedPrompt lowerCopy;
        std::vector<int64_t> mergedIds;
        bool merged = false;
        bool gone = false;
        _registry->Update(sessionId, [&](SessionInfo& ss) {
            const auto li = std::find_if(ss.queue.begin(), ss.queue.end(), [&](const QueuedPrompt& x) { return x.id == promptId; });
            if (li == ss.queue.end() || !PromptWaits(*li))
            {
                gone = true;
                return;
            }
            QueuedPrompt* upper = nullptr;
            size_t position = 0;
            size_t upos = 0;
            for (auto& q : ss.queue)
            {
                if (!PromptWaits(q))
                {
                    continue;
                }
                ++position;
                if (q.id == promptId)
                {
                    break;
                }
                upper = &q;
                upos = position;
            }
            if (!upper || !IsTopicPrompt(*upper) || upperPrefix.empty() || upper->id.rfind(upperPrefix, 0) != 0)
            {
                return; // the order changed since the preview (or the one above is not the topic's): refused below
            }
            std::vector<RemotePromptPart> parts = PromptPartsOf(*upper);
            const std::vector<RemotePromptPart> lowerParts = PromptPartsOf(*li);
            parts.insert(parts.end(), lowerParts.begin(), lowerParts.end());
            upper->remoteParts = parts;
            upper->text = MergedRemoteText(parts); // a NEW line between them (the user's rule)
            upper->label = TgFirstLine(upper->text, 56);
            if (upper->remoteQueuedBusyUnixMs == 0)
            {
                // §21.44: the merged prompt waited if either part did — its "sent from the queue" line is then posted.
                upper->remoteQueuedBusyUnixMs = li->remoteQueuedBusyUnixMs;
            }
            mergedIds = PromptMessageIds(*upper);
            upperId = upper->id;
            upperPosition = upos;
            lowerCopy = *li;
            ss.queue.erase(li); // the upper prompt sits before it, so erasing the lower one leaves `upper` valid (it is not used after)
            merged = true;
        });
        if (gone)
        {
            LogNav(L"remote telegram queue merge " + ShortId(sessionId) + L" -> refused:gone");
            return L"Already sent, or no longer in the queue.";
        }
        if (!merged)
        {
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _tickets.find(promptId); it != _tickets.end() && it->second.phase == TicketPhase::MergePreview)
                {
                    it->second.phase = TicketPhase::Live;
                    it->second.mergeUpperId.clear();
                    ++_ticketGen[promptId];
                    _stateDirty = true;
                }
            }
            _syncTicketsOf(sessionId);
            LogNav(L"remote telegram queue merge " + ShortId(sessionId) + L" -> refused:order");
            return L"The queue changed since the preview: tap \U0001F517 Merge again.";
        }
        const std::vector<int64_t> lowerIds = PromptMessageIds(lowerCopy);
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _tickets.find(promptId); it != _tickets.end())
            {
                it->second.phase = TicketPhase::Merged;
                it->second.mergedIntoPosition = upperPosition;
                it->second.mergeUpperId.clear();
            }
            ++_ticketGen[promptId];
            ++_ticketGen[upperId]; // its line changes ("2 messages")
            _reacted.erase(promptId);
            _reactedWhere.erase(promptId);
            _sendingMarks.erase(promptId);
            _ticketSeen.erase(promptId);
            // The merged prompt's status reaches every message it is made of from now on (the reaction sync, the vanish check).
            _reactedWhere[upperId] = ReactedWhere{ sessionId, mergedIds };
            _stateDirty = true;
        }
        for (const int64_t id : lowerIds)
        {
            _react(id, kMergedReaction); // 🤝 instead of ✍: it went into the message above
        }
        _log(L"[msg] " + ShortId(sessionId) + L" queued message " + IdsText(lowerIds) + L" merged into the one above (messages " + IdsText(mergedIds) + L")");
        LogNav(L"remote telegram queue merge " + ShortId(sessionId) + L" -> merged");
        _syncTicketsOf(sessionId);
        return L"\U0001F517 Merged.";
    }

    std::wstring MessagingService::_retryFailedPrompt(const std::wstring& sessionId, const std::wstring& promptId, int64_t threadId)
    {
        const auto s = _session(sessionId);
        const QueuedPrompt* failed = s ? FindPrompt(*s, promptId) : nullptr;
        if (!failed || failed->status != PromptStatus::Failed)
        {
            return L"Nothing to retry: it is not a failed message any more.";
        }
        if (!s->live)
        {
            return L"This session is closed: resume it from the \U0001F50E Search topic first.";
        }
        const int64_t now = WallMs();
        // A NEW prompt with the failed one's words and messages; the failed record stays in the history (Rule #4: a Sent or failed
        // prompt is never sent a second time as itself).
        QueuedPrompt p = *failed;
        p.id = NewSessionId();
        p.status = PromptStatus::Pending;
        p.attempts = 0;
        p.sentAtUnixMs = 0;
        p.echoed = false;
        p.enterRetries = 0;
        p.injectedAtUnixMs = 0;
        p.committedRender.clear();
        p.queuedAtUnixMs = 0;
        p.remoteLeftOver = false;
        p.guardPattern = std::wstring{ kAnswersQuestionOk }; // the user's own words answer an open question themselves (D30)
        if (p.remoteThreadId <= 0)
        {
            p.remoteThreadId = threadId;
        }
        bool inserted = false;
        const bool queued = _registry->Update(sessionId, [&](SessionInfo& ss) {
            const size_t at = FreshRemoteInsertIndex(ss.queue);
            p.remoteQueuedBusyUnixMs = RemotePromptWait(ss, at) != RemoteWait::None ? now : 0; // §21.44
            ss.queue.insert(ss.queue.begin() + static_cast<std::ptrdiff_t>(at), p);
            inserted = true;
        });
        if (!queued || !inserted)
        {
            return L"This session is gone.";
        }
        const std::vector<int64_t> ids = PromptMessageIds(p);
        {
            std::lock_guard lk{ _mtx };
            _reacted[p.id] = PromptStatus::Pending;
            _reactedWhere[p.id] = ReactedWhere{ sessionId, ids };
            if (const auto it = _tickets.find(promptId); it != _tickets.end())
            {
                it->second.phase = TicketPhase::Requeued;
            }
            ++_ticketGen[promptId];
            _stateDirty = true;
        }
        for (const int64_t id : ids)
        {
            _react(id, kQueuedMark);
        }
        if (!s->started && !s->external && _hooks.sessionAction)
        {
            try
            {
                _hooks.sessionAction(sessionId, L"start", L"", nullptr); // a dormant tab starts so the queue can deliver (§7.1)
            }
            catch (...)
            {
                // Rule #18. Recovery: the words wait in the queue; they go when the tab is entered or activated.
                LogSwallowedException(L"MessagingService::_retryFailedPrompt (start)");
            }
        }
        if (_scheduler)
        {
            _scheduler->RequestAdvance(sessionId);
        }
        _log(L"[msg] " + ShortId(sessionId) + L" a failed phone prompt was queued again by Retry (message " + IdsText(ids) + L")");
        LogNav(L"remote telegram queue retry " + ShortId(sessionId) + L" -> queued");
        _syncTicketsOf(sessionId);
        return L"\U0001F501 Queued again.";
    }

    // ===== a message deleted in Telegram =================================================================================

    bool MessagingService::_remotePreSendCheck(const std::wstring& sessionId, const QueuedPrompt& prompt) noexcept
    {
        // Counted FIRST, then `_stop` read: Stop sets `_stop` before it waits for this count to reach 0, so a check that counts
        // itself after that wait began sees `_stop` and never touches `_api` (which Stop frees right after the wait).
        _preSendChecksInFlight.fetch_add(1);
        struct Leave
        {
            std::atomic<int>& count;
            ~Leave()
            {
                count.fetch_sub(1);
            }
        } leave{ _preSendChecksInFlight };
        try
        {
            if (!_running.load() || _stop.load() || !_api)
            {
                return true; // the bot is not up: nothing can be asked, and the prompt goes as it would without the check
            }
            const std::vector<int64_t> ids = PromptMessageIds(prompt);
            if (ids.empty())
            {
                return true;
            }
            const int64_t now = WallMs();
            std::vector<int64_t> gone;
            bool anyMarked = false;
            for (const int64_t id : ids)
            {
                {
                    std::lock_guard lk{ _mtx };
                    if (const auto it = _aliveChecks.find(id); it != _aliveChecks.end() && now - it->second < kPreSendCheckReuseMs)
                    {
                        continue; // found alive a moment ago (a declined send retried): no second call
                    }
                }
                if (_stop.load() || !_awaitCallSlot(L"setMessageReaction"))
                {
                    return true; // stopping, or reactions are flood-parked: the answer is unknown — send
                }
                auto p = json::Value::MkObj();
                p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
                p.Set(L"message_id", json::Value::MkNum(static_cast<double>(id)));
                auto arr = json::Value::MkArr();
                auto re = json::Value::MkObj();
                re.Set(L"type", json::Value::MkStr(L"emoji"));
                re.Set(L"emoji", json::Value::MkStr(std::wstring{ kSteerSendingReaction })); // ⚡: it is being sent
                arr.Push(std::move(re));
                p.Set(L"reaction", std::move(arr));
                const TgResult r = _api->Call(L"setMessageReaction", p, static_cast<uint32_t>(kPreSendCheckTimeoutMs));
                if (HeldMessageGone(r.ok, r.httpStatus, r.description))
                {
                    gone.push_back(id);
                    continue;
                }
                if (r.ok)
                {
                    anyMarked = true;
                    std::lock_guard lk{ _mtx };
                    _aliveChecks[id] = now;
                }
                else if (r.httpStatus == 429)
                {
                    // Paced like every direct call, but never slept out here: this is the scheduler's thread.
                    _noteFlood(L"setMessageReaction", r.retryAfterSec);
                    if (r.retryAfterSec > kFloodParkAfterSec)
                    {
                        _parkForFlood(L"setMessageReaction", r.retryAfterSec);
                    }
                }
            }
            {
                std::lock_guard lk{ _mtx };
                if (anyMarked)
                {
                    _sendingMarks[prompt.id] = now; // the reaction sync puts ✍ back if the send is declined
                }
                for (auto it = _aliveChecks.begin(); it != _aliveChecks.end();)
                {
                    it = now - it->second >= kPreSendCheckReuseMs ? _aliveChecks.erase(it) : std::next(it); // bounded
                }
            }
            if (gone.empty())
            {
                return true;
            }
            // The user deleted the message (or some parts of a merged prompt): those leave the queue.
            bool whole = false;
            bool changed = false;
            _registry->Update(sessionId, [&](SessionInfo& ss) {
                const auto it = std::find_if(ss.queue.begin(), ss.queue.end(), [&](const QueuedPrompt& x) { return x.id == prompt.id; });
                if (it == ss.queue.end() || !PromptWaits(*it))
                {
                    return;
                }
                std::vector<RemotePromptPart> parts = PromptPartsOf(*it);
                parts.erase(std::remove_if(parts.begin(), parts.end(), [&](const RemotePromptPart& part) {
                                return std::find(gone.begin(), gone.end(), part.messageId) != gone.end();
                            }),
                            parts.end());
                if (parts.empty())
                {
                    ss.queue.erase(it);
                    whole = true;
                    changed = true;
                    return;
                }
                if (parts.size() == 1)
                {
                    it->remoteSource = L"telegram:" + std::to_wstring(parts.front().messageId);
                    it->remoteParts.clear();
                }
                else
                {
                    it->remoteParts = parts;
                }
                it->text = MergedRemoteText(parts);
                it->label = TgFirstLine(it->text, 56);
                changed = true;
            });
            if (!changed)
            {
                return true; // it went meanwhile, or the deleted message is none of its own any more
            }
            {
                std::lock_guard lk{ _mtx };
                if (whole)
                {
                    if (const auto it = _tickets.find(prompt.id); it != _tickets.end())
                    {
                        it->second.phase = TicketPhase::Gone;
                    }
                    _reacted.erase(prompt.id);
                    _reactedWhere.erase(prompt.id);
                    _sendingMarks.erase(prompt.id);
                    _ticketSeen.erase(prompt.id);
                }
                ++_ticketGen[prompt.id];
                _stateDirty = true;
                _tickRequested = true; // the ticket ends at the next tick, not a period later
            }
            _tickCv.notify_all();
            _log(L"[msg] " + ShortId(sessionId) + L" phone message " + IdsText(gone) + L" was deleted in Telegram before its send; " +
                 (whole ? L"left out of the queue" : L"its part left out of the merged prompt"));
            LogNav(L"remote telegram queue deleted-message " + ShortId(sessionId) + (whole ? L" -> left out" : L" -> part left out"));
            return false;
        }
        catch (...)
        {
            // Rule #18. Recovery: the prompt goes as planned — the check could not tell, and a lost prompt costs more than a stray one.
            LogSwallowedException(L"MessagingService::_remotePreSendCheck");
            return true;
        }
    }

    // ===== the /queue list ================================================================================================

    void MessagingService::_queueListParts(const SessionInfo& s, std::wstring& html, json::Value& markup)
    {
        const bool globalPause = _scheduler ? _scheduler->GlobalPaused() : false;
        std::vector<QueueListItem> items;
        size_t position = 0;
        for (const auto& q : s.queue)
        {
            if (!PromptWaits(q))
            {
                continue;
            }
            ++position;
            QueueListItem item;
            item.promptId = q.id;
            item.text = q.text;
            item.fromPhone = IsTopicPrompt(q);
            if (item.fromPhone)
            {
                // Only what holds the SESSION ("after 1️⃣" is what the list's order says already).
                const TicketWhy why = DecideTicketWhy(TicketWhyFactsFor(s, q, position, globalPause));
                if (why != TicketWhy::None && why != TicketWhy::After)
                {
                    item.why = TicketWhyWords(why, {});
                }
            }
            items.push_back(std::move(item));
        }
        html = BuildQueueListHtml(items);
        const auto rows = BuildQueueListButtons(items, s.id);
        markup = rows.empty() ? json::Value{} : _buttons(rows);
    }

    // ===== the ❌ and ➕ replies ===========================================================================================

    bool MessagingService::_takeQueueReply(const std::wstring& sessionId, const json::Value& msg, int64_t threadId)
    {
        const QueueReplyKind kind = ClassifyQueueReply(msg.StrAt(L"text"));
        if (kind == QueueReplyKind::None)
        {
            return false;
        }
        const json::Value* rt = msg.Find(L"reply_to_message");
        if (!rt || rt->type != json::Value::Type::Obj)
        {
            return false; // a lone ❌ or + that replies to nothing is an ordinary message
        }
        const int64_t target = rt->I64At(L"message_id", 0);
        // In a topic, a message that replies to nothing still carries reply_to_message: the topic's creation message (its id is the
        // thread's). Only a REAL reply is a gesture.
        if (target <= 0 || target == threadId || rt->Find(L"forum_topic_created"))
        {
            return false;
        }
        const int64_t msgId = msg.I64At(L"message_id", 0);
        const int64_t now = WallMs();
        const auto s = _session(sessionId);
        if (!s)
        {
            return false;
        }
        // What the reply points at: a ticket, a voice note's 🎤 echo, or a message of a prompt.
        std::wstring ticketPrompt;
        int64_t noteId = 0;
        {
            std::lock_guard lk{ _mtx };
            for (const auto& [pid, t] : _tickets)
            {
                if (t.messageId == target)
                {
                    ticketPrompt = pid;
                    break;
                }
            }
            if (const auto e = _voiceEchoNotes.find(target); e != _voiceEchoNotes.end())
            {
                noteId = e->second;
            }
        }
        const auto matches = [&](const QueuedPrompt& q) {
            return (!ticketPrompt.empty() && q.id == ticketPrompt) || PromptHasMessage(q, target) || (noteId > 0 && PromptHasMessage(q, noteId));
        };
        if (kind == QueueReplyKind::Remove)
        {
            const QueuedPrompt* waiting = nullptr;
            const QueuedPrompt* any = nullptr;
            for (auto it = s->queue.rbegin(); it != s->queue.rend(); ++it)
            {
                if (!matches(*it))
                {
                    continue;
                }
                if (!any)
                {
                    any = &*it;
                }
                if (PromptWaits(*it))
                {
                    waiting = &*it;
                    break;
                }
            }
            if (waiting)
            {
                const std::wstring pid = waiting->id; // copied: the removal changes the queue
                _removeQueuedPrompt(sessionId, pid, L"a reply \x274C");
                _deleteChatMessage(msgId, SendPriority::Final, {}); // the gesture did its work: the ❌ goes (a bot may, in a private chat)
                return true;
            }
            if (any && any->status == PromptStatus::Sent)
            {
                _reply(threadId, L"Already sent at " + TgClockText(any->sentAtUnixMs > 0 ? any->sentAtUnixMs : now, now) + L" \x2014 nothing to remove.", false, json::Value{}, msgId);
            }
            else
            {
                _reply(threadId, L"That message is not waiting in the queue \x2014 nothing to remove.", false, json::Value{}, msgId);
            }
            LogNav(L"remote telegram queue reply-x " + ShortId(sessionId) + L" -> refused:not-waiting");
            return true;
        }
        // ➕: the words of the replied-to message's prompt (a merged prompt's part when the reply points at that part's message).
        std::wstring words;
        for (auto it = s->queue.rbegin(); it != s->queue.rend() && words.empty(); ++it)
        {
            if (!matches(*it))
            {
                continue;
            }
            for (const auto& part : it->remoteParts)
            {
                if (part.messageId == target || (noteId > 0 && part.messageId == noteId))
                {
                    words = part.text;
                    break;
                }
            }
            if (words.empty())
            {
                words = it->text;
            }
        }
        if (words.empty())
        {
            // Not a prompt of the queue's history (an old message, or one sent before this record): the quoted message's own text —
            // the user's words as they are, or a bot's "👤 You" line without its header and title row.
            int64_t author = 0;
            if (const auto* from = rt->Find(L"from"); from && from->type == json::Value::Type::Obj)
            {
                author = from->I64At(L"id", 0);
            }
            const std::wstring quoted = rt->StrAt(L"text");
            if (author != 0 && author == _ownerId)
            {
                words = quoted;
            }
            else if (author != 0 && author == _botId.load())
            {
                words = PromptWordsFromBotLine(quoted);
            }
        }
        words = Trimmed(words);
        if (words.empty())
        {
            _reply(threadId, L"Nothing to queue again in that message.", false, json::Value{}, msgId);
            LogNav(L"remote telegram queue reply-plus " + ShortId(sessionId) + L" -> refused:no-words");
            return true;
        }
        // Read like a typed message: a refused command, or one of Agentmaster's, is never queued from here.
        const QueuedEditDecision d = DecideQueuedEdit(ParseRemoteCommand(words));
        if (d.outcome != QueuedEditOutcome::Replace)
        {
            _reply(threadId, L"That cannot be queued again from here" + (d.name.empty() ? std::wstring{} : L" (<code>/" + TgEscapeHtml(d.name) + L"</code>)") +
                                 L" \x2014 send it as a new message.",
                   false, json::Value{}, msgId);
            LogNav(L"remote telegram queue reply-plus " + ShortId(sessionId) + L" -> refused:command");
            return true;
        }
        // The post that stands for the user's message ("it will be like my message was sent"): queued by the tick once it is in.
        const std::wstring base = AgainPostHtml(now, d.text);
        const std::wstring html = WithTopicTitleRowAfterHeader(base, _titleRowClassic(threadId));
        const std::wstring queuedWords = d.text;
        auto self = weak_from_this();
        _sendText(
            threadId,
            html,
            true,
            SendPriority::Final,
            L"",
            json::Value{},
            [self, sessionId, threadId, queuedWords, base](const TgResult& r) {
                auto me = self.lock();
                if (!me)
                {
                    return;
                }
                const int64_t id = r.ok ? r.result.I64At(L"message_id", 0) : 0;
                if (id <= 0)
                {
                    me->_log(L"[msg] " + ShortId(sessionId) + L" a \x2795 re-queue's post was refused (" + r.description + L"): nothing queued");
                    return;
                }
                {
                    std::lock_guard lk{ me->_mtx };
                    me->_againPending.push_back(AgainPost{ sessionId, threadId, id, queuedWords, base });
                    me->_tickRequested = true;
                }
                me->_tickCv.notify_all();
            },
            0,
            true, // a new message of the conversation: the sticky card follows it
            nullptr,
            nullptr,
            false); // its title row is in the text (its ticket's edits keep it)
        _deleteChatMessage(msgId, SendPriority::Final, {}); // the ➕ did its work
        LogNav(L"remote telegram queue reply-plus " + ShortId(sessionId) + L" chars=" + std::to_wstring(d.text.size()) + L" -> posted");
        return true;
    }
}
