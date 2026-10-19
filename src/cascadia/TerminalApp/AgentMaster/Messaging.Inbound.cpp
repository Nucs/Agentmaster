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
#include <tuple> // the /idle list's (id, title, color square) rows (§21.13)

#include "ClaudeSpawn.h" // LogNav, ShortId, ResolveClaudeTranscriptPath, NewSessionId, LogSwallowedException
#include "FolderProbe.h" // §21.17.4: SharedFolderProber — the poll thread's folder checks wait a bounded time
#include "Persistence.h" // DeriveSessionTitle
#include "ProcessInspect.h" // AnalyzeSessionTranscript, ReadGitBranchForDir
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8
#include "Scheduler.h"
#include "SessionRegistry.h"
#include "SessionStore.h" // favorites, tags
#include "TranscriptTurns.h" // RenderTranscriptTurnsFile
#include "ScreenImage.h" // §21.24: RenderScreenSnapshotPng + ParseScreenSnapshot (the card's 🖼 Screen Image)

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

        // How many listed Search rows the service remembers for the picks (MessagingService::_searchRowInfo): about twenty
        // answers of ten rows. A pick of a row that fell out (or of an answer from before a restart) still works: its
        // menu then reads the registry's record, else the short id.
        constexpr size_t kSearchRowMemory = 200;

        // How long a pick menu's action buttons live: the action tokens' lifetime (the pick itself never expires, so a
        // tap on it opens a fresh menu).
        constexpr int kSearchActionTtlHours = 2;

        // §21.16.7: a second tap on the same directory dot within this window only answers. The first tap's re-post is queued
        // or just sent, and the topic is first already; another would cost two more calls of the chat's flood budget (~20 a
        // minute) for nothing. Longer than a double tap and the tick's few seconds, short enough that a later tap still works.
        constexpr int64_t kFrontTapGapMs = 10 * 1000;
        // §21.24: at most this many screen pictures are drawn at once (each one a worker thread and ~90 to ~185 ms of Direct2D
        // drawing, measured for a 200 x 60 to 300 x 80 screen); a request past it is told to ask again.
        constexpr int kScreenImageMaxRenders = 2;
        // §21.24: a second picture of the same session within this window asks nothing new - a double tap must not draw and
        // send two photos on the chat's ~20-calls-a-minute budget. Short enough that a deliberate "again" a few seconds later works.
        constexpr int64_t kScreenImageTapGapMs = 3 * 1000;
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

    void MessagingService::_handleBusinessConnection(const json::Value& bc, bool viaSecretary)
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
        // Which bot this update came to (§21.11): the SECRETARY bot (a second bot), or the topics bot itself.
        const int64_t thisBot = viaSecretary ? _secretaryBotId.load() : _botId.load();
        const wchar_t* who = viaSecretary ? L"the secretary bot" : L"the topics bot";
        bool ignored = false;
        {
            std::lock_guard lk{ _mtx };
            // Telegram allows ONE business bot per account: moving the connection from one of our bots to the other sends
            // the old one a disconnect and the new one a connect, in any order. A disconnect of the bot that does not hold
            // the stored connection must not wipe the new one.
            const bool holdsIt = _state.businessConnectionId.empty() || _state.businessConnectionId == id || _state.businessBotId == thisBot ||
                                 (_state.businessBotId == 0 && !viaSecretary);
            if (!enabled && !holdsIt)
            {
                ignored = true;
            }
            else
            {
                _state.businessConnectionId = id;
                _state.businessEnabled = enabled;
                _state.businessCanReply = canReply;
                _state.businessBotId = thisBot;
                _state.businessMode = 0; // every (re)connection is probed afresh
                _state.businessNote.clear();
                _stateDirty = true;
            }
        }
        if (ignored)
        {
            _log(std::wstring{ L"[msg] secretary: " } + who + L" was disconnected; the connection stays with the other bot");
            return;
        }
        _secretaryFailures.store(0);
        _log(std::wstring{ L"[msg] secretary: business connection to " } + who + L" " + (enabled ? L"enabled" : L"disabled") + L" (can reply: " + (canReply ? L"yes" : L"no") + L")");
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
                      enabled ? L"\x26A0 <b>Secretary mode</b>: the bot is connected to your account, but without the right to reply. In Telegram \x2192 Settings \x2192 <b>Chat Automation</b> \x2192 the bot, allow it to <b>reply to messages</b>; prompts keep the \x2328 line until then." :
                                L"<b>Secretary mode</b> is off (the bot was disconnected under Settings \x2192 Chat Automation): prompts typed at the PC show as the bot's \x2328 line again.",
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
                              L"\x26A0 <b>Secretary mode</b>: the bot is connected to your account, but Telegram refused to send into this chat on your behalf (<code>" + TgEscapeHtml(r.description) + L"</code>). Prompts keep the \x2328 line.\n\n"
                              L"Telegram documents a connected bot sending to your chats with OTHER bots \x2014 so the way that works is a <b>second bot</b> as the secretary: create one in @BotFather, turn on its <b>Secretary Mode</b> and <b>Bot-to-Bot Communication</b>, paste its token in Agentmaster \x2192 Settings \x2192 Messaging \x2192 Secretary bot, and connect IT under Telegram \x2192 Settings \x2192 Chat Automation instead of this bot.",
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
            // §21.28: a tapped ▶ Reactivate / 🔧 Resume / 🔄 Restart stays busy until the card shows the started tab (its button goes);
            // nothing without a tap (a typed /reactivate is answered by its reply).
            _awaitCardChange(sessionId);
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
            _awaitCardChange(sessionId); // §21.28: until the card shows the restarted session (its next state)
            return L"\x25B6 Restarting it \x2014 the same conversation resumes in its tab.";
        }
        LogNav(L"remote telegram reactivate " + ShortId(sessionId) + L" -> already running");
        return std::wstring{ L"It is running already (" } + StateLabel(s->state) + L").";
    }

    std::wstring MessagingService::_bringTopicToFront(int64_t threadId)
    {
        const int64_t now = WallMs();
        std::wstring sessionId;
        bool historyPending = false;
        bool repeated = false;
        // §21.28: the dot stays busy until the re-post has made the topic first in the list — the re-post carries the tap
        // (CardState::tapHolds). Taken before the lock (it allocates); unused, it goes with this function.
        std::vector<TapHoldRef> taps = _holdTaps();
        {
            std::lock_guard lk{ _mtx };
            // By THREAD: the dot was drawn with the topic's thread id, which survives a rekey of the topic to the tab's next
            // conversation (a /clear or a /resume), while the session id it was drawn for may not.
            for (const auto& [sid, t] : _state.topics)
            {
                if (t.threadId == threadId)
                {
                    sessionId = sid;
                    historyPending = TopicBackfillPending(t);
                    break;
                }
            }
            if (!sessionId.empty())
            {
                auto& lastTap = _frontTapMs[threadId];
                repeated = lastTap > 0 && now - lastTap < kFrontTapGapMs;
                if (!repeated)
                {
                    lastTap = now;
                    // The next per-topic pass re-posts the card SILENTLY (_refreshCard: the flag is kept until a post can carry
                    // it, so a card post in flight or a history replay only delays it), and the tick folds the request into
                    // _frontHeld at its re-sort pass, so the quiet re-sort leaves the topic first.
                    _cards[sessionId].repostWanted = true;
                    if (!historyPending)
                    {
                        // §21.28: a topic still replaying its history moves only once the replay is in (minutes), so its dot is
                        // answered at once, its toast saying so; any other dot waits for its re-post.
                        MoveTapHolds(taps, _cards[sessionId].tapHolds);
                    }
                    _frontRequests[sessionId] = now;
                    _tickRequested = true;
                }
            }
        }
        if (sessionId.empty())
        {
            // The directory is edited at most every two minutes, so a dot can outlive its topic for a moment: the tab closed or
            // went idle past its grace (its topic was deleted), or the topics were made again (new thread ids).
            LogNav(L"remote telegram front thread " + std::to_wstring(threadId) + L" -> no topic");
            return L"That thread is gone: its tab closed or went idle. The list follows within two minutes.";
        }
        const auto s = _session(sessionId);
        const std::wstring answerText = DirectoryFrontAnswer(s ? s->title : std::wstring{}, historyPending);
        if (repeated)
        {
            return answerText; // a double tap: the first tap's re-post is on its way, so nothing new is queued
        }
        _tickCv.notify_all(); // the re-post goes at the next tick, not at the end of the tick's wait (kTickMs)
        _log(L"[msg] " + ShortId(sessionId) + L" put first in the thread list (a directory dot)" +
             (historyPending ? std::wstring{ L"; its card moves once its history replay is in" } : std::wstring{}));
        LogNav(L"remote telegram front " + ShortId(sessionId));
        return answerText;
    }

    bool MessagingService::_jumpToSearchTopic(int64_t fromThreadId, int64_t messageId)
    {
        int64_t search = 0;
        {
            std::lock_guard lk{ _mtx };
            search = _state.searchThreadId;
        }
        if (search == 0)
        {
            // No Search topic to go to: the first connect is still making it, or it is being made again after the user deleted
            // it (§21.18). The answer stays where the command was typed, and so does the command (the answer replies to it).
            _reply(fromThreadId, L"The \U0001F50E Search topic is not there yet \x2014 it is being made. Send /search_tab again in a moment.", false, json::Value{}, messageId);
            _log(L"[msg] /search-tab (thread " + std::to_wstring(fromThreadId) + L"): no Search topic yet");
            LogNav(L"remote telegram search-tab -> no search topic yet");
            return false; // the command stays where it was typed, with the answer under it
        }
        // 1. The command goes first: it is a move, not content (_deleteCommandMessage says why it must not stay).
        _deleteCommandMessage(messageId, L"/search-tab");
        // 2. One SILENT line in the Search topic, queued after the delete (the same priority goes in order): it is now the chat's
        //    newest message, so Telegram lists the Search topic FIRST on every app (§21.16.7's rule: a bot chat's topics go by
        //    their last message), and Android's "All" view puts "Continue last thread ›" under it. Silent: the user is at the
        //    phone and asked for it. The quiet re-sort never moves it down: it orders the session topics only (_resortTick).
        _sendText(search,
                  L"\U0001F50E <b>Search</b> \x2014 first in the thread list now (/search_tab). Ask for a session in plain words here, or with /search &lt;words&gt; from any topic; /help lists the rest.",
                  true, SendPriority::Final, L"", json::Value{}, nullptr);
        _log(L"[msg] /search-tab (thread " + std::to_wstring(fromThreadId) + L"): the Search topic (thread " + std::to_wstring(search) + L") is put first in the thread list");
        LogNav(L"remote telegram search-tab from thread " + std::to_wstring(fromThreadId));
        return true; // the command left the thread it was typed in
    }

    bool MessagingService::_relaySearchRequest(int64_t fromThreadId, const json::Value& msg, const std::wstring& words)
    {
        const int64_t messageId = msg.I64At(L"message_id", 0);
        const int64_t dateMs = msg.I64At(L"date", 0) * 1000;
        int64_t search = 0;
        std::wstring fromSessionId; // the session whose topic the command was typed in ("" = the main view, a thread no session owns)
        std::wstring fromSquare; // that topic's color square, as its name shows it (§21.13)
        {
            std::lock_guard lk{ _mtx };
            search = _state.searchThreadId;
            if (fromThreadId != 0)
            {
                for (const auto& [sid, t] : _state.topics)
                {
                    if (t.threadId == fromThreadId)
                    {
                        fromSessionId = sid;
                        fromSquare = t.nameColor;
                        break;
                    }
                }
            }
        }
        if (search == 0)
        {
            // No Search topic to send it to (the first connect is still making it, or it is being made again, §21.18): the
            // answer stays where the command was typed, and so does the command — the user still has the words.
            _reply(fromThreadId, L"The \U0001F50E Search topic is not there yet \x2014 it is being made. Send the /search again in a moment.", false, json::Value{}, messageId);
            _log(L"[msg] /search (thread " + std::to_wstring(fromThreadId) + L"): no Search topic yet");
            LogNav(L"remote telegram search-relay -> no search topic yet");
            return false; // the command stays, with the answer under it
        }
        if (IsBacklogUpdate(dateMs, WallMs()))
        {
            // D2, the Search topic's own rule for a request: a search sent while Agentmaster was closed is answered, never run
            // unasked. The command stays, so the user sees what the answer is about.
            _reply(fromThreadId, L"(sent " + TgShortSpan(WallMs() - dateMs) + L" ago, while Agentmaster was closed \x2014 send the /search again if you still want it)", false,
                   json::Value{}, messageId);
            LogNav(L"remote telegram search-relay -> backlog");
            return false; // the command stays, with the answer under it
        }
        // 1. The command leaves the thread it was typed in (_deleteCommandMessage), so a session's topic is not put back on top.
        _deleteCommandMessage(messageId, L"/search");
        // 2. The request in the Search topic as the user's line, naming the session's topic it came from (a link back to it):
        //    the Search topic then reads like a request typed there, and the answer below it makes sense. Silent, like the
        //    "Searching…" after it; the answer rings (D5). The words are the user's: escaped, never markup (UserLineBody).
        std::wstring fromRow;
        if (!fromSessionId.empty())
        {
            const auto s = _session(fromSessionId);
            fromRow = UserLineTitleRow(s ? s->title : std::wstring{}, fromSessionId, fromSquare, fromThreadId);
        }
        _sendText(search, BuildRelayedSearchLineHtml(dateMs, words, fromRow, fromThreadId == 0), true, SendPriority::Final, L"", json::Value{}, nullptr);
        _log(L"[msg] /search (thread " + std::to_wstring(fromThreadId) + L"): " + std::to_wstring(words.size()) + L" chars sent to the Search topic (thread " +
             std::to_wstring(search) + L")");
        LogNav(L"remote telegram search-relay from thread " + std::to_wstring(fromThreadId) + L" chars=" + std::to_wstring(words.size()));
        // 3. Asked of the Search agent IN the Search topic: its "Searching…" and its answer go there (_postSearchResults).
        _askSearch(words, search);
        return true; // the command left the thread it was typed in
    }

    void MessagingService::_deleteCommandMessage(int64_t messageId, const wchar_t* what)
    {
        if (messageId <= 0)
        {
            return; // no id to delete (a caller that had none)
        }
        OutItem del;
        del.prio = SendPriority::Final;
        del.method = L"deleteMessage";
        del.params = json::Value::MkObj();
        del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        auto self = weak_from_this();
        const std::wstring name{ what ? what : L"a command" };
        del.onDone = [self, messageId, name](const TgResult& r) {
            if (auto me = self.lock(); me && !r.ok)
            {
                me->_log(L"[msg] " + name + L": the command message " + std::to_wstring(messageId) + L" could not be deleted (" + r.description + L"); it stays where it was typed");
            }
        };
        _enqueue(std::move(del));
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
            // foreign sender the line below is about — except the "topic renamed" notice of a state glyph rename
            // (§21.11), which is deleted: every state change would otherwise leave a line in the topic. Three refused
            // deletes in a row stop the deletes (they would only cost calls).
            if (msg.Find(L"forum_topic_edited") && _renameNoticeFailures.load() < 3)
            {
                const int64_t noticeId = msg.I64At(L"message_id", 0);
                if (noticeId > 0)
                {
                    OutItem del;
                    del.prio = SendPriority::Backfill;
                    del.method = L"deleteMessage";
                    del.params = json::Value::MkObj();
                    del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
                    del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(noticeId)));
                    auto self = weak_from_this();
                    del.onDone = [self](const TgResult& r) {
                        auto me = self.lock();
                        if (!me)
                        {
                            return;
                        }
                        if (r.ok)
                        {
                            me->_renameNoticeFailures.store(0);
                            return;
                        }
                        if (me->_renameNoticeFailures.fetch_add(1) + 1 == 3)
                        {
                            me->_log(L"[msg] the \u201Ctopic renamed\u201D notices cannot be deleted (" + r.description + L"); they are left in the topics from now on");
                        }
                    };
                    _enqueue(std::move(del));
                }
            }
            // §21.16: the note of the DIRECTORY's pin ("… pinned «🔵 5 running …»") is deleted too — the directory is a fixed
            // part of the chat's main view, and its note would be the line the user reads above every new message there. Only
            // the directory's: the topic cards are no longer pinned (§21.5), and a pin the user makes leaves no bot note.
            if (const auto* pinned = msg.Find(L"pinned_message"); pinned && pinned->type == json::Value::Type::Obj)
            {
                int64_t directory = 0;
                {
                    std::lock_guard lk{ _mtx };
                    directory = _state.directoryMessageId;
                }
                const int64_t noteId = msg.I64At(L"message_id", 0);
                if (directory > 0 && noteId > 0 && pinned->I64At(L"message_id", 0) == directory)
                {
                    OutItem del;
                    del.prio = SendPriority::Backfill;
                    del.method = L"deleteMessage";
                    del.params = json::Value::MkObj();
                    del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
                    del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(noteId)));
                    auto self = weak_from_this();
                    del.onDone = [self](const TgResult& r) {
                        if (auto me = self.lock(); me && !r.ok)
                        {
                            me->_log(L"[msg] directory: the note of its pin could not be deleted (" + r.description + L"); it stays in the chat");
                        }
                    };
                    _enqueue(std::move(del));
                }
            }
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
            // §21.12: it still names THIS bot's id of the user's bubble — the turn's status reaction and the reply's quote
            // need it when the bubble is the topic's current prompt.
            _notePromptEcho(msg.I64At(L"message_thread_id", 0), msg.StrAt(L"text"), msg.I64At(L"message_id", 0));
            return;
        }
        const int64_t threadId = msg.I64At(L"message_thread_id", 0);
        int64_t searchThread = 0;
        std::wstring sessionId;
        // §21.17: the thread is a session's topic AND still has its new-thread bind — the tick is binding it right now and hands
        // the bind's messages over (_adoptBoundThreads). A message then joins the bind behind the ones it holds, so the session
        // gets them in the order the user wrote them; sent straight to the topic, it would be queued ahead of them.
        bool bindHandingOver = false;
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
            if (!sessionId.empty())
            {
                bindHandingOver = std::any_of(_threadBinds.begin(), _threadBinds.end(), [threadId](const ThreadBind& b) { return b.threadId == threadId; });
            }
        }
        // Service messages (a topic created by the user, a pinned-message note) carry no text.
        if (msg.Find(L"forum_topic_created"))
        {
            // §21.17: a thread the user opened starts a NEW SESSION — its folder picker goes up at once (before §21.17 the bot
            // answered that topics are made from the tabs).
            if (threadId != 0 && threadId != searchThread && sessionId.empty())
            {
                _onUserThreadCreated(threadId, msg);
            }
            return;
        }
        // §21.17: the user RENAMED a session's topic — the session takes the name as its title (the bot's own renames come back
        // with the bot as the sender, handled above). An edit of the icon alone carries no name and changes nothing.
        if (const auto* editedTopic = msg.Find(L"forum_topic_edited"); editedTopic && editedTopic->type == json::Value::Type::Obj)
        {
            if (const auto* newName = editedTopic->Find(L"name"); newName && newName->type == json::Value::Type::Str)
            {
                if (!sessionId.empty())
                {
                    _onUserTopicRenamed(sessionId, threadId, newName->str);
                }
                else if (threadId != 0 && threadId != searchThread)
                {
                    // §21.25: a thread still in its wizard, or whose launch runs — the name is the coming session's title (the
                    // picker says "rename this thread to change it"; before §21.25 such a rename was ignored and the bind
                    // named the topic back).
                    _onNewThreadRenamed(threadId, newName->str);
                }
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
            _notePromptEcho(threadId, text, msg.I64At(L"message_id", 0)); // §21.12: the current prompt's id in this bot's view
            _log(L"[msg] secretary: our own send as the user came back as a message (thread " + std::to_wstring(threadId) + L"); ignored");
            return;
        }
        // §21.19: /search-tab (and /search_tab, the "/" menu's spelling) jumps to the 🔎 Search topic from ANY thread — the main
        // view, a session's topic, a thread no session owns yet (the §21.17 folder picker never holds it), and Search itself.
        // Read HERE, before a session topic's own handling, which would note the command as the topic's newest post and
        // re-post the topic's sticky card a few seconds later — putting that topic back on top of the list the jump reorders.
        // An EDIT that turns a message into the command does nothing: an edit makes no message the newest.
        // §21.25: a NEW thread whose first and only message is such a command (the user opened a thread to search) is empty once
        // the command left it: the creation never started, and the empty thread is deleted (the user's rule for an aborted
        // creation). A thread with a picker, held messages or a name the user gave it is kept (_abortUntouchedNewThread).
        const bool wizardCandidate = sessionId.empty() && threadId != 0 && threadId != searchThread;
        if (!edited && IsSearchTabCommand(text))
        {
            if (_jumpToSearchTopic(threadId, msg.I64At(L"message_id", 0)) && wizardCandidate)
            {
                _abortUntouchedNewThread(threadId, L"its only message was /search_tab, which went to the Search topic");
            }
            return;
        }
        // §21.19: /search <words> SENDS a search to the Search topic from any other thread — the request is posted there and its
        // answer comes there (_relaySearchRequest) — read here for the same reason as /search-tab. A bare /search has nothing
        // to ask: it is the jump. In the Search topic itself the words are a request like plain words
        // (ClassifySearchTopicMessage), so that one case falls through to the Search topic's own reading below.
        if (!edited)
        {
            if (const std::optional<std::wstring> words = SearchCommandWords(text))
            {
                if (words->empty())
                {
                    if (_jumpToSearchTopic(threadId, msg.I64At(L"message_id", 0)) && wizardCandidate)
                    {
                        _abortUntouchedNewThread(threadId, L"its only message was /search, which went to the Search topic");
                    }
                    return;
                }
                if (searchThread == 0 || threadId != searchThread)
                {
                    if (_relaySearchRequest(threadId, msg, *words) && wizardCandidate)
                    {
                        _abortUntouchedNewThread(threadId, L"its only message was a /search request, which went to the Search topic");
                    }
                    return;
                }
            }
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
                if (bindHandingOver)
                {
                    // §21.17: the message may still sit in the binding thread's bind, not handed over yet — the edit replaces it
                    // there, as in a thread whose pick is launching (_holdForNewThread). Else it is a queued prompt, below.
                    bool replacedHeld = false;
                    {
                        std::lock_guard lk{ _mtx };
                        for (auto& b : _threadBinds)
                        {
                            if (b.threadId != threadId)
                            {
                                continue;
                            }
                            for (auto& h : b.held)
                            {
                                if (h.I64At(L"message_id", 0) == msgId)
                                {
                                    h = msg;
                                    replacedHeld = true;
                                }
                            }
                        }
                    }
                    if (replacedHeld)
                    {
                        return;
                    }
                }
                // §21.30: a message an open question card holds (its "answer or next prompt?" question waits for a tap) takes the
                // new text there.
                if (_questionTextEdited(sessionId, msg))
                {
                    return;
                }
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
            if (bindHandingOver)
            {
                // §21.17: into the bind, behind what it holds; the tick hands it over after them. A bind that is gone by now
                // (its hand-over ended since the look above) leaves _holdForNewThread to send it to the topic itself.
                _holdForNewThread(threadId, msg, false);
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
            _reply(0, L"This is Agentmaster's bot. Each working session has its own topic; the \U0001F50E Search topic finds old ones (/search &lt;words&gt; asks it from anywhere, /search_tab puts it first in the list); a new thread starts a new session (you pick its folder). Send /help for the commands.", false); // §21.17: the new thread; §21.19: /search, /search_tab
            return;
        }
        // §21.17: a thread no session owns — one the user opened, or one whose session closed while the thread stayed — starts a
        // new session: the message waits for the thread's folder picker (before §21.17 the bot answered that the topic belongs to
        // no session any more). A recreation's old topic, about to be deleted, is told so instead.
        bool orphan = false;
        {
            std::lock_guard lk{ _mtx };
            orphan = std::find(_state.orphanThreads.begin(), _state.orphanThreads.end(), threadId) != _state.orphanThreads.end();
        }
        if (orphan)
        {
            _reply(threadId, L"This topic is being made again and goes away in a moment; write in its new topic.", false);
            return;
        }
        _holdForNewThread(threadId, msg, edited);
    }

    void MessagingService::_handleTopicMessage(const std::wstring& sessionId, const json::Value& msg, int64_t threadId, bool held)
    {
        const std::wstring text = msg.StrAt(L"text");
        const int64_t msgId = msg.I64At(L"message_id", 0);
        const int64_t dateMs = msg.I64At(L"date", 0) * 1000;
        // Any action in a topic marks the session READ on the PC (the Waiting -> Idle decay then works for a
        // session handled only from the phone).
        _registry->UpdateQuiet(sessionId, [](SessionInfo& s) { s.readUnixMs = WallMs(); });
        // The user's own message is now the bottom of the topic: the sticky card follows it (§21.5).
        _noteTopicPosted(threadId);
        // §21.30: while the session's QUESTION CARD is open, a plain message is the card's — the answer or the note the user asked
        // to write (✍️ / 📝) or replied to the card with, else asked about (the answer, or the next prompt?). A /command stays a
        // command; a message a new thread held was written before any question.
        if (!held && _takeQuestionText(sessionId, msg, threadId))
        {
            return;
        }

        const RemoteCommand cmd = ParseRemoteCommand(text);
        switch (cmd.kind)
        {
        case RemoteKind::AgentmasterCommand:
            if (cmd.name == kSteerCommand)
            {
                // §21.33: /steer is taken HERE, not in _runSessionCommand, because only this path knows whether a new thread held
                // the message (it is then no backlog, however long the folder pick took). _runSessionCommand keeps a branch for it.
                _steerRemotePrompt(sessionId, cmd.args, msg, threadId, held);
                return;
            }
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
        if (!held && IsBacklogUpdate(dateMs, WallMs()))
        {
            // D2: a message sent while Agentmaster was closed is asked about, never queued silently. §21.17: a message a new
            // thread's picker HELD was read live; however long the folder pick took, it is no backlog.
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

    void MessagingService::_steerRemotePrompt(const std::wstring& sessionId, const std::wstring& words, const json::Value& msg, int64_t threadId, bool held)
    {
        const int64_t msgId = msg.I64At(L"message_id", 0);
        const int64_t dateMs = msg.I64At(L"date", 0) * 1000;
        const auto s = _registry->Get(sessionId);
        // A session the registry no longer knows reads as closed (no open tab): the same answer as a closed one.
        const SteerAction action = s ? DecideSteer(words, s->live, s->started, s->external, s->state, s->pendingBoxState) :
                                       DecideSteer(words, false, false, false, SessionState::Idle, InputBoxState::Unknown);
        const auto nav = [&](const wchar_t* outcome) {
            LogNav(L"remote telegram /steer " + ShortId(sessionId) + L" chars=" + std::to_wstring(words.size()) + L" -> " + outcome);
        };
        if (action == SteerAction::Usage || action == SteerAction::Closed)
        {
            _reply(threadId, SteerReplyHtml(action), false, json::Value{}, msgId);
            nav(action == SteerAction::Usage ? L"usage" : L"refused:closed");
            return;
        }
        if (!held && IsBacklogUpdate(dateMs, WallMs()))
        {
            // D2: a /steer written while Agentmaster was closed was meant for a turn that is over by now. Asked about like a plain
            // message of the backlog: [Queue] queues its words as a plain prompt (the backlog-queue token), [Drop] forgets them.
            const std::wstring age = TgShortSpan(WallMs() - dateMs);
            const std::wstring token = _tokenFor(L"backlog-queue", sessionId, std::to_wstring(msgId) + L"|" + words, kTokenTtlMsFor(2));
            const std::wstring drop = _tokenFor(L"backlog-drop", sessionId, std::to_wstring(msgId), kTokenTtlMsFor(2));
            _reply(threadId,
                   L"This /steer is " + age + L" old (sent while Agentmaster was closed), and the turn it was meant for is over. Queue its message now?",
                   false,
                   _buttons({ { { L"Queue", token }, { L"Drop", drop } } }),
                   msgId);
            nav(L"asked:backlog");
            return;
        }
        const bool sendNow = action == SteerAction::Send;
        const int64_t nowMs = WallMs();
        const QueuedPrompt p = BuildSteerPrompt(NewSessionId(), words, msgId, sendNow, nowMs);
        // The reaction's record and the watch go in BEFORE the prompt reaches the queue: the tick may meet the prompt the moment it
        // is there, and a prompt it meets already taken (Sent and echoed) with no record of it is recorded WITHOUT its 👨‍💻 reaction
        // (_syncReactions' rule for the prompts of an earlier run).
        {
            std::lock_guard lk{ _mtx };
            _reacted[p.id] = PromptStatus::Pending;
            if (sendNow)
            {
                _steers[p.id] = SteerWatch{ sessionId, msgId, threadId, nowMs };
            }
        }
        const bool inserted = _registry->Update(sessionId, [&](SessionInfo& ss) {
            ss.queue.insert(ss.queue.begin() + static_cast<std::ptrdiff_t>(SteerInsertIndex(ss.queue)), p);
        });
        if (!inserted)
        {
            // The session left the registry between the look above and now: nothing was queued, so nothing is watched either.
            {
                std::lock_guard lk{ _mtx };
                _reacted.erase(p.id);
                _steers.erase(p.id);
            }
            _reply(threadId, SteerReplyHtml(SteerAction::Closed), false, json::Value{}, msgId);
            nav(L"refused:closed");
            return;
        }
        if (sendNow)
        {
            _react(msgId, kSteerSendingReaction);
            if (_registry->SubmitPrompt({ sessionId, p.id, p.text, false }))
            {
                // Accepted: the hosting window types the words (its draft swap keeps the user's unsent draft, its verified
                // placement reads the box first) and owns the outcome from here. Their echo turns the reaction to 👨‍💻
                // (_syncReactions); a decline puts them back in the queue, and _sweepSteers says so.
                _log(L"[msg] " + ShortId(sessionId) + L" steered from topic (message " + std::to_wstring(msgId) + L", " + std::to_wstring(p.text.size()) +
                     L" chars): sent now");
                nav(L"sent");
                return;
            }
            // Declined at once: the delivery gate is held (another delivery, or a box clear, owns the input box). The watch goes
            // BEFORE the rollback, so the tick can never tell this a second time; the words wait first in the queue, and the runner
            // sends them at the next ready moment (a remote prompt goes whatever the runner's mode, D30).
            {
                std::lock_guard lk{ _mtx };
                _steers.erase(p.id);
            }
            _registry->RollbackPromptToPending(sessionId, p.id, false);
            _react(msgId, kSteerQueuedReaction);
            _reply(threadId, SteerDeclinedNoticeHtml(SteerDecline::BoxHeld), false, json::Value{}, msgId);
            _log(L"[msg] " + ShortId(sessionId) + L" steer from topic (message " + std::to_wstring(msgId) + L") declined at once (the input box is held): queued first");
            nav(L"queued:box-held");
        }
        else
        {
            // Not sent at once (DecideSteer): first in the queue, and the reply says why.
            _react(msgId, kSteerQueuedReaction);
            _reply(threadId, SteerReplyHtml(action), false, json::Value{}, msgId);
            const wchar_t* outcome = action == SteerAction::QueueDormant ? L"queued:dormant" : (action == SteerAction::QueueEnded ? L"queued:ended" : L"queued:menu");
            _log(L"[msg] " + ShortId(sessionId) + L" steer from topic (message " + std::to_wstring(msgId) + L") not sent at once (" + outcome + L"): queued first");
            nav(outcome);
            if (action == SteerAction::QueueDormant && _hooks.sessionAction)
            {
                // A dormant tab (restored, revived, parked) is started so the words can go, as for a plain message (§7.1).
                try
                {
                    _hooks.sessionAction(sessionId, L"start", L"", nullptr);
                }
                catch (...)
                {
                    // Rule #18. Recovery: the words stay first in the queue; they go when the tab is entered or activated.
                    LogSwallowedException(L"MessagingService::_steerRemotePrompt (start)");
                }
            }
        }
        if (_scheduler)
        {
            _scheduler->RequestAdvance(sessionId);
        }
    }

    void MessagingService::_sweepSteers(const SessionInfo& s)
    {
        // The watches of THIS session, copied out: the outcome needs no lock, and a reply must never be queued under _mtx (_enqueue
        // takes it, and it is not recursive).
        const int64_t nowMs = WallMs();
        std::vector<std::pair<std::wstring, SteerWatch>> watched;
        {
            std::lock_guard lk{ _mtx };
            for (auto it = _steers.begin(); it != _steers.end();)
            {
                if (it->second.sessionId == s.id)
                {
                    watched.emplace_back(it->first, it->second);
                    ++it;
                }
                else if (nowMs - it->second.sinceMs >= kSteerWatchMaxMs)
                {
                    // Another session's watch past its limit: it would only read Expired, and a session whose topic is gone is
                    // swept by no tick any more — so it goes here, and the map never grows.
                    it = _steers.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        if (watched.empty())
        {
            return;
        }
        for (const auto& [promptId, w] : watched)
        {
            const auto q = std::find_if(s.queue.begin(), s.queue.end(), [&](const QueuedPrompt& x) { return x.id == promptId; });
            const SteerOutcome outcome = ClassifySteerWatch(q == s.queue.end() ? nullptr : &*q, w.sinceMs, nowMs);
            if (outcome == SteerOutcome::Watching)
            {
                continue;
            }
            bool stillWatched = false;
            {
                std::lock_guard lk{ _mtx };
                stillWatched = _steers.erase(promptId) > 0;
            }
            if (!stillWatched || outcome != SteerOutcome::BackInQueue)
            {
                // Taken (👨‍💻 says so), resolved (the delivery's own 👎 line), gone or expired: nothing more to tell. A watch another
                // pass already ended is told by nobody twice.
                continue;
            }
            // The hosting window put the words back in the queue: ✍ on the message, and one line that says why.
            _react(w.messageId, kSteerQueuedReaction);
            _reply(w.threadId, SteerDeclinedNoticeHtml(SteerDeclineFor(s.pendingBoxState, s.started, s.external)), false, json::Value{}, w.messageId);
            _log(L"[msg] " + ShortId(s.id) + L" steer from topic (message " + std::to_wstring(w.messageId) + L") came back to the queue: the window declined it");
            LogNav(L"remote telegram /steer " + ShortId(s.id) + L" -> queued:declined");
        }
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
            _cardMarkupFor(sessionId, CardButtonFactsFor(*s), CardPage::Main, markup); // §21.24: a copy starts on Main and flips by itself
            CardExtras extras;
            extras.colorSquare = _titleSquare(&*s); // §21.13: the title with the tab's color, as the tab shows it now
            extras.threadId = _threadOf(sessionId); // §21.17: the title links to the session's topic
            _reply(threadId, BuildCardText(*s, WallMs(), idleLeaves, _scheduler ? _scheduler->GlobalPaused() : false, &extras), false, markup);
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
        if (name == kSteerCommand)
        {
            // §21.33: the belt for a path that reaches a session command without _handleTopicMessage, which takes /steer itself.
            // _steerRemotePrompt logs its own outcome.
            _steerRemotePrompt(sessionId, cmd.args, msg, threadId, false);
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
            // §21.29: the title the session has already, as typed or as the topic PRINTS it (TgOneLineTitle: a multi-line tab title on
            // one line), changes nothing, so no change line will come, and this reply is the command's only answer. The tab's title
            // is left as it is (a multi-line one is not flattened), the rule of a thread rename (_onUserTopicRenamed).
            if (s->title == title || TgOneLineTitle(s->title) == title)
            {
                // §21.15: a chip in a rich reply; §21.17: a link to the session's topic.
                _reply(threadId, L"The title is already <b>" + TgTitleToken(TgTitleForm::Plain, _titleSquare(&*s), title, _threadOf(sessionId)) + L"</b>.", false);
                done(L"unchanged");
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
            // §21.29, the user's rule of 2026-10-19 ("We dont need the first as long as second always sends on a change"): NO reply.
            // It used to answer "Renamed to 🟦 new (the topic follows in a moment).", and the topic's change line ("“🟦 old” changed
            // the title to “🟦 new”", _renameTopic) followed it, two messages for one rename. The change line is the one answer now:
            // the topic sync renames the topic within a tick or two of the registry's new title, and the line follows every change —
            // held while the topic's history is replayed, and the rename made again when its call got no answer.
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
            // §21.13: with the tab's color square; §21.15: a chip when rich; §21.17: a link to the session's topic.
            std::wstring html = L"<b>" + TgTitleToken(TgTitleForm::Plain, _titleSquare(&*s), s->title, _threadOf(sessionId)) + L"</b>\n";
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
            // §21.28: a tapped 🖥 Screen Text stays busy until the screen's text is in the topic: the window's reply carries the tap
            // (its holds go with the reply function: when every window's copy of it is gone).
            std::vector<TapHoldRef> taps = _holdTaps();
            _hooks.sessionAction(sessionId, L"screen", L"", [self, threadId, taps](std::wstring text) {
                if (auto me = self.lock())
                {
                    const TapScope tapScope{ taps }; // the reply below holds the tap until Telegram answered it
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
        if (name == L"screenimage")
        {
            // §21.24: the terminal's screen as a PICTURE - the hosting window reads the live screen with its colors (a snapshot),
            // and _startScreenImage draws it off the UI thread and sends it as a photo.
            if (!_hooks.sessionAction)
            {
                _reply(threadId, L"Not available here.", false);
                return;
            }
            if (!s->live || s->tabToken.empty())
            {
                // No window hosts it, so no window would answer: say so now instead of leaving the request unanswered.
                _reply(threadId, L"This session is not open in a tab, so it has no screen to show.", false);
                done(L"refused:no-tab");
                return;
            }
            {
                std::lock_guard lk{ _mtx };
                const int64_t now = WallMs();
                auto& last = _screenImageTapMs[sessionId];
                if (last > 0 && now - last < kScreenImageTapGapMs)
                {
                    done(L"repeated"); // the first request's picture is on its way
                    return;
                }
                last = now;
            }
            auto self = weak_from_this();
            // §21.28: a tapped 🖼 Screen Image stays busy until the picture is in the topic: the window's reply carries the tap to
            // _startScreenImage, which hands it to the drawing's worker.
            std::vector<TapHoldRef> taps = _holdTaps();
            _hooks.sessionAction(sessionId, L"screen-snapshot", L"", [self, sessionId, threadId, taps](std::wstring snapshot) {
                // The hosting window's UI thread: _startScreenImage only hands the drawing to a worker thread.
                if (auto me = self.lock())
                {
                    const TapScope tapScope{ taps }; // _startScreenImage's own lines and its worker's holds come from it
                    me->_startScreenImage(sessionId, threadId, std::move(snapshot));
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
                _reply(threadId, verb + L" <b>" + TgTitleToken(TgTitleForm::Plain, _titleSquare(&*s), s->title, _threadOf(sessionId)) + L"</b> while a turn is running? The turn is cut off.", false,
                       _buttons({ { { verb + L" now", token }, { L"Cancel", _tokenFor(L"cancel", sessionId, L"", kTokenTtlMsFor(1)) } } }));
                done(L"asked");
                return;
            }
            if (name == L"restartsession")
            {
                if (!s->started && !s->external)
                {
                    // §21.24: a PARKED tab (restored or deactivated, its claude never started) is started in place, as
                    // /reactivate does: the window's restart refuses a connection that never started (_restartPaneConnection's
                    // guard, an AV otherwise), so a restart there did nothing while this reply said it restarted.
                    _reply(threadId, TgEscapeHtml(_reactivate(sessionId)), false);
                }
                else
                {
                    if (_hooks.restartSession)
                    {
                        _hooks.restartSession(sessionId);
                    }
                    _reply(threadId, L"Restarting the session.", false);
                }
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
        if (name == L"verbose")
        {
            _runVerboseCommand(sessionId, cmd.args, threadId);
            return;
        }
        if (name == L"recreate")
        {
            // §21.11: THIS topic again in a mode, or every topic (which makes the mode the default). It deletes messages, so
            // it asks first (single-use buttons, an hour).
            const std::wstring oneOn = _tokenFor(L"recreate-one", sessionId, L"1", kTokenTtlMsFor(1));
            const std::wstring oneOff = _tokenFor(L"recreate-one", sessionId, L"0", kTokenTtlMsFor(1));
            const std::wstring allOn = _tokenFor(L"recreate-all", L"", L"1", kTokenTtlMsFor(1));
            const std::wstring allOff = _tokenFor(L"recreate-all", L"", L"0", kTokenTtlMsFor(1));
            const std::wstring no = _tokenFor(L"cancel", L"", L"", kTokenTtlMsFor(1));
            _reply(threadId,
                   L"Make this topic again with the session's whole history? Its messages in Telegram go with it; the conversation stays in its session.\n\n"
                   L"<b>Verbose</b> shows the work: what Claude says before each tool and the tool calls (Bash commands and more). "
                   L"<b>Conversation</b> shows your messages and each turn's last answer \x2014 you, Claude, you, Claude.\n"
                   L"<b>All topics</b> makes every topic again and the mode the default.",
                   false,
                   _buttons({ { { L"\x267B This topic \u00B7 verbose", oneOn }, { L"\x267B This topic \u00B7 conversation", oneOff } },
                              { { L"\x267B All topics \u00B7 verbose", allOn }, { L"\x267B All topics \u00B7 conversation", allOff } },
                              { { L"Cancel", no } } }));
            done(L"asked");
            return;
        }
        if (detail::InList(name, std::begin(kSearchTabCommands), std::end(kSearchTabCommands)))
        {
            // §21.19: _handleMessage takes /search-tab before a topic's own handling; this is the belt for a path that reaches a
            // session command without it (every name of kAgentmasterCommands has its branch here).
            _jumpToSearchTopic(threadId, msgId);
            done(L"search-tab");
            return;
        }
        if (name == kSearchCommand)
        {
            // §21.19: the same belt for /search (_handleMessage reads it first): words go to the Search topic, a bare one jumps.
            if (cmd.args.empty())
            {
                _jumpToSearchTopic(threadId, msgId);
                done(L"search-tab");
                return;
            }
            _relaySearchRequest(threadId, msg, cmd.args);
            done(L"search-relay");
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
        const int64_t msgId = msg.I64At(L"message_id", 0);
        // The Search topic's own reading (§21.7): /clear and /reset clear THIS conversation, a session's command is
        // refused because this conversation is not a session, and Claude Code's commands have no session here.
        const SearchTopicMessage m = ClassifySearchTopicMessage(text);
        // The backlog rule (D2, M9) for what acts on the conversation: a request or a clear sent while Agentmaster was
        // closed is answered, never carried out unasked.
        const int64_t dateMs = msg.I64At(L"date", 0) * 1000;
        const bool backlog = IsBacklogUpdate(dateMs, WallMs());
        switch (m.verb)
        {
        case SearchTopicVerb::Clear:
            if (backlog)
            {
                _reply(threadId, L"(sent " + TgShortSpan(WallMs() - dateMs) + L" ago, while Agentmaster was closed \x2014 send /clear again to clear the search conversation)", false, json::Value{}, msgId);
                return;
            }
            _clearSearch(threadId, msgId);
            return;
        case SearchTopicVerb::FleetCommand:
            _runSearchCommand(m.cmd, msg, threadId);
            return;
        case SearchTopicVerb::SessionCommand:
            _reply(threadId, L"<code>/" + TgEscapeHtml(m.cmd.name) + L"</code> works in a session's topic. This conversation is not a session: it cannot be woken, resumed, forked or pinned \x2014 it carries on by itself after a restart, and /clear starts it over.", false, json::Value{}, msgId);
            return;
        case SearchTopicVerb::ClaudeCommand:
            _reply(threadId, L"Claude Code commands go in a session's topic. Here: a search in plain words, /clear, or /help.", false, json::Value{}, msgId);
            return;
        case SearchTopicVerb::Request:
        default:
            break;
        }
        if (backlog)
        {
            _reply(threadId, L"(sent " + TgShortSpan(WallMs() - dateMs) + L" ago, while Agentmaster was closed \x2014 ask again if you still want it)", false, json::Value{}, msgId);
            return;
        }
        _askSearch(m.cmd.text, threadId);
    }

    void MessagingService::_runSearchCommand(const RemoteCommand& cmd, const json::Value& msg, int64_t threadId)
    {
        // (The message itself is read only for /search-tab, whose command message is deleted: every other Search command acts
        // on its arguments alone. The parameter stays for the backlog question a later build asks on a command with side effects.)
        const std::wstring& name = cmd.name;
        if (detail::InList(name, std::begin(kSearchTabCommands), std::end(kSearchTabCommands)))
        {
            // §21.19: _handleMessage takes /search-tab first; this belt keeps the Search topic's own reading complete.
            _jumpToSearchTopic(threadId, msg.I64At(L"message_id", 0));
            return;
        }
        if (name == kSearchCommand)
        {
            // §21.19: a bare /search is the jump; ClassifySearchTopicMessage makes "/search <words>" a REQUEST, so words reach
            // here only through a path that bypassed it — they are asked, as a request typed here would be.
            if (cmd.args.empty())
            {
                _jumpToSearchTopic(threadId, msg.I64At(L"message_id", 0));
            }
            else
            {
                _askSearch(cmd.args, threadId);
            }
            return;
        }
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
        if (name == L"recreate")
        {
            // §21.8 / §21.9: make the session topics again in MOST RECENT order, the side panel's, each with its session's
            // whole history. It deletes every session topic and its messages, so it asks first (a single-use button, an hour).
            size_t topics = 0;
            {
                std::lock_guard lk{ _mtx };
                topics = _state.topics.size();
            }
            // §21.11: in a mode — which becomes the default for all topics (the per-topic modes go with the old topics).
            const std::wstring allOn = _tokenFor(L"recreate-all", L"", L"1", kTokenTtlMsFor(1));
            const std::wstring allOff = _tokenFor(L"recreate-all", L"", L"0", kTokenTtlMsFor(1));
            const std::wstring no = _tokenFor(L"cancel", L"", L"", kTokenTtlMsFor(1));
            _reply(threadId,
                   L"Delete the " + std::to_wstring(topics) + L" session topic(s) and make them again in MOST RECENT order (like the side panel)? Their messages in Telegram go with them; "
                   L"every conversation stays in its session, and each new topic gets its session's whole history again (after the live messages, within Telegram's flood limit).\n\n"
                   L"<b>Verbose</b> shows the work (Claude's remarks, the tool calls); <b>conversation</b> shows your messages and each turn's last answer. The choice becomes the default for all topics.",
                   false,
                   _buttons({ { { L"\x267B Recreate \u00B7 verbose", allOn }, { L"\x267B Recreate \u00B7 conversation", allOff } }, { { L"Cancel", no } } }));
            LogNav(L"remote telegram /recreate -> asked");
            return;
        }
        if (name == L"verbose")
        {
            _runVerboseCommand(L"", cmd.args, threadId);
            return;
        }
        if (name == L"idle" || name == L"board")
        {
            size_t counts[6] = { 0, 0, 0, 0, 0, 0 };
            std::vector<std::tuple<std::wstring, std::wstring, std::wstring>> idle; // id, title, its tab color's square (§21.13)
            for (const auto& s : _registry->SnapshotLiveForCards())
            {
                if (s.kind != AgentKind::Claude || s.tabToken.empty())
                {
                    continue;
                }
                counts[static_cast<int>(s.state)]++;
                if (s.state == SessionState::Idle)
                {
                    idle.emplace_back(s.id, s.title.empty() ? ShortId(s.id) : s.title, _titleSquare(&s));
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
            for (const auto& [id, title, square] : idle)
            {
                ++n;
                html += L"\n" + std::to_wstring(n) + L". " + TgTitleToken(TgTitleForm::Plain, square, title, _threadOf(id)); // §21.15: a chip when rich; §21.17: a link while its topic stands
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
            // §21.17.4: the folder check is BOUNDED (FolderProbe.h), like a new thread's pick — this runs on the poll thread, the
            // one that reads Telegram, which an offline network share held for the network's timeout.
            const FolderProbeAnswer probe = SharedFolderProber().Probe(folder, kNewThreadPickProbeWaitMs, 0);
            if (probe == FolderProbeAnswer::NoAnswer)
            {
                _reply(threadId, NewThreadFolderNoAnswerText(L"<code>" + TgEscapeHtml(folder) + L"</code>"), false);
                return;
            }
            if (probe != FolderProbeAnswer::Folder)
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
        if (name == L"cc")
        {
            _reply(threadId, L"/cc works in a session's topic.", false);
            return;
        }
        _reply(threadId, L"Here: a search in plain words, or /clear, /idle, /board, /newsession &lt;folder&gt;, /pauseall, /resumeall, /verbose, /recreate, /status, /help.", false);
    }

    bool MessagingService::_ensureSearchBridge(int64_t threadId)
    {
        bool noFactory = false;
        {
            std::lock_guard lk{ _mtx };
            if (_search)
            {
                return true;
            }
            if (!_searchFactory)
            {
                noFactory = true; // the test harness: no agent in this build
            }
            else
            {
                // The factory attaches this start's sink to the process's one Search conversation (it is made on the
                // first use of the process and outlives every bridge). It may throw (the agent could not be made): the
                // caller's update handler logs it (Rule #18), and the next message tries again.
                auto self = weak_from_this();
                _search = _searchFactory([self](const SearchBridgeEvent& ev) {
                    if (auto me = self.lock())
                    {
                        me->_onSearchEvent(ev);
                    }
                },
                                         _searchThinkingWord);
                if (_search)
                {
                    return true;
                }
            }
        }
        // Outside `_mtx`: a reply takes it (_enqueue), and std::mutex is not recursive.
        _reply(threadId, noFactory ? L"Search is not available in this build." : L"Search could not start (see hooks.log).", false);
        return false;
    }

    void MessagingService::_askSearch(const std::wstring& text, int64_t threadId)
    {
        if (!_ensureSearchBridge(threadId))
        {
            return;
        }
        {
            std::lock_guard lk{ _mtx };
            _searchLastUsedMs = WallMs();
            _searchParked = false;
        }
        // `_search` is read outside `_mtx`: it is created and used only on this (the poll) thread, and Stop() resets it
        // only after it has joined this thread.
        const uint64_t turn = _search->Ask(text);
        LogNav(L"remote telegram search turn=" + std::to_wstring(turn) + L" chars=" + std::to_wstring(text.size()));
        _reply(threadId, L"<i>Searching\x2026</i>", false);
    }

    void MessagingService::_clearSearch(int64_t threadId, int64_t replyTo)
    {
        // The bridge is made for a clear too: a record that an earlier run left would otherwise survive a /clear sent
        // before this run's first request, and the next request would continue it.
        if (!_ensureSearchBridge(threadId))
        {
            return;
        }
        // Outside `_mtx`, like Ask (the poll thread owns `_search`): Reset ends a running request, deletes the record and
        // the view file, and restarts the process when one was up.
        _search->Reset();
        {
            std::lock_guard lk{ _mtx };
            // Kept: `_searchRowInfo`. The answers above this line stay in the chat, and their picks still open a menu.
            _searchLastUsedMs = WallMs();
            _searchParked = false;
        }
        LogNav(L"remote telegram search clear");
        _reply(threadId, L"\x2298 <b>Search conversation cleared.</b> The next request starts fresh.", false, json::Value{}, replyTo);
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
            }
            switch (ev.kind)
            {
            case SearchBridgeEvent::Kind::Searching:
                break; // quiet: the final answer carries the rows
            case SearchBridgeEvent::Kind::Shown:
                break; // the bridge hands the shown rows to the request's Answer
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
        // THIS request's rows only (the bridge clears them after each answer): an answer whose request showed nothing
        // lists nothing, rather than the previous request's rows under a sentence about something else.
        const std::vector<SearchRow>& rows = ev.rows;
        const size_t listed = (std::min)(rows.size(), kSearchAnswerMaxRows);
        int64_t thread = 0;
        {
            std::lock_guard lk{ _mtx };
            thread = _state.searchThreadId;
            // Remember the listed rows for their picks (the newest listing of a session wins), within the cap.
            for (size_t i = 0; i < listed; ++i)
            {
                const auto& row = rows[i];
                if (_searchRowInfo.insert_or_assign(row.sessionId, row).second)
                {
                    _searchRowOrder.push_back(row.sessionId);
                }
            }
            while (_searchRowOrder.size() > kSearchRowMemory)
            {
                _searchRowInfo.erase(_searchRowOrder.front());
                _searchRowOrder.pop_front();
            }
        }
        // §21.10, the EVIDENCE CARDS (the user's "C1" of 2026-10-17): the model's verdict, then one card per listed
        // session — number, glyph, title; folder · day · prompts; the model's why as its one quote (BuildSearchAnswerHtml).
        // Where each session stands is read NOW (the glyph only; the menu reads it again at the tap).
        std::vector<SearchCard> cards;
        cards.reserve(listed);
        for (size_t i = 0; i < listed; ++i)
        {
            const auto& row = rows[i];
            const auto s = _registry->Get(row.sessionId);
            SearchCard card;
            card.number = i + 1;
            card.sessionId = row.sessionId;
            card.title = row.title;
            card.dir = row.dir;
            card.branch = row.branch;
            card.createdMs = row.createdMs;
            card.lastActivityMs = row.lastActivityMs;
            card.prompts = row.prompts;
            card.why = row.why;
            card.colorSquare = _titleSquare(s ? &*s : nullptr, row.dir); // §21.13: its tab's color, else its folder's
            card.threadId = _threadOf(row.sessionId); // §21.17: a session with a topic: its title links there
            card.state = ClassifySearchPickState(s.has_value(), s && s->live, s && !s->tabToken.empty(), s ? s->state : SessionState::Idle, s && s->pinned);
            cards.push_back(std::move(card));
        }
        const std::wstring html = BuildSearchAnswerHtml(ev.text, cards, rows.size() - listed, WallMs());
        // The keyboard: one PICK per listed row, by its number (§21.7: pick the session, then its menu asks what to do),
        // then "⊘ Clear" — the Sessions page's crossed circle, on every answer so the newest one always has it.
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> buttons;
        for (const auto& line : SearchPickKeyboardLayout(listed))
        {
            std::vector<std::pair<std::wstring, std::wstring>> picks;
            for (const size_t n : line)
            {
                if (std::wstring data = SearchPickCallbackData(n, rows[n - 1].sessionId); !data.empty())
                {
                    picks.push_back({ std::to_wstring(n), std::move(data) });
                }
            }
            if (!picks.empty())
            {
                buttons.push_back(std::move(picks));
            }
        }
        buttons.push_back({ { L"\x2298 Clear", std::wstring{ kSearchClearCallbackData } } });
        // With sound: the answer is the request's final message (D5).
        _reply(thread, html, true, _buttons(buttons));
        // Every shown session reaches the phone (the user's rule of 2026-10-18, "searches must never miss"): the rows past the
        // cards follow as one line each, in as few silent messages as fit (BuildSearchMoreLinesHtml). The facts are the rows'
        // own; where each session stands is read now, like the cards' glyphs.
        if (rows.size() > listed)
        {
            std::vector<SearchCard> rest;
            rest.reserve(rows.size() - listed);
            for (size_t i = listed; i < rows.size(); ++i)
            {
                const auto& row = rows[i];
                const auto s = _registry->Get(row.sessionId);
                SearchCard card;
                card.number = i + 1;
                card.sessionId = row.sessionId;
                card.title = row.title;
                card.dir = row.dir;
                card.lastActivityMs = row.lastActivityMs;
                card.colorSquare = _titleSquare(s ? &*s : nullptr, row.dir);
                card.state = ClassifySearchPickState(s.has_value(), s && s->live, s && !s->tabToken.empty(), s ? s->state : SessionState::Idle, s && s->pinned);
                rest.push_back(std::move(card));
            }
            for (const auto& more : BuildSearchMoreLinesHtml(rest, WallMs()))
            {
                _reply(thread, more, false);
            }
        }
    }

    void MessagingService::_showSearchPick(const SearchPick& pick, int64_t threadId, int64_t answerMessageId)
    {
        // The row as the answer listed it (this run), else the registry's record (a session Agentmaster knows), else the
        // short id; the launch finds a missing folder in the transcript (_requestLaunch).
        SearchRow row;
        bool remembered = false;
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _searchRowInfo.find(pick.sessionId); it != _searchRowInfo.end())
            {
                row = it->second;
                remembered = true;
            }
        }
        const auto s = _registry->Get(pick.sessionId);
        // The title a launch is given: "" when nothing knows one, so the launch derives it from the folder (a short id
        // would become the resumed tab's title). The menu shows the short id instead.
        std::wstring title = remembered ? row.title : std::wstring{};
        if (title.empty() && s)
        {
            title = s->title;
        }
        std::wstring dir = remembered ? row.dir : std::wstring{};
        if (dir.empty() && s)
        {
            dir = s->workingDir;
        }
        const SearchPickState state = ClassifySearchPickState(s.has_value(), s && s->live, s && !s->tabToken.empty(), s ? s->state : SessionState::Idle, s && s->pinned);
        // §21.10: the menu repeats the card's facts in full (the whole folder, the branch, when it ran) and its quote,
        // then where the session stands now (BuildSearchPickMenuHtml). A session the answer did not list this run (a
        // pick after a restart) shows what the registry knows: no quote, the transcript's own times.
        SearchCard card;
        card.number = pick.number;
        card.sessionId = pick.sessionId;
        card.title = title; // "" = the menu shows the short id
        card.dir = dir;
        card.branch = remembered && !row.branch.empty() ? row.branch : (s ? s->branch : std::wstring{});
        card.createdMs = remembered && row.createdMs > 0 ? row.createdMs : (s ? s->convCreatedUnixMs : 0);
        card.lastActivityMs = remembered && row.lastActivityMs > 0 ? row.lastActivityMs : (s ? s->convLastActivityUnixMs : 0);
        card.prompts = remembered ? row.prompts : 0;
        card.why = remembered ? row.why : std::wstring{};
        card.colorSquare = _titleSquare(s ? &*s : nullptr, dir); // §21.13: its tab's color, else its folder's
        card.threadId = _threadOf(pick.sessionId); // §21.17: a session with a topic: its title links there
        card.state = state;
        const std::wstring html = BuildSearchPickMenuHtml(card, WallMs());
        // The actions: single-use tokens (M5) that expire, carrying what the outcome line and a launch need. Each acts on
        // the session's state at the tap (_onSearchPickAction), which may differ from the line above.
        const std::wstring payload = std::to_wstring(pick.number) + L"\n" + dir + L"\n" + title;
        const int64_t ttl = kTokenTtlMsFor(kSearchActionTtlHours);
        const std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows = {
            { { L"\x25B6 Resume", _tokenFor(L"pick-resume", pick.sessionId, payload, ttl) }, { L"\x25B6 Resume \U0001F4CC", _tokenFor(L"pick-resume-pin", pick.sessionId, payload, ttl) } },
            { { L"\x2442 Fork", _tokenFor(L"pick-fork", pick.sessionId, payload, ttl) }, { L"\x2715 Cancel", _tokenFor(L"pick-cancel", pick.sessionId, payload, ttl) } },
        };
        LogNav(L"remote telegram search pick " + std::to_wstring(pick.number) + L" " + ShortId(pick.sessionId) + L" (" + SearchPickStateWord(state) + L")");
        _reply(threadId, html, false, _buttons(rows), answerMessageId);
    }

    void MessagingService::_onSearchPickAction(const std::wstring& kind, const std::wstring& sessionId, const std::wstring& payload, int64_t threadId, int64_t menuMessageId)
    {
        // The payload: "<row number>\n<folder>\n<title>" (_showSearchPick); the title is "" when nothing knew one.
        const size_t nl1 = payload.find(L'\n');
        const size_t nl2 = nl1 == std::wstring::npos ? std::wstring::npos : payload.find(L'\n', nl1 + 1);
        const std::wstring number = payload.substr(0, nl1);
        const std::wstring dir = nl1 == std::wstring::npos ? std::wstring{} : payload.substr(nl1 + 1, nl2 == std::wstring::npos ? std::wstring::npos : nl2 - nl1 - 1);
        const std::wstring title = nl2 == std::wstring::npos ? std::wstring{} : payload.substr(nl2 + 1);
        // §21.13: the head line names the session with its tab's color square (its folder's for a session Agentmaster does not
        // know), read lean here: the resume below reads the full record again, on the state of that moment. §21.15: a title token —
        // the menu is edited in the form chips want (_editText), so the head is a chip in a rich menu.
        const auto known = _registry->GetWithoutQueue(sessionId);
        const std::wstring head = L"<b>" + TgEscapeHtml(number) + L". " +
                                  TgTitleToken(TgTitleForm::Plain, _titleSquare(known ? &*known : nullptr, dir), title.empty() ? ShortId(sessionId) : title, _threadOf(sessionId)) + L"</b>";
        if (kind == L"pick-cancel")
        {
            // The menu goes; the answer above it keeps its pick buttons. A menu already gone (the user deleted it) is a
            // harmless 400.
            if (menuMessageId > 0)
            {
                OutItem del;
                del.prio = SendPriority::Final;
                del.method = L"deleteMessage";
                del.params = json::Value::MkObj();
                del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
                del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(menuMessageId)));
                _enqueue(std::move(del));
            }
            LogNav(L"remote telegram search pick-cancel " + ShortId(sessionId));
            return;
        }
        if (kind == L"pick-fork")
        {
            // Never pinned: there is no fork-and-pin (the user's rule, §21.7).
            _editText(menuMessageId, head + L"\n\x2192 \x2442 Fork", SendPriority::Final, L"", json::Value{});
            RemoteLaunchRequest req;
            req.kind = RemoteLaunchRequest::Kind::Fork;
            req.sessionId = sessionId;
            req.dir = dir;
            req.title = title;
            _requestLaunch(req, threadId); // it says how the fork goes (and why not, when it cannot open)
            return;
        }
        // Resume / Resume 📌, on the session's state NOW.
        const bool pin = kind == L"pick-resume-pin";
        const std::wstring choice = pin ? L"\x25B6 Resume \U0001F4CC" : L"\x25B6 Resume";
        const auto s = _registry->Get(sessionId);
        switch (ClassifySearchPickState(s.has_value(), s && s->live, s && !s->tabToken.empty(), s ? s->state : SessionState::Idle, s && s->pinned))
        {
        case SearchPickState::IdleTab:
            // Its tab is open: waking it (Waiting for you) is all a resume needs, and its topic appears at the next sync.
            _wake(sessionId, pin);
            _editText(menuMessageId, head + L"\n\x2192 " + choice + L": woken, its topic appears in a moment.", SendPriority::Final, L"", json::Value{});
            return;
        case SearchPickState::Listed:
        {
            // Open with its topic already: nothing to open. 📌 still pins it (never idle).
            const bool newlyPinned = pin && s && !s->pinned;
            if (newlyPinned)
            {
                _setPinned(sessionId, true);
            }
            _editText(menuMessageId, head + L"\n\x2192 " + choice + L": it is open already, with its own topic." + (newlyPinned ? L" Pinned \U0001F4CC." : L""), SendPriority::Final, L"", json::Value{});
            LogNav(L"remote telegram search resume " + ShortId(sessionId) + L" -> already listed" + (newlyPinned ? L" (pinned)" : L""));
            return;
        }
        case SearchPickState::Closed:
        default:
        {
            _editText(menuMessageId, head + L"\n\x2192 " + choice, SendPriority::Final, L"", json::Value{});
            RemoteLaunchRequest req;
            req.kind = RemoteLaunchRequest::Kind::Resume;
            req.sessionId = sessionId;
            req.dir = dir;
            req.title = title;
            req.pin = pin;
            _requestLaunch(req, threadId); // it says how the resume goes; the revive sweep pins it once it is live
            return;
        }
        }
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
        // §21.28, the user's rule: "buttons we click on should look 'active' as long as they are waiting for serverside to react".
        // Telegram keeps a tapped button's loading indicator until the bot answers the tap, so the answer is HELD while what the
        // tap sets off runs. The tap is open from now on (its hold's limit, kTapAnswerHoldMaxMs, counts from here); the handler
        // runs inside a scope of its own hold, so every outbox item it queues — and every continuation it starts with _holdTaps —
        // holds the answer too, and the answer goes (from the send loop) once the last of those holds went.
        const auto tap = std::make_shared<TapAnswer>(cqId, TapAnswerLogLabel(cq.StrAt(L"data")), static_cast<int64_t>(::GetTickCount64()));
        {
            std::lock_guard lk{ _tapMtx };
            _openTaps.push_back(tap);
        }
        // The handler's own hold (declared before the scope, so it outlives it): it goes when this function returns — normally or
        // by a throw — and with nothing else holding the tap, the answer goes at once, as it always did.
        const std::vector<TapHoldRef> handlerHold{ std::make_shared<TapHold>(tap, std::weak_ptr<TapReleaseSink>{ weak_from_this() }) };
        const TapScope tapScope{ handlerHold };
        // The handler's word on the tap: since §21.28 it sets the TOAST its answer carries (the last call wins; "" = none), and the
        // answer goes when the tap's work is in — never here.
        const auto answer = [&tap](const std::wstring& text) {
            tap->SetToast(text);
        };
        if (from == 0 || from != _ownerId)
        {
            answer(L"This bot is private.");
            return;
        }
        const std::wstring data = cq.StrAt(L"data");
        int64_t threadId = 0;
        int64_t messageId = 0; // the message the button sits on: a Search answer for a pick, a pick's menu for its actions
        if (const auto* m = cq.Find(L"message"); m && m->type == json::Value::Type::Obj)
        {
            threadId = m->I64At(L"message_thread_id", 0);
            messageId = m->I64At(L"message_id", 0);
        }
        // §21.30: a QUESTION CARD's button ("q:<card>:<verb>[:<arg>]") — an option, the user's own answer or a note, the next
        // question, Send, Chat about this, the previews' picture, the "answer or next prompt?" question's two buttons. The toast
        // says what happened; the tap stays busy until the card's edit (and any message it asked for) is in.
        if (data.rfind(L"q:", 0) == 0)
        {
            answer(_onQuestionTap(data, threadId, messageId));
            return;
        }
        // The Search topic's STATELESS buttons (§21.7): "⊘ Clear" and the PICK of an answer's row. They carry no token,
        // so they work again and after a restart; a pick only opens the action menu, whose buttons are tokens.
        if (data == kSearchClearCallbackData)
        {
            answer(L"");
            _clearSearch(threadId, 0);
            return;
        }
        if (const auto pick = ParseSearchPickCallbackData(data))
        {
            answer(L"");
            _showSearchPick(*pick, threadId, messageId);
            return;
        }
        // A directory DOT (§21.16.7): stateless like the Search buttons — it names the topic's thread, so it works again and after
        // a restart. The toast says which thread goes first in the list (or why none does).
        if (const int64_t frontThread = ParseDirectoryFrontCallbackData(data); frontThread > 0)
        {
            answer(_bringTopicToFront(frontThread));
            return;
        }
        // A new thread's PICKER (§21.17): the data names the list and an index or a page; the picker it belongs to is the one of
        // the thread the button sits in. The toast says what happens (or that the picker is from before a restart).
        if (const NewThreadAction pick = ParseNewThreadCallbackData(data); pick.kind != NewThreadAction::Kind::None)
        {
            answer(_onNewThreadAction(pick, threadId));
            return;
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
            else if (verb == L"reactivate" || verb == kCardVerbResume)
            {
                // The outcome as the button's toast: nothing is posted in the thread, which is the conversation (§21.6).
                // §21.24: 🔧 Resume Session is ▶ Reactivate's name for a session in Error - the same action.
                answer(_reactivate(sid));
                return;
            }
            else if (verb == kCardVerbSessionPage || verb == kCardVerbMainPage)
            {
                // §21.24: ⚙ Session / ◀ Back flip the keyboard of the message the button sits on; nothing is posted.
                answer(L"");
                _showCardPage(sid, messageId, verb == kCardVerbSessionPage ? CardPage::Session : CardPage::Main);
                return;
            }
            else if (verb == kCardVerbFork || verb == kCardVerbNewSession)
            {
                // §21.24: ⑂ Fork / 🆕 New Session - the outcome as the toast; the new topic appearing is the rest of the answer.
                answer(_launchFromCard(sid, verb == kCardVerbFork, threadId));
                return;
            }
            else if (verb == kCardVerbRestart)
            {
                // §21.24: 🔄 Restart Session - a toast at rest; while a turn runs, a question in the thread (the turn would be cut off).
                answer(_restartFromCard(sid, threadId));
                return;
            }
            else if (verb == kCardVerbScreenImage)
            {
                // §21.24: 🖼 Screen Image - the photo lands in the topic (or a line saying why it cannot). §21.28: no toast — the
                // button stays busy until the picture is in (the window's reply and the drawing's worker carry the tap), so a
                // "Drawing the screen…" toast would only show up after the picture; past the hold's limit the answer says the work
                // goes on.
                cmd.name = L"screenimage";
                _runSessionCommand(sid, cmd, fake, threadId);
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
            if (tok.kind == L"recreate-one")
            {
                // §21.11: the topic the button sits in is deleted in a moment, so the outcome is the button's toast, and the
                // tick makes the topic again (one per tick, before the sync's plan).
                const int mode = tok.payload == L"1" ? 1 : 0;
                {
                    std::lock_guard lk{ _mtx };
                    _recreateOne.emplace_back(tok.sessionId, mode);
                    _tickRequested = true;
                }
                _tickCv.notify_all();
                answer(mode == 1 ? L"\x267B Making this topic again, verbose \u2014 it comes back at the top with its history." :
                                   L"\x267B Making this topic again, conversation only \u2014 it comes back at the top with its history.");
                LogNav(L"remote telegram /recreate " + ShortId(tok.sessionId) + L" -> confirmed");
                return;
            }
            answer(L"");
            const std::wstring& k = tok.kind;
            if (k == L"cancel")
            {
                _reply(threadId, L"Cancelled.", false);
            }
            else if (k == L"recreate-yes")
            {
                // The tick thread runs it (it owns the topic sync): at its next pass, after the start-up hold.
                _recreateRequested.store(true);
                {
                    std::lock_guard lk{ _mtx };
                    _tickRequested = true;
                }
                _tickCv.notify_all();
                _reply(threadId, L"\x267B Making the topics again \x2014 the old ones go first, then the new list, the most recent on top, then each session's history.", false);
                LogNav(L"remote telegram /recreate -> confirmed");
            }
            else if (k == L"recreate-all")
            {
                // §21.11: the chosen mode becomes the default first (the new topics are made in it), then the recreation runs
                // on the tick thread like /recreate's.
                const bool verbose = tok.payload == L"1";
                _setDefaultVerbose(verbose, -1);
                _recreateRequested.store(true);
                {
                    std::lock_guard lk{ _mtx };
                    _tickRequested = true;
                }
                _tickCv.notify_all();
                _reply(threadId,
                       std::wstring{ L"\x267B Making the topics again, " } + (verbose ? L"verbose" : L"conversation only") +
                           L" (now the default for all topics) \x2014 the old ones go first, then the new list, the most recent on top, then each session's history.",
                       false);
                LogNav(std::wstring{ L"remote telegram /recreate -> confirmed (" } + (verbose ? L"verbose" : L"conversation") + L")");
            }
            else if (k == L"verbose-topic")
            {
                _setTopicVerbose(tok.sessionId, tok.payload == L"1" ? 1 : (tok.payload == L"0" ? 0 : -1), threadId);
            }
            else if (k == L"verbose-default")
            {
                _setDefaultVerbose(tok.payload == L"1", threadId);
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
                // §21.28: the restart is the window's work, and its card shows it (the turn cut off, the state after): the button stays
                // busy until the card changes.
                _awaitCardChange(tok.sessionId);
                LogNav(L"remote telegram /restartsession " + ShortId(tok.sessionId) + L" -> confirmed");
            }
            else if (k == L"confirm-deactivate")
            {
                if (_hooks.deactivateSession)
                {
                    _hooks.deactivateSession(tok.sessionId);
                }
                _reply(threadId, L"Parked.", false);
                _awaitCardChange(tok.sessionId); // §21.28: until the card shows the parked tab (its ▶ Reactivate)
                LogNav(L"remote telegram /deactivate " + ShortId(tok.sessionId) + L" -> confirmed");
            }
            else if (k == L"confirm-interrupt")
            {
                // One Esc into the session (the typed path; a linked session will use the link's abort).
                const bool ok = _registry->Inject(tok.sessionId, L"\x1b");
                _reply(threadId, ok ? L"\x25A0 Interrupt sent." : L"Could not reach the session (no input bound).", false);
                if (ok)
                {
                    _awaitCardChange(tok.sessionId); // §21.28: until the card shows the stopped turn
                }
                LogNav(L"remote telegram /interrupt " + ShortId(tok.sessionId) + (ok ? L" -> sent" : L" -> refused:no-injector"));
            }
            else if (k == L"wake" || k == L"wake-pin")
            {
                _wake(tok.sessionId, k == L"wake-pin");
                _reply(threadId, L"Woken: its topic appears in a moment.", false);
            }
            else if (k == L"pick-resume" || k == L"pick-resume-pin" || k == L"pick-fork" || k == L"pick-cancel")
            {
                // A Search pick's action menu (_showSearchPick): it acts on the session's state NOW, and edits or deletes
                // the menu it sits on.
                _onSearchPickAction(k, tok.sessionId, tok.payload, threadId, messageId);
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
        // §21.23: the revive goes in BEFORE the request goes out, under the launch's token: the window answers on its own thread,
        // possibly before _askLaunch returns, and its answer must find the revive (_settleLaunchAnswers).
        // §21.28: it also carries a tapped ⑂ Fork / 🆕 New Session / ▶ Resume, whose button stays busy until the thread says how
        // the launch went ("Opened …", or the window's refusal) — empty for a typed command.
        std::vector<TapHoldRef> taps = _holdTaps();
        uint64_t token = 0;
        {
            std::lock_guard lk{ _mtx };
            token = ++_launchTokenSeq;
            Revive rv;
            rv.launchToken = token;
            rv.req = req;
            rv.requestedMs = WallMs();
            rv.threadId = threadId;
            rv.tapHolds = std::move(taps);
            _revives.push_back(std::move(rv));
        }
        const auto dropRevive = [this, token]() {
            std::lock_guard lk{ _mtx };
            _revives.erase(std::remove_if(_revives.begin(), _revives.end(), [token](const Revive& r) { return r.launchToken == token; }), _revives.end());
        };
        bool ok = false;
        try
        {
            ok = _askLaunch(req, token);
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_requestLaunch");
        }
        if (!ok)
        {
            dropRevive(); // no window took it: no answer follows, and nothing will open
            _reply(threadId, L"No window could open it right now.", false);
            return;
        }
        const wchar_t* what = req.kind == RemoteLaunchRequest::Kind::Fork ? L"Forking" : (req.kind == RemoteLaunchRequest::Kind::Resume ? L"Resuming" : L"Opening a new session");
        _reply(threadId, std::wstring{ what } + L" into a tab\x2026 its topic appears once it is up.", false);
        {
            // §21.23: the line above is queued; a refusal the window answered with may go out now, after it, never above it.
            std::lock_guard lk{ _mtx };
            for (auto& rv : _revives)
            {
                if (rv.launchToken == token)
                {
                    rv.announced = true;
                    break;
                }
            }
        }
        LogNav(L"remote telegram launch " + std::wstring{ what } + L" " + ShortId(req.sessionId) + L" dir=" + req.dir + (req.pin ? L" (pin)" : L"") + L" token=" + std::to_wstring(token));
    }

    std::wstring MessagingService::_launchFromCard(const std::wstring& sessionId, bool fork, int64_t threadId)
    {
        // The topic's /forksession and /newsession with no model (a button carries none): the very same request through the
        // very same path, so the button and the typed command can never behave differently - the thread gets the command's
        // own replies (how the launch goes, and "Opened …" once the new session is live). The toast only says it was asked.
        RemoteCommand cmd;
        cmd.kind = RemoteKind::AgentmasterCommand;
        cmd.name = fork ? L"forksession" : L"newsession";
        _runSessionCommand(sessionId, cmd, json::Value::MkObj(), threadId);
        // §21.28: no toast. The button stays busy until the thread says how the launch went (the revive carries the tap), and a
        // "Forking it into a new tab…" toast would only show up after the "Opened …" line.
        return {};
    }

    std::wstring MessagingService::_restartFromCard(const std::wstring& sessionId, int64_t threadId)
    {
        const auto s = _registry->Get(sessionId);
        if (!s || !s->live)
        {
            LogNav(L"remote telegram restart (card) " + ShortId(sessionId) + L" -> closed");
            return L"This session is closed. Resume it from the \U0001F50E Search topic.";
        }
        if (s->state == SessionState::Running)
        {
            // A turn would be cut off: the question /restartsession asks, with its two single-use buttons, goes in the thread.
            RemoteCommand cmd;
            cmd.kind = RemoteKind::AgentmasterCommand;
            cmd.name = L"restartsession";
            _runSessionCommand(sessionId, cmd, json::Value::MkObj(), threadId);
            return L"A turn is running \x2014 confirm the restart below.";
        }
        if (!s->started && !s->external)
        {
            // A PARKED tab (restored or deactivated, its claude never started): the window's restart refuses a connection that
            // never started (_restartPaneConnection's guard - an AV otherwise), so it is started in place, as ▶ Reactivate does
            // (the same conversation resumes; a cut-off turn goes on by itself). The toast is Reactivate's own outcome.
            LogNav(L"remote telegram restart (card) " + ShortId(sessionId) + L" -> start (parked)");
            return _reactivate(sessionId);
        }
        if (!_hooks.restartSession)
        {
            return L"Restarting is not available in this build.";
        }
        try
        {
            _hooks.restartSession(sessionId);
        }
        catch (...)
        {
            // Rule #18: logged with its throw site; the toast says it did not go through.
            LogSwallowedException(L"MessagingService::_restartFromCard");
            return L"The restart could not be asked for (hooks.log says why).";
        }
        LogNav(L"remote telegram restart (card) " + ShortId(sessionId) + L" -> restart");
        // §21.28: the restart is the window's work, and the card shows it (the next state, the buttons that go with it): the button
        // stays busy until the card changes.
        _awaitCardChange(sessionId);
        return L"\U0001F504 Restarting it \x2014 the same conversation resumes in its tab.";
    }

    void MessagingService::_showCardPage(const std::wstring& sessionId, int64_t messageId, CardPage page)
    {
        // The topic whose STICKY card the button sits on, found by the message: after a /clear or /resume in the tab the
        // card belongs to the tab's next session (a Rekey), whatever session the button's data names.
        std::wstring cardOwner;
        {
            std::lock_guard lk{ _mtx };
            if (messageId > 0)
            {
                for (const auto& [sid, t] : _state.topics)
                {
                    if (t.cardMessageId == messageId)
                    {
                        cardOwner = sid;
                        _cards[sid].page = page; // remembered: a re-post of the card keeps the page
                        break;
                    }
                }
            }
        }
        LogNav(L"remote telegram card-page " + ShortId(cardOwner.empty() ? sessionId : cardOwner) + (page == CardPage::Session ? L" session" : L" main") +
               (cardOwner.empty() ? L" (a copy of the card)" : L""));
        if (!cardOwner.empty())
        {
            // The card's hash covers its buttons, so the new page is an edit (or a re-post, when the card was due for one) -
            // at an answer's priority, so the keyboard flips at once.
            _refreshCard(cardOwner, true, false, true);
            return;
        }
        // A copy of the card (a /status reply): only its buttons change, in place. Never the sticky card itself - its recorded
        // hash would then not match what it shows, and its next refresh would skip an edit it needs.
        const auto s = _registry->Get(sessionId);
        if (!s || messageId <= 0)
        {
            return;
        }
        json::Value markup;
        _cardMarkupFor(sessionId, CardButtonFactsFor(*s), page, markup);
        OutItem item;
        item.prio = SendPriority::Final;
        item.method = L"editMessageReplyMarkup";
        item.key = L"kb:" + std::to_wstring(messageId); // a quick second tap replaces a first one that is still queued
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        item.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        item.params.Set(L"reply_markup", std::move(markup));
        _enqueue(std::move(item));
    }

    void MessagingService::_startScreenImage(const std::wstring& sessionId, int64_t threadId, std::wstring snapshot)
    {
        // The hosting window's UI thread: nothing here may block it. The drawing runs on a worker thread of its own.
        if (snapshot.empty())
        {
            _reply(threadId, L"The screen could not be read (the tab is dormant, or in another window that is not up).", false);
            LogNav(L"remote telegram screenimage " + ShortId(sessionId) + L" -> unreadable");
            return;
        }
        if (_screenImageRenders.fetch_add(1) >= kScreenImageMaxRenders)
        {
            _screenImageRenders.fetch_sub(1);
            _reply(threadId, L"Other screen pictures are being drawn right now; ask again in a moment.", false);
            return;
        }
        std::wstring title;
        // The title only (the caption, the file's name), so the lean read: this runs on the hosting window's UI thread.
        if (const auto s = _registry->GetWithoutQueue(sessionId))
        {
            title = s->title;
        }
        try
        {
            // §21.28: the worker carries the tapped 🖼 Screen Image along (`taps`, from the window's reply scope: empty for a typed
            // /screenimage), so the button stays busy until the photo — or the line saying why there is none — is in the topic.
            std::thread([self = weak_from_this(), sessionId, threadId, text = std::move(snapshot), title, taps = _holdTaps()]() noexcept {
                const auto me = self.lock();
                if (!me)
                {
                    return; // the service is gone (an app quit): nothing to send to
                }
                const TapScope tapScope{ taps }; // never throws; the sends below hold the tap until Telegram answered them
                try
                {
                    const int64_t startedMs = static_cast<int64_t>(::GetTickCount64());
                    const auto parsed = ParseScreenSnapshot(text);
                    if (!parsed)
                    {
                        me->_reply(threadId, L"The screen could not be read (its snapshot was not understood).", false);
                        me->_log(L"[msg] screen picture of " + ShortId(sessionId) + L": the snapshot did not parse (" + std::to_wstring(text.size()) + L" chars)");
                    }
                    else
                    {
                        const ScreenImage image = RenderScreenSnapshotPng(*parsed);
                        if (image.png.empty())
                        {
                            me->_reply(threadId, L"The screen could not be drawn: " + TgEscapeHtml(image.error), false);
                            me->_log(L"[msg] screen picture of " + ShortId(sessionId) + L" failed: " + image.error);
                        }
                        else
                        {
                            const int64_t now = WallMs();
                            const std::wstring caption = L"\U0001F5BC " + (TgTitleLine(title, 120).empty() ? ShortId(sessionId) : TgTitleLine(title, 120)) + L" \x00B7 " +
                                                         TgClockText(now, now) + L" \x00B7 " + std::to_wstring(parsed->cols) + L"\x00D7" + std::to_wstring(parsed->rows);
                            me->_log(L"[msg] screen picture of " + ShortId(sessionId) + L": " + std::to_wstring(image.grid.widthPx) + L"x" + std::to_wstring(image.grid.heightPx) +
                                     L" px, " + std::to_wstring(image.png.size() / 1024) + L" KB, " + image.fontFamily + L", drawn in " +
                                     std::to_wstring(static_cast<int64_t>(::GetTickCount64()) - startedMs) + L" ms");
                            me->_sendPhoto(threadId, L"screen-" + SafeFileStem(title) + L".png", image.png, caption, SendPriority::Final);
                        }
                    }
                }
                catch (...)
                {
                    // Rule #18: an exception escaping a std::thread would end the process.
                    LogSwallowedException(L"MessagingService::_startScreenImage (worker)");
                }
                me->_screenImageRenders.fetch_sub(1);
            }).detach();
        }
        catch (...)
        {
            // The thread could not start (std::system_error): nothing is drawing, so the count goes back.
            _screenImageRenders.fetch_sub(1);
            LogSwallowedException(L"MessagingService::_startScreenImage");
            _reply(threadId, L"The picture could not be started (hooks.log says why).", false);
        }
    }

    void MessagingService::_sweepRevives(const SessionInfo& s)
    {
        // A revive is "done" when its session is LIVE: pin it when asked, hold it in Waiting for you so its
        // topic appears, and forget the revive. §21.23: "its session" is the one the window ANSWERED it opened
        // (Revive::openedId) — exactly that one. Until the answer comes the revive waits (the answer normally comes
        // within a second; _expireRevives tells the thread when it never does). Before §21.23 a revive matched the
        // request's own id or a live session in its folder, which for a fork of a live session — or a /newsession
        // typed in a live session's topic — was the SOURCE, live at once, settled at the next tick (§21.21).
        std::vector<Revive> matched;
        {
            std::lock_guard lk{ _mtx };
            for (auto it = _revives.begin(); it != _revives.end();)
            {
                if (it->answered && s.live && !it->openedId.empty() && s.id == it->openedId)
                {
                    matched.push_back(*it);
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
            // §21.15: the title is a chip when rich. §21.23: the answer says HOW it opened — a resume of a session that was open
            // already (its topic may stand: then it is linked, and nothing is "coming up"), or a NEW conversation in place of the one
            // asked for (a resume or a fork with no transcript to continue).
            // §21.28: the "Opened …" line below answers a tapped launch button (the revive's holds: the line holds them until
            // Telegram answered it, and the revive's copy goes with `matched`).
            const TapScope tapScope{ rv.tapHolds };
            const std::wstring title = s.title.empty() ? ShortId(s.id) : s.title;
            const int64_t ownThread = _threadOf(s.id);
            std::wstring line;
            if (rv.alreadyOpen)
            {
                line = L"<b>" + TgTitleToken(TgTitleForm::Plain, _titleSquare(&s), title, ownThread) + L"</b> is open already" +
                       (ownThread != 0 ? std::wstring{ L", with its own topic." } : std::wstring{ L" \x2014 its topic is coming up." });
            }
            else if (rv.fresh)
            {
                line = L"Opened <b>" + TgTitleToken(TgTitleForm::Plain, _titleSquare(&s), title) + L"</b> as a NEW session in the folder (there was no transcript to continue) \x2014 its topic is coming up.";
            }
            else
            {
                line = L"Opened <b>" + TgTitleToken(TgTitleForm::Plain, _titleSquare(&s), title) + L"</b> \x2014 its topic is coming up.";
            }
            _reply(rv.threadId, line + (rv.req.pin ? L" \U0001F4CC" : L""), false);
            _log(L"[msg] revive done " + ShortId(s.id) + (rv.alreadyOpen ? L" (open already)" : (rv.fresh ? L" (a fresh session: no transcript to continue)" : L"")) + L" token=" + std::to_wstring(rv.launchToken));
        }
    }

    bool MessagingService::_askLaunch(const RemoteLaunchRequest& req, uint64_t token)
    {
        if (_hooks.launchAnswered)
        {
            // The answer may outlive the service (a window answers whenever its UI thread runs the request): a weak reference, and
            // the queue behind its own lock (_onLaunchAnswered), so the window's thread never waits on the service's.
            auto self = weak_from_this();
            return _hooks.launchAnswered(req, [self, token](const RemoteLaunchOutcome& outcome) {
                if (const auto me = self.lock())
                {
                    me->_onLaunchAnswered(token, outcome);
                }
            });
        }
        // A host without the answered launch (before §21.23): the launch goes, and the revive or bind waits for its timeout.
        return _hooks.launch && _hooks.launch(req);
    }

    void MessagingService::_onLaunchAnswered(uint64_t token, const RemoteLaunchOutcome& outcome) noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _launchAnswerMtx };
                LaunchAnswerItem item;
                item.token = token;
                item.outcome = outcome;
                item.atMs = WallMs();
                _launchAnswers.push_back(std::move(item));
            }
            // Without `_mtx` (the leaf lock above is the only one this thread takes): the tick's wait reads the flag, and a wake-up
            // lost to the race between its check and this store costs one tick period.
            _launchAnswersPending.store(true);
            _tickCv.notify_one();
        }
        catch (...)
        {
            // Rule #18. Recovery: the answer is lost — the revive or the bind waits for its timeout, which tells the thread.
            LogSwallowedException(L"MessagingService::_onLaunchAnswered");
        }
    }

    void MessagingService::_settleLaunchAnswers() noexcept
    {
        try
        {
            _launchAnswersPending.store(false); // before the take: an answer queued after it wakes the next tick
            std::vector<LaunchAnswerItem> answers;
            {
                std::lock_guard lk{ _launchAnswerMtx };
                answers.swap(_launchAnswers);
            }
            if (answers.empty())
            {
                return;
            }
            // How long a refusal waits for _requestLaunch to say it has queued its own "… into a tab…" line (Revive::announced)
            // before it is posted anyway: that line goes out right after the launch call returns, on the poll thread, so the wait
            // is a belt for a request that threw in between.
            constexpr int64_t kAnnounceWaitMs = 5000;
            const int64_t now = WallMs();
            std::vector<LaunchAnswerItem> later; // refusals whose revive has not announced itself yet: the next tick looks again
            struct Notice
            {
                int64_t threadId{ 0 };
                std::wstring html;
                int64_t replyTo{ 0 };
                bool picker{ false }; // a new thread back at its picker: post the picker after the line
                bool reloadModels{ false }; // §21.25: back at the model step — its choices are read again (a preset may be fixed)
                // §21.28: the holds of the tapped button that asked for the launch (a revive's, a new thread's): the notice and the
                // wizard posted with it answer that tap, so they are posted inside a scope of these holds.
                std::vector<TapHoldRef> tapHolds;
            };
            std::vector<Notice> notices;
            std::vector<std::wstring> logs; // written after the lock: a log line is a file write, never made with `_mtx` held
            // The sessions of the binds a refusal names, and whether each is live: a throw in the window AFTER its tab opened
            // answers "failed" with the session running, and such a bind is bound like an opened one. Read with no lock held (the
            // registry has its own), between a look at the binds and the pass that changes them — both on this thread, the only
            // one that removes a bind.
            std::unordered_set<std::wstring> liveBindSessions;
            {
                std::vector<std::wstring> refusedBindSessions;
                {
                    std::lock_guard lk{ _mtx };
                    for (const auto& a : answers)
                    {
                        if (RemoteLaunchOpened(a.outcome))
                        {
                            continue;
                        }
                        for (const auto& b : _threadBinds)
                        {
                            if (b.launchToken == a.token)
                            {
                                refusedBindSessions.push_back(b.sessionId);
                                break;
                            }
                        }
                    }
                }
                for (const auto& sid : refusedBindSessions)
                {
                    if (const auto s = _registry->GetWithoutQueue(sid); s && s->live)
                    {
                        liveBindSessions.insert(sid);
                    }
                }
            }
            {
                std::lock_guard lk{ _mtx };
                for (auto& a : answers)
                {
                    const bool opened = RemoteLaunchOpened(a.outcome);
                    const std::wstring reason = RemoteLaunchRefusalText(a.outcome);
                    // A new thread's bind (§21.17).
                    const auto bind = std::find_if(_threadBinds.begin(), _threadBinds.end(), [&a](const ThreadBind& b) { return b.launchToken == a.token; });
                    if (bind != _threadBinds.end())
                    {
                        // The bind's own session is live already: whatever the answer says, the tab is up and the tick binds it.
                        if (opened || liveBindSessions.count(bind->sessionId) > 0)
                        {
                            // §21.28: the tab is up, so the tapped wizard button that launched it has its answer (its "Opening …" line
                            // is in): its holds go here, under `_mtx` (OnTapHoldsGone only queues the tap).
                            _bindTapHolds.erase(a.token);
                            bind->answered = true; // the tick binds the thread once the session is live (_adoptBoundThreads)
                            logs.push_back(L"[msg] new thread " + std::to_wstring(bind->threadId) + L": the window opened session " + ShortId(bind->sessionId) +
                                 (opened ? std::wstring{} : L" (it answered " + std::wstring{ RemoteLaunchResultName(a.outcome.result) } + L", but the session is live)"));
                            continue;
                        }
                        // Refused: the creation is not over (a refusal is not an abort) — the thread goes back into its WIZARD
                        // (§21.25), with every message the bind held (the ones written before the launch, and since), at the step
                        // that can fix it (NewThreadStepAfterRefusal: a refused preset → the model step, else the window step, or
                        // the model step when the window step was skipped), its choices as they were; one tap retries.
                        ThreadBind b = std::move(*bind);
                        _threadBinds.erase(bind);
                        _stateDirty = true; // the binds and the wizards are persisted
                        const auto [rec, inserted] = _newThreads.try_emplace(b.threadId);
                        auto& nt = rec->second;
                        NewThreadStep backTo = nt.wizard.step;
                        if (inserted)
                        {
                            nt.wizard = b.wizard;
                            nt.topicName = b.title;
                            backTo = NewThreadStepAfterRefusal(nt.wizard, a.outcome.result);
                            ReturnNewThreadToStep(nt.wizard, backTo, L"");
                        }
                        nt.held.insert(nt.held.begin(), b.held.begin(), b.held.end());
                        if (nt.held.size() > kNewThreadHeldMax)
                        {
                            nt.held.resize(kNewThreadHeldMax); // the oldest ones (the bind's own come first)
                        }
                        if (nt.topicName.empty())
                        {
                            nt.topicName = b.title;
                        }
                        nt.lastUseMs = now;
                        logs.push_back(L"[msg] new thread " + std::to_wstring(b.threadId) + L": the window refused session " + ShortId(b.sessionId) + L" in " + b.dir + L" (" +
                             RemoteLaunchResultName(a.outcome.result) + (a.outcome.detail.empty() ? std::wstring{} : L": " + a.outcome.detail) + L"); back to its wizard's " +
                             NewThreadStepName(backTo) + L" step with " + std::to_wstring(nt.held.size()) + L" held message(s)");
                        Notice n;
                        n.threadId = b.threadId;
                        n.html = L"\x26A0 The new session did not start. " + TgEscapeHtml(reason) + L"\nChoose again below, or cancel.";
                        n.replyTo = b.replyTo;
                        n.picker = true;
                        n.reloadModels = inserted && backTo == NewThreadStep::Model;
                        if (const auto h = _bindTapHolds.find(a.token); h != _bindTapHolds.end())
                        {
                            n.tapHolds = std::move(h->second); // §21.28: the refusal and the wizard again answer the tapped button
                            _bindTapHolds.erase(h);
                        }
                        notices.push_back(std::move(n));
                        continue;
                    }
                    // A revive (_requestLaunch).
                    const auto rv = std::find_if(_revives.begin(), _revives.end(), [&a](const Revive& r) { return r.launchToken == a.token; });
                    if (rv != _revives.end())
                    {
                        if (opened)
                        {
                            // What the window opened is what the revive settles on (_sweepRevives, this tick's revive pass).
                            rv->answered = true;
                            rv->openedId = a.outcome.sessionId;
                            rv->alreadyOpen = a.outcome.result == RemoteLaunchResult::AlreadyOpen;
                            rv->fresh = a.outcome.fresh;
                            logs.push_back(L"[msg] launch token=" + std::to_wstring(a.token) + L": the window " + (rv->alreadyOpen ? L"had " : L"opened ") + ShortId(a.outcome.sessionId) +
                                 (rv->fresh ? L" (a fresh session: no transcript to continue)" : L"") + L" for the request of " + ShortId(rv->req.sessionId));
                            continue;
                        }
                        if (!rv->announced && now - a.atMs < kAnnounceWaitMs)
                        {
                            later.push_back(std::move(a)); // _requestLaunch has not queued its own line yet: this goes after it
                            continue;
                        }
                        const std::wstring subject = RemoteLaunchSubject(rv->req, true);
                        logs.push_back(L"[msg] launch token=" + std::to_wstring(a.token) + L": the window refused " + subject + L" (" + RemoteLaunchResultName(a.outcome.result) +
                             (a.outcome.detail.empty() ? std::wstring{} : L": " + a.outcome.detail) + L")");
                        Notice n;
                        n.threadId = rv->threadId;
                        n.html = L"\x26A0 " + TgEscapeHtml(subject) + L" did not open. " + TgEscapeHtml(reason);
                        n.tapHolds = std::move(rv->tapHolds); // §21.28: the refusal answers the tapped launch button
                        notices.push_back(std::move(n));
                        _revives.erase(rv);
                        continue;
                    }
                    // §21.28: a new thread's launch whose bind is gone keeps no hold (a belt: _adoptBoundThreads drops those too).
                    _bindTapHolds.erase(a.token);
                    // Nothing waits for it: the revive or bind gave up on its timeout before the window answered, or the bot was
                    // stopped and started since. The window did what it said; only the line in the chat is missing.
                    logs.push_back(L"[msg] launch token=" + std::to_wstring(a.token) + L" answered " + RemoteLaunchResultName(a.outcome.result) +
                         (a.outcome.sessionId.empty() ? std::wstring{} : L" " + ShortId(a.outcome.sessionId)) + L" after its request was given up: no line in the chat");
                }
            }
            if (!later.empty())
            {
                std::lock_guard lk{ _launchAnswerMtx };
                _launchAnswers.insert(_launchAnswers.begin(), std::make_move_iterator(later.begin()), std::make_move_iterator(later.end()));
            }
            for (const auto& line : logs)
            {
                _log(line);
            }
            for (const auto& n : notices)
            {
                const TapScope tapScope{ n.tapHolds }; // §21.28: what follows answers the tapped launch button, if one waits
                _reply(n.threadId, n.html, false, json::Value{}, n.replyTo);
                if (n.reloadModels)
                {
                    _enterNewThreadModelStep(n.threadId); // §21.25: the model step lists the menu as it is NOW (a preset may be fixed)
                }
                if (n.picker)
                {
                    _showNewThreadPicker(n.threadId); // a NEW picker under the line (the old one went at the launch)
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the answers taken in this pass are lost; their revives and binds wait for their timeouts, which
            // tell the thread.
            LogSwallowedException(L"MessagingService::_settleLaunchAnswers");
        }
    }

    void MessagingService::_expireRevives() noexcept
    {
        try
        {
            std::vector<Revive> expired;
            {
                std::lock_guard lk{ _mtx };
                const int64_t now = WallMs();
                for (auto it = _revives.begin(); it != _revives.end();)
                {
                    if (now - it->requestedMs > kRemoteLaunchTimeoutMs)
                    {
                        expired.push_back(std::move(*it));
                        it = _revives.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            const std::wstring minutes = std::to_wstring(kRemoteLaunchTimeoutMs / 60000);
            for (const auto& rv : expired)
            {
                // The window opened what it said and the session never went live, or the window never answered at all (its UI thread
                // busy all along) — the second may still open later, and its topic then appears on its own.
                const std::wstring subject = RemoteLaunchSubject(rv.req, true);
                _log(L"[msg] launch token=" + std::to_wstring(rv.launchToken) + L" given up after " + minutes + L" min: " + subject +
                     (rv.answered ? L" (the window opened " + ShortId(rv.openedId) + L"; it never went live)" : std::wstring{ L" (no window answered)" }));
                _reply(rv.threadId,
                       rv.answered ? TgEscapeHtml(subject) + L" opened on the PC, but its session did not come up within " + minutes + L" minutes." :
                                     L"The PC did not answer within " + minutes + L" minutes: " + TgEscapeHtml(RemoteLaunchSubject(rv.req, false)) +
                                         L" may still open \x2014 its topic appears if it does.",
                       false);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: those revives are gone without their line; nothing else depends on them.
            LogSwallowedException(L"MessagingService::_expireRevives");
        }
    }

    // ===== a thread the user opens starts a new session (§21.17), through the wizard (§21.25) =======================

    namespace
    {
        // §21.25: the wizard's folder work that may BLOCK — the case-insensitive walk of a typed path (an offline share, a sleeping
        // disk) and the creation of a folder — runs on threads of its own, waited for with a bound: the poll thread reads Telegram
        // and must never wait on a folder longer than the wizard allows (§21.17.4's rule, FolderProbe.h, whose yes / no answer
        // cannot carry a resolution). At most this many run at once: an unreachable server holds each one for the network's
        // timeout, and an ask past the cap answers "no answer" at once and starts nothing.
        constexpr int kNewThreadFsWorkMax = 4;
        // The most entries the listing fallback reads of one folder (a lookup in a case-sensitive folder missed): a folder with
        // more is searched in its first ones only — a miss past them offers a case-variant creation, which Windows then makes or
        // refuses, the user having confirmed it.
        constexpr size_t kNewThreadListEntriesMax = 20000;
        // The folder work running now. Process-wide, not a member: a detached worker may outlive the service that started it.
        std::atomic<int> g_newThreadFsWork{ 0 };
        // A button of a picker the service holds no record of: launched already, cancelled, or its thread deleted.
        constexpr const wchar_t* kNewThreadGone = L"This picker was already used (or its thread has nothing left to set up): send a message in this thread for a new one.";

        /// <summary>
        /// Runs `work` on a detached thread and waits at most `waitMs` for its answer (§21.25: the bounded folder work). A work that
        /// outlasts the wait goes on, and its answer is dropped: the shared slot outlives this call.
        /// </summary>
        /// <typeparam name="T">The answer's type (movable).</typeparam>
        /// <param name="work">The work; it may block; a throw is logged (Rule #18) and reads as no answer.</param>
        /// <param name="waitMs">How long the caller waits.</param>
        /// <returns>The answer; nullopt when none came within the wait, the cap was reached, or no thread could start.</returns>
        template<class T>
        std::optional<T> RunNewThreadFsWork(std::function<T()> work, uint32_t waitMs) noexcept
        {
            struct Slot
            {
                std::mutex m;
                std::condition_variable cv;
                bool done{ false };
                std::optional<T> value;
            };
            try
            {
                const auto slot = std::make_shared<Slot>();
                if (g_newThreadFsWork.fetch_add(1) >= kNewThreadFsWorkMax)
                {
                    g_newThreadFsWork.fetch_sub(1);
                    return std::nullopt; // the cap: slow folders hold enough threads already
                }
                try
                {
                    std::thread([slot, work = std::move(work)]() noexcept {
                        std::optional<T> value;
                        try
                        {
                            value = work();
                        }
                        catch (...)
                        {
                            // Rule #18. Recovery: no answer — the asker reads "did not answer" (or has given up already).
                            LogSwallowedException(L"MessagingService (new-thread folder work)");
                        }
                        try
                        {
                            {
                                std::lock_guard lk{ slot->m };
                                slot->value = std::move(value);
                                slot->done = true;
                            }
                            slot->cv.notify_all();
                        }
                        catch (...)
                        {
                            // Rule #18. Recovery: the asker times out ("did not answer"); the count below is still released.
                            LogSwallowedException(L"MessagingService (new-thread folder work: hand the answer over)");
                        }
                        g_newThreadFsWork.fetch_sub(1);
                    }).detach();
                }
                catch (...)
                {
                    // Rule #18. Recovery: the thread never started — uncount it; no answer.
                    g_newThreadFsWork.fetch_sub(1);
                    LogSwallowedException(L"MessagingService (start new-thread folder work)");
                    return std::nullopt;
                }
                std::unique_lock lk{ slot->m };
                slot->cv.wait_for(lk, std::chrono::milliseconds{ waitMs }, [&slot] { return slot->done; });
                if (!slot->done)
                {
                    return std::nullopt; // it goes on in the background; its answer lands in the slot nobody reads
                }
                return std::move(slot->value);
            }
            catch (...)
            {
                // Rule #18. Recovery: no answer (a thread that did start still records its answer in the slot it holds).
                LogSwallowedException(L"MessagingService (new-thread folder work: wait)");
                return std::nullopt;
            }
        }

        /// <summary>Closes a FindFirstFileExW handle on every path out (a vector that cannot grow throws past the loop).</summary>
        struct NewThreadFindCloser
        {
            HANDLE h{ INVALID_HANDLE_VALUE };
            ~NewThreadFindCloser()
            {
                if (h != INVALID_HANDLE_VALUE)
                {
                    ::FindClose(h);
                }
            }
        };

        /// <summary>
        /// Whether a ROOT is there — a drive ("K:\") or a share ("\\server\share\") — by GetFileAttributesW. BLOCKS as long as the
        /// file system takes (an offline share: the network's timeout): call it through RunNewThreadFsWork.
        /// </summary>
        /// <param name="root">The root, with its trailing separator.</param>
        /// <returns>True when it answers as a folder.</returns>
        bool NewThreadRootExistsNow(const std::wstring& root)
        {
            const DWORD attrs = ::GetFileAttributesW(root.c_str());
            return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }

        /// <summary>
        /// One path's entry by FindFirstFileExW: a Windows lookup ignores case (but in a folder marked case-sensitive) and reports
        /// the name as the DISK spells it — what makes "k:\source" resolve to "K:\source" in its real casing. BLOCKS like
        /// NewThreadRootExistsNow.
        /// </summary>
        /// <param name="path">The path. It holds no wildcard: the wizard refuses * ? &lt; &gt; " before any lookup
        /// (NewThreadFolderNameIllegal), so the lookup finds at most this one entry.</param>
        /// <returns>The entry; `exists` false when nothing is there (or it cannot be read).</returns>
        /// <exception cref="std::bad_alloc">If the name cannot be copied.</exception>
        FolderStat NewThreadStatNow(const std::wstring& path)
        {
            FolderStat st;
            WIN32_FIND_DATAW fd{};
            NewThreadFindCloser find{ ::FindFirstFileExW(path.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0) };
            if (find.h == INVALID_HANDLE_VALUE)
            {
                return st;
            }
            st.exists = true;
            st.directory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            st.name = fd.cFileName;
            return st;
        }

        /// <summary>
        /// The entries of a folder — the walk's fallback when a lookup missed (a case-sensitive folder answers only its exact
        /// spelling). BLOCKS like NewThreadRootExistsNow.
        /// </summary>
        /// <param name="folder">The folder.</param>
        /// <returns>Its entries ("." and ".." left out; at most kNewThreadListEntriesMax); nullopt when it cannot be listed.</returns>
        /// <exception cref="std::bad_alloc">If the list cannot grow.</exception>
        std::optional<std::vector<FolderStat>> NewThreadListNow(const std::wstring& folder)
        {
            WIN32_FIND_DATAW fd{};
            const std::wstring pattern = JoinFolderPath(folder, L"*");
            NewThreadFindCloser find{ ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH) };
            if (find.h == INVALID_HANDLE_VALUE)
            {
                return std::nullopt;
            }
            std::vector<FolderStat> entries;
            do
            {
                const std::wstring_view name{ fd.cFileName };
                if (name == L"." || name == L"..")
                {
                    continue;
                }
                FolderStat st;
                st.exists = true;
                st.directory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                st.name = std::wstring{ name };
                entries.push_back(std::move(st));
            } while (entries.size() < kNewThreadListEntriesMax && ::FindNextFileW(find.h, &fd));
            return entries;
        }

        /// <summary>The real file system for ResolveNewThreadFolder (each call blocking: run the walk through RunNewThreadFsWork).</summary>
        /// <returns>The three functions over Win32.</returns>
        FolderFs RealNewThreadFolderFs()
        {
            FolderFs fs;
            fs.rootExists = &NewThreadRootExistsNow;
            fs.stat = &NewThreadStatNow;
            fs.list = &NewThreadListNow;
            return fs;
        }

        /// <summary>Releases a FormatMessageW buffer on every path out.</summary>
        struct NewThreadLocalFreer
        {
            wchar_t* p{ nullptr };
            ~NewThreadLocalFreer()
            {
                if (p)
                {
                    ::LocalFree(p);
                }
            }
        };

        /// <summary>Windows' own words for an error ("Access is denied.") with its code; "Windows refused it" when it has none.</summary>
        /// <param name="error">The GetLastError code.</param>
        /// <returns>The text, plain.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring NewThreadWindowsErrorText(DWORD error)
        {
            NewThreadLocalFreer buffer;
            const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                                             reinterpret_cast<wchar_t*>(&buffer.p), 0, nullptr);
            std::wstring text = n > 0 && buffer.p ? std::wstring{ buffer.p, n } : std::wstring{};
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            {
                text.pop_back();
            }
            return (text.empty() ? std::wstring{ L"Windows refused it" } : text) + L" (error " + std::to_wstring(error) + L")";
        }

        /// <summary>
        /// Makes the missing levels of a typed folder, outermost first (CreateDirectoryW, §21.25's CreateFolder step). A level that
        /// exists by now — made by someone else, or by an earlier Create whose answer came too late — counts as made. BLOCKS like
        /// NewThreadRootExistsNow.
        /// </summary>
        /// <param name="existingPrefix">The deepest level that existed at the check (in the disk's spelling).</param>
        /// <param name="missing">The levels to make, outermost first (validated names: NewThreadFolderNameProblem).</param>
        /// <returns>"" when the whole folder exists now; else why not, plain (Windows' words and the level).</returns>
        /// <exception cref="std::bad_alloc">If a path cannot be allocated.</exception>
        std::wstring CreateNewThreadFolderNow(const std::wstring& existingPrefix, const std::vector<std::wstring>& missing)
        {
            std::wstring path = existingPrefix;
            for (const auto& level : missing)
            {
                path = JoinFolderPath(path, level);
                if (::CreateDirectoryW(path.c_str(), nullptr))
                {
                    continue;
                }
                const DWORD error = ::GetLastError();
                const DWORD attrs = ::GetFileAttributesW(path.c_str());
                if (error == ERROR_ALREADY_EXISTS && attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    continue; // made meanwhile: the level is there, as asked
                }
                if (error == ERROR_ALREADY_EXISTS)
                {
                    return L"a file is in the way: " + path;
                }
                return NewThreadWindowsErrorText(error) + L" \x2014 " + path;
            }
            const DWORD attrs = ::GetFileAttributesW(path.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                return L"the folder is not there after its creation: " + path;
            }
            return {};
        }

        /// <summary>The thread an outbox item posts into (its message_thread_id); 0 = none (the main view, or not a post).</summary>
        /// <param name="params">The item's parameters.</param>
        /// <returns>The thread id.</returns>
        int64_t NewThreadOutItemThread(const json::Value& params) noexcept
        {
            const auto* th = params.Find(L"message_thread_id");
            return th && th->type == json::Value::Type::Num ? static_cast<int64_t>(th->num) : 0;
        }
    }

    void MessagingService::_onUserThreadCreated(int64_t threadId, const json::Value& msg)
    {
        // The name the user gave the thread becomes the session's title; one Telegram made up (is_name_implicit: the user only
        // typed a message, and the app named the thread after it) does not — the folder's name is the title then, and renaming
        // the thread later renames the session.
        std::wstring name;
        bool implicit = true;
        if (const auto* created = msg.Find(L"forum_topic_created"); created && created->type == json::Value::Type::Obj)
        {
            name = StripTopicNameDecoration(created->StrAt(L"name"));
            implicit = created->BoolAt(L"is_name_implicit", false);
        }
        {
            std::lock_guard lk{ _mtx };
            auto& nt = _newThreads[threadId];
            nt.topicName = implicit ? std::wstring{} : name;
            nt.lastUseMs = WallMs();
            _stateDirty = true; // §21.25: the wizards are persisted
        }
        // A thread Telegram named (is_name_implicit) was made from the user's first message, which comes right after this service
        // message: the picker goes up with it (_holdForNewThread), one post instead of a post and an edit that counts the message.
        // A thread the user named may have no message yet: its picker goes up now.
        _log(L"[msg] new thread " + std::to_wstring(threadId) + L" opened by the user" + (implicit ? std::wstring{ L" (an implicit name): its folder picker goes up with the first message" } : L" \x201C" + name + L"\x201D: its folder picker goes up"));
        LogNav(L"remote telegram new-thread " + std::to_wstring(threadId));
        if (!implicit)
        {
            _showNewThreadPicker(threadId);
        }
    }

    bool MessagingService::_onNewThreadRenamed(int64_t threadId, const std::wstring& name)
    {
        const std::wstring title = StripTopicNameDecoration(name);
        bool known = false;
        bool wizard = false;
        bool changed = false;
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _newThreads.find(threadId); it != _newThreads.end())
            {
                known = true;
                wizard = true;
                if (!title.empty() && it->second.topicName != title)
                {
                    it->second.topicName = title;
                    it->second.lastUseMs = WallMs();
                    changed = true;
                }
            }
            for (auto& b : _threadBinds)
            {
                if (b.threadId != threadId)
                {
                    continue;
                }
                known = true;
                if (!title.empty() && b.title != title)
                {
                    b.title = title; // the bind renames the session to it once the session is live (_adoptBoundThreads)
                    changed = true;
                }
            }
            if (changed)
            {
                _stateDirty = true;
            }
        }
        if (!known)
        {
            return false; // a thread the service knows nothing about: nothing to name
        }
        if (changed)
        {
            _log(L"[msg] new thread " + std::to_wstring(threadId) + L" renamed by the user \x2192 \x201C" + title + L"\x201D (the coming session's title)");
            LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + L" renamed");
            if (wizard)
            {
                _showNewThreadPicker(threadId); // the folder step names the title
            }
        }
        return true;
    }

    void MessagingService::_holdForNewThread(int64_t threadId, const json::Value& msg, bool edited)
    {
        const std::wstring text = msg.StrAt(L"text");
        const int64_t msgId = msg.I64At(L"message_id", 0);
        const int64_t now = WallMs();
        if (edited)
        {
            // An edit of a message the thread still holds replaces it (it has not reached any session yet); an edit of anything
            // else is left alone — a message that already went to the session answers that itself, a stray one needs nothing, and a
            // CHOICE the user typed (a folder, a model, a window number) was taken when it came.
            std::lock_guard lk{ _mtx };
            const auto replace = [&](std::vector<json::Value>& held) {
                for (auto& h : held)
                {
                    if (h.I64At(L"message_id", 0) == msgId)
                    {
                        h = msg;
                        return true;
                    }
                }
                return false;
            };
            if (const auto it = _newThreads.find(threadId); it != _newThreads.end() && replace(it->second.held))
            {
                it->second.lastUseMs = now;
                _stateDirty = true; // §21.25: the held messages are persisted with the wizard
                return;
            }
            for (auto& b : _threadBinds)
            {
                if (b.threadId == threadId && replace(b.held))
                {
                    _stateDirty = true;
                    return;
                }
            }
            return;
        }
        // A thread whose pick is launching, or whose bind is handing its messages over: the message waits for that session too.
        {
            bool bound = false;
            bool full = false;
            // §21.17: the session that owns the thread now — its bind became the thread's topic, and its hand-over ended, after
            // the caller looked for a topic (_handleMessage). Read in the SAME lock scope as the binds: a bind leaves the list
            // only after its topic is recorded (_adoptBoundThreads), so this scope sees the bind or the topic, never neither —
            // which is what posted a stray folder picker in a just-bound topic before 2026-10-18.
            std::wstring topicSession;
            {
                std::lock_guard lk{ _mtx };
                for (auto& b : _threadBinds)
                {
                    if (b.threadId == threadId)
                    {
                        bound = true;
                        full = b.held.size() >= kNewThreadHeldMax;
                        if (!full)
                        {
                            b.held.push_back(msg);
                            _stateDirty = true; // §21.25: the binds are persisted
                        }
                        break;
                    }
                }
                if (!bound && threadId != 0)
                {
                    for (const auto& [sid, t] : _state.topics)
                    {
                        if (t.threadId == threadId)
                        {
                            topicSession = sid;
                            break;
                        }
                    }
                }
            }
            if (bound)
            {
                if (full)
                {
                    _reply(threadId, L"The new session is starting; send this again once it is up.", false, json::Value{}, msgId);
                }
                else
                {
                    _react(msgId, L"\U0001F440"); // seen: it goes to the session once the session is up
                }
                return;
            }
            if (!topicSession.empty())
            {
                _handleTopicMessage(topicSession, msg, threadId); // the thread's topic now: as if the caller had found it
                return;
            }
        }
        // §21.25: the thread's WIZARD reads the message first (ApplyNewThreadText) — a lone folder path at every step, a model id
        // at the model steps, a window number at the window step; anything else is the user's own words, held for the session.
        // A thread the bot never saw (a thread made before §21.17, or one whose session closed while the thread stayed) gets its
        // record here, at the folder step.
        NewThreadOutcome out;
        bool full = false;
        NewThreadStep step = NewThreadStep::Folder;
        {
            std::lock_guard lk{ _mtx };
            auto& nt = _newThreads[threadId];
            nt.lastUseMs = now;
            step = nt.wizard.step;
            out = ApplyNewThreadText(nt, text);
            if (out.kind == NewThreadOutcome::Kind::Hold)
            {
                full = nt.held.size() >= kNewThreadHeldMax;
                if (!full)
                {
                    nt.held.push_back(msg);
                }
            }
            _stateDirty = true;
        }
        if (out.kind != NewThreadOutcome::Kind::Hold)
        {
            // A CHOICE typed as a message: carried out like a button's, the picker re-posted below the message (where the user looks).
            LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + L" typed a choice at the " + NewThreadStepName(step) + L" step");
            _applyNewThreadOutcome(threadId, out, msgId);
            return;
        }
        if (full)
        {
            _reply(threadId, L"This thread holds " + std::to_wstring(kNewThreadHeldMax) + L" messages already: finish the steps above first.", false, json::Value{}, msgId);
            return;
        }
        _react(msgId, L"\U0001F440"); // seen: it waits for the session
        LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + L" held chars=" + std::to_wstring(text.size()));
        _showNewThreadPicker(threadId); // posts the picker, or edits it to count what it holds (and to show a notice the text set)
    }

    void MessagingService::_showNewThreadPicker(int64_t threadId, bool repost)
    {
        bool needLists = false;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return;
            }
            // The lists are the folder step's only: a wizard past it never pays for them (a refused launch's record has none).
            needLists = it->second.wizard.step == NewThreadStep::Folder && !it->second.listsBuilt;
        }
        // The lists, once per picker and outside the lock: a registry copy of where every session worked, and one probe per
        // listed folder (a folder deleted since is not offered). A UNC path is kept unprobed — an offline share would hold the
        // poll thread for the network's timeout; the pick checks it again. Every other folder is probed with a BOUND
        // (FolderProbe.h, §21.17.4), since a drive letter mapped to a share, or a removable or sleeping disk, can hold a probe
        // just as long: each waits at most kNewThreadListProbeEachMs and all of them together at most kNewThreadListProbeBudgetMs,
        // an answer up to kNewThreadListProbeMaxAgeMs old is taken as it is, and a folder that did not answer is listed like a
        // UNC one (the pick checks it; its probe goes on in the background and leaves its answer for the next picker).
        NewThreadLists lists;
        if (needLists)
        {
            std::vector<SessionFolderFacts> facts;
            for (const auto& sample : _registry->PlaceSamples())
            {
                facts.push_back(SessionFolderFacts{ sample.workingDir, sample.inferredWorkingDir, sample.lastActivityUnixMs });
            }
            std::vector<std::wstring> scratchRoots;
            try
            {
                for (const auto& root : CollectMachineTempRoots())
                {
                    scratchRoots.push_back(NormDirKey(TrimFolderPath(root)));
                }
            }
            catch (...)
            {
                // Rule #18. Recovery: no scratch roots — a temp folder a session ran in may be offered; nothing else changes.
                LogSwallowedException(L"MessagingService::_showNewThreadPicker (temp roots)");
            }
            const uint64_t probeStartTick = ::GetTickCount64(); // the list's whole budget counts from here
            lists = BuildNewThreadLists(facts, scratchRoots, [probeStartTick](const std::wstring& path) {
                if (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/'))
                {
                    return true;
                }
                // What is left of the budget, at most one folder's share; 0 once it is spent (the probe only starts then).
                const uint64_t spentMs = ::GetTickCount64() - probeStartTick;
                const uint32_t waitMs = spentMs >= kNewThreadListProbeBudgetMs ?
                                            0 :
                                            static_cast<uint32_t>((std::min<uint64_t>)(kNewThreadListProbeEachMs, kNewThreadListProbeBudgetMs - spentMs)); // (std::min): the <windows.h> macro
                return SharedFolderProber().Probe(path, waitMs, kNewThreadListProbeMaxAgeMs) != FolderProbeAnswer::NotFolder;
            });
        }
        std::wstring html;
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
        int64_t pickerId = 0;
        int64_t replacedId = 0; // a repost: the picker above the user's message, deleted before the new one goes
        bool kickCensus = false;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return; // launched or cancelled while the lists were built
            }
            auto& nt = it->second;
            if (needLists && !nt.listsBuilt)
            {
                nt.lists = std::move(lists);
                nt.listsBuilt = true;
                _stateDirty = true; // persisted: a tap after a restart maps to the list it saw
            }
            if (nt.pickerMessageId < 0)
            {
                nt.pickerStale = true; // the post is in flight: its answer shows this state (_showNewThreadPicker again)
                return;
            }
            const NewThreadView view = BuildNewThreadView(nt, WallMs());
            html = view.html;
            for (const auto& row : view.rows)
            {
                std::vector<std::pair<std::wstring, std::wstring>> r;
                for (const auto& b : row)
                {
                    r.emplace_back(b.text, b.data);
                }
                rows.push_back(std::move(r));
            }
            if (repost && nt.pickerMessageId > 0)
            {
                replacedId = nt.pickerMessageId;
                nt.pickerMessageId = 0;
                _stateDirty = true;
            }
            pickerId = nt.pickerMessageId;
            if (pickerId == 0)
            {
                nt.pickerMessageId = -1;
            }
            nt.pickerStale = false;
            // §21.25: a window step waiting for a census nobody runs (a record restored from the state file, a census lost to a
            // stop): this render shows "looking at the windows…", so one is started now.
            kickCensus = nt.wizard.step == NewThreadStep::Window && !nt.wizard.windowsCollected && nt.wizard.windowsSerial == 0;
        }
        if (replacedId > 0)
        {
            // Queued before the new picker (one priority, one order): the thread never shows two pickers for long.
            OutItem del;
            del.prio = SendPriority::Final;
            del.method = L"deleteMessage";
            del.params = json::Value::MkObj();
            del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(replacedId)));
            _enqueue(std::move(del));
        }
        auto self = weak_from_this();
        if (pickerId > 0)
        {
            // In place: a page change, one more held message, a step a button reached. Keyed per picker, so a newer render replaces
            // one still queued. §21.25: the answer says when the picker or the thread is GONE (the user deleted it).
            _editText(pickerId, html, SendPriority::Final, L"ntp:" + std::to_wstring(threadId), _buttons(rows), [self, threadId, pickerId](const TgResult& r) {
                auto me = self.lock();
                if (!me || r.ok || r.NotModified())
                {
                    return;
                }
                if (r.ThreadNotFound())
                {
                    // The user deleted the thread: nothing is left to set up (Telegram tells a bot nothing when a topic goes).
                    bool dropped = false;
                    {
                        std::lock_guard lk{ me->_mtx };
                        dropped = me->_newThreads.erase(threadId) > 0;
                        me->_stateDirty = me->_stateDirty || dropped;
                    }
                    if (dropped)
                    {
                        me->_log(L"[msg] new thread " + std::to_wstring(threadId) + L": the thread is gone (" + r.description + L"); its wizard is dropped");
                    }
                    return;
                }
                if (HeldMessageGone(r.ok, r.httpStatus, r.description))
                {
                    // The picker message is gone ("message to edit not found": the user deleted it): a new one is posted.
                    bool again = false;
                    {
                        std::lock_guard lk{ me->_mtx };
                        if (const auto it = me->_newThreads.find(threadId); it != me->_newThreads.end() && it->second.pickerMessageId == pickerId)
                        {
                            it->second.pickerMessageId = 0;
                            me->_stateDirty = true;
                            again = true;
                        }
                    }
                    if (again)
                    {
                        me->_log(L"[msg] new thread " + std::to_wstring(threadId) + L": its picker is gone (" + r.description + L"); a new one is posted");
                        me->_showNewThreadPicker(threadId);
                    }
                    return;
                }
                me->_log(L"[msg] new thread " + std::to_wstring(threadId) + L": the picker's edit was refused (" + r.description + L")");
            });
        }
        else
        {
            _sendText(threadId, html, true, SendPriority::Final, L"", _buttons(rows), [self, threadId](const TgResult& r) {
                auto me = self.lock();
                if (!me)
                {
                    return;
                }
                const int64_t id = r.ok ? r.result.I64At(L"message_id", 0) : 0;
                bool again = false;
                bool orphaned = false;
                bool threadGone = false;
                {
                    std::lock_guard lk{ me->_mtx };
                    if (const auto it = me->_newThreads.find(threadId); it != me->_newThreads.end())
                    {
                        if (!r.ok && r.ThreadNotFound())
                        {
                            // §21.25: the user deleted the thread — nothing is left to set up.
                            me->_newThreads.erase(it);
                            threadGone = true;
                        }
                        else
                        {
                            it->second.pickerMessageId = id; // 0 = refused: the next render posts it again
                            again = id > 0 && it->second.pickerStale;
                        }
                        me->_stateDirty = true; // the picker's id is persisted (its buttons work after a restart)
                    }
                    else
                    {
                        orphaned = id > 0; // launched or cancelled while this post flew: it shows nothing anyone needs
                    }
                }
                if (threadGone)
                {
                    me->_log(L"[msg] new thread " + std::to_wstring(threadId) + L": the thread is gone (" + r.description + L"); its wizard is dropped");
                    return;
                }
                if (!r.ok)
                {
                    me->_log(L"[msg] new thread " + std::to_wstring(threadId) + L": the folder picker was refused (" + r.description + L")");
                }
                if (orphaned)
                {
                    OutItem del;
                    del.prio = SendPriority::Final;
                    del.method = L"deleteMessage";
                    del.params = json::Value::MkObj();
                    del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(me->_ownerId)));
                    del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(id)));
                    me->_enqueue(std::move(del));
                }
                if (again)
                {
                    me->_showNewThreadPicker(threadId);
                }
            });
        }
        if (kickCensus)
        {
            _collectNewThreadWindows(threadId, 0);
        }
    }

    std::wstring MessagingService::_onNewThreadAction(const NewThreadAction& action, int64_t threadId)
    {
        // §21.25: the wizard decides (ApplyNewThreadButton: a button of an earlier step changes nothing), the dispatcher acts.
        NewThreadOutcome out;
        NewThreadStep step = NewThreadStep::Folder;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return kNewThreadGone;
            }
            step = it->second.wizard.step;
            it->second.lastUseMs = WallMs(); // every tap counts as a use (the idle abort counts from the last one)
            out = ApplyNewThreadButton(it->second, action);
            _stateDirty = true;
        }
        if (out.kind != NewThreadOutcome::Kind::Stale && out.kind != NewThreadOutcome::Kind::Toast)
        {
            LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + L" button at the " + NewThreadStepName(step) + L" step");
        }
        return _applyNewThreadOutcome(threadId, out, 0);
    }

    std::wstring MessagingService::_applyNewThreadOutcome(int64_t threadId, const NewThreadOutcome& out, int64_t replyTo)
    {
        using Kind = NewThreadOutcome::Kind;
        switch (out.kind)
        {
        case Kind::Stale:
        case Kind::Toast:
            return out.text;
        case Kind::Hold:
            return {}; // the caller holds the message (it owns it)
        case Kind::Render:
            _showNewThreadPicker(threadId, replyTo != 0);
            return {};
        case Kind::CheckFolder:
            return _checkNewThreadFolder(threadId, out.path, out.typed, replyTo);
        case Kind::CreateFolder:
            return _createNewThreadFolder(threadId, out.path);
        case Kind::EnterModel:
            _enterNewThreadModelStep(threadId);
            _showNewThreadPicker(threadId, replyTo != 0);
            return {};
        case Kind::CollectWindows:
            if (!out.rememberModel.empty())
            {
                // A TYPED model id joins the "Specify a model" droplist and becomes its pre-fill — the PC prompt's own rule, so the
                // next wizard (and the PC's next prompt) offers it in one tap.
                try
                {
                    RememberSpecifiedModel(out.rememberModel);
                }
                catch (...)
                {
                    // Rule #18. Recovery: the id is not remembered; this launch uses it all the same.
                    LogSwallowedException(L"MessagingService::_applyNewThreadOutcome (remember the model)");
                }
            }
            _collectNewThreadWindows(threadId, replyTo);
            return {};
        case Kind::Launch:
            return _launchNewThread(threadId, replyTo);
        case Kind::Abort:
            _abortNewThread(threadId, L"cancelled by the user", true);
            return L"Cancelled \x2014 the thread is deleted.";
        case Kind::Reply:
            _reply(threadId, TgEscapeHtml(out.text), false, json::Value{}, replyTo);
            return out.text;
        default:
            return {};
        }
    }

    std::wstring MessagingService::_checkNewThreadFolder(int64_t threadId, const std::wstring& path, bool typed, int64_t replyTo)
    {
        FolderResolution res;
        if (!typed)
        {
            // A LIST pick (§21.17.4): the bounded probe a pick always made — a FRESH answer (one still running from the picker's
            // lists is joined, never doubled), waited for at most kNewThreadPickProbeWaitMs. The list's own spelling stands.
            const FolderProbeAnswer probe = SharedFolderProber().Probe(path, kNewThreadPickProbeWaitMs, 0);
            res.path = path;
            if (probe == FolderProbeAnswer::Folder)
            {
                res.kind = FolderResolveKind::Exists;
            }
            else if (probe == FolderProbeAnswer::NotFolder)
            {
                res.kind = FolderResolveKind::Missing; // gone since it was listed: the lists are read again, nothing is offered
            }
            else
            {
                res.kind = FolderResolveKind::NoAnswer;
                res.reason = NewThreadFolderNoAnswerText(path);
            }
        }
        else
        {
            // A TYPED path (§21.25): the whole case-insensitive walk — the disk's spelling of what exists, what a Create would
            // make, or why not — on a worker of its own, waited for at most kNewThreadPickProbeWaitMs (an offline share holds a
            // lookup for the network's timeout; the walk goes on in the background and its answer is dropped).
            const FolderFs fs = RealNewThreadFolderFs();
            const std::wstring typedPath = path;
            if (auto answer = RunNewThreadFsWork<FolderResolution>([typedPath, fs]() { return ResolveNewThreadFolder(typedPath, fs); }, kNewThreadPickProbeWaitMs))
            {
                res = std::move(*answer);
            }
            else
            {
                res.kind = FolderResolveKind::NoAnswer;
                res.path = TrimFolderPath(path);
                res.reason = NewThreadFolderNoAnswerText(res.path);
            }
        }
        NewThreadOutcome next;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return kNewThreadGone; // cancelled or launched while the folder was checked
            }
            next = ApplyNewThreadFolderResolved(it->second, res, typed);
            it->second.lastUseMs = WallMs();
            _stateDirty = true;
        }
        static constexpr const wchar_t* kKinds[] = { L"exists", L"missing", L"a file", L"no root", L"invalid", L"no answer" };
        _log(L"[msg] new thread " + std::to_wstring(threadId) + L": " + (typed ? L"typed" : L"picked") + L" folder " + path + L" \x2192 " +
             kKinds[static_cast<size_t>(res.kind) < std::size(kKinds) ? static_cast<size_t>(res.kind) : 4] + (res.path.empty() || res.path == path ? std::wstring{} : L" (" + res.path + L")"));
        if (next.kind == NewThreadOutcome::Kind::EnterModel)
        {
            _enterNewThreadModelStep(threadId);
        }
        _showNewThreadPicker(threadId, replyTo != 0);
        if (next.kind == NewThreadOutcome::Kind::EnterModel || (typed && res.kind == FolderResolveKind::Missing))
        {
            return {}; // the step moved on (the model step, or "create it?"): the picker says so
        }
        return res.kind == FolderResolveKind::Missing ? std::wstring{ L"That folder is gone \x2014 the lists are read again." } : res.reason;
    }

    std::wstring MessagingService::_createNewThreadFolder(int64_t threadId, const std::wstring& path)
    {
        std::wstring prefix;
        std::vector<std::wstring> missing;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return kNewThreadGone;
            }
            const auto& w = it->second.wizard;
            prefix = w.createExistingPrefix;
            missing = w.createMissing;
        }
        if (missing.empty() || prefix.empty())
        {
            return kNewThreadStaleTap; // nothing to make: the step was left meanwhile
        }
        // Bounded like every folder call of the poll thread: a creation on a slow drive goes on in the background, and a second
        // Create finds what it made (an existing level counts as made).
        std::wstring error;
        bool ok = false;
        if (auto answer = RunNewThreadFsWork<std::wstring>([prefix, missing]() { return CreateNewThreadFolderNow(prefix, missing); }, kNewThreadCreateWaitMs))
        {
            ok = answer->empty();
            error = std::move(*answer);
        }
        else
        {
            error = L"the creation did not finish within " + std::to_wstring(kNewThreadCreateWaitMs / 1000) + L" s (a slow or offline drive?) \x2014 tap Create again to check";
        }
        NewThreadOutcome next;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return kNewThreadGone;
            }
            next = ApplyNewThreadFolderCreated(it->second, ok, error);
            it->second.lastUseMs = WallMs();
            _stateDirty = true;
        }
        _log(L"[msg] new thread " + std::to_wstring(threadId) + L": " + (ok ? L"created the folder " + path : L"could not create the folder " + path + L": " + error));
        LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + (ok ? L" created-folder" : L" create-folder-failed"));
        if (next.kind == NewThreadOutcome::Kind::EnterModel)
        {
            _enterNewThreadModelStep(threadId);
        }
        _showNewThreadPicker(threadId);
        return ok ? L"Created " + path : L"Not created: " + error;
    }

    void MessagingService::_enterNewThreadModelStep(int64_t threadId)
    {
        LaunchMenuSource source;
        std::wstring spec;
        std::wstring settingsModel;
        {
            std::lock_guard lk{ _mtx };
            source = _launchMenuSource;
            spec = _launchModelsSpec;
            settingsModel = _settingsModel;
        }
        // Outside the lock: the menu reads the presets file (stamp-cached), the droplist reads model-list.json.
        LaunchMenuInfo info;
        if (source)
        {
            try
            {
                info = source(spec);
            }
            catch (...)
            {
                // Rule #18. Recovery: the step offers Default, the last typed model and ✏ Specify… only.
                LogSwallowedException(L"MessagingService::_enterNewThreadModelStep (the launch menu)");
            }
        }
        std::wstring last;
        try
        {
            last = LoadLastSpecifiedModel();
        }
        catch (...)
        {
            // Rule #18. Recovery: no "last typed" choice.
            LogSwallowedException(L"MessagingService::_enterNewThreadModelStep (the last specified model)");
        }
        std::vector<NewThreadModelChoice> choices = BuildNewThreadModelChoices(info.entries, last);
        std::lock_guard lk{ _mtx };
        if (const auto it = _newThreads.find(threadId); it != _newThreads.end())
        {
            EnterNewThreadModelStep(it->second.wizard, std::move(choices), info.offerAddPresets, settingsModel);
            _stateDirty = true;
        }
    }

    void MessagingService::_collectNewThreadWindows(int64_t threadId, int64_t replyTo)
    {
        uint64_t serial = 0;
        std::wstring folder;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end() || it->second.wizard.step != NewThreadStep::Window)
            {
                return;
            }
            auto& w = it->second.wizard;
            serial = ++_censusSerial;
            w.windowsSerial = serial; // a census asked before this one is stale from now on
            w.windowsCollected = false;
            folder = w.folder;
        }
        _showNewThreadPicker(threadId, replyTo != 0); // "looking at the open windows…" (a keyed edit: a fast answer replaces it)
        if (!_hooks.windowCensus)
        {
            _applyNewThreadWindows(threadId, serial, NewThreadWindowPlan{}); // no window step in this host: the engine's choice
            return;
        }
        // The census BLOCKS until every window answered from its UI thread (up to kNewThreadCensusTimeoutMs): a worker of its own,
        // holding the service weakly. Its answer is applied only while the serial still matches (_applyNewThreadWindows).
        auto self = weak_from_this();
        const auto census = _hooks.windowCensus;
        const auto registry = _registry;
        try
        {
            // §21.28: the worker carries the tapped model button along (`taps`; empty for a typed model id), so the button stays busy
            // until the window step's list — or the launch it skips to — is in the thread.
            std::thread([self, threadId, serial, folder, census, registry, taps = _holdTaps()]() noexcept {
                NewThreadWindowPlan plan;
                try
                {
                    std::vector<WindowCensus> windows;
                    try
                    {
                        windows = census(kNewThreadCensusTimeoutMs);
                    }
                    catch (...)
                    {
                        // Rule #18. Recovery: no window answered — the step is skipped (or says so, after a refused launch).
                        LogSwallowedException(L"MessagingService (new-thread window census)");
                    }
                    // The registry's facts per tab (the census reports only each window's strip): where each session works and how
                    // new it is. A lean copy (no queues, no transcript texts).
                    std::unordered_map<std::wstring, NewThreadTabFacts> facts;
                    for (const auto& s : registry->SnapshotLiveForCards())
                    {
                        NewThreadTabFacts f;
                        f.dirKey = NormDirKey(TrimFolderPath(s.workingDir));
                        f.worksInKey = s.inferredWorkingDir.empty() ? std::wstring{} : NormDirKey(TrimFolderPath(s.inferredWorkingDir));
                        // The creation stamp; a session from before it falls back to its transcript's age (the observer's).
                        f.createdMs = s.createdUnixMs > 0 ? s.createdUnixMs : s.convCreatedUnixMs;
                        f.recentMs = MostRecentSortKeyMs(s);
                        facts.emplace(s.id, std::move(f));
                    }
                    plan = BuildNewThreadWindowPlan(windows, facts, NormDirKey(TrimFolderPath(folder)), NewThreadFolderLeaf(folder));
                }
                catch (...)
                {
                    // Rule #18. Recovery: an empty plan — the step goes on as if no window answered (skipped to the engine's choice,
                    // or "no window answered" after a refused launch), instead of waiting for an answer that never comes.
                    LogSwallowedException(L"MessagingService (new-thread window step)");
                    plan = NewThreadWindowPlan{};
                }
                if (const auto me = self.lock())
                {
                    const TapScope tapScope{ taps }; // §21.28: never throws; the step's render (or launch) holds the tap
                    me->_applyNewThreadWindows(threadId, serial, std::move(plan)); // never throws (it contains its own failures)
                }
            }).detach();
        }
        catch (...)
        {
            // Rule #18. Recovery: no thread for the census — the step goes on as if no window answered.
            LogSwallowedException(L"MessagingService::_collectNewThreadWindows (start the census)");
            _applyNewThreadWindows(threadId, serial, NewThreadWindowPlan{});
        }
    }

    void MessagingService::_applyNewThreadWindows(int64_t threadId, uint64_t serial, NewThreadWindowPlan plan)
    {
        try
        {
            if (_stop.load())
            {
                return; // the bot stopped while the windows answered: the restored record asks again at the next start
            }
            NewThreadOutcome out;
            size_t windows = 0;
            int defaultWindow = -1;
            {
                std::lock_guard lk{ _mtx };
                const auto it = _newThreads.find(threadId);
                if (it == _newThreads.end())
                {
                    return; // cancelled or launched meanwhile
                }
                auto& w = it->second.wizard;
                if (w.step != NewThreadStep::Window || w.windowsSerial != serial)
                {
                    return; // stale: the step moved on, or a newer census was asked (🔄 Refresh)
                }
                out = ApplyNewThreadWindows(w, std::move(plan));
                windows = w.windows.size();
                defaultWindow = w.defaultWindow;
                _stateDirty = true;
            }
            _log(L"[msg] new thread " + std::to_wstring(threadId) + L": " + std::to_wstring(windows) + L" window(s) answered" +
                 (defaultWindow >= 0 ? L", default window " + std::to_wstring(defaultWindow + 1) : std::wstring{}) +
                 (out.kind == NewThreadOutcome::Kind::Launch ? std::wstring{ L"; the window step is skipped" } : std::wstring{}));
            if (out.kind == NewThreadOutcome::Kind::Launch)
            {
                _launchNewThread(threadId, 0);
            }
            else
            {
                _showNewThreadPicker(threadId);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the step keeps its last state; 🔄 Refresh asks the windows again.
            LogSwallowedException(L"MessagingService::_applyNewThreadWindows");
        }
    }

    std::wstring MessagingService::_launchNewThread(int64_t threadId, int64_t replyTo)
    {
        if (!_hooks.launch && !_hooks.launchAnswered)
        {
            const std::wstring none = L"Opening sessions is not available in this build.";
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _newThreads.find(threadId); it != _newThreads.end())
                {
                    ReturnNewThreadToStep(it->second.wizard, NewThreadStep::Model, L"\x26A0 " + none);
                    _stateDirty = true;
                }
            }
            _showNewThreadPicker(threadId);
            return none;
        }
        // The bind goes in BEFORE the launch: the launch is asynchronous (the window's UI thread opens the tab), and the tick may
        // see the session live at any moment after it. §21.23: so does the window's ANSWER, which finds the bind by its token. The
        // wizard's RECORD leaves the map in the same lock scope (the bind carries its choices, §21.25): a refusal the window
        // answers with puts a NEW record in its place (_settleLaunchAnswers, on the tick thread), and an erase after the launch
        // returned could remove that one. A launch no window takes puts the record back.
        NewThreadLaunchPlan plan;
        ThreadBind bind;
        int64_t pickerId = 0;
        std::optional<NewThread> picker; // the thread's record, out of the map while the launch is asked for
        // §21.28: a tapped window (or model) button stays busy until the window answers the launch — an opened tab, or a refusal and
        // the wizard again (_settleLaunchAnswers); the holds wait in `_bindTapHolds` under the launch's token. Empty for a typed
        // choice. Taken before the lock (it allocates); unused, they go with this function.
        std::vector<TapHoldRef> taps = _holdTaps();
        {
            std::lock_guard lk{ _mtx };
            // One launch per thread at a time: a second launch while one is starting would start a second session for the thread.
            if (std::any_of(_threadBinds.begin(), _threadBinds.end(), [threadId](const ThreadBind& b) { return b.threadId == threadId; }))
            {
                return L"A new session is already starting in this thread\x2026";
            }
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return kNewThreadGone;
            }
            plan = BuildNewThreadLaunchPlan(it->second);
            bind.sessionId = NewSessionId();
            bind.threadId = threadId;
            bind.dir = plan.dir;
            bind.title = plan.title; // "" = the folder-derived default title
            bind.held = it->second.held; // the bind holds the messages now
            bind.requestedMs = WallMs();
            bind.replyTo = replyTo;
            bind.wizard = it->second.wizard;
            pickerId = it->second.pickerMessageId;
            picker = std::move(it->second);
            _newThreads.erase(it);
            bind.launchToken = ++_launchTokenSeq;
            _threadBinds.push_back(bind);
            if (!taps.empty())
            {
                _bindTapHolds[bind.launchToken] = std::move(taps); // §21.28: in the bind's own lock scope (the answer may come at once)
            }
            _stateDirty = true;
        }
        RemoteLaunchRequest req;
        req.kind = RemoteLaunchRequest::Kind::NewSession;
        req.dir = plan.dir;
        req.title = plan.title;
        req.newSessionId = bind.sessionId;
        req.model = plan.model; // "" = Default; a model id (checked when typed: ParseNewThreadModelId); "@<presetId>"
        req.windowId = plan.windowId; // "" = the engine's choice
        req.afterSessionId = plan.afterSessionId;
        // What is opening, in the thread — BEFORE the launch (§21.23): the window's answer, which the tick may post a moment later
        // from another thread, can never land above it. A typed last choice is quoted; a button's line stands alone.
        _reply(threadId, plan.summaryHtml, false, json::Value{}, replyTo);
        bool ok = false;
        try
        {
            ok = _askLaunch(req, bind.launchToken);
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_launchNewThread (launch)");
        }
        if (!ok)
        {
            // No window took it — the window the user chose closed since the census, or no window is open at all: the wizard comes
            // back (its picker still stands, its buttons answer) at the step that can fix it, with what the bind held meanwhile.
            const std::wstring notice = plan.windowId.empty() ? std::wstring{ L"\x26A0 No window could open it right now \x2014 is Agentmaster open on the PC? Choose again to retry." } :
                                                                  std::wstring{ L"\x26A0 That window is not open any more \x2014 pick another one." };
            NewThreadStep backTo = NewThreadStep::Model;
            {
                std::lock_guard lk{ _mtx };
                // §21.28: no window will answer, so the tap's holds go; the wizard rendered below, inside the tap's own scope, answers it.
                _bindTapHolds.erase(bind.launchToken);
                const auto it = std::find_if(_threadBinds.begin(), _threadBinds.end(), [&bind](const ThreadBind& b) { return b.launchToken == bind.launchToken; });
                if (it != _threadBinds.end())
                {
                    if (picker)
                    {
                        picker->held = std::move(it->held); // what the bind held: the record's own, and any message written meanwhile
                    }
                    _threadBinds.erase(it);
                }
                if (picker)
                {
                    if (const auto existing = _newThreads.find(threadId); existing == _newThreads.end())
                    {
                        backTo = NewThreadStepAfterRefusal(picker->wizard, RemoteLaunchResult::WindowClosed);
                        ReturnNewThreadToStep(picker->wizard, backTo, notice);
                        _newThreads.emplace(threadId, std::move(*picker));
                    }
                    else
                    {
                        // A record came back meanwhile (nothing makes one while a bind holds the thread — a belt): it keeps its
                        // own state, and the messages are not lost.
                        auto& held = existing->second.held;
                        held.insert(held.begin(), picker->held.begin(), picker->held.end());
                        if (held.size() > kNewThreadHeldMax)
                        {
                            held.resize(kNewThreadHeldMax);
                        }
                        backTo = existing->second.wizard.step;
                    }
                }
                _stateDirty = true;
            }
            _log(L"[msg] new thread " + std::to_wstring(threadId) + L": no window took the launch" + (plan.windowId.empty() ? std::wstring{} : L" (window " + plan.windowId + L" is gone)") +
                 L"; back to its wizard's " + NewThreadStepName(backTo) + L" step");
            if (backTo == NewThreadStep::Model)
            {
                _enterNewThreadModelStep(threadId);
            }
            _showNewThreadPicker(threadId);
            return notice.substr(2); // the toast, without the warning sign
        }
        if (pickerId > 0)
        {
            // The user's "once picked, removed": the picker goes; the thread shows the session's card once it is up.
            OutItem del;
            del.prio = SendPriority::Final;
            del.method = L"deleteMessage";
            del.params = json::Value::MkObj();
            del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(pickerId)));
            _enqueue(std::move(del));
        }
        _log(L"[msg] new thread " + std::to_wstring(threadId) + L": opening session " + ShortId(bind.sessionId) + L" in " + plan.dir +
             (bind.title.empty() ? std::wstring{} : L" titled \x201C" + bind.title + L"\x201D") + (plan.model.empty() ? std::wstring{} : L" model " + plan.model) +
             (plan.windowId.empty() ? std::wstring{} : L" in window " + plan.windowId) + (plan.afterSessionId.empty() ? std::wstring{} : L" after " + ShortId(plan.afterSessionId)) +
             L" (" + std::to_wstring(bind.held.size()) + L" held message(s))");
        LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + L" -> launch " + ShortId(bind.sessionId) + L" dir=" + plan.dir +
               (plan.model.empty() ? std::wstring{} : L" model=" + plan.model) + L" token=" + std::to_wstring(bind.launchToken));
        return plan.summaryText;
    }

    bool MessagingService::_abortNewThread(int64_t threadId, const std::wstring& why, bool cancelled)
    {
        NewThread rec;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return false;
            }
            rec = std::move(it->second);
            _newThreads.erase(it);
            _stateDirty = true;
            // Whatever is queued for the thread (a picker edit, a reply) would fail once it is gone, each costing a call of the
            // chat's budget: dropped first. The delete is queued after this, so it is not dropped with them.
            const std::wstring pickerKey = L"ntp:" + std::to_wstring(threadId);
            for (auto o = _outbox.begin(); o != _outbox.end();)
            {
                const bool mine = o->key == pickerKey || NewThreadOutItemThread(o->params) == threadId;
                o = mine ? _outbox.erase(o) : std::next(o);
            }
        }
        // The user's rule (2026-10-18): "If the creation process aborts in any way, the new topic needs to be deleted".
        OutItem del;
        del.prio = SendPriority::Final;
        del.method = L"deleteForumTopic";
        del.params = json::Value::MkObj();
        del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        del.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
        auto self = weak_from_this();
        const int64_t pickerId = rec.pickerMessageId;
        del.onDone = [self, threadId, pickerId](const TgResult& r) {
            auto me = self.lock();
            if (!me || r.ok || r.ThreadNotFound())
            {
                return; // deleted (or the user deleted it first)
            }
            // Refused: at least the picker goes (its buttons would answer that nothing is left to set up).
            me->_log(L"[msg] new thread " + std::to_wstring(threadId) + L": the thread could not be deleted (" + r.description + L"); its picker is deleted instead");
            if (pickerId > 0)
            {
                OutItem delPicker;
                delPicker.prio = SendPriority::Final;
                delPicker.method = L"deleteMessage";
                delPicker.params = json::Value::MkObj();
                delPicker.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(me->_ownerId)));
                delPicker.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(pickerId)));
                me->_enqueue(std::move(delPicker));
            }
        };
        _enqueue(std::move(del));
        // What the user wrote there goes with the thread: its only copy is a silent line in the chat's main view.
        std::vector<std::wstring> texts;
        for (const auto& m : rec.held)
        {
            texts.push_back(m.StrAt(L"text"));
        }
        if (!texts.empty())
        {
            _reply(0, BuildNewThreadAbortHtml(rec.topicName, texts, cancelled, why), false);
        }
        _log(L"[msg] new thread " + std::to_wstring(threadId) + L" aborted (" + why + L"): the thread is deleted" +
             (texts.empty() ? std::wstring{} : L"; " + std::to_wstring(texts.size()) + L" held message(s) copied into the main view"));
        LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + (cancelled ? L" cancelled" : L" abandoned") + L" -> thread deleted");
        return true;
    }

    void MessagingService::_abortUntouchedNewThread(int64_t threadId, const std::wstring& why)
    {
        bool untouched = false;
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _newThreads.find(threadId); it != _newThreads.end())
            {
                // Nothing the user made there: no held message, no picker yet (an implicit-named thread gets its picker with its
                // first held message), no name the user gave it.
                const auto& nt = it->second;
                untouched = nt.held.empty() && nt.pickerMessageId == 0 && nt.topicName.empty();
            }
        }
        if (untouched)
        {
            _abortNewThread(threadId, why, false);
        }
    }

    void MessagingService::_onUserTopicRenamed(const std::wstring& sessionId, int64_t threadId, const std::wstring& name)
    {
        const std::wstring title = StripTopicNameDecoration(name);
        if (title.empty())
        {
            return; // nothing but the decoration: no title to take
        }
        const auto s = _registry->GetWithoutQueue(sessionId);
        // A name that is the session's title as the service PRINTS it — on one line (TgOneLineTitle, 2026-10-18) — is the
        // service's own name saved back unchanged: taking it would flatten a tab title that has line breaks ("a\rb" -> "a b").
        if (!s || s->title == title || TgOneLineTitle(s->title) == title)
        {
            return; // the name the service gave it, or one only re-decorated: the session's title stands
        }
        if (_hooks.sessionAction)
        {
            _hooks.sessionAction(sessionId, L"rename", title, nullptr); // the hosting window pins the tab + the registry (Rule #11)
        }
        else
        {
            _registry->Update(sessionId, [&](SessionInfo& ss) { ss.title = title; });
        }
        _log(L"[msg] " + ShortId(sessionId) + L" renamed from its thread " + std::to_wstring(threadId) + L" \x2192 \x201C" + title + L"\x201D (the topic name follows)");
        LogNav(L"remote telegram topic-rename " + ShortId(sessionId));
    }

    // ===== the topics' verbose mode (§21.11) ======================================================

    void MessagingService::_runVerboseCommand(const std::wstring& sessionId, const std::wstring& args, int64_t threadId)
    {
        const std::wstring word = detail::Lower(Trimmed(args));
        const bool topic = !sessionId.empty();
        if (word == L"on" || word == L"verbose" || word == L"1" || word == L"true")
        {
            topic ? _setTopicVerbose(sessionId, 1, threadId) : _setDefaultVerbose(true, threadId);
            return;
        }
        if (word == L"off" || word == L"conversation" || word == L"0" || word == L"false")
        {
            topic ? _setTopicVerbose(sessionId, 0, threadId) : _setDefaultVerbose(false, threadId);
            return;
        }
        if (topic && (word == L"default" || word == L"auto"))
        {
            _setTopicVerbose(sessionId, -1, threadId);
            return;
        }
        if (!word.empty())
        {
            _reply(threadId, topic ? L"Usage: /verbose on | off | default" : L"Usage: /verbose on | off", false);
            return;
        }
        // No argument: the mode in effect, and buttons to change it.
        const bool def = _defaultVerbose.load();
        const std::wstring explain =
            L"\n\n<b>Verbose</b> shows the whole session as it runs: what Claude says before each tool, a work log of the tool calls (Bash commands and more), compactions. "
            L"<b>Conversation</b> shows your messages and each turn's last answer \x2014 you, Claude, you, Claude \x2014 plus questions, plans, interrupts and API errors.";
        if (topic)
        {
            int mode = -1;
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
                {
                    mode = it->second.verbose;
                }
            }
            const bool on = mode < 0 ? def : mode == 1;
            _reply(threadId,
                   std::wstring{ L"<b>This topic</b>: " } + (on ? L"verbose" : L"conversation only") + (mode < 0 ? L" (the default for all topics)." : L" (set for this topic).") + explain +
                       L"\nA change applies to the messages from now on; /recreate redoes this topic's history in a mode.",
                   false,
                   _buttons({ { { L"\U0001F50D Verbose", _tokenFor(L"verbose-topic", sessionId, L"1", kTokenTtlMsFor(2)) },
                                { L"\U0001F4AC Conversation", _tokenFor(L"verbose-topic", sessionId, L"0", kTokenTtlMsFor(2)) } },
                              { { std::wstring{ L"\u21BA Follow the default (" } + (def ? L"verbose" : L"conversation") + L")", _tokenFor(L"verbose-topic", sessionId, L"-1", kTokenTtlMsFor(2)) } } }));
            return;
        }
        _reply(threadId,
               std::wstring{ L"<b>Default for all topics</b>: " } + (def ? L"verbose." : L"conversation only.") + explain +
                   L"\nA topic set with /verbose in it keeps its own mode. A change applies to the messages from now on; /recreate redoes the history.",
               false,
               _buttons({ { { L"\U0001F50D Verbose", _tokenFor(L"verbose-default", L"", L"1", kTokenTtlMsFor(2)) },
                            { L"\U0001F4AC Conversation", _tokenFor(L"verbose-default", L"", L"0", kTokenTtlMsFor(2)) } } }));
    }

    void MessagingService::_setTopicVerbose(const std::wstring& sessionId, int verbose, int64_t threadId)
    {
        bool found = false;
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
            {
                it->second.verbose = std::clamp(verbose, -1, 1);
                _stateDirty = true;
                found = true;
            }
        }
        if (!found)
        {
            _reply(threadId, L"This session has no topic any more.", false);
            return;
        }
        const bool def = _defaultVerbose.load();
        const bool on = verbose < 0 ? def : verbose == 1;
        _reply(threadId,
               std::wstring{ L"This topic is now " } + (on ? L"<b>verbose</b>" : L"<b>conversation only</b>") + (verbose < 0 ? L" (it follows the default for all topics)" : L"") +
                   L", from the next message on. /recreate redoes its history in a mode.",
               false);
        LogNav(L"remote telegram /verbose " + ShortId(sessionId) + L" -> " + (verbose < 0 ? std::wstring{ L"default" } : (verbose == 1 ? std::wstring{ L"on" } : std::wstring{ L"off" })));
    }

    void MessagingService::_setDefaultVerbose(bool verbose, int64_t threadId)
    {
        // settings.json, read-modify-write of the freshest file (the cog's Messaging tab shows the same switch, and its Save
        // writes what it shows), then the service at once.
        bool saved = false;
        try
        {
            AppSettings s = LoadAppSettings();
            s.messagingVerbose = verbose;
            SaveAppSettings(s);
            saved = true;
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_setDefaultVerbose (settings.json)");
        }
        _defaultVerbose.store(verbose);
        LogNav(std::wstring{ L"remote telegram /verbose default -> " } + (verbose ? L"on" : L"off") + (saved ? L"" : L" (settings.json not written)"));
        if (threadId >= 0)
        {
            _reply(threadId,
                   std::wstring{ L"Topics are now " } + (verbose ? L"<b>verbose</b>" : L"<b>conversation only</b>") +
                       L" by default, from the next message on (a topic set with /verbose in it keeps its own mode). /recreate redoes the history." +
                       (saved ? L"" : L"\n\u26A0 The setting could not be saved: it lasts until Agentmaster restarts."),
                   false);
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
            html = L"<b>\U0001F50E Search</b>\nWrite what you look for in plain words; the agent reads the sessions it finds and answers with one numbered card each, quoting the evidence it saw. Tap a number to pick one, then Resume or Fork it. A follow-up refines the last answer.\n"
                   L"It is one conversation for the whole app: it carries on after a restart, and /clear (or \x2298 Clear under an answer) starts it over.\n"
                   L"/idle \x2014 the idle tabs\n/board \x2014 counts per state\n/newsession &lt;folder&gt; [model] \x2014 or open a new thread and pick its folder\n/pauseall \x00B7 /resumeall\n/status\n/clear \x2014 forget the search conversation\n/verbose on|off \x2014 the default for all topics: the work too, or the conversation only\n/recreate \x2014 make the session topics again in MOST RECENT order, with their history, verbose or not (asks first)\n"
                   // §21.19: the hyphenated spelling only inside <code>: a client makes a command LINK of "/search-tab" only up
                   // to the hyphen, so the word would show half linked, and its tap would send a bare "/search" (the jump too,
                   // but not what the line says). The menu's "/search_tab" is a link as a whole.
                   L"/search_tab \x2014 from any topic: put this topic first in the thread list (typed, <code>/search-tab</code> works too)\n"
                   L"/search &lt;words&gt; \x2014 from any topic: a search asked here, its answer here (in this topic the words alone do the same)";
        }
        else
        {
            // §21.33: /steer is the one way into a running turn, so the first line names it beside the rule it is the exception to.
            html = L"<b>In a session's topic</b>\nAny text is queued as the session's next prompt (never typed into a running turn \x2014 /steer &lt;text&gt; does that: it sends the words NOW, like Send now at the PC). Claude Code commands (/compact, /model x, your skills) are queued the same way; commands that leave no record are refused.\n"
                   L"/queue \x00B7 /sendnow \x00B7 /steer &lt;text&gt; \x00B7 /unqueue n\n/runner auto|paused\n/pin \x00B7 /unpin \x2014 never idle\n/wake \x00B7 /moveidle \x00B7 /markunread\n/rename &lt;title&gt;\n/favorite \x00B7 /tags \x00B7 /tag x \x00B7 /untag x\n"
                   L"/sessionid \x00B7 /path \x00B7 /gitbranch \x00B7 /draft\n/summary \x00B7 /transcript [level] \x00B7 /screen \x00B7 /screenimage \x00B7 /status\n/activate \x00B7 /restartsession \x00B7 /deactivate \x00B7 /interrupt\n/forksession [model] \x00B7 /newsession [model]\n/takedraft \x2014 queue the unsent draft at the PC\n/cc /x \x2014 force /x through to Claude Code\n/verbose on|off|default \x2014 this topic shows the work too, or the conversation only\n/recreate \x2014 make this topic (or all) again with its history, verbose or not\n"
                   // §21.19: <code> keeps "/search-tab" from becoming a "/search" link (see the Search help above).
                   L"/search_tab \x2014 jump to the \U0001F50E Search topic: it goes first in the thread list (typed, <code>/search-tab</code> works too)\n"
                   L"/search &lt;words&gt; \x2014 ask the \U0001F50E Search agent from here: the request and its answer go to the Search topic";
        }
        _reply(threadId, html, false);
    }
}
