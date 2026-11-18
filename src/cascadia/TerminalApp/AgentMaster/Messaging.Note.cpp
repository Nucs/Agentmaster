// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging, the session's NOTE (see Messaging.h, MESSAGING.md §21.59; the note itself: SESSION_NOTES.md). One
// more TU of the service beside Messaging.cpp, Messaging.Inbound.cpp, Messaging.Questions.cpp, Messaging.Btw.cpp and the rest —
// one class, several files.
//
// The user's request (2026-11-18): "We need note in telegram bottom pinned message and through cog icon we can edit the note and
// such", and their picks on mockups: the note under the card's title, a 🗒️ Note ▸ page from ⚙️ Session, a prompt whose reply OR
// next message becomes the note (with an expiration message), ➕ Add a line, /note, and a line when the note changes at the PC.
//
// What lives here: the note the cards show (a cache the epoch keeps true), the NOTE PROMPT's window (opened by ✏️ / ➕, read by
// _takeNoteText, ended by its text, ✖ / ❌, or the tick's expiry), the one writer of the Telegram side (_writeSessionNote),
// /note, and the tick's epoch pass that tells a change made at the PC from the service's own. The pure rules are MessagingNote.h.
//
// Locks: `_notesMtx` is a LEAF - taken alone, never across a store read, a Telegram call or another lock. Every function here
// copies what it needs out of it first.

#include "Messaging.h"

#include <chrono>
#include <optional>
#include <utility>
#include <vector>

#include "ClaudeSpawn.h" // LogNav, ShortId, LogSwallowedException
#include "MessagingNote.h"
#include "SessionRegistry.h"
#include "SessionStore.h" // GetSessionNote, SetSessionNote, SessionNotesEpoch, NormalizeSessionNote

namespace Agentmaster::Msg
{
    namespace
    {
        /// <summary>The wall clock in Unix ms (the note's stamp, the window's ends: both compared with Telegram's dates).</summary>
        /// <returns>Milliseconds since 1970.</returns>
        int64_t WallMs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /// <summary>
        /// The note as the store holds it NOW, or "" when the read failed (logged, Rule #18). Used where the text matters more than
        /// the cache: the prompt's copy block, an ➕ that adds to the latest note (a PC edit included).
        /// </summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="context">The caller, for the log line.</param>
        /// <returns>The stored note ("" = none, or unreadable).</returns>
        StoredSessionNote ReadStoredNote(const std::wstring& sessionId, const wchar_t* context) noexcept
        {
            try
            {
                return GetSessionNote(sessionId);
            }
            catch (...)
            {
                // Recovery: read as "no note" for this one use; the store and the cards are left as they are.
                LogSwallowedException(context);
                return {};
            }
        }
    }

    // ===== the note the cards show =========================================================================================

    std::wstring MessagingService::_noteShownFor(const std::wstring& sessionId) const
    {
        if (sessionId.empty())
        {
            return {};
        }
        {
            std::lock_guard lk{ _notesMtx };
            if (const auto it = _notesShown.find(sessionId); it != _notesShown.end())
            {
                return it->second;
            }
        }
        // First sight: one record read, OUTSIDE the leaf lock (the store takes its own cache lock and may stat the record).
        std::wstring stored = ReadStoredNote(sessionId, L"MessagingService::_noteShownFor").text;
        std::lock_guard lk{ _notesMtx };
        // A write or the tick's pass may have filled it meanwhile: theirs is the newer value, so it is kept (try_emplace).
        return _notesShown.try_emplace(sessionId, std::move(stored)).first->second;
    }

    bool MessagingService::_notePromptOpenFor(const std::wstring& sessionId) const
    {
        std::lock_guard lk{ _notesMtx };
        return _noteWindows.find(sessionId) != _noteWindows.end();
    }

    // ===== the one writer ==================================================================================================

