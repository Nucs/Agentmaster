// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging, the INBOUND half (see Messaging.h): Telegram updates → what the owner meant.
// A message in a session's topic is a queued prompt (§7.1) or a /command (§7.2–§7.3); a button press is
// an action token or a card verb; a message in the Search topic is a request to the search agent or a
// fleet command (§5.4). Every entry point checks the SENDER first (H3) and acts only for the paired owner.

#include "Messaging.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>

#include "ClaudeSpawn.h" // LogNav, ShortId, ResolveClaudeTranscriptPath, NewSessionId, LogSwallowedException
#include "Persistence.h" // DeriveSessionTitle
#include "ProcessInspect.h" // AnalyzeSessionTranscript, ReadGitBranchForDir
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8
#include "Scheduler.h"
#include "SessionRegistry.h"
#include "SessionStore.h" // favorites, tags
#include "TranscriptTurns.h" // RenderTranscriptTurnsFile

namespace Agentmaster::Msg
{
    namespace
    {
        int64_t WallMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // A token id: base-36 of a sequence + 4 random-ish characters from the clock, "t:<id>" ≤ 20 bytes.
        std::wstring MakeTokenId(uint64_t seq)
        {
            static const wchar_t* digits = L"0123456789abcdefghijklmnopqrstuvwxyz";
            std::wstring out;
            uint64_t v = seq * 1000003ull + static_cast<uint64_t>(::GetTickCount64() & 0xFFFF);
            do
            {
                out.insert(out.begin(), digits[v % 36]);
                v /= 36;
            } while (v);
            return out;
        }

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

        // The first word and the rest of an argument string.
        std::pair<std::wstring, std::wstring> SplitFirst(const std::wstring& args)
        {
            const size_t ws = args.find_first_of(L" \t");
            if (ws == std::wstring::npos)
            {
                return { args, {} };
            }
            return { args.substr(0, ws), Trimmed(args.substr(ws)) };
        }

        std::optional<TurnsLevel> LevelFromWord(std::wstring_view w)
        {
            if (w.empty() || w == L"default")
            {
                return TurnsLevel::Default;
            }
            if (w == L"minimal")
            {
                return TurnsLevel::Minimal;
            }
            if (w == L"compact")
            {
                return TurnsLevel::Compact;
            }
            if (w == L"full")
            {
                return TurnsLevel::Full;
            }
            if (w == L"max")
            {
                return TurnsLevel::Max;
            }
            return std::nullopt;
        }

        // The `cwd` of a transcript's first line that has one, from its first 64 KB; "" when unreadable.
        std::wstring TranscriptCwd(const std::wstring& path)
        {
            if (path.empty())
            {
                return {};
            }
            HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return {};
            }
            std::string head(64 * 1024, '\0');
            DWORD read = 0;
            const BOOL ok = ::ReadFile(h, head.data(), static_cast<DWORD>(head.size()), &read, nullptr);
            ::CloseHandle(h);
            if (!ok)
            {
                return {};
            }
            head.resize(read);
            const size_t lastNl = head.rfind('\n');
            if (lastNl == std::string::npos)
            {
                return {};
            }
            head.resize(lastNl + 1);
            for (const auto& line : ParseTranscriptJsonl(head))
            {
                if (const auto cwd = line.StrAt(L"cwd"); !cwd.empty())
                {
                    return cwd;
                }
            }
            return {};
        }