    bool MessagingService::_writeSessionNote(const std::wstring& sessionId, const std::wstring& raw, bool& cut, std::wstring_view via)
    {
        cut = false;
        const std::wstring normalized = NormalizeSessionNote(raw, &cut);
        // The note the cards show goes FIRST: the store write moves SessionNotesEpoch(), and the tick's pass must then find the store
        // equal to what is shown - this write is the service's own, never "changed at the PC".
        std::optional<std::wstring> previous;
        {
            std::lock_guard lk{ _notesMtx };
            if (const auto it = _notesShown.find(sessionId); it != _notesShown.end())
            {
                previous = it->second;
                it->second = normalized;
            }
            else
            {
                _notesShown.emplace(sessionId, normalized);
            }
        }
        bool ok = false;
        try
        {
            ok = SetSessionNote(sessionId, normalized, WallMs());
        }
        catch (...)
        {
            // Rule #18. Recovery: read as a refused write (below) - nothing is claimed written.
            LogSwallowedException(L"MessagingService::_writeSessionNote");
            ok = false;
        }
        if (!ok)
        {
            // Nothing was written (an unreadable record is never replaced, Rule #16): the cards go back to the note they showed,
            // or ask the store again at their next render.
            {
                std::lock_guard lk{ _notesMtx };
                if (previous)
                {
                    _notesShown[sessionId] = *previous;
                }
                else
                {
                    _notesShown.erase(sessionId);
                }
            }
            _log(L"[msg] " + ShortId(sessionId) + L" note not written (" + std::wstring{ via } + L"): the session store refused it");
            return false;
        }
        // The size, never the text (a note is the user's own words; hooks.log is a diagnostic log).
        _log(L"[msg] " + ShortId(sessionId) + L" note " +
             (normalized.empty() ? std::wstring{ L"removed" } : L"written, " + std::to_wstring(normalized.size()) + L" chars" + (cut ? L", cut at the cap" : L"")) + L" (" +
             std::wstring{ via } + L")");
        // At an answer's priority: the user just acted, and the card showing the new note is the answer.
        _refreshCard(sessionId, true, false, true);
        return true;
    }

    // ===== the prompt ======================================================================================================

    void MessagingService::_deleteNoteMessage(int64_t messageId)
    {
        if (messageId <= 0)
        {
            return;
        }
        OutItem del;
        // The answers' priority: a tap that ended the prompt is answered right after it, and the delete is one cheap call.
        del.prio = SendPriority::Final;
        del.method = L"deleteMessage";
        del.params = json::Value::MkObj();
        del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        auto self = weak_from_this();
        del.onDone = [self, messageId](const TgResult& r) {
            if (const auto me = self.lock(); me && !r.ok)
            {
                me->_log(L"[msg] a note prompt (" + std::to_wstring(messageId) + L") could not be deleted (" +
                         (r.description.empty() ? std::wstring{ L"no response" } : r.description) + L"); it stays in the topic");
            }
        };
        _enqueue(std::move(del));
    }

    std::wstring MessagingService::_openNotePrompt(const std::wstring& sessionId, int64_t threadId, NoteWriteMode mode)
    {
        const int64_t now = WallMs();
        const int64_t expires = now + kNoteWindowMs;
        int64_t oldPrompt = 0;
        uint64_t seq = 0;
        {
            std::lock_guard lk{ _notesMtx };
            if (const auto it = _noteWindows.find(sessionId); it != _noteWindows.end())
            {
                // A second tap REPLACES the open prompt. Its message goes now when it is known; one still on its way (-1) goes when
                // its post is answered, since its seq no longer matches the window (the send callback below).
                oldPrompt = it->second.promptMessageId;
            }
            NoteWindow w;
            w.sessionId = sessionId;
            w.threadId = threadId;
            w.mode = mode;
            w.openedMs = now;
            w.expiresMs = expires;
            w.promptMessageId = -1; // posted below, not answered yet
            w.seq = ++_noteWindowSeq;
            seq = w.seq;
            _noteWindows[sessionId] = std::move(w);
        }
        if (oldPrompt > 0)
        {
            _deleteNoteMessage(oldPrompt);
        }
        // The note as stored NOW, not the cards' cache: the prompt's copy block is the text the user edits from.
        const std::wstring current = ReadStoredNote(sessionId, L"MessagingService::_openNotePrompt").text;
        // ForceReply: the phone opens its reply box under the prompt, with the placeholder in the input field (the question card's
        // ✍️ recipe). A ForceReply message can carry no button: its way out is ✖ Cancel the prompt on the card's Note page, or ❌.
        json::Value markup = json::Value::MkObj();
        markup.Set(L"force_reply", json::Value::MkBool(true));
        markup.Set(L"input_field_placeholder", json::Value::MkStr(std::wstring{ NotePromptPlaceholder(mode) }));
        auto self = weak_from_this();
        _sendText(
            threadId,
            NotePromptHtml(mode, current, expires, now),
            true,
            SendPriority::Final,
            L"",
            std::move(markup),
            [self, sessionId, seq](const TgResult& r) {
                const auto me = self.lock();
                if (!me)
                {
                    return;
                }
                const int64_t id = r.ok ? r.result.I64At(L"message_id", 0) : 0;
                bool orphan = false;
                {
                    std::lock_guard lk{ me->_notesMtx };
                    const auto it = me->_noteWindows.find(sessionId);
                    if (it != me->_noteWindows.end() && it->second.seq == seq)
                    {
                        // 0 for a refused post: the window stays open (the next message still counts, as the toast said), with no
                        // prompt to reply to or to delete.
                        it->second.promptMessageId = id > 0 ? id : 0;
                    }
                    else
                    {
                        // The window ended (its text came, ✖, the expiry) or was replaced while the prompt was on its way: it goes
                        // at once, so no stale prompt invites a reply nobody reads.
                        orphan = id > 0;
                    }
                }
                if (orphan)
                {
                    me->_deleteNoteMessage(id);
                }
                if (!r.ok)
                {
                    me->_log(L"[msg] " + ShortId(sessionId) + L" note prompt not posted (" + (r.description.empty() ? std::wstring{ L"no response" } : r.description) +
                             L"); the next message still writes the note");
                }
            },
            0,
            false); // outside the sticky card's clock: a transient prompt, deleted when it is done (the question card's rule)
        _log(L"[msg] " + ShortId(sessionId) + L" note prompt opened (" + (mode == NoteWriteMode::Append ? std::wstring{ L"add a line" } : std::wstring{ L"edit" }) +
             L", until " + TgClockText(expires, now) + L")");
        LogNav(L"remote telegram note-prompt " + ShortId(sessionId) + (mode == NoteWriteMode::Append ? L" add" : L" edit"));
        // The Note page now shows ✖ Cancel the prompt (the card's hash covers its buttons, so this is one edit).
        _refreshCard(sessionId, true, false, true);
        return NoteOpenToast(mode, expires, now);
    }

    bool MessagingService::_endNotePrompt(const std::wstring& sessionId, NotePromptEnd how, uint64_t seq)
    {
        NoteWindow ended;
        {
            std::lock_guard lk{ _notesMtx };
            const auto it = _noteWindows.find(sessionId);
            if (it == _noteWindows.end() || (seq != 0 && it->second.seq != seq))
            {
                return false; // none open, or a newer window replaced the one the caller meant
            }
            ended = std::move(it->second);
            _noteWindows.erase(it);
        }
        // The prompt goes whatever ended it: a stale ForceReply prompt would invite a reply nobody reads any more. One still on its
        // way (-1) is deleted by its post's answer, which finds no window with its seq.
        if (ended.promptMessageId > 0)
        {
            _deleteNoteMessage(ended.promptMessageId);
        }
        const int64_t now = WallMs();
        if (how == NotePromptEnd::Expired)
        {
            // The user's "with expiration msg": a NEW message (an edit of the prompt would not be noticed), silent, in the session's
            // topic as it is now (a recreated topic has another thread than the one the prompt went to).
            const int64_t thread = _threadOf(sessionId);
            _reply(thread > 0 ? thread : ended.threadId, NoteExpiredHtml(ended.expiresMs, now), false);
        }
        const wchar_t* word = how == NotePromptEnd::Written ? L"written" : how == NotePromptEnd::Cancelled ? L"cancelled" : L"expired";
        _log(L"[msg] " + ShortId(sessionId) + L" note prompt ended (" + word + L")");
        LogNav(L"remote telegram note-prompt " + ShortId(sessionId) + L" -> " + word);
        // The Note page loses ✖ Cancel the prompt. A user's act goes at an answer's priority; the expiry at the card's own pace.
        _refreshCard(sessionId, true, false, how != NotePromptEnd::Expired);
        return true;
    }