        std::wstring SafeFileStem(std::wstring_view title)
        {
            std::wstring out;
            for (const wchar_t c : title)
            {
                const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-' || c == L'_' || c == L'.';
                out += ok ? c : L'_';
                if (out.size() >= 40)
                {
                    break;
                }
            }
            return out.empty() ? std::wstring{ L"session" } : out;
        }
    }

    // ===== updates ===========================================================================

    void MessagingService::_handleUpdate(const json::Value& update) noexcept
    {
        try
        {
            const int64_t updateId = update.I64At(L"update_id", 0);
            {
                std::lock_guard lk{ _mtx };
                if (updateId + 1 > _state.updateOffset)
                {
                    _state.updateOffset = updateId + 1;
                    _stateDirty = true;
                }
            }
            if (const auto* msg = update.Find(L"message"); msg && msg->type == json::Value::Type::Obj)
            {
                _handleMessage(*msg, false);
            }
            else if (const auto* em = update.Find(L"edited_message"); em && em->type == json::Value::Type::Obj)
            {
                _handleMessage(*em, true);
            }
            else if (const auto* cq = update.Find(L"callback_query"); cq && cq->type == json::Value::Type::Obj)
            {
                _handleCallback(*cq);
            }
            else if (const auto* bc = update.Find(L"business_connection"); bc && bc->type == json::Value::Type::Obj)
            {
                _handleBusinessConnection(*bc);
            }
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_handleUpdate");
        }
    }

    void MessagingService::_handleBusinessConnection(const json::Value& bc)
    {
        // H3 again: the connecting user first. A business connection from anyone else is not ours to use.
        int64_t user = 0;
        if (const auto* u = bc.Find(L"user"); u && u->type == json::Value::Type::Obj)
        {
            user = u->I64At(L"id", 0);
        }
        if (user == 0 || user != _ownerId)
        {
            _log(L"[msg] secretary: a business connection from a user who is not the paired owner was ignored");
            return;
        }
        const std::wstring id = bc.StrAt(L"id");
        const bool enabled = bc.BoolAt(L"is_enabled", false);
        bool canReply = bc.BoolAt(L"can_reply", false); // the pre-9.0 shape
        if (const auto* rights = bc.Find(L"rights"); rights && rights->type == json::Value::Type::Obj)
        {
            canReply = rights->BoolAt(L"can_reply", false);
        }
        {
            std::lock_guard lk{ _mtx };
            _state.businessConnectionId = id;
            _state.businessEnabled = enabled;
            _state.businessCanReply = canReply;
            _state.businessMode = 0; // every (re)connection is probed afresh
            _state.businessNote.clear();
            _stateDirty = true;
        }
        _secretaryFailures.store(0);
        _log(std::wstring{ L"[msg] secretary: business connection " } + (enabled ? L"enabled" : L"disabled") + L" (can reply: " + (canReply ? L"yes" : L"no") + L")");
        if (enabled && canReply)
        {
            _probeWanted.store(true);
            {
                std::lock_guard lk{ _mtx };
                _tickRequested = true;
            }
            _tickCv.notify_one();
            return;
        }
        int64_t search = 0;
        {
            std::lock_guard lk{ _mtx };
            search = _state.searchThreadId;
        }
        if (search != 0)
        {
            _sendText(search,
                      enabled ? L"\x26A0 <b>Secretary mode</b>: the bot is connected to your Telegram Business account, but without the right to reply. In Telegram \x2192 Settings \x2192 Business \x2192 Chatbots \x2192 this bot, allow <b>Reply to messages</b>; prompts keep the \x2328 line until then." :
                                L"<b>Secretary mode</b> is off (the bot was disconnected from your Telegram Business account): prompts typed at the PC show as the bot's \x2328 line again.",
                      true,
                      SendPriority::Final,
                      L"secretary-note",
                      json::Value{},
                      nullptr);
        }
    }

    void MessagingService::_probeSecretary()
    {
        int64_t search = 0;
        {
            std::lock_guard lk{ _mtx };
            search = _state.searchThreadId;
        }
        if (search == 0)
        {
            _probeWanted.store(true); // the tick runs it once the Search topic exists
            return;
        }
        _log(L"[msg] secretary: probing one send as the user into the Search topic");
        auto self = weak_from_this();
        _sendAsUser(search, L"Agentmaster \x2014 secretary check: this message was sent as you. Prompts typed at the PC will appear in their topics like this.", [self, search](const TgResult& r) {
            auto me = self.lock();
            if (!me)
            {
                return;
            }
            {
                std::lock_guard lk{ me->_mtx };
                me->_state.businessMode = r.ok ? 1 : -1;
                me->_state.businessNote = r.ok ? std::wstring{} : r.description;
                me->_stateDirty = true;
            }
            me->_secretaryFailures.store(0);
            if (r.ok)
            {
                me->_log(L"[msg] secretary: works - prompts typed at the PC now appear as the user's own messages");
                me->_sendText(search, L"\x2705 <b>Secretary mode works</b>: from now on a prompt typed at the PC appears in its topic as your own message, and the bot's messages read as the replies.", true, SendPriority::Final, L"secretary-note", json::Value{}, nullptr);
            }
            else
            {
                me->_log(L"[msg] secretary: Telegram refused a send as the user (" + r.description + L"); prompts keep the bot's line");
                me->_sendText(search,
                              L"\x26A0 <b>Secretary mode</b>: the bot is connected to your Telegram Business account, but Telegram refused to send into this chat on your behalf (<code>" + TgEscapeHtml(r.description) + L"</code>). "
                              L"Business sends reach your chats with other people; this chat is your chat with the bot itself. Prompts keep the \x2328 line. (Reconnect the bot under Settings \x2192 Business \x2192 Chatbots to try again.)",
                              true,
                              SendPriority::Final,
                              L"secretary-note",
                              json::Value{},
                              nullptr);
            }
        });
    }

    std::wstring MessagingService::_reactivate(const std::wstring& sessionId)
    {
        const auto s = _registry->Get(sessionId);
        if (!s || !s->live)
        {
            LogNav(L"remote telegram reactivate " + ShortId(sessionId) + L" -> closed");
            return L"This session is closed. Resume it from the \U0001F50E Search topic.";
        }
        if (!s->started && !s->external)
        {
            // A parked tab (restored, parked, revived, or cut off with the app): its claude starts in place and
            // resumes the conversation; with CLAUDE_CODE_RESUME_INTERRUPTED_TURN a cut-off turn goes on by itself.
            if (_hooks.sessionAction)
            {
                try
                {
                    _hooks.sessionAction(sessionId, L"start", L"", nullptr);
                }
                catch (...)
                {
                    LogSwallowedException(L"MessagingService::_reactivate (start)");
                }
            }
            LogNav(L"remote telegram reactivate " + ShortId(sessionId) + L" -> start");
            return L"\x25B6 Resuming it in its tab. A cut-off turn goes on by itself; a message sent here goes out once it is ready.";
        }
        if (s->state == SessionState::Error || s->state == SessionState::Done)
        {
            // Its claude is up but the turn failed (an API error, a cut-off turn it did not resume), or claude exited
            // and the tab holds a finished process: a restart resumes the same conversation id in the same tab.
            if (_hooks.restartSession)
            {
                try
                {
                    _hooks.restartSession(sessionId);
                }
                catch (...)
                {
                    LogSwallowedException(L"MessagingService::_reactivate (restart)");
                }
            }
            LogNav(L"remote telegram reactivate " + ShortId(sessionId) + L" -> restart (" + (s->state == SessionState::Done ? std::wstring{ L"exited" } : std::wstring{ L"error" }) + L")");
            return L"\x25B6 Restarting it \x2014 the same conversation resumes in its tab.";
        }
        LogNav(L"remote telegram reactivate " + ShortId(sessionId) + L" -> already running");
        return std::wstring{ L"It is running already (" } + StateLabel(s->state) + L").";
    }

    void MessagingService::_handleMessage(const json::Value& msg, bool edited)
    {
        // H3: the sender first, nothing else read before it.
        int64_t from = 0;
        if (const auto* f = msg.Find(L"from"); f && f->type == json::Value::Type::Obj)
        {
            from = f->I64At(L"id", 0);
        }
        if (from != 0 && from == _botId.load())
        {
            // The bot's OWN service messages come back through getUpdates with the bot as the sender: the
            // forum_topic_created of every topic it makes, the pinned-message note of every card it pins
            // (measured 2026-10-16: one per created topic). Nothing to do with them, and they are not the
            // foreign sender the line below is about.
            return;
        }
        if (from == 0 || from != _ownerId)
        {
            _log(L"[msg] dropped a message from a sender that is not the paired owner");
            return;
        }
        int64_t chatId = 0;
        if (const auto* c = msg.Find(L"chat"); c && c->type == json::Value::Type::Obj)
        {
            chatId = c->I64At(L"id", 0);
        }
        if (chatId != _ownerId)
        {
            return; // a group the bot was added to: not this feature
        }
        // The SECRETARY ECHO guard (§21.6): a message sent ON THE USER'S BEHALF through the business connection — our
        // own send of a prompt typed at the PC, which shows as the user's bubble — is never a new prompt from the
        // phone: queued, it would be sent to claude again and mirrored once more, a loop. Telegram marks what a
        // business bot sent (sender_business_bot) and every message of a business chat (business_connection_id); a
        // message with neither mark that repeats a text we just sent as the user in this topic is dropped below.
        if (msg.Find(L"sender_business_bot") || !msg.StrAt(L"business_connection_id").empty())
        {
            return;
        }
        const int64_t threadId = msg.I64At(L"message_thread_id", 0);
        int64_t searchThread = 0;
        std::wstring sessionId;
        {
            std::lock_guard lk{ _mtx };
            searchThread = _state.searchThreadId;
            for (const auto& [sid, t] : _state.topics)
            {
                if (t.threadId == threadId && threadId != 0)
                {
                    sessionId = sid;
                    break;
                }
            }
        }
        // Service messages (a topic created by the user, a pinned-message note) carry no text.
        if (msg.Find(L"forum_topic_created"))
        {
            if (threadId != 0 && threadId != searchThread && sessionId.empty())
            {
                _reply(threadId, L"Topics are made from Agentmaster's tabs. To start a new session from the phone, send <code>/newsession &lt;folder&gt;</code> in the \U0001F50E Search topic; this topic can be deleted.", false);
            }
            return;
        }
        const std::wstring text = msg.StrAt(L"text");
        if (text.empty())
        {
            if (msg.Find(L"photo") || msg.Find(L"document") || msg.Find(L"voice"))
            {
                _reply(threadId, L"Files, photos and voice notes are not taken yet (a later build). Text only for now.", false, json::Value{}, msg.I64At(L"message_id", 0));
            }
            return;
        }
        if (!edited && threadId != 0 && _takeSecretaryEcho(threadId, text))
        {
            _log(L"[msg] secretary: our own send as the user came back as a message (thread " + std::to_wstring(threadId) + L"); ignored");
            return;
        }
        if (threadId != 0 && threadId == searchThread)
        {
            if (!edited)
            {
                _handleSearchMessage(msg, threadId);
            }
            return;
        }
        if (!sessionId.empty())
        {
            if (edited)
            {
                // An edit of a message that is still a QUEUED prompt replaces its text; a sent one cannot change.
                const int64_t msgId = msg.I64At(L"message_id", 0);
                const std::wstring source = L"telegram:" + std::to_wstring(msgId);
                bool replaced = false;
                _registry->Update(sessionId, [&](SessionInfo& s) {
                    for (auto& q : s.queue)
                    {
                        if (q.remoteSource == source && q.status == PromptStatus::Pending)
                        {
                            q.text = text;
                            q.label = TgFirstLine(text, 56);
                            replaced = true;
                        }
                    }
                });
                if (!replaced)
                {
                    _reply(threadId, L"Already sent \x2014 send the change as a new message.", false, json::Value{}, msgId);
                }
                return;
            }
            _handleTopicMessage(sessionId, msg, threadId);
            return;
        }
        // The chat's main view, or a topic the map does not know (deleted on our side, or made by the user).
        if (threadId == 0)
        {
            const RemoteCommand cmd = ParseRemoteCommand(text);
            if (cmd.kind == RemoteKind::AgentmasterCommand && (cmd.name == L"start" || cmd.name == L"help"))
            {
                _sendHelp(0, true);
                return;
            }
            _reply(0, L"This is Agentmaster's bot. Each working session has its own topic; the \U0001F50E Search topic finds old ones. Send /help for the commands.", false);
            return;
        }
        _reply(threadId, L"This topic belongs to no session any more (its tab closed or went idle). The \U0001F50E Search topic brings a session back; this topic can be deleted.", false);
    }

    void MessagingService::_handleTopicMessage(const std::wstring& sessionId, const json::Value& msg, int64_t threadId)
    {
        const std::wstring text = msg.StrAt(L"text");
        const int64_t msgId = msg.I64At(L"message_id", 0);
        const int64_t dateMs = msg.I64At(L"date", 0) * 1000;
        // Any action in a topic marks the session READ on the PC (the Waiting -> Idle decay then works for a
        // session handled only from the phone).
        _registry->UpdateQuiet(sessionId, [](SessionInfo& s) { s.readUnixMs = WallMs(); });
        // The user's own message is now the bottom of the topic: the sticky card follows it (§21.5).
        _noteTopicPosted(threadId);

        const RemoteCommand cmd = ParseRemoteCommand(text);
        switch (cmd.kind)
        {
        case RemoteKind::AgentmasterCommand:
            _runSessionCommand(sessionId, cmd, msg, threadId);
            return;
        case RemoteKind::RefusedNoRecord:
            _reply(threadId, L"<code>/" + TgEscapeHtml(cmd.name) + L"</code> leaves no record in the session (you would not see it when the session is reopened) \x2014 run it at the PC.", false, json::Value{}, msgId);
            LogNav(L"remote telegram refused /" + cmd.name + L" " + ShortId(sessionId) + L" -> refused:no-record");
            return;
        case RemoteKind::RefusedLeavesTab:
            _reply(threadId, L"<code>/" + TgEscapeHtml(cmd.name) + L"</code> would take the conversation out of its tab \x2014 use /forksession or /newsession here, or run it at the PC.", false, json::Value{}, msgId);
            LogNav(L"remote telegram refused /" + cmd.name + L" " + ShortId(sessionId) + L" -> refused:leaves-tab");
            return;
        case RemoteKind::ClaudeCommand:
        case RemoteKind::Text:
        default:
            break;
        }
        const std::wstring prompt = cmd.text;
        if (prompt.empty())
        {
            return;
        }
        if (IsBacklogUpdate(dateMs, WallMs()))
        {
            // D2: a message sent while Agentmaster was closed is asked about, never queued silently.
            const std::wstring age = TgShortSpan(WallMs() - dateMs);
            const std::wstring token = _tokenFor(L"backlog-queue", sessionId, std::to_wstring(msgId) + L"|" + prompt, kTokenTtlMsFor(2));
            const std::wstring drop = _tokenFor(L"backlog-drop", sessionId, std::to_wstring(msgId), kTokenTtlMsFor(2));
            _reply(threadId, L"This message is " + age + L" old (sent while Agentmaster was closed). Queue it now?", false,
                   _buttons({ { { L"Queue", token }, { L"Drop", drop } } }), msgId);
            return;
        }
        _queueRemotePrompt(sessionId, prompt, msgId, threadId, false);
    }

    // The ttl helper (hours) — tokens live kTokenTtlMs by default; a backlog question a little longer.
    int64_t MessagingService::kTokenTtlMsFor(int hours) noexcept
    {
        return static_cast<int64_t>(hours) * 3600 * 1000;
    }

    bool MessagingService::_queueRemotePrompt(const std::wstring& sessionId, const std::wstring& text, int64_t messageId, int64_t threadId, bool fromBacklog)
    {
        const auto s = _registry->Get(sessionId);
        if (!s || !s->live)
        {
            _reply(threadId, L"This session is closed. Resume it from the \U0001F50E Search topic.", false, json::Value{}, messageId);
            return false;
        }
        QueuedPrompt p;
        p.id = NewSessionId();
        p.text = text;
        p.label = TgFirstLine(text, 56);
        p.status = PromptStatus::Pending;
        p.gate = PromptGate::OnTurnComplete;
        p.origin = PromptOrigin::Autorun; // every delivery-evidence rule keys on Autorun (QueuedPrompt::remoteSource)
        p.guardPattern = std::wstring{ kAnswersQuestionOk }; // the user's own message is the answer to an open question
        p.remoteSource = L"telegram:" + std::to_wstring(messageId);
        size_t position = 0;
        _registry->Update(sessionId, [&](SessionInfo& ss) {
            // D30: next in line — before the runner's own planned prompts, after earlier remote ones.
            auto at = ss.queue.end();
            for (auto it = ss.queue.begin(); it != ss.queue.end(); ++it)
            {
                if (it->status == PromptStatus::Pending && it->remoteSource.empty())
                {
                    at = it;
                    break;
                }
            }
            at = ss.queue.insert(at, p);
            position = static_cast<size_t>(std::distance(ss.queue.begin(), at));
        });
        {
            std::lock_guard lk{ _mtx };
            _reacted[p.id] = PromptStatus::Pending;
        }
        _react(messageId, L"\x270D");
        LogNav(L"remote telegram queue " + ShortId(sessionId) + L" chars=" + std::to_wstring(text.size()) + (fromBacklog ? L" (from the backlog)" : L""));
        _log(L"[msg] " + ShortId(sessionId) + L" queued from topic (message " + std::to_wstring(messageId) + L", " + std::to_wstring(text.size()) + L" chars)");
        // A dormant tab (restored, revived, parked): start it so the queue can deliver (§7.1).
        if (!s->started && !s->external && _hooks.sessionAction)
        {
            try
            {
                _hooks.sessionAction(sessionId, L"start", L"", nullptr);
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_queueRemotePrompt (start)");
            }
        }
        if (_scheduler)
        {
            _scheduler->RequestAdvance(sessionId);
        }
        return true;
    }

    // ===== commands in a session's topic ====================================================

    void MessagingService::_runSessionCommand(const std::wstring& sessionId, const RemoteCommand& cmd, const json::Value& msg, int64_t threadId)
    {
        const int64_t msgId = msg.I64At(L"message_id", 0);
        auto s = _registry->Get(sessionId);
        if (!s)
        {
            _reply(threadId, L"Unknown session.", false);
            return;
        }
        const auto done = [&](const std::wstring& code) {
            LogNav(L"remote telegram /" + cmd.name + L" " + ShortId(sessionId) + L" -> " + code);
        };
        const std::wstring& name = cmd.name;
        if (name == L"help" || name == L"start")
        {
            _sendHelp(threadId, false);
            return;
        }
        if (name == L"status" || name == L"card")
        {
            int64_t idleLeaves = 0;
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
                {
                    idleLeaves = IdleTopicLeavesAtMs(it->second.idleSinceMs, s->lastActivityUnixMs, _idleGraceMs.load());
                }
            }
            json::Value markup;
            _cardMarkupFor(sessionId, s->pinned, s->autorunner.mode, markup, SessionIsReactivatable(*s));
            _reply(threadId, BuildCardText(*s, WallMs(), idleLeaves, _scheduler ? _scheduler->GlobalPaused() : false), false, markup);
            done(L"ok");
            return;
        }
        if (name == L"queue")
        {
            std::wstring html = L"<b>Queue</b>";
            size_t n = 0;
            std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
            for (const auto& q : s->queue)
            {
                if (q.status != PromptStatus::Pending)
                {
                    continue;
                }
                ++n;
                html += L"\n" + std::to_wstring(n) + L". " + (q.remoteSource.empty() ? L"" : L"\U0001F4F1 ") + TgEscapeHtml(TgFirstLine(q.text, 120));
                if (n <= 8)
                {
                    rows.push_back({ { L"\x2716 " + std::to_wstring(n), _tokenFor(L"unqueue", sessionId, q.id, kTokenTtlMsFor(2)) } });
                }
            }
            if (n == 0)
            {
                html += L"\nempty";
            }
            else
            {
                rows.push_back({ { L"\x25B6 Send next now", L"c:sendnow:" + sessionId } });
            }
            _reply(threadId, html, false, rows.empty() ? json::Value{} : _buttons(rows));
            done(L"ok");
            return;
        }
        if (name == L"sendnow")
        {
            _sendnow(sessionId, threadId);
            done(L"ok");
            return;
        }
        if (name == L"unqueue")
        {
            int n = 0;
            try
            {
                n = cmd.args.empty() ? 0 : std::stoi(cmd.args);
            }
            catch (...)
            {
                n = 0;
            }
            if (n <= 0)
            {
                _reply(threadId, L"Usage: /unqueue &lt;number&gt; (see /queue).", false);
                return;
            }
            bool removed = false;
            std::wstring removedText;
            _registry->Update(sessionId, [&](SessionInfo& ss) {
                int k = 0;
                for (auto it = ss.queue.begin(); it != ss.queue.end(); ++it)
                {
                    if (it->status == PromptStatus::Pending && ++k == n)
                    {
                        removedText = TgFirstLine(it->text, 80);
                        ss.queue.erase(it);
                        removed = true;
                        break;
                    }
                }
            });
            _reply(threadId, removed ? L"Removed: " + TgEscapeHtml(removedText) : L"No queued prompt #" + std::to_wstring(n) + L".", false);
            done(removed ? L"ok" : L"refused:unknown-item");
            return;
        }
        if (name == L"runner")
        {
            const std::wstring w = detail::Lower(cmd.args);
            std::optional<AutorunnerMode> want;
            if (w == L"auto" || w == L"on" || w == L"full")
            {
                want = AutorunnerMode::Full;
            }
            else if (w == L"paused" || w == L"off" || w == L"pause")
            {
                want = AutorunnerMode::Off;
            }
            if (!want)
            {
                _reply(threadId, std::wstring{ L"Runner is " } + (s->autorunner.mode == AutorunnerMode::Full ? L"<b>Auto</b>" : L"<b>Paused</b>") + L". Usage: /runner auto | paused", false);
                return;
            }
            _registry->Update(sessionId, [&](SessionInfo& ss) {
                ss.autorunner.mode = *want;
                ClearInterruptHold(ss.autorunner);
            });
            _reply(threadId, std::wstring{ L"Runner " } + (*want == AutorunnerMode::Full ? L"<b>Auto</b>" : L"<b>Paused</b>"), false);
            done(L"ok");
            return;
        }
        if (name == L"pin" || name == L"unpin")
        {
            _setPinned(sessionId, name == L"pin");
            _reply(threadId, name == L"pin" ? L"\U0001F4CC Pinned: this session never goes idle, so its topic stays." : L"Unpinned: the normal idle rule applies from now on.", false);
            done(L"ok");
            return;
        }
        if (name == L"wake")
        {
            _wake(sessionId, false);
            _reply(threadId, L"Moved to Waiting for you.", false);
            done(L"ok");
            return;
        }
        if (name == L"moveidle")
        {
            if (s->pinned)
            {
                _reply(threadId, L"Pinned sessions never go idle (/unpin first).", false);
                return;
            }
            _registry->Update(sessionId, [](SessionInfo& ss) {
                if (ss.state == SessionState::WaitingForInput)
                {
                    ss.state = SessionState::Idle;
                    ss.manualUnread = false;
                    ss.readUnixMs = WallMs();
                }
            });
            _reply(threadId, L"Moved to Idle \x2014 this topic leaves the list after the idle grace (/pin keeps it).", false);
            done(L"ok");
            return;
        }
        if (name == L"markunread")
        {
            if (_hooks.sessionAction)
            {
                _hooks.sessionAction(sessionId, L"markunread", L"", nullptr);
            }
            else
            {
                _registry->Update(sessionId, [](SessionInfo& ss) { ss.manualUnread = true; });
            }
            _reply(threadId, L"Marked unread at the PC.", false);
            done(L"ok");
            return;
        }
        if (name == L"rename")
        {
            const std::wstring title = Trimmed(cmd.args);
            if (title.empty())
            {
                _reply(threadId, L"Usage: /rename &lt;new title&gt;", false);
                return;
            }
            if (_hooks.sessionAction)
            {
                _hooks.sessionAction(sessionId, L"rename", title, nullptr); // the hosting window pins the tab + the registry (Rule #11)
            }
            else
            {
                _registry->Update(sessionId, [&](SessionInfo& ss) { ss.title = title; });
            }
            _reply(threadId, L"Renamed to <b>" + TgEscapeHtml(title) + L"</b> (the topic follows in a moment).", false);
            done(L"ok");
            return;
        }
        if (name == L"favorite" || name == L"unfavorite")
        {
            SetSessionFavorite(sessionId, name == L"favorite");
            if (_hooks.sessionAction)
            {
                _hooks.sessionAction(sessionId, L"refresh-favorite", L"", nullptr);
            }
            _reply(threadId, name == L"favorite" ? L"\x2605 Favorited." : L"\x2606 Unfavorited.", false);
            done(L"ok");
            return;
        }
        if (name == L"tags" || name == L"tag" || name == L"untag")
        {
            if (name == L"tags" || cmd.args.empty())
            {
                const auto tags = GetSessionTags(sessionId);
                std::wstring html = L"<b>Tags:</b> ";
                for (size_t i = 0; i < tags.size(); ++i)
                {
                    html += (i ? L", " : L"") + TgEscapeHtml(tags[i]);
                }
                if (tags.empty())
                {
                    html += L"none";
                }
                html += L"\n/tag &lt;name&gt; adds one, /untag &lt;name&gt; removes one.";
                _reply(threadId, html, false);
                return;
            }
            const std::wstring tag = NormalizeTagName(cmd.args);
            if (tag.empty())
            {
                _reply(threadId, L"That is not a usable tag name.", false);
                return;
            }
            bool changed = false;
            if (name == L"tag")
            {
                changed = AddSessionTag(sessionId, tag);
                if (changed)
                {
                    RegisterKnownTag(tag);
                }
            }
            else
            {
                changed = RemoveSessionTag(sessionId, tag);
            }
            if (changed && _hooks.sessionAction)
            {
                _hooks.sessionAction(sessionId, L"refresh-tags", L"", nullptr);
            }
            _reply(threadId, changed ? (name == L"tag" ? L"Tagged <b>" : L"Untagged <b>") + TgEscapeHtml(tag) + L"</b>" : L"No change.", false);
            done(changed ? L"ok" : L"no-change");
            return;
        }
        if (name == L"sessionid" || name == L"path" || name == L"gitbranch" || name == L"draft")
        {
            std::wstring value;
            std::wstring label;
            if (name == L"sessionid")
            {
                value = sessionId;
                label = L"Session id";
            }
            else if (name == L"path")
            {
                value = s->inferredWorkingDir.empty() ? s->workingDir : s->inferredWorkingDir;
                label = L"Path";
            }
            else if (name == L"gitbranch")
            {
                value = s->branch.empty() ? ReadGitBranchForDir(s->workingDir) : s->branch;
                label = L"Branch";
            }
            else
            {
                value = s->pendingInput;
                label = L"Unsent draft at the PC";
            }
            if (value.empty())
            {
                _reply(threadId, label + L": none", false);
                return;
            }
            json::Value markup;
            if (value.size() <= kTelegramCopyTextMaxChars)
            {
                markup = _buttons({ { { L"Copy", L"copy:" + value } } });
            }
            _reply(threadId, L"<b>" + label + L"</b>\n<code>" + TgEscapeHtml(value) + L"</code>", false, markup);
            done(L"ok");
            return;
        }
        if (name == L"summary")
        {
            const std::wstring path = ResolveClaudeTranscriptPath(sessionId);
            if (path.empty())
            {
                _reply(threadId, L"No transcript yet.", false);
                return;
            }
            const auto a = AnalyzeSessionTranscript(path, 0);
            std::wstring html = L"<b>" + TgEscapeHtml(s->title) + L"</b>\n";
            html += TgEscapeHtml(s->workingDir) + (a.branch.empty() ? L"" : L" \x00B7 " + TgEscapeHtml(a.branch)) + L"\n";
            if (!a.firstTs.empty() && !a.lastTs.empty())
            {
                html += L"duration " + TgEscapeHtml(FormatSessionDuration(a.firstTs, a.lastTs)) + L"\n";
            }
            html += std::to_wstring(a.userMsgs.size()) + L" message(s) \x00B7 " + std::to_wstring(a.filesEdited.size() + a.filesCreated.size()) + L" file(s) edited \x00B7 " + std::to_wstring(a.filesRead.size()) + L" read";
            if (a.tasksCompleted || a.tasksPending)
            {
                html += L" \x00B7 tasks " + std::to_wstring(a.tasksCompleted) + L"/" + std::to_wstring(a.tasksCompleted + a.tasksPending);
            }
            if (!a.userMsgs.empty())
            {
                html += L"\n<b>Messages</b>";
                const size_t from = a.userMsgs.size() > 12 ? a.userMsgs.size() - 12 : 0;
                for (size_t i = from; i < a.userMsgs.size(); ++i)
                {
                    html += L"\n" + std::to_wstring(i + 1) + L". " + TgEscapeHtml(TgFirstLine(a.userMsgs[i], 140));
                }
            }
            if (!a.filesEdited.empty() || !a.filesCreated.empty())
            {
                html += L"\n<b>Files edited</b>";
                size_t n = 0;
                for (const auto& f : a.filesCreated)
                {
                    if (n++ < 15)
                    {
                        html += L"\n+ " + TgEscapeHtml(detail::ShortPath(f, 3));
                    }
                }
                for (const auto& f : a.filesEdited)
                {
                    if (n++ < 15)
                    {
                        html += L"\n~ " + TgEscapeHtml(detail::ShortPath(f, 3));
                    }
                }
                if (n > 15)
                {
                    html += L"\n\x2026 and " + std::to_wstring(n - 15) + L" more";
                }
            }
            _reply(threadId, html, false);
            done(L"ok");
            return;
        }
        if (name == L"transcript")
        {
            const auto level = LevelFromWord(detail::Lower(cmd.args));
            if (!level)
            {
                _reply(threadId, L"Usage: /transcript [minimal|compact|default|full|max]", false);
                return;
            }
            std::wstring path = ResolveClaudeTranscriptPath(sessionId);
            if (path.empty() && !s->forkParentId.empty())
            {
                path = ResolveClaudeTranscriptPath(s->forkParentId); // a fork with no turn of its own shows its source
            }
            if (path.empty())
            {
                _reply(threadId, L"No transcript yet.", false);
                return;
            }
            const auto md = RenderTranscriptTurnsFile(path, TurnsOptionsForLevel(*level), false);
            if (!md || md->empty())
            {
                _reply(threadId, L"The transcript could not be rendered.", false);
                return;
            }
            const std::wstring fileName = SafeFileStem(s->title) + L"-" + TurnsLevelName(*level) + L".md";
            _sendDocument(threadId, fileName, Profiles::detail::WideToUtf8(*md), L"Transcript \x00B7 " + std::wstring{ TurnsLevelName(*level) }, SendPriority::Final);
            done(L"ok");
            return;
        }
        if (name == L"screen")
        {
            if (!_hooks.sessionAction)
            {
                _reply(threadId, L"Not available here.", false);
                return;
            }
            auto self = weak_from_this();
            _hooks.sessionAction(sessionId, L"screen", L"", [self, threadId](std::wstring text) {
                if (auto me = self.lock())
                {
                    if (text.empty())
                    {
                        me->_reply(threadId, L"The screen could not be read (the tab is dormant, or in another window that is not up).", false);
                    }
                    else
                    {
                        if (text.size() > 3500)
                        {
                            text = text.substr(text.size() - 3500);
                        }
                        me->_reply(threadId, L"<pre>" + TgEscapeHtml(text) + L"</pre>", false);
                    }
                }
            });
            done(L"ok");
            return;
        }
        if (name == L"reactivate")
        {
            // A typed command gets its answer in the thread (the card's button answers with a toast instead).
            _reply(threadId, TgEscapeHtml(_reactivate(sessionId)), false, json::Value{}, msgId);
            done(L"ok");
            return;
        }
        if (name == L"activate")
        {
            if (s->started)
            {
                _reply(threadId, L"Already running.", false);
                return;
            }
            if (_hooks.sessionAction)
            {
                _hooks.sessionAction(sessionId, L"start", L"", nullptr);
            }
            _reply(threadId, L"Starting the tab's claude in place.", false);
            done(L"ok");
            return;
        }
        if (name == L"restartsession" || name == L"deactivate" || name == L"interrupt")
        {
            const bool running = s->state == SessionState::Running;
            const std::wstring verb = name == L"restartsession" ? L"Restart" : (name == L"deactivate" ? L"Park" : L"Interrupt");
            if (running || name == L"interrupt")
            {
                if (name == L"interrupt" && !running)
                {
                    _reply(threadId, L"Nothing is running.", false);
                    return;
                }
                const std::wstring token = _tokenFor(L"confirm-" + name, sessionId, L"", kTokenTtlMsFor(1));
                _reply(threadId, verb + L" <b>" + TgEscapeHtml(s->title) + L"</b> while a turn is running? The turn is cut off.", false,
                       _buttons({ { { verb + L" now", token }, { L"Cancel", _tokenFor(L"cancel", sessionId, L"", kTokenTtlMsFor(1)) } } }));
                done(L"asked");
                return;
            }
            if (name == L"restartsession")
            {
                if (_hooks.restartSession)
                {
                    _hooks.restartSession(sessionId);
                }
                _reply(threadId, L"Restarting the session.", false);
            }
            else
            {
                if (_hooks.deactivateSession)
                {
                    _hooks.deactivateSession(sessionId);
                }
                _reply(threadId, L"Parked: the tab keeps its place; the next message starts it again.", false);
            }
            done(L"ok");
            return;
        }
        if (name == L"forksession" || name == L"newsession")
        {
            RemoteLaunchRequest req;
            req.kind = name == L"forksession" ? RemoteLaunchRequest::Kind::Fork : RemoteLaunchRequest::Kind::NewSession;
            req.sessionId = sessionId;
            req.dir = s->workingDir;
            req.title = name == L"forksession" ? s->title : std::wstring{};
            req.model = Trimmed(cmd.args);
            _requestLaunch(req, threadId);
            done(L"ok");
            return;
        }
        if (name == L"takedraft")
        {
            if (s->pendingInput.empty())
            {
                _reply(threadId, L"No unsent draft at the PC.", false);
                return;
            }
            // v1 queues a COPY (the box at the PC keeps its text; the mail button's verified clear is a UI-thread
            // action a later build routes through the hosting window).
            _queueRemotePrompt(sessionId, s->pendingInput, msgId, threadId, false);
            _reply(threadId, L"Queued a copy of the unsent draft (the box at the PC keeps it).", false);
            done(L"ok");
            return;
        }
        if (name == L"idle" || name == L"board" || name == L"pauseall" || name == L"resumeall" || name == L"cc")
        {
            _runSearchCommand(cmd, msg, threadId); // the fleet commands work in any topic
            return;
        }
        _reply(threadId, L"Unknown command here: /" + TgEscapeHtml(name) + L". /help lists them.", false);
    }

    // ===== the Search topic ==================================================================

    void MessagingService::_handleSearchMessage(const json::Value& msg, int64_t threadId)
    {
        const std::wstring text = msg.StrAt(L"text");
        const RemoteCommand cmd = ParseRemoteCommand(text);
        if (cmd.kind == RemoteKind::AgentmasterCommand)
        {
            _runSearchCommand(cmd, msg, threadId);
            return;
        }
        if (cmd.kind != RemoteKind::Text)
        {
            _reply(threadId, L"Claude Code commands go in a session's topic. Here: a search in plain words, or /help.", false);
            return;
        }
        const int64_t dateMs = msg.I64At(L"date", 0) * 1000;
        if (IsBacklogUpdate(dateMs, WallMs()))
        {
            _reply(threadId, L"(sent " + TgShortSpan(WallMs() - dateMs) + L" ago, while Agentmaster was closed \x2014 ask again if you still want it)", false, json::Value{}, msg.I64At(L"message_id", 0));
            return;
        }
        _askSearch(cmd.text, threadId);
    }

    void MessagingService::_runSearchCommand(const RemoteCommand& cmd, const json::Value& /*msg*/, int64_t threadId)
    {
        // (The message itself is not read here yet: every Search command acts on its arguments alone. The
        // parameter stays for the backlog question a later build asks on a command with side effects.)
        const std::wstring& name = cmd.name;
        if (name == L"help" || name == L"start")
        {
            _sendHelp(threadId, true);
            return;
        }
        if (name == L"status")
        {
            std::wstring html = L"<b>Agentmaster</b> \x2014 " + TgEscapeHtml(StatusText());
            size_t topics = 0;
            {
                std::lock_guard lk{ _mtx };
                topics = _state.topics.size();
            }
            html += L"\n" + std::to_wstring(topics) + L" topic(s)";
            _reply(threadId, html, false);
            return;
        }
        if (name == L"idle" || name == L"board")
        {
            size_t counts[6] = { 0, 0, 0, 0, 0, 0 };
            std::vector<std::pair<std::wstring, std::wstring>> idle;
            for (const auto& s : _registry->SnapshotLiveForCards())
            {
                if (s.kind != AgentKind::Claude || s.tabToken.empty())
                {
                    continue;
                }
                counts[static_cast<int>(s.state)]++;
                if (s.state == SessionState::Idle)
                {
                    idle.emplace_back(s.id, s.title.empty() ? ShortId(s.id) : s.title);
                }
            }
            if (name == L"board")
            {
                std::wstring html = L"<b>Board</b>\n";
                html += std::wstring{ StateLabel(SessionState::Running) } + L" " + std::to_wstring(counts[1]) + L"\n";
                html += std::wstring{ StateLabel(SessionState::WaitingForInput) } + L" " + std::to_wstring(counts[2]) + L"\n";
                html += std::wstring{ StateLabel(SessionState::NeedsApproval) } + L" " + std::to_wstring(counts[3]) + L"\n";
                html += std::wstring{ StateLabel(SessionState::Error) } + L" " + std::to_wstring(counts[4]) + L"\n";
                html += std::wstring{ StateLabel(SessionState::Done) } + L" " + std::to_wstring(counts[5]) + L"\n";
                html += std::wstring{ StateLabel(SessionState::Idle) } + L" " + std::to_wstring(counts[0]) + L" (/idle lists them)";
                _reply(threadId, html, false);
                return;
            }
            if (idle.empty())
            {
                _reply(threadId, L"No idle tab.", false);
                return;
            }
            std::wstring html = L"<b>Idle tabs</b> (" + std::to_wstring(idle.size()) + L")";
            std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
            size_t n = 0;
            for (const auto& [id, title] : idle)
            {
                ++n;
                html += L"\n" + std::to_wstring(n) + L". " + TgEscapeHtml(title);
                if (n <= 10)
                {
                    rows.push_back({ { L"Wake " + std::to_wstring(n), _tokenFor(L"wake", id, L"", kTokenTtlMsFor(2)) },
                                     { L"Wake \U0001F4CC " + std::to_wstring(n), _tokenFor(L"wake-pin", id, L"", kTokenTtlMsFor(2)) } });
                }
            }
            _reply(threadId, html, false, _buttons(rows));
            return;
        }
        if (name == L"pauseall" || name == L"resumeall")
        {
            if (_scheduler)
            {
                _scheduler->SetGlobalPause(name == L"pauseall");
            }
            _reply(threadId, name == L"pauseall" ? L"\x23F8 Every runner paused." : L"\x25B6 Runners resumed.", false);
            LogNav(L"remote telegram /" + name + L" -> ok");
            return;
        }
        if (name == L"newsession")
        {
            auto [folder, rest] = SplitFirst(Trimmed(cmd.args));
            // A quoted folder with spaces.
            if (!folder.empty() && folder.front() == L'"')
            {
                const std::wstring whole = Trimmed(cmd.args);
                const size_t close = whole.find(L'"', 1);
                if (close != std::wstring::npos)
                {
                    folder = whole.substr(1, close - 1);
                    rest = Trimmed(whole.substr(close + 1));
                }
            }
            if (folder.empty())
            {
                _reply(threadId, L"Usage: /newsession &lt;folder&gt; [model]", false);
                return;
            }
            const DWORD attrs = ::GetFileAttributesW(folder.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            {
                _reply(threadId, L"Not a folder on this PC: <code>" + TgEscapeHtml(folder) + L"</code>", false);
                return;
            }
            RemoteLaunchRequest req;
            req.kind = RemoteLaunchRequest::Kind::NewSession;
            req.dir = folder;
            req.model = rest;
            _requestLaunch(req, threadId);
            return;
        }
        if (name == L"reset")
        {
            std::lock_guard lk{ _mtx };
            if (_search)
            {
                _search->Reset();
                _searchRows.clear();
            }
            _reply(threadId, L"The search conversation was reset.", false);
            return;
        }
        if (name == L"cc")
        {
            _reply(threadId, L"/cc works in a session's topic.", false);
            return;
        }
        _reply(threadId, L"Here: a search in plain words, or /idle, /board, /newsession &lt;folder&gt;, /pauseall, /resumeall, /status, /help.", false);
    }

    void MessagingService::_askSearch(const std::wstring& text, int64_t threadId)
    {
        std::unique_ptr<SearchBridge> made;
        {
            std::lock_guard lk{ _mtx };
            if (!_search)
            {
                if (!_searchFactory)
                {
                    _reply(threadId, L"Search is not available in this build.", false);
                    return;
                }
                auto self = weak_from_this();
                made = _searchFactory([self](const SearchBridgeEvent& ev) {
                    if (auto me = self.lock())
                    {
                        me->_onSearchEvent(ev);
                    }
                },
                                      _searchThinkingWord);
                _search = std::move(made);
            }
            _searchLastUsedMs = WallMs();
            _searchParked = false;
        }
        if (!_search)
        {
            _reply(threadId, L"Search could not start (see hooks.log).", false);
            return;
        }
        const uint64_t turn = _search->Ask(text);
        LogNav(L"remote telegram search turn=" + std::to_wstring(turn) + L" chars=" + std::to_wstring(text.size()));
        _reply(threadId, L"<i>Searching\x2026</i>", false);
    }

    void MessagingService::_onSearchEvent(const SearchBridgeEvent& ev) noexcept
    {
        try
        {
            int64_t thread = 0;
            {
                std::lock_guard lk{ _mtx };
                thread = _state.searchThreadId;
                _searchLastUsedMs = WallMs();
                if (ev.kind == SearchBridgeEvent::Kind::Shown || (ev.kind == SearchBridgeEvent::Kind::Answer && !ev.rows.empty()))
                {
                    _searchRows = ev.rows;
                }
            }
            switch (ev.kind)
            {
            case SearchBridgeEvent::Kind::Searching:
                break; // quiet: the final answer carries the rows
            case SearchBridgeEvent::Kind::Shown:
                break;
            case SearchBridgeEvent::Kind::Answer:
                _postSearchResults(ev);
                break;
            case SearchBridgeEvent::Kind::Failed:
                _reply(thread, L"\x26A0 " + TgEscapeHtml(ev.text.empty() ? L"the search failed" : ev.text), true);
                break;
            }
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_onSearchEvent");
        }
    }

    void MessagingService::_postSearchResults(const SearchBridgeEvent& ev)
    {
        int64_t thread = 0;
        std::vector<SearchRow> rows;
        {
            std::lock_guard lk{ _mtx };
            thread = _state.searchThreadId;
            rows = _searchRows;
        }
        std::wstring html = ev.text.empty() ? std::wstring{} : TgEscapeHtml(ev.text);
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> buttons;
        size_t n = 0;
        for (const auto& row : rows)
        {
            if (n >= 10)
            {
                break;
            }
            ++n;
            const auto s = _registry->Get(row.sessionId);
            std::wstring state;
            std::vector<std::pair<std::wstring, std::wstring>> line;
            const std::wstring payload = row.dir + L"\n" + row.title;
            if (s && s->live && !s->tabToken.empty())
            {
                if (s->state == SessionState::Idle && !s->pinned)
                {
                    state = L"idle tab";
                    line.push_back({ L"Wake " + std::to_wstring(n), _tokenFor(L"wake", row.sessionId, L"", kTokenTtlMsFor(2)) });
                    line.push_back({ L"Wake \U0001F4CC " + std::to_wstring(n), _tokenFor(L"wake-pin", row.sessionId, L"", kTokenTtlMsFor(2)) });
                }
                else
                {
                    state = L"has a topic";
                }
            }
            else
            {
                state = L"closed";
                line.push_back({ L"Resume " + std::to_wstring(n), _tokenFor(L"resume", row.sessionId, payload, kTokenTtlMsFor(2)) });
                line.push_back({ L"Resume \U0001F4CC " + std::to_wstring(n), _tokenFor(L"resume-pin", row.sessionId, payload, kTokenTtlMsFor(2)) });
                line.push_back({ L"Fork " + std::to_wstring(n), _tokenFor(L"fork", row.sessionId, payload, kTokenTtlMsFor(2)) });
            }
            html += L"\n<b>" + std::to_wstring(n) + L".</b> " + TgEscapeHtml(row.title.empty() ? ShortId(row.sessionId) : row.title) + L" <i>(" + state + L")</i>";
            if (!row.dir.empty())
            {
                html += L"\n    " + TgEscapeHtml(detail::ShortPath(row.dir, 2));
            }
            if (row.lastActivityMs > 0)
            {
                html += L" \x00B7 " + TgShortSpan(WallMs() - row.lastActivityMs) + L" ago";
            }
            if (!line.empty())
            {
                buttons.push_back(std::move(line));
            }
        }
        if (rows.empty() && html.empty())
        {
            html = L"Nothing found.";
        }
        _reply(thread, html, true, buttons.empty() ? json::Value{} : _buttons(buttons));
    }

    // ===== buttons ===========================================================================

    void MessagingService::_handleCallback(const json::Value& cq)
    {
        int64_t from = 0;
        if (const auto* f = cq.Find(L"from"); f && f->type == json::Value::Type::Obj)
        {
            from = f->I64At(L"id", 0);
        }
        const std::wstring cqId = cq.StrAt(L"id");
        const auto answer = [&](const std::wstring& text) {
            OutItem item;
            item.prio = SendPriority::Final;
            item.method = L"answerCallbackQuery";
            item.paced = false;
            item.params = json::Value::MkObj();
            item.params.Set(L"callback_query_id", json::Value::MkStr(cqId));
            if (!text.empty())
            {
                item.params.Set(L"text", json::Value::MkStr(text.size() > 190 ? text.substr(0, 190) : text));
            }
            _enqueue(std::move(item));
        };
        if (from == 0 || from != _ownerId)
        {
            answer(L"This bot is private.");
            return;
        }
        const std::wstring data = cq.StrAt(L"data");
        int64_t threadId = 0;
        if (const auto* m = cq.Find(L"message"); m && m->type == json::Value::Type::Obj)
        {
            threadId = m->I64At(L"message_thread_id", 0);
        }
        if (data.rfind(L"c:", 0) == 0)
        {
            // A card verb: "c:<verb>:<session id>", acting on the session's CURRENT state.
            const size_t sep = data.find(L':', 2);
            if (sep == std::wstring::npos)
            {
                answer(L"");
                return;
            }
            const std::wstring verb = data.substr(2, sep - 2);
            const std::wstring sid = data.substr(sep + 1);
            const auto s = _registry->Get(sid);
            if (!s)
            {
                answer(L"Unknown session.");
                return;
            }
            RemoteCommand cmd;
            cmd.kind = RemoteKind::AgentmasterCommand;
            json::Value fake = json::Value::MkObj();
            if (verb == L"runner")
            {
                cmd.name = L"runner";
                cmd.args = s->autorunner.mode == AutorunnerMode::Full ? L"paused" : L"auto";
            }
            else if (verb == L"pin")
            {
                cmd.name = s->pinned ? L"unpin" : L"pin";
            }
            else if (verb == L"queue" || verb == L"transcript" || verb == L"screen" || verb == L"sendnow")
            {
                cmd.name = verb;
            }
            else if (verb == L"reactivate")
            {
                // The outcome as the button's toast: nothing is posted in the thread, which is the conversation (§21.6).
                answer(_reactivate(sid));
                return;
            }
            else
            {
                answer(L"");
                return;
            }
            answer(L"");
            _runSessionCommand(sid, cmd, fake, threadId);
            return;
        }
        if (data.rfind(L"t:", 0) == 0)
        {
            Token tok;
            {
                std::lock_guard lk{ _mtx };
                const auto it = _tokens.find(data);
                if (it == _tokens.end())
                {
                    answer(L"That button expired.");
                    return;
                }
                tok = it->second;
                _tokens.erase(it); // single use (M5)
            }
            if (tok.expiresMs < WallMs())
            {
                answer(L"That button expired.");
                return;
            }
            answer(L"");
            const std::wstring& k = tok.kind;
            if (k == L"cancel")
            {
                _reply(threadId, L"Cancelled.", false);
            }
            else if (k == L"backlog-drop")
            {
                _reply(threadId, L"Dropped.", false);
            }
            else if (k == L"backlog-queue")
            {
                const size_t bar = tok.payload.find(L'|');
                int64_t msgId = 0;
                try
                {
                    msgId = std::stoll(tok.payload.substr(0, bar));
                }
                catch (...)
                {
                    msgId = 0;
                }
                _queueRemotePrompt(tok.sessionId, bar == std::wstring::npos ? std::wstring{} : tok.payload.substr(bar + 1), msgId, threadId, true);
            }
            else if (k == L"unqueue")
            {
                bool removed = false;
                _registry->Update(tok.sessionId, [&](SessionInfo& ss) {
                    for (auto it = ss.queue.begin(); it != ss.queue.end(); ++it)
                    {
                        if (it->id == tok.payload && it->status == PromptStatus::Pending)
                        {
                            ss.queue.erase(it);
                            removed = true;
                            break;
                        }
                    }
                });
                _reply(threadId, removed ? L"Removed from the queue." : L"It was already sent.", false);
            }
            else if (k == L"confirm-restartsession")
            {
                if (_hooks.restartSession)
                {
                    _hooks.restartSession(tok.sessionId);
                }
                _reply(threadId, L"Restarting the session.", false);
                LogNav(L"remote telegram /restartsession " + ShortId(tok.sessionId) + L" -> confirmed");
            }
            else if (k == L"confirm-deactivate")
            {
                if (_hooks.deactivateSession)
                {
                    _hooks.deactivateSession(tok.sessionId);
                }
                _reply(threadId, L"Parked.", false);
                LogNav(L"remote telegram /deactivate " + ShortId(tok.sessionId) + L" -> confirmed");
            }
            else if (k == L"confirm-interrupt")
            {
                // One Esc into the session (the typed path; a linked session will use the link's abort).
                const bool ok = _registry->Inject(tok.sessionId, L"\x1b");
                _reply(threadId, ok ? L"\x25A0 Interrupt sent." : L"Could not reach the session (no input bound).", false);
                LogNav(L"remote telegram /interrupt " + ShortId(tok.sessionId) + (ok ? L" -> sent" : L" -> refused:no-injector"));
            }
            else if (k == L"wake" || k == L"wake-pin")
            {
                _wake(tok.sessionId, k == L"wake-pin");
                _reply(threadId, L"Woken: its topic appears in a moment.", false);
            }
            else if (k == L"resume" || k == L"resume-pin" || k == L"fork")
            {
                RemoteLaunchRequest req;
                req.kind = k == L"fork" ? RemoteLaunchRequest::Kind::Fork : RemoteLaunchRequest::Kind::Resume;
                req.sessionId = tok.sessionId;
                const size_t nl = tok.payload.find(L'\n');
                req.dir = nl == std::wstring::npos ? tok.payload : tok.payload.substr(0, nl);
                req.title = nl == std::wstring::npos ? std::wstring{} : tok.payload.substr(nl + 1);
                req.pin = k == L"resume-pin";
                _requestLaunch(req, threadId);
            }
            return;
        }
        answer(L"");
    }

    std::wstring MessagingService::_tokenFor(std::wstring kind, std::wstring sessionId, std::wstring payload, int64_t ttlMs)
    {
        std::lock_guard lk{ _mtx };
        Token t;
        t.kind = std::move(kind);
        t.sessionId = std::move(sessionId);
        t.payload = std::move(payload);
        t.expiresMs = WallMs() + ttlMs;
        const std::wstring id = L"t:" + MakeTokenId(++_tokenSeq);
        _tokens[id] = std::move(t);
        return id;
    }

    void MessagingService::_sweepTokens()
    {
        std::lock_guard lk{ _mtx };
        const int64_t now = WallMs();
        for (auto it = _tokens.begin(); it != _tokens.end();)
        {
            it = it->second.expiresMs < now ? _tokens.erase(it) : std::next(it);
        }
    }

    // ===== actions ===========================================================================

    void MessagingService::_sendnow(const std::wstring& sessionId, int64_t threadId)
    {
        // The Manager's Send-now recipe: mark the first Pending prompt Sent, then the ONE send seam.
        std::wstring text;
        std::wstring promptId;
        _registry->Update(sessionId, [&](SessionInfo& s) {
            for (auto& p : s.queue)
            {
                if (p.status == PromptStatus::Pending)
                {
                    text = p.text;
                    promptId = p.id;
                    p.status = PromptStatus::Sent;
                    p.attempts += 1;
                    p.sentAtUnixMs = WallMs();
                    p.echoed = false;
                    p.enterRetries = 0;
                    p.injectedAtUnixMs = 0;
                    break;
                }
            }
        });
        if (text.empty())
        {
            _reply(threadId, L"Nothing queued.", false);
            return;
        }
        const bool delivered = _registry->SubmitPrompt({ sessionId, promptId, text, false });
        if (!delivered)
        {
            _registry->RollbackPromptToPending(sessionId, promptId, false);
            _reply(threadId, L"Not sent: the session's input is not reachable right now (dormant, or a delivery in flight). It stays queued.", false);
        }
        else
        {
            _reply(threadId, L"Sending: " + TgEscapeHtml(TgFirstLine(text, 100)), false);
        }
        LogNav(L"remote telegram send-now " + ShortId(sessionId) + (delivered ? L" -> sent" : L" -> refused"));
    }

    void MessagingService::_wake(const std::wstring& sessionId, bool pin)
    {
        _registry->Update(sessionId, [&](SessionInfo& s) {
            if (pin)
            {
                s.pinned = true;
            }
            if (s.state == SessionState::Idle || s.state == SessionState::Done)
            {
                s.state = SessionState::WaitingForInput;
                s.manualUnread = false;
                s.lastActivityUnixMs = WallMs(); // the Waiting-for-you window restarts (never instant-decays)
                s.readUnixMs = 0; // unread for this "turn", like a real turn-complete
            }
        });
        LogNav(L"remote telegram wake " + ShortId(sessionId) + (pin ? L" (pinned)" : L""));
    }

    void MessagingService::_setPinned(const std::wstring& sessionId, bool pinned)
    {
        _registry->Update(sessionId, [&](SessionInfo& s) {
            s.pinned = pinned;
            if (pinned && s.live && (s.state == SessionState::Idle || s.state == SessionState::Done))
            {
                s.state = SessionState::WaitingForInput;
                s.lastActivityUnixMs = WallMs();
                s.readUnixMs = 0;
            }
        });
        LogNav(L"remote telegram " + std::wstring{ pinned ? L"pin " : L"unpin " } + ShortId(sessionId));
        _refreshCard(sessionId, true);
    }

    void MessagingService::_requestLaunch(RemoteLaunchRequest req, int64_t threadId)
    {
        if (!_hooks.launch)
        {
            _reply(threadId, L"Opening sessions is not available in this build.", false);
            return;
        }
        if (req.kind != RemoteLaunchRequest::Kind::NewSession && req.dir.empty())
        {
            // The Search row carried no folder: the registry's record first, else the transcript's own cwd
            // (its project folder's name is lossy; the lines are exact) — a cheap head read.
            if (const auto s = _registry->Get(req.sessionId); s && !s->workingDir.empty())
            {
                req.dir = s->workingDir;
            }
            else
            {
                req.dir = TranscriptCwd(ResolveClaudeTranscriptPath(req.sessionId));
            }
        }
        if (req.kind != RemoteLaunchRequest::Kind::NewSession && req.dir.empty())
        {
            _reply(threadId, L"The session's folder is not known; resume it from the Sessions page at the PC.", false);
            return;
        }
        bool ok = false;
        try
        {
            ok = _hooks.launch(req);
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_requestLaunch");
        }
        if (!ok)
        {
            _reply(threadId, L"No window could open it right now.", false);
            return;
        }
        {
            std::lock_guard lk{ _mtx };
            Revive rv;
            rv.req = req;
            rv.requestedMs = WallMs();
            rv.threadId = threadId;
            _revives.push_back(std::move(rv));
        }
        const wchar_t* what = req.kind == RemoteLaunchRequest::Kind::Fork ? L"Forking" : (req.kind == RemoteLaunchRequest::Kind::Resume ? L"Resuming" : L"Opening a new session");
        _reply(threadId, std::wstring{ what } + L" into a tab\x2026 its topic appears once it is up.", false);
        LogNav(L"remote telegram launch " + std::wstring{ what } + L" " + ShortId(req.sessionId) + L" dir=" + req.dir + (req.pin ? L" (pin)" : L""));
    }

    void MessagingService::_sweepRevives(const SessionInfo& s)
    {
        // A revive is "done" when its session is LIVE: pin it when asked, hold it in Waiting for you so its
        // topic appears, and forget the revive. A New-session revive matches a live session created in its
        // folder after the request. Revives older than 3 minutes are dropped.
        std::vector<Revive> matched;
        {
            std::lock_guard lk{ _mtx };
            const int64_t now = WallMs();
            for (auto it = _revives.begin(); it != _revives.end();)
            {
                bool match = false;
                if (!it->req.sessionId.empty())
                {
                    // Resume: the same id. Fork: a live session whose forkParentId is the source (or the
                    // source itself when the fork degraded to a resume).
                    match = s.live && (s.id == it->req.sessionId || (it->req.kind == RemoteLaunchRequest::Kind::Fork && s.forkParentId == it->req.sessionId));
                }
                else
                {
                    match = s.live && s.stateChangedAtUnixMs >= it->requestedMs - 2000 && NormDirKey(s.workingDir) == NormDirKey(it->req.dir) && s.kind == AgentKind::Claude;
                }
                if (match)
                {
                    matched.push_back(*it);
                    it = _revives.erase(it);
                }
                else if (now - it->requestedMs > 3 * 60 * 1000)
                {
                    it = _revives.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        for (const auto& rv : matched)
        {
            _registry->Update(s.id, [&](SessionInfo& ss) {
                if (rv.req.pin)
                {
                    ss.pinned = true;
                }
                if (ss.state == SessionState::Idle || ss.state == SessionState::Done)
                {
                    ss.state = SessionState::WaitingForInput;
                    ss.lastActivityUnixMs = WallMs();
                    ss.readUnixMs = 0;
                }
            });
            _reply(rv.threadId, L"Opened <b>" + TgEscapeHtml(s.title.empty() ? ShortId(s.id) : s.title) + L"</b> \x2014 its topic is coming up." + (rv.req.pin ? L" \U0001F4CC" : L""), false);
            _log(L"[msg] revive done " + ShortId(s.id));
        }
    }

    // ===== replies ===========================================================================

    void MessagingService::_reply(int64_t threadId, const std::wstring& html, bool sound, json::Value markup, int64_t replyTo)
    {
        _sendText(threadId, html, !sound, SendPriority::Final, L"", std::move(markup), nullptr, replyTo);
    }

    void MessagingService::_sendHelp(int64_t threadId, bool searchTopic)
    {
        std::wstring html;
        if (searchTopic)
        {
            html = L"<b>\U0001F50E Search</b>\nWrite what you look for in plain words; the agent answers with sessions and buttons to wake / resume / fork them.\n"
                   L"/idle \x2014 the idle tabs\n/board \x2014 counts per state\n/newsession &lt;folder&gt; [model]\n/pauseall \x00B7 /resumeall\n/status\n/reset \x2014 forget the search conversation";
        }
        else
        {
            html = L"<b>In a session's topic</b>\nAny text is queued as the session's next prompt (never typed into a running turn). Claude Code commands (/compact, /model x, your skills) are queued the same way; commands that leave no record are refused.\n"
                   L"/queue \x00B7 /sendnow \x00B7 /unqueue n\n/runner auto|paused\n/pin \x00B7 /unpin \x2014 never idle\n/wake \x00B7 /moveidle \x00B7 /markunread\n/rename &lt;title&gt;\n/favorite \x00B7 /tags \x00B7 /tag x \x00B7 /untag x\n"
                   L"/sessionid \x00B7 /path \x00B7 /gitbranch \x00B7 /draft\n/summary \x00B7 /transcript [level] \x00B7 /screen \x00B7 /status\n/activate \x00B7 /restartsession \x00B7 /deactivate \x00B7 /interrupt\n/forksession [model] \x00B7 /newsession [model]\n/takedraft \x2014 queue the unsent draft at the PC\n/cc /x \x2014 force /x through to Claude Code";
        }
        _reply(threadId, html, false);
    }
}