    bool MessagingService::_takeNoteText(const std::wstring& sessionId, const json::Value& msg, int64_t threadId, bool held)
    {
        if (held)
        {
            return false; // written before the session existed: never a note (DecideNoteWindowMessage would say so too)
        }
        NoteWindow w;
        {
            std::lock_guard lk{ _notesMtx };
            const auto it = _noteWindows.find(sessionId);
            if (it == _noteWindows.end() || it->second.threadId != threadId)
            {
                return false; // no prompt open, or it waits in another topic (the session's topic was recreated meanwhile)
            }
            w = it->second;
        }
        const std::wstring text = msg.StrAt(L"text");
        NoteMessageFacts f;
        f.held = held;
        f.text = text;
        // In a topic, a message that replies to nothing still carries reply_to_message: the topic's creation message. Its id is
        // never the prompt's, and DecideNoteWindowMessage reads only a ❌ / ➕ there as a gesture, which a plain note is not.
        if (const json::Value* rt = msg.Find(L"reply_to_message"))
        {
            f.replyToMessageId = rt->I64At(L"message_id", 0);
        }
        f.promptMessageId = w.promptMessageId;
        f.messageDateMs = msg.I64At(L"date", 0) * 1000;
        f.openedMs = w.openedMs;
        f.expiresMs = w.expiresMs;
        const NoteMessageVerdict verdict = DecideNoteWindowMessage(f);
        if (verdict == NoteMessageVerdict::NotNote)
        {
            return false;
        }
        const int64_t msgId = msg.I64At(L"message_id", 0);
        // CLAIM the window first (only this window, by its seq): one message takes it. Another one decided at the same moment - a
        // voice note's transcript on the tick thread beside a typed message on the poll thread - finds it gone and goes its usual way.
        if (!_endNotePrompt(sessionId, verdict == NoteMessageVerdict::Cancel ? NotePromptEnd::Cancelled : NotePromptEnd::Written, w.seq))
        {
            return false;
        }
        if (verdict == NoteMessageVerdict::Cancel)
        {
            _react(msgId, kNoteWrittenReaction); // the ❌ was read: the prompt is gone, the note unchanged
            return true;
        }
        // ➕ adds to the note as stored NOW (an edit made at the PC while the prompt waited included), not to the cards' cache.
        const std::wstring current = w.mode == NoteWriteMode::Append ? ReadStoredNote(sessionId, L"MessagingService::_takeNoteText").text : std::wstring{};
        bool cut = false;
        if (!_writeSessionNote(sessionId, ComposeWindowNote(w.mode, current, text), cut, w.mode == NoteWriteMode::Append ? L"prompt, a line added" : L"prompt"))
        {
            // The words are still in the user's message, so nothing is lost: they can send them again, or /note them.
            _reply(threadId, std::wstring{ kNoteWriteFailedText }, false, json::Value{}, msgId);
            LogNav(L"remote telegram note " + ShortId(sessionId) + L" -> refused:store");
            return true;
        }
        _react(msgId, kNoteWrittenReaction);
        if (cut)
        {
            _reply(threadId, NoteCutHtml(), false, json::Value{}, msgId);
        }
        LogNav(L"remote telegram note " + ShortId(sessionId) + L" -> written" + (msg.BoolAt(L"am_voice_live") ? std::wstring{ L" (a voice note)" } : std::wstring{}));
        return true;
    }

    // ===== the Note page's buttons and /note ===============================================================================

    std::wstring MessagingService::_noteFromCard(const std::wstring& sessionId, std::wstring_view verb, int64_t threadId, int64_t messageId)
    {
        // The card's OWNER: after a /clear or an in-session /resume the sticky card belongs to the tab's next session (a Rekey),
        // whatever session the button's data names - the note edited is the one the card shows.
        std::wstring owner = sessionId;
        {
            std::lock_guard lk{ _mtx };
            if (messageId > 0)
            {
                for (const auto& [sid, t] : _state.topics)
                {
                    if (t.cardMessageId == messageId)
                    {
                        owner = sid;
                        break;
                    }
                }
            }
        }
        if (verb == kCardVerbNoteEdit || verb == kCardVerbNoteAdd)
        {
            // ➕ on a card whose note was removed meanwhile (at the PC) adds to nothing, which is writing: ComposeWindowNote says so.
            return _openNotePrompt(owner, threadId, verb == kCardVerbNoteAdd ? NoteWriteMode::Append : NoteWriteMode::Replace);
        }
        if (verb == kCardVerbNoteCancel)
        {
            return _endNotePrompt(owner, NotePromptEnd::Cancelled) ? std::wstring{ kNoteCancelledToast } : std::wstring{ kNoteNoPromptToast };
        }
        if (verb == kCardVerbNoteRemoveYes)
        {
            std::wstring toast{ kNoteRemovedToast };
            if (ReadStoredNote(owner, L"MessagingService::_noteFromCard").text.empty())
            {
                // Removed at the PC meanwhile: nothing to write, but the cards are told (the shown note may still be the old one).
                {
                    std::lock_guard lk{ _notesMtx };
                    _notesShown[owner] = std::wstring{};
                }
                toast = std::wstring{ kNoteNothingToRemoveToast };
            }
            else
            {
                bool cut = false;
                if (!_writeSessionNote(owner, std::wstring{}, cut, L"remove"))
                {
                    return std::wstring{ kNoteWriteFailedText };
                }
                LogNav(L"remote telegram note " + ShortId(owner) + L" -> removed");
            }
            // An open prompt stays: writing a new note after removing the old one is a fair thing to do. The card goes back to the
            // Note page (now ✏️ Write a note), a page flip like ◀ Keep it's.
            _showCardPage(owner, messageId, CardPage::Note);
            return toast;
        }
        return {};
    }

    void MessagingService::_runNoteCommand(const std::wstring& sessionId, const std::wstring& args, int64_t messageId, int64_t threadId)
    {
        const NoteCommand c = ParseNoteCommandArgs(args);
        if (c.kind == NoteCommandKind::Usage)
        {
            _reply(threadId, NoteUsageHtml(), false, json::Value{}, messageId);
            LogNav(L"remote telegram /note " + ShortId(sessionId) + L" -> usage");
            return;
        }
        if (c.kind == NoteCommandKind::Show)
        {
            const StoredSessionNote n = ReadStoredNote(sessionId, L"MessagingService::_runNoteCommand");
            _reply(threadId, NoteShowHtml(n.text, n.atUnixMs, WallMs()), false, json::Value{}, messageId);
            LogNav(L"remote telegram /note " + ShortId(sessionId) + L" -> shown");
            return;
        }
        std::wstring raw;
        if (c.kind == NoteCommandKind::Set)
        {
            raw = c.text;
        }
        else if (c.kind == NoteCommandKind::Append)
        {
            // Added to the note as stored NOW (never the cards' cache), on a line of its own.
            raw = ComposeWindowNote(NoteWriteMode::Append, ReadStoredNote(sessionId, L"MessagingService::_runNoteCommand").text, c.text);
        }
        // Remove: raw stays "" - the store removes both keys.
        // An open prompt ends: its text came by the command (its message goes; nothing is posted for it).
        _endNotePrompt(sessionId, NotePromptEnd::Written);
        bool cut = false;
        if (!_writeSessionNote(sessionId, raw, cut, L"/note"))
        {
            _reply(threadId, std::wstring{ kNoteWriteFailedText }, false, json::Value{}, messageId);
            LogNav(L"remote telegram /note " + ShortId(sessionId) + L" -> refused:store");
            return;
        }
        _react(messageId, kNoteWrittenReaction);
        if (cut)
        {
            _reply(threadId, NoteCutHtml(), false, json::Value{}, messageId);
        }
        LogNav(L"remote telegram /note " + ShortId(sessionId) + L" -> " +
               (c.kind == NoteCommandKind::Remove ? L"removed" : c.kind == NoteCommandKind::Append ? L"line added" : L"written"));
    }

    // ===== the tick ========================================================================================================

    void MessagingService::_noteTick(int64_t nowMs) noexcept
    {
        try
        {
            // 1. The prompts past their five minutes, copied out of the leaf lock (each end takes it again, by seq).
            std::vector<NoteWindow> due;
            {
                std::lock_guard lk{ _notesMtx };
                for (const auto& [sid, w] : _noteWindows)
                {
                    if (nowMs > w.expiresMs)
                    {
                        due.push_back(w);
                    }
                }
            }
            for (const NoteWindow& w : due)
            {
                // A voice note sent INSIDE the window may still be in its topic's transcription lane: the note was spoken in time,
                // so the expiry waits for it (kNoteWindowVoiceGraceMs at most). Read under the voice lock alone.
                bool voicePending = false;
                {
                    std::lock_guard lk{ _voiceMtx };
                    if (const auto it = _voiceLanes.find(w.threadId); it != _voiceLanes.end())
                    {
                        for (const auto& item : it->second)
                        {
                            if (item.isVoice && !item.forSearch && NoteSentInWindow(w, item.msg.I64At(L"date", 0) * 1000))
                            {
                                voicePending = true;
                                break;
                            }
                        }
                    }
                }
                if (DecideNoteWindowTick(w, nowMs, voicePending) == NoteWindowTick::Expire)
                {
                    _endNotePrompt(w.sessionId, NotePromptEnd::Expired, w.seq); // a window a tap replaced meanwhile is left alone
                }
            }

            // 2. The epoch pass: only when a note was written somewhere since the last one (SessionNotesEpoch() moves on every
            //    write that CHANGED a note, by the PC or by this service), so a quiet tick reads nothing.
            // The epoch is read BEFORE the pass and recorded only AFTER it: a write during the pass moves the epoch past the recorded
            // value, so the next tick reads again, and a pass cut short by an exception is repeated rather than lost.
            const uint64_t epoch = SessionNotesEpoch();
            if (epoch == _notesEpochSeen)
            {
                return;
            }
            std::vector<std::pair<std::wstring, int64_t>> topics; // session id -> its topic's thread, copied out of `_mtx`
            {
                std::lock_guard lk{ _mtx };
                topics.reserve(_state.topics.size());
                for (const auto& [sid, t] : _state.topics)
                {
                    if (t.threadId > 0)
                    {
                        topics.emplace_back(sid, t.threadId);
                    }
                }
            }
            for (const auto& [sid, thread] : topics)
            {
                const std::wstring stored = ReadStoredNote(sid, L"MessagingService::_noteTick").text;
                NoteChange change = NoteChange::None;
                {
                    std::lock_guard lk{ _notesMtx };
                    const auto it = _notesShown.find(sid);
                    change = ClassifyNoteChange(it == _notesShown.end() ? nullptr : &it->second, stored);
                    if (change != NoteChange::None)
                    {
                        _notesShown[sid] = stored;
                    }
                }
                if (change == NoteChange::None)
                {
                    continue;
                }
                if (change == NoteChange::Changed || change == NoteChange::Removed)
                {
                    // The user's extra "Line when PC changes it": a write the service did not make came from the PC's editor.
                    _reply(thread, NotePcChangeHtml(stored), false);
                    _log(L"[msg] " + ShortId(sid) + L" note " + (change == NoteChange::Removed ? std::wstring{ L"removed" } : std::wstring{ L"changed" }) + L" at the PC: the topic is told");
                    // A real change: the card shows it now. A first sight (a new topic, a start) only when there is a note to show,
                    // at the card's own pace - the cards of a whole fleet must never be forced at once.
                    _refreshCard(sid, true);
                }
                else if (!stored.empty())
                {
                    _refreshCard(sid, false);
                }
            }
            _notesEpochSeen = epoch; // the whole pass is done: a quiet tick reads nothing until the next write
        }
        catch (...)
        {
            // Rule #18. Recovery: this tick's note work stops here. The epoch was not recorded, so the next tick runs the pass again;
            // a topic it already updated reads None then (its shown note is the stored one), so nothing is said twice.
            LogSwallowedException(L"MessagingService::_noteTick");
        }
    }
}
