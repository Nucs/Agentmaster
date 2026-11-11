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

#include "ApiErrorRewind.h" // HOOKS.md "Interrupted turns": PlanInterruptRewindForSession, AssessForkContinuation, AssessRestartContinuation (the restart and fork questions)
#include "ClaudeSpawn.h" // LogNav, ShortId, ResolveClaudeTranscriptPath, NewSessionId, LogSwallowedException, ClaudeConversationExists
#include "FolderProbe.h" // §21.17.4: SharedFolderProber — the poll thread's folder checks wait a bounded time
#include "MessagingVoice.h" // §21.43: IsVoiceNoteMessage (a just-bound thread's voice note goes to the voice path)
#include "Persistence.h" // DeriveSessionTitle
#include "ProcessInspect.h" // AnalyzeSessionTranscript, ReadGitBranchForDir
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8
#include "Scheduler.h"
#include "SessionRegistry.h"
#include "SessionStore.h" // favorites, tags
#include "TranscriptTurns.h" // RenderTranscriptTurnsFile
#include "ScreenImage.h" // §21.24: RenderScreenSnapshotPng + ParseScreenSnapshot (the card's 🖼 Screen Image)
#include "ConversationImage.h" // §21.39: RenderConversationImages (the card's 📜 All Screen: the whole conversation as tall pictures)

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

        /// <summary>
        /// §21.48: a copy of a Telegram object with one member SET — any member of that name removed first, then the new value
        /// appended. json::Value::Set only appends, and Find returns the FIRST match, so setting a member an object already has
        /// would leave the old value in force: a moved message's copy must name its new thread whatever the original carried.
        /// </summary>
        /// <param name="v">The object (a copy; a non-object is returned as it is).</param>
        /// <param name="key">The member's name.</param>
        /// <param name="value">The member's new value.</param>
        /// <returns>The object with `key` = `value`.</returns>
        /// <exception cref="std::bad_alloc">If the member cannot be added.</exception>
        json::Value WithMember(json::Value v, std::wstring_view key, json::Value value)
        {
            if (v.type != json::Value::Type::Obj)
            {
                return v;
            }
            v.members.erase(std::remove_if(v.members.begin(), v.members.end(), [key](const auto& m) { return m.first == key; }), v.members.end());
            v.members.emplace_back(std::wstring{ key }, std::move(value));
            return v;
        }

        /// <summary>
        /// §21.48: a copy of a Telegram object WITHOUT one member — a command answered in the main view after its thread is deleted
        /// loses its message_id, so no answer quotes a message that went with the thread (a reply to a message of another topic
        /// ignores allow_sending_without_reply, and would be refused once the message is gone).
        /// </summary>
        /// <param name="v">The object (a copy; a non-object is returned as it is).</param>
        /// <param name="key">The member to drop (every occurrence).</param>
        /// <returns>The object without `key`.</returns>
        json::Value WithoutMember(json::Value v, std::wstring_view key)
        {
            if (v.type == json::Value::Type::Obj)
            {
                v.members.erase(std::remove_if(v.members.begin(), v.members.end(), [key](const auto& m) { return m.first == key; }), v.members.end());
            }
            return v;
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
        // §21.39: at most this many CONVERSATIONS are drawn at once, process-wide. One takes seconds and hundreds of MB (measured
        // 2026-10-25 in a scratch process: a 95 MB transcript peaked at ~315 MB of commit and took ~5.3 s for 6 parts, mostly the
        // transcript's own parse - its Compact level peaked the same), so a second one waits for the first rather than doubling that.
        constexpr int kConversationImageMaxRenders = 1;

        /// <summary>
        /// §21.41: the ONE answer of a session action the service sent to every window (ServiceHooks::sessionAction) and whose
        /// outcome it reports to the user - ⎋ Esc, 🌙 Deactivate and their typed commands. Shared by every window's copy of the
        /// reply function (each window's sink copies it once): the hosting window's reply Claim()s it, and when the LAST copy goes
        /// without any window having claimed it - no window hosts the tab (it closed meanwhile, a claude outside Agentmaster's tabs)
        /// - the destructor runs `onUnanswered`, so a tap never just stops spinning and a typed command is never left unanswered.
        /// </summary>
        /// <remarks>
        /// The destructor runs on whichever thread drops the last copy: a window's UI thread (its dispatcher item ran and missed),
        /// or the poll thread when no window is registered at all (then before the request's call even returns). It is noexcept
        /// like every destructor, so `onUnanswered` is contained here (Rule #18): a throw costs only the report, never the window's
        /// dispatcher item. Unlike Engine.cpp's LaunchAnswer it carries no outcome: the outcome is the window's own reply.
        /// </remarks>
        class SessionActionAnswer
        {
        public:
            /// <summary>Arms the guard; nothing runs until the last copy of the reply goes.</summary>
            /// <param name="onUnanswered">Reports "no window answered" (it should not throw; a throw is logged and dropped).</param>
            explicit SessionActionAnswer(std::function<void()> onUnanswered) noexcept :
                _onUnanswered{ std::move(onUnanswered) }
            {
            }

            /// <summary>Runs `onUnanswered` when no window claimed the answer; contains anything it throws.</summary>
            ~SessionActionAnswer()
            {
                if (_claimed.exchange(true))
                {
                    return; // the hosting window answered: its reply told the user
                }
                try
                {
                    if (_onUnanswered)
                    {
                        _onUnanswered();
                    }
                }
                catch (...)
                {
                    // Rule #18. Recovery: the "no window answered" report is lost; a tapped button is still answered (its hold's limit
                    // at worst, with kTapStillWorkingToast), and a typed command simply gets no line.
                    LogSwallowedException(L"SessionActionAnswer (no window answered)");
                }
            }

            SessionActionAnswer(const SessionActionAnswer&) = delete;
            SessionActionAnswer& operator=(const SessionActionAnswer&) = delete;

            /// <summary>Claims the one answer for the calling reply; the guard is then silent.</summary>
            /// <returns>True for the first caller (it reports the outcome); false for any later one (it must report nothing).</returns>
            bool Claim() noexcept
            {
                return !_claimed.exchange(true);
            }

        private:
            std::atomic<bool> _claimed{ false }; // a window answered (or the destructor ran): exactly one report per request
            std::function<void()> _onUnanswered; // the "no window answered" report
        };
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
                    // HOOKS.md "Interrupted turns": an errored or exited session holds no turn the USER interrupted to ask
                    // about — the restart goes as it always did.
                    _hooks.restartSession(sessionId, RestartTurnChoice::KeepInterrupted);
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

    void MessagingService::_handleMessage(const json::Value& msg, bool edited, bool fromVoiceLane)
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
        // §21.48: the OPENER of a thread the user opened from "All". Telegram for Android 13.0.x creates the thread and then sends
        // the message that named it WITHOUT message_thread_id, so it read as a main-view message here — the bot answered "This is
        // Agentmaster's bot …" (to /help: the Search topic's help), the new thread stayed empty, and the user had to write in it
        // again to start the wizard (seen in the owner's chat 2026-11-07: five such threads in 16 minutes). The follow
        // (NewThreadOpenerFollow, started by _onUserThreadCreated) takes a thread-less message Telegram stamped within seconds of
        // the creation as that thread's: a COPY that names the thread and carries kNewThreadMovedKey goes through this function
        // again, so every reader below — the voice path, the /search pair, the wizard, the hand-over — reads the message where the
        // user meant it. An edit of a moved message is routed the same way. The mark keeps the copy from being routed twice; a
        // message the voice lane hands back was routed when it first came; a service message (no content) never takes part.
        if (!fromVoiceLane && !msg.BoolAt(kNewThreadMovedKey) && NewThreadMessageCarriesContent(msg))
        {
            const int64_t messageId = msg.I64At(L"message_id", 0);
            int64_t routed = 0;
            {
                std::lock_guard lk{ _mtx };
                routed = _openerFollow.Route(threadId, msg.I64At(L"date", 0), messageId, edited);
            }
            if (threadId == 0 && routed != 0)
            {
                const json::Value moved = WithMember(WithMember(msg, L"message_thread_id", json::Value::MkNum(static_cast<double>(routed))),
                                                     kNewThreadMovedKey,
                                                     json::Value::MkBool(true));
                _log(L"[msg] new thread " + std::to_wstring(routed) + L": " +
                     (edited ? L"an edit of its opener (message " + std::to_wstring(messageId) + L") came without the thread's id; it goes to the thread" :
                               L"its opener (message " + std::to_wstring(messageId) + L") came without the thread's id (Telegram Android's \x201C" L"All\x201D view); taken as the thread's"));
                LogNav(L"remote telegram new-thread " + std::to_wstring(routed) + (edited ? L" opener-edit" : L" opener") + L" msg=" + std::to_wstring(messageId));
                _handleMessage(moved, edited, fromVoiceLane);
                return;
            }
        }
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
        // §21.43 (VOICE.md §6): a VOICE NOTE (an audio file, a round video note) goes to the voice thread and its thread's lane,
        // and any later message of a thread whose lane still holds something waits there behind it — so a note and a typed
        // follow-up reach the session (or, in the 🔎 Search topic, the Search agent) in the order the user sent them. The tick
        // hands a held message back here with fromVoiceLane set, which skips this step (it must never be held twice).
        if (!fromVoiceLane && _voiceTake(msg, sessionId, threadId, bindHandingOver, edited))
        {
            return;
        }
        const std::wstring text = msg.StrAt(L"text");
        if (text.empty())
        {
            if (msg.Find(L"photo") || msg.Find(L"document"))
            {
                _reply(threadId, L"Files and photos are not taken yet (a later build). Text and voice notes only for now.", false, json::Value{}, msg.I64At(L"message_id", 0));
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
            if (edited)
            {
                // §21.48: an edit asks for nothing new — an edit of a message moved into a thread was routed there above. Before
                // §21.48 an edited main-view message got the main view's answer once more.
                return;
            }
            const RemoteCommand cmd = ParseRemoteCommand(text);
            if (cmd.kind == RemoteKind::AgentmasterCommand && cmd.name == L"newsession")
            {
                // §21.48: /newsession works here too (the user's "my expectation was for it to work anywhere I use it"): the bot
                // opens a thread for the new session and its wizard asks there — the folder, the model, the window. Before, it
                // got the line below. (Telegram's own "All" view on Android and Desktop opens a thread for the command itself; that
                // command reaches the thread's wizard through the follow above or with its thread id, never here.)
                _openNewSessionThread(cmd.args, msg, 0, false);
                return;
            }
            if (IsMainViewFleetCommand(cmd))
            {
                // §21.48: the main view's OWN help (before, /help here sent the 🔎 Search topic's, which read as "you are in the
                // Search topic"), and the fleet commands the Search topic runs — §21.40 brought /status here first; /idle, /board,
                // /pauseall, /resumeall, /verbose and /recreate got the line below until §21.48.
                _runMainViewCommand(cmd, msg);
                return;
            }
            if (cmd.kind == RemoteKind::Text)
            {
                // §21.48: WORDS written in the main view open a new session, as Telegram's own "All" view does where it opens a thread
                // for them (Android's and Desktop's; those words reach the thread's wizard through the follow above or with their
                // thread id). A client that posts them here instead — or a follow that missed — gets a thread the bot opens, named
                // after them, which holds them as the session's first prompt. Before, they got the line below and were lost.
                _openNewSessionThread(L"", msg, 0, true);
                return;
            }
            if (cmd.kind == RemoteKind::AgentmasterCommand)
            {
                // §21.48: a session's own command (/queue, /rename, /steer, …) has no session here.
                _reply(0, L"<code>/" + TgEscapeHtml(cmd.name) + L"</code> works in a session's topic. Here: /newsession (a new session), /help, /status, /idle, /board, /pauseall, /resumeall, /search &lt;words&gt;.", false);
                return;
            }
            // §21.17: the new thread; §21.19: /search, /search_tab; §21.48: /newsession, and Claude Code's commands need a session.
            _reply(0, L"This is Agentmaster's bot. Claude Code's commands go in a session's topic \x2014 /newsession opens a new session (its thread asks for the folder, the model and the window), and the \U0001F50E Search topic finds old ones (/search &lt;words&gt; asks it from anywhere). Send /help for the commands.", false);
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
        // §21.48: a thread whose ONLY message is a command of the main view (/help, /status, /idle, …) was opened by Telegram for
        // that command — writing in "All" opens a thread for every message — not by a user who wants a session there. The command
        // is answered in the MAIN VIEW, where the user wrote it, and the empty thread is deleted, as a thread opened only for
        // /search is (§21.25). Before §21.48 such a thread stayed, named after the command, holding the command as the first
        // prompt of a session nobody asked for (or, from Android, empty: the command itself came thread-less). The answer quotes
        // nothing: the message may go with the thread (WithoutMember). A thread the user named, or whose wizard has begun, keeps
        // the message for its session as before.
        if (!edited)
        {
            const RemoteCommand cmd = ParseRemoteCommand(text);
            if (IsMainViewFleetCommand(cmd) && _newThreadUntouched(threadId))
            {
                _runMainViewCommand(cmd, WithoutMember(msg, L"message_id"));
                _abortUntouchedNewThread(threadId, L"its only message was /" + cmd.name + L", answered in the main view");
                return;
            }
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
            if (cmd.name == kEnterCommand)
            {
                // §21.47: /enter is taken here for /steer's reason: only this path knows whether a new thread held the message, and a
                // held one is no backlog (an Enter written hours ago is never pressed, one the folder pick held is). _runSessionCommand
                // keeps a branch for it.
                _enterRemote(sessionId, cmd, msg, threadId, held);
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
        // §21.43: a VOICE NOTE's transcript carries "am_voice_live" when the note was no backlog when it ARRIVED (_voiceTake) —
        // a slow transcription (a first worker start loading its model) must not turn a live note into a backlog question.
        if (!held && !msg.BoolAt(L"am_voice_live") && IsBacklogUpdate(dateMs, WallMs()))
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

    bool MessagingService::_queueRemotePrompt(const std::wstring& sessionId, const std::wstring& text, int64_t messageId, int64_t threadId, bool fromBacklog, int addDirOption)
    {
        const auto s = _registry->Get(sessionId);
        if (!s || !s->live)
        {
            _reply(threadId, L"This session is closed. Resume it from the \U0001F50E Search topic.", false, json::Value{}, messageId);
            return false;
        }
        // §21.34: /add-dir is a local command whose dialog is answered from here (its watch, below). A BARE /add-dir would open a
        // path INPUT at the PC instead — nothing a phone can fill, and a box nobody answers: never sent, the usage line instead.
        // Checked here, the one funnel every queued message takes (a typed one, a backlog [Queue], a held one, /cc /add-dir).
        std::wstring addDirArgs;
        const bool addDir = ParseAddDirCommandText(text, addDirArgs);
        if (addDir && addDirArgs.empty())
        {
            _reply(threadId, AddDirUsageHtml(), false, json::Value{}, messageId);
            LogNav(L"remote telegram refused /add-dir " + ShortId(sessionId) + L" -> usage (no path)");
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
        if (addDir)
        {
            // §21.34: watched from now on — its dialog becomes a card in this topic, answered by a tap or by itself after a minute.
            // BEFORE the prompt reaches the queue: the watch reads the transcript from where it stands now, and the runner may
            // deliver the command (and a command without a dialog write its result) the moment it is queued. §21.25.9: a new
            // thread's own /add-dir carries its answer (addDirOption), pressed as soon as the dialog shows.
            try
            {
                _watchAddDir(sessionId, p.id, text, addDirArgs, messageId, threadId, addDirOption);
            }
            catch (...)
            {
                // Rule #18. Recovery: the command still goes (it is queued below); its dialog is then answered at the PC, as before §21.34.
                LogSwallowedException(L"MessagingService::_queueRemotePrompt (_watchAddDir)");
            }
        }
        size_t position = 0;
        RemoteWait waits = RemoteWait::None; // §21.44: what the message waits behind, decided with the queue it goes into
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
            // §21.44: will it WAIT for a turn (a turn or a delivery in flight, an earlier message ahead)? Read in the update that
            // inserts it, so the state and the queue are those of this very moment: a turn that ends a beat later cannot split
            // the decision from the insertion. Such a message will sit in the topic above the turn that kept it waiting, so its
            // send gets the "📤 sent from the queue" line (the mirror reads the stamp at its echo). Stamped on the queue's copy
            // only; the scheduler never reads it, so the message goes exactly when it would have.
            waits = RemotePromptWait(ss, static_cast<size_t>(std::distance(ss.queue.begin(), at)));
            at = ss.queue.insert(at, p);
            if (waits != RemoteWait::None)
            {
                at->remoteQueuedBusyUnixMs = WallMs();
            }
            position = static_cast<size_t>(std::distance(ss.queue.begin(), at));
        });
        {
            std::lock_guard lk{ _mtx };
            _reacted[p.id] = PromptStatus::Pending;
        }
        _react(messageId, L"\x270D");
        LogNav(L"remote telegram queue " + ShortId(sessionId) + L" chars=" + std::to_wstring(text.size()) + (fromBacklog ? L" (from the backlog)" : L""));
        // §21.44: a message that waits says behind what (its send will post "📤 sent from the queue"; the log ties the two together).
        _log(L"[msg] " + ShortId(sessionId) + L" queued from topic (message " + std::to_wstring(messageId) + L", " + std::to_wstring(text.size()) + L" chars" +
             (waits != RemoteWait::None ? std::wstring{ L"; waits: " } + RemoteWaitWords(waits) : std::wstring{}) + L")");
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
        // §21.34: "/steer /add-dir <path>" is the same command sent now: watched like a queued one (below). A bare one would open a
        // path INPUT at the PC that nobody here can fill: never sent, the usage line instead.
        std::wstring addDirArgs;
        const bool addDir = ParseAddDirCommandText(words, addDirArgs);
        if (addDir && addDirArgs.empty())
        {
            _reply(threadId, AddDirUsageHtml(), false, json::Value{}, msgId);
            nav(L"usage:add-dir");
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
        if (addDir)
        {
            // §21.34: its dialog's watch, BEFORE the send (the watch reads the transcript from where it stands now). A prompt that
            // never reaches the queue leaves a watch that ends at its first tick (the prompt is not found).
            try
            {
                _watchAddDir(sessionId, p.id, p.text, addDirArgs, msgId, threadId);
            }
            catch (...)
            {
                // Rule #18. Recovery: the words still go; the dialog is then answered at the PC, as before §21.34.
                LogSwallowedException(L"MessagingService::_steerRemotePrompt (_watchAddDir)");
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
            // §21.40: /status then sends what Claude Code's own /status shows (its Status tab, the account, the plan usage), read on
            // a worker thread and sent as a reply to the command; /card stays the card copy alone.
            done(name == L"status" ? L"ok, " + std::wstring{ _startClaudeStatusReport(sessionId, threadId, msgId) } : std::wstring{ L"ok" });
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
        if (name == kEnterCommand)
        {
            // §21.47: the belt for a path that reaches a session command without _handleTopicMessage, which takes /enter itself (and
            // knows whether a new thread held it). _enterRemote logs its own outcome.
            _enterRemote(sessionId, cmd, msg, threadId, false);
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
        if (name == L"download")
        {
            // §21.32.4: a file's download link (the same delivery as the links in Claude's messages) and a "📤 Send as file"
            // button that uploads it through Telegram (Messaging.Files.cpp).
            done(_filesDownloadCommand(sessionId, *s, cmd.args, threadId, msgId));
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
        if (name == L"allscreen")
        {
            // §21.39: the WHOLE conversation as tall pictures. Drawn from the TRANSCRIPT, not the terminal: a fullscreen Claude
            // Code (the alternate screen) keeps no scrollback, so the terminal never holds more than one screen
            // (ConversationImage.h). So no window is asked, and a parked tab or a closed session draws as well as a live one.
            // The level: the one named (a level button of the card's picker names one), else the cog's default (§21.39: Full
            // unless set otherwise) - NOT LevelFromWord's Default, which /transcript keeps for a bare /transcript. "diffs" (the
            // picker's checked File Diffs) draws every file change as a diff, whatever the level; "last" (its checked Last Turn)
            // draws the newest turn only.
            const TurnsLevel defaultLevel = static_cast<TurnsLevel>(_allScreenLevel.load());
            const auto pick = ParseAllScreenArgs(cmd.args, defaultLevel);
            if (!pick)
            {
                _reply(threadId, L"Usage: /allscreen [minimal|compact|default|full|max] [diffs] [last] \x2014 bare, it draws " + std::wstring{ AllScreenLevelWord(defaultLevel) } +
                                     L" (the default level, set in the cog's Messaging tab); \x201C" L"diffs\x201D adds every file change as a diff, whatever the level; \x201C" L"last\x201D draws the last turn only.",
                       false);
                return;
            }
            if (s->kind != AgentKind::Claude)
            {
                // A Codex rollout is not a Claude Code transcript: the turns view (and so the picture) reads Claude Code's only.
                _reply(threadId, L"All Screen draws Claude Code conversations; this session is not one.", false);
                done(L"refused:not-claude");
                return;
            }
            std::wstring path = ResolveClaudeTranscriptPath(sessionId);
            if (path.empty() && !s->forkParentId.empty())
            {
                path = ResolveClaudeTranscriptPath(s->forkParentId); // a fork with no turn of its own shows its source (as /transcript does)
            }
            if (path.empty())
            {
                _reply(threadId, L"No transcript yet: the session has no conversation to draw.", false);
                done(L"refused:no-transcript");
                return;
            }
            {
                // One drawing per session at a time: a second tap while the first one's parts are being drawn would draw and send
                // the whole conversation twice on the chat's call budget. The worker unmarks the session when it ends.
                std::lock_guard lk{ _mtx };
                if (!_conversationImageSessions.insert(sessionId).second)
                {
                    _reply(threadId, L"This conversation's pictures are being drawn already; they come in a moment.", false);
                    done(L"repeated");
                    return;
                }
            }
            done(_startConversationImage(sessionId, threadId, path, static_cast<int>(pick->level), pick->diffs, pick->lastTurn));
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
            // §21.41: for /deactivate and /interrupt a turn WAITING on a permission or a question (Needs approval) is in flight too -
            // a park cuts it off (it reads Error on its resume) and an Esc turns its dialog down - so both ask there as well, as the
            // card's 🌙 Deactivate does. /restartsession keeps its rule (Running only), as the card's 🔄 Restart Session does.
            const bool turnInFlight = running || (name != L"restartsession" && SessionTurnInFlight(*s));
            const std::wstring verb = name == L"restartsession" ? L"Restart" : (name == L"deactivate" ? L"Park" : L"Interrupt");
            if (name == L"interrupt" && s->kind != AgentKind::Claude)
            {
                // §21.41: Agentmaster types nothing into a Codex session, so there is no Esc to confirm - before, the question was asked
                // and its "Interrupt now" then answered that the session could not be reached.
                _reply(threadId, TgEscapeHtml(RemoteEscOutcomeText(RemoteEscResult::NotClaude, s->state)), false);
                done(L"refused:not-claude");
                return;
            }
            if (name == L"restartsession" && running)
            {
                // HOOKS.md "Interrupted turns" (the third question, 2026-11-06): the restart's shutdown cuts the running turn, and the
                // restarted claude continues it by itself when CLAUDE_CODE_RESUME_INTERRUPTED_TURN is on — so the confirm below
                // ("the turn is cut off") would not say what happens. With the switch on, the topic asks "Restart interrupted /
                // uninterrupted" instead, and its buttons restart. With it off the turn really is cut off: the confirm stays.
                if (_askRestartTurnBeforeRestart(sessionId, *s, threadId))
                {
                    done(L"asked");
                    return;
                }
            }
            if (turnInFlight || name == L"interrupt")
            {
                if (name == L"interrupt" && !turnInFlight)
                {
                    _reply(threadId, L"Nothing is running.", false);
                    return;
                }
                const std::wstring token = _tokenFor(L"confirm-" + name, sessionId, L"", kTokenTtlMsFor(1));
                // The question names what would be cut off: a running turn, or one that waits on the user's answer at the PC.
                const wchar_t* const cutOff = running ? L"</b> while a turn is running? The turn is cut off." : L"</b> while it waits for your answer at the PC? The turn is cut off.";
                _reply(threadId, verb + L" <b>" + TgTitleToken(TgTitleForm::Plain, _titleSquare(&*s), s->title, _threadOf(sessionId)) + cutOff, false,
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
                    // HOOKS.md "Interrupted turns": a turn the USER interrupted is asked about here, in the topic — ▶ Restart
                    // uninterrupted continues it after the restart (the transcript gets one rewind line first), ⏸ Restart
                    // interrupted keeps it interrupted. The buttons restart.
                    if (_askUninterruptBeforeRestart(sessionId, *s, threadId))
                    {
                        done(L"asked");
                        return;
                    }
                    // ...and when the user interrupted nothing, a turn the restarted claude would continue by itself (Needs
                    // approval, a call in flight, a turn a crash cut off) gets the third question: restart interrupted or
                    // uninterrupted. The buttons restart.
                    if (_askRestartTurnBeforeRestart(sessionId, *s, threadId))
                    {
                        done(L"asked");
                        return;
                    }
                    if (_hooks.restartSession)
                    {
                        _hooks.restartSession(sessionId, RestartTurnChoice::KeepInterrupted);
                    }
                    _reply(threadId, L"Restarting the session.", false);
                }
            }
            else
            {
                if (_hooks.sessionAction)
                {
                    // §21.41: through the hosting window, which parks with the tab menu's own path and ANSWERS what it did - an
                    // adopted shell tab, a held input box and a parked tab are refused there, and the line in the thread now says so
                    // (it used to say "Parked" whatever happened). The card's 🌙 Deactivate takes the very same path.
                    _deactivateRemote(sessionId, threadId, /*cutOffTurn*/ false, /*fromCard*/ false);
                }
                else
                {
                    // A host without the window round trip (the m5 harness): the fire-and-forget park, whose line cannot know the outcome.
                    if (_hooks.deactivateSession)
                    {
                        _hooks.deactivateSession(sessionId);
                    }
                    _reply(threadId, L"Parked: the tab keeps its place; the next message starts it again.", false);
                }
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
            // HOOKS.md "Interrupted turns": a fork of a session in the middle of a turn is asked about first (_requestLaunch).
            req.forkTurn = name == L"forksession" ? ForkTurnChoice::Ask : ForkTurnChoice::Uninterrupted;
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
        // closed is answered, never carried out unasked. §21.43: a VOICE NOTE's transcript carries "am_voice_live" when the note
        // was no backlog when it ARRIVED (_voiceTake) — a slow transcription (a first worker start loading its model, a long
        // note) must not turn a spoken search sent live into an "ask again" answer.
        const int64_t dateMs = msg.I64At(L"date", 0) * 1000;
        const bool backlog = !msg.BoolAt(L"am_voice_live") && IsBacklogUpdate(dateMs, WallMs());
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
            // §21.40: then Claude Code's account and plan usage (they are the account's, not a session's), as Claude Code prints
            // them, read on a worker thread and sent as a reply to the command.
            LogNav(L"remote telegram /status (fleet) -> " + std::wstring{ _startClaudeStatusReport(L"", threadId, msg.I64At(L"message_id", 0)) });
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
                // §21.48: a bare /newsession opens a THREAD for the new session, whose wizard asks for the folder, the model and the
                // window, as everywhere else (before, it answered "Usage: /newsession <folder> [model]"). With a folder it still
                // opens the session at once, below.
                _openNewSessionThread(L"", msg, threadId, false);
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
        _reply(threadId, L"Here: a search in plain words, or /clear, /idle, /board, /newsession [folder], /pauseall, /resumeall, /verbose, /recreate, /status, /help.", false); // §21.48: a bare /newsession works here
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
        // `_search` is read outside `_mtx`, which holds because it is WRITTEN only under `_mtx` — made once by
        // _ensureSearchBridge (just above, through `_mtx`, which orders this read after the write, whichever thread made it)
        // and reset only by Stop() after it has joined the poll and tick threads. The asking thread is the poll thread for a
        // typed request, or the tick thread for a voice note's transcript and a message that waited behind one in a voice
        // lane (§21.43); SearchBridge's methods may be called from any of the service's threads.
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
        // Outside `_mtx`, like Ask (see _askSearch for why the unlocked read holds on the poll and the tick thread alike):
        // Reset ends a running request, deletes the record and the view file, and restarts the process when one was up.
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
            card.state = ClassifySearchPickStateOf(s ? &*s : nullptr); // §21.38: a deactivated tab shows 🌙, an exited one ⏹
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
                card.state = ClassifySearchPickStateOf(s ? &*s : nullptr);
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
        // §21.38: where it stands decides the buttons too — a session that is open in a tab is never offered ▶ Resume ("open
        // it"): a deactivated tab gets ▶ Activate, an idle one ⏰ Wake, one with a topic 💬 Go to topic.
        const SearchPickState state = ClassifySearchPickStateOf(s ? &*s : nullptr);
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
        // The actions: single-use tokens (M5) that expire, carrying what the outcome line and a launch need — the buttons that
        // fit where the session stands (SearchPickMenuRows, §21.38). Each acts on the session's state at the tap
        // (_onSearchPickAction), which may differ from the line above.
        const std::wstring payload = std::to_wstring(pick.number) + L"\n" + dir + L"\n" + title;
        const int64_t ttl = kTokenTtlMsFor(kSearchActionTtlHours);
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
        for (const auto& kinds : SearchPickMenuRows(state))
        {
            std::vector<std::pair<std::wstring, std::wstring>> buttons;
            for (const std::wstring_view kind : kinds)
            {
                buttons.push_back({ SearchPickActionLabel(kind), _tokenFor(std::wstring{ kind }, pick.sessionId, payload, ttl) });
            }
            rows.push_back(std::move(buttons));
        }
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
        if (kind == kSearchPickCancel)
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
        if (kind == kSearchPickFork)
        {
            // Never pinned: there is no fork-and-pin (the user's rule, §21.7).
            _editText(menuMessageId, head + L"\n\x2192 \x2442 Fork", SendPriority::Final, L"", json::Value{});
            RemoteLaunchRequest req;
            req.kind = RemoteLaunchRequest::Kind::Fork;
            req.sessionId = sessionId;
            req.dir = dir;
            req.title = title;
            req.forkTurn = ForkTurnChoice::Ask; // HOOKS.md "Interrupted turns": a running session's fork is asked about first
            _requestLaunch(req, threadId); // it says how the fork goes (and why not, when it cannot open)
            return;
        }
        // Every other action acts on the session's state NOW (§21.38): the menu was drawn for the state of the pick, and the
        // session may have been entered, deactivated, closed or idled since. The outcome line repeats the tapped button.
        const std::wstring choice = SearchPickActionLabel(kind);
        const auto s = _registry->Get(sessionId);
        const SearchPickState now = ClassifySearchPickStateOf(s ? &*s : nullptr);
        if (kind == kSearchPickTopic)
        {
            // 💬 Go to topic: its topic first in the thread list, the directory dot's way (§21.16.7: Telegram lists a bot chat's
            // topics by their last message, and no link opens a bot-chat topic on Android). The thread is read now: a session
            // that went idle past its grace since the menu was drawn has none any more.
            const int64_t thread = _threadOf(sessionId);
            if (thread == 0)
            {
                _editText(menuMessageId, head + L"\n\x2192 " + choice + L": it has no topic now (it went idle, or it closed) \x2014 pick it again for what fits now.", SendPriority::Final, L"",
                          json::Value{});
                LogNav(L"remote telegram search topic " + ShortId(sessionId) + L" -> no topic (" + SearchPickStateWord(now) + L")");
                return;
            }
            // _bringTopicToFront carries the tap (§21.28: the button stays busy until the card's re-post made the topic first).
            const std::wstring outcome = _bringTopicToFront(thread);
            _editText(menuMessageId, head + L"\n\x2192 " + choice + L": " + TgEscapeHtml(outcome) + L".", SendPriority::Final, L"", json::Value{});
            LogNav(L"remote telegram search topic " + ShortId(sessionId) + L" -> thread " + std::to_wstring(thread));
            return;
        }
        // ▶ Resume / ▶ Activate / ⏰ Wake ask for one thing: make the session usable from here — open in a tab, its claude
        // running, its topic listed. Which of them the menu offered followed the state of the pick; what is done follows the
        // state NOW. None of them pins (the user's "Remove the pin overloads", 2026-10-23: a session is pinned in its own topic).
        switch (now)
        {
        case SearchPickState::IdleTab:
            // Its tab is open and its claude runs: waking it (Waiting for you) is all it needs, and its topic appears at the next sync.
            _wake(sessionId, false);
            _editText(menuMessageId, head + L"\n\x2192 " + choice + L": woken, its topic appears in a moment.", SendPriority::Final, L"", json::Value{});
            return;
        case SearchPickState::Listed:
            // Open with its topic already and its claude running: nothing to open or start.
            _editText(menuMessageId, head + L"\n\x2192 " + choice + L": it is open already, with its own topic.", SendPriority::Final, L"", json::Value{});
            LogNav(L"remote telegram search resume " + ShortId(sessionId) + L" -> already listed");
            return;
        case SearchPickState::ParkedTab:
        case SearchPickState::ParkedListed:
        case SearchPickState::ExitedTab:
        {
            // §21.38 (the user's "I expected Activate button"): the tab is there but its claude does not run. The claude FIRST:
            // _reactivate starts a parked tab in place (the window's "start" action = the tab menu's Activate Tab, no focus
            // change on the PC) and starts an exited claude again (the window's restart, the same conversation id). It tells a
            // restart from a start by the state it reads, so it must read it before the wake below turns Done into Waiting for
            // you. Its own answer is a toast for a card's button; the menu below says what happens in this menu's words.
            (void)_reactivate(sessionId);
            // Then the topic: an Idle (or exited) session is held in Waiting for you, as ⏰ Wake does, so its topic appears and
            // stays — a claude's SessionStart keeps Waiting for you (NextSessionState), and so does a restart's normalization
            // (RestoredSessionState). Any other state keeps its topic by itself.
            const bool wakeable = s->state == SessionState::Idle || s->state == SessionState::Done;
            if (wakeable)
            {
                _wake(sessionId, false);
            }
            std::wstring outcome = now == SearchPickState::ExitedTab ? L": starting its Claude again in its tab, in the same conversation." :
                                                                       L": starting its Claude in its tab (nothing changes on the PC's screen).";
            if (now == SearchPickState::ParkedTab)
            {
                outcome += L" Its topic appears in a moment.";
            }
            _editText(menuMessageId, head + L"\n\x2192 " + choice + outcome, SendPriority::Final, L"", json::Value{});
            LogNav(L"remote telegram search activate " + ShortId(sessionId) + L" (" + SearchPickStateWord(now) + L")" + (wakeable ? L" woken" : L""));
            return;
        }
        case SearchPickState::Closed:
        default:
        {
            // Not open in any tab (an Activate or Wake menu whose tab was closed since lands here too): resumed into a
            // background tab of the most recently active window (D27), unpinned.
            _editText(menuMessageId, head + L"\n\x2192 " + choice, SendPriority::Final, L"", json::Value{});
            RemoteLaunchRequest req;
            req.kind = RemoteLaunchRequest::Kind::Resume;
            req.sessionId = sessionId;
            req.dir = dir;
            req.title = title;
            _requestLaunch(req, threadId); // it says how the resume goes
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
        // §21.34: a MENU CARD's option ("tm:<card>:<digit>") — a local command's dialog (/add-dir's) answered from here. The toast
        // says what happens; the tap stays busy until the hosting window pressed the option (or said why it did not).
        if (data.rfind(L"tm:", 0) == 0)
        {
            answer(_onMenuTap(data, messageId));
            return;
        }
        // §21.32.4: /download's "📤 Send as file" ("dl:<offer>"): the file is read again and sent as a document; the toast says
        // what happens, and the tap stays busy until the document is in (the worker carries the tap's holds).
        if (data.rfind(L"dl:", 0) == 0)
        {
            answer(_filesOnTap(data, threadId));
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
            else if (verb == kCardVerbEsc)
            {
                // §21.41: ⎋ Esc - ONE Esc into the session at once, pressed by the hosting window. A refusal decided here is the toast;
                // otherwise the WINDOW's answer sets it, from its own UI thread and possibly before _escFromCard returns - so the tap
                // is not answered here at all: an answer("") after that reply would wipe its toast.
                if (const std::wstring toast = _escFromCard(sid, threadId); !toast.empty())
                {
                    answer(toast);
                }
                return;
            }
            else if (verb == kCardVerbDeactivate)
            {
                // §21.41: 🌙 Deactivate - at rest the hosting window parks the tab and its answer is the toast (so, as for ⎋ Esc, no
                // answer here then); while a turn is in flight /deactivate's question goes into the thread and the toast points at it.
                if (const std::wstring toast = _deactivateFromCard(sid, threadId); !toast.empty())
                {
                    answer(toast);
                }
                return;
            }
            else if (verb == kCardVerbCardPicture)
            {
                // §21.49: 🖼️ on Main - the card's OWN picture, on demand (the only way it changes): the hosting window reads the
                // screen once, and the card's next edit carries it. A refusal decided here is the toast; otherwise the read's outcome
                // sets it (from a window's UI thread or the worker, possibly before _cardPictureFromTap returns), or the picture on the
                // card answers the tap - so, as for ⎋ Esc, no answer here then: an answer("") after that would wipe its toast.
                if (const std::wstring toast = _cardPictureFromTap(sid, messageId); !toast.empty())
                {
                    answer(toast);
                }
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
            else if (verb == kCardVerbAllScreen)
            {
                // §21.39: 📜 All Screen ▸ opens the level picker (the ALL SCREEN page) on the message the button sits on - a page
                // flip like ⚙ Session's, nothing posted. (The verb's first build drew at once: an old card's button now opens
                // the picker, which draws nothing unasked.)
                answer(L"");
                _showCardPage(sid, messageId, CardPage::AllScreen);
                return;
            }
            else if (verb == kCardVerbAllScreenDiffs)
            {
                // §21.39: the picker's ⬜ File Diffs was tapped - the same picker with the box checked (its ☑️ names the unchecked
                // page again). A page flip; nothing is drawn until a level is picked.
                answer(L"");
                _showCardPage(sid, messageId, CardPage::AllScreenDiffs);
                return;
            }
            else if (verb == kCardVerbAllScreenLast)
            {
                // §21.39 (the user's request of 2026-10-29, "Last Turn"): a box tap that shows the picker with ☑️ Last Turn and ⬜ File
                // Diffs - the Last Turn box checked on the unchecked page, or File Diffs unchecked on the page with both checked.
                // A page flip, like the File Diffs box's; nothing is drawn until a level is picked.
                answer(L"");
                _showCardPage(sid, messageId, CardPage::AllScreenLast);
                return;
            }
            else if (verb == kCardVerbAllScreenLastDiffs)
            {
                // §21.39: a box tap that shows the picker with both boxes checked.
                answer(L"");
                _showCardPage(sid, messageId, CardPage::AllScreenLastDiffs);
                return;
            }
            else if (const auto pick = ParseCardAllScreenLevelVerb(verb); pick)
            {
                // §21.39: a level of the picker - /allscreen <level>, with "diffs" and / or "last" when the button sat on a page
                // with that box checked (its data says so: what the card showed is what is drawn). No toast, as for 🖼 Screen Image
                // (§21.28): the tap's answer still waits until the last picture is in (the worker carries the tap), and past the
                // hold's limit it says the work goes on.
                cmd.name = L"allscreen";
                cmd.args = std::wstring{ AllScreenLevelWord(pick->level) } + (pick->diffs ? L" diffs" : L"") + (pick->lastTurn ? L" last" : L"");
                _runSessionCommand(sid, cmd, fake, threadId);
                // Then the card goes back to MAIN at once (the user's rule of 2026-10-26: "after picking which, reset menu to main
                // view") - the same page flip as ◀ Back, at an answer's priority, so it lands before the first picture. After the
                // drawing started or after a refusal alike ("being drawn already", a Codex session): either way the pick is done,
                // and the thread says what came of it. The flip takes the tapped button away, and with it the spinner §21.28 keeps
                // on it: the keyboard flipping is the tap's visible answer, and the pictures (or the line saying why there are
                // none) follow in the thread.
                _showCardPage(sid, messageId, CardPage::Main);
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
                    // HOOKS.md "Interrupted turns": a RUNNING turn cut off by the restart is no user interrupt. This confirm is
                    // posted only when the restarted claude would NOT continue the turn by itself (CLAUDE_CODE_RESUME_INTERRUPTED_TURN
                    // off, or nothing to continue): with the switch on, the third question asks instead
                    // (_askRestartTurnBeforeRestart), so "Restart now" has no turn to stop — the restart goes as it always did.
                    _hooks.restartSession(tok.sessionId, RestartTurnChoice::KeepInterrupted);
                }
                _reply(threadId, L"Restarting the session.", false);
                // §21.28: the restart is the window's work, and its card shows it (the turn cut off, the state after): the button stays
                // busy until the card changes.
                _awaitCardChange(tok.sessionId);
                LogNav(L"remote telegram /restartsession " + ShortId(tok.sessionId) + L" -> confirmed");
            }
            else if (k == L"restart-uninterrupt" || k == L"restart-keep")
            {
                // HOOKS.md "Interrupted turns": the answer to the first restart question, a turn the USER interrupted
                // (_askUninterruptBeforeRestart): ▶ Restart uninterrupted (restart-uninterrupt) / ⏸ Restart interrupted
                // (restart-keep). The tokens keep their names (the words on the buttons changed on 2026-11-06, the answers did
                // not). The hosting window plans again on the transcript as it is now (Uninterrupt) and restarts.
                const bool uninterrupt = k == L"restart-uninterrupt";
                if (_hooks.restartSession)
                {
                    _hooks.restartSession(tok.sessionId, uninterrupt ? RestartTurnChoice::Uninterrupt : RestartTurnChoice::KeepInterrupted);
                }
                _reply(threadId, uninterrupt ? L"\x25B6 Restarting it uninterrupted \x2014 the interrupt is taken back, and the turn goes on." : L"\x23F8 Restarting it interrupted \x2014 the turn stays interrupted.", false);
                _awaitCardChange(tok.sessionId); // §21.28: until the card shows the restarted session
                LogNav(L"remote telegram /restartsession " + ShortId(tok.sessionId) + (uninterrupt ? L" -> restart uninterrupted (the user's interrupt taken back)" : L" -> restart interrupted (the user's interrupt kept)"));
            }
            else if (k == L"restart-interrupted" || k == L"restart-uninterrupted")
            {
                // HOOKS.md "Interrupted turns": the answer to the third question, "restart interrupted or uninterrupted?"
                // (_askRestartTurnBeforeRestart). The hosting window restarts with it: StopContinuedTurn records the stop intent and
                // arms its stop watch (the tab locked, ONE Esc the moment the continued turn runs); LetTurnContinue lets the turn
                // run. For a turn that stopped on an API error (situation B) BOTH take the error back with the switch set for the
                // restarted claude, and StopContinuedTurn's Esc then leaves a regular interrupt where the error was (the user's rule
                // of 2026-11-06). The re-plan there decides which situation applies, so the two buttons serve both; the token's
                // payload ("api-error") only picks the words of this reply.
                const bool stopTurn = k == L"restart-interrupted";
                const bool apiError = tok.payload == L"api-error";
                if (_hooks.restartSession)
                {
                    _hooks.restartSession(tok.sessionId, stopTurn ? RestartTurnChoice::StopContinuedTurn : RestartTurnChoice::LetTurnContinue);
                }
                if (apiError)
                {
                    _reply(threadId, stopTurn ? L"\x23F8 Restarting it interrupted \x2014 the API error is taken back, and Agentmaster stops the turn with Esc the moment it goes on: it waits for you with a regular interrupt." :
                                                L"\x25B6 Restarting it uninterrupted \x2014 the API error is taken back, and the turn goes on by itself.",
                           false);
                }
                else
                {
                    _reply(threadId, stopTurn ? L"\x23F8 Restarting it \x2014 it waits for you: the turn does not go on by itself." : L"\x25B6 Restarting it \x2014 the turn goes on by itself.", false);
                }
                _awaitCardChange(tok.sessionId); // §21.28: until the card shows the restarted session
                LogNav(L"remote telegram /restartsession " + ShortId(tok.sessionId) + (stopTurn ? L" -> restart interrupted" : L" -> restart uninterrupted") + (apiError ? L" (an API error)" : L""));
            }
            else if (k == L"fork-interrupted" || k == L"fork-uninterrupted" || k == L"fork-take-back-and-interrupt" || k == L"fork-take-back-and-continue" || k == L"fork-keep-interrupt")
            {
                // HOOKS.md "Interrupted turns": the answer to the fork question (_askForkTurnBeforeLaunch). The payload carries the
                // request's dir, title and model (U+001F-separated), so the fork goes exactly as asked. Each situation has its own
                // tokens, because one button means another action in each:
                //   A (the fork would continue by itself): fork-interrupted = the stop watch, fork-uninterrupted = a plain fork.
                //   B (the source's turn died on an API error): fork-take-back-and-interrupt ("Fork interrupted") = the take-back
                //     AND the stop watch (a regular interrupt replaces the error), fork-take-back-and-continue ("Fork
                //     uninterrupted") = the take-back (the window appends the line to the source's transcript, launches the fork
                //     with the switch and starts it at once).
                //   C (the user interrupted the source's turn): fork-keep-interrupt ("Fork interrupted") = a plain fork that waits
                //     at the interrupt, fork-take-back-and-continue ("Fork uninterrupted") = the take-back of the interrupt.
                RemoteLaunchRequest req;
                req.kind = RemoteLaunchRequest::Kind::Fork;
                req.sessionId = tok.sessionId;
                const std::wstring& p = tok.payload;
                const size_t a = p.find(L'\x1F');
                const size_t b = a == std::wstring::npos ? std::wstring::npos : p.find(L'\x1F', a + 1);
                req.dir = p.substr(0, a);
                req.title = a == std::wstring::npos ? std::wstring{} : p.substr(a + 1, b == std::wstring::npos ? std::wstring::npos : b - a - 1);
                req.model = b == std::wstring::npos ? std::wstring{} : p.substr(b + 1);
                req.forkTurn = k == L"fork-interrupted"             ? ForkTurnChoice::Interrupted :
                               k == L"fork-take-back-and-interrupt" ? ForkTurnChoice::TakeBackAndInterrupt :
                               k == L"fork-take-back-and-continue"  ? ForkTurnChoice::TakeBackAndContinue :
                                                                      ForkTurnChoice::Uninterrupted; // fork-uninterrupted, fork-keep-interrupt
                LogNav(L"remote telegram fork " + ShortId(tok.sessionId) + L" -> " +
                       (k == L"fork-keep-interrupt" ? std::wstring{ L"fork interrupted (the fork waits at the user's interrupt)" } : std::wstring{ ForkTurnChoiceName(req.forkTurn) }));
                _requestLaunch(req, threadId); // it says how the fork goes
            }
            else if (k == L"confirm-deactivate")
            {
                if (_hooks.sessionAction)
                {
                    // §21.41: "Park now" - the user allowed the cut-off, so the hosting window parks a turn in flight too
                    // (kDeactivateCutOffTurnArg), and its answer is the line; §21.28: the button stays busy until it (and, after a
                    // park, the card showing ▶ Reactivate) is in.
                    _deactivateRemote(tok.sessionId, threadId, /*cutOffTurn*/ true, /*fromCard*/ false);
                }
                else
                {
                    // A host without the window round trip (the m5 harness): the fire-and-forget park.
                    if (_hooks.deactivateSession)
                    {
                        _hooks.deactivateSession(tok.sessionId);
                    }
                    _reply(threadId, L"Parked.", false);
                    _awaitCardChange(tok.sessionId); // §21.28: until the card shows the parked tab (its ▶ Reactivate)
                }
                LogNav(L"remote telegram /deactivate " + ShortId(tok.sessionId) + L" -> confirmed");
            }
            else if (k == L"confirm-interrupt")
            {
                if (_hooks.sessionAction)
                {
                    // §21.41: the card's ⎋ Esc path. The hosting window writes the Esc on the UI thread every keystroke of the tab
                    // goes through, and refuses while a delivery or a draft swap owns the box (an Esc between a delivery's keystrokes
                    // or right after its Enter would put the prompt back, and the Enter-retry watch would send it again); its answer
                    // is the line. The press is claimed against kRemoteEscGapMs like a card tap: a ⎋ Esc tapped a moment before must
                    // not make this the second Esc of Claude Code's Rewind double press.
                    if (!_claimEscPress(tok.sessionId))
                    {
                        _reply(threadId, TgEscapeHtml(std::wstring{ kRemoteEscRepeatedText }), false);
                        LogNav(L"remote telegram /interrupt " + ShortId(tok.sessionId) + L" -> repeated (within " + std::to_wstring(kRemoteEscGapMs / 1000) + L" s of the last Esc)");
                    }
                    else
                    {
                        // The question was asked while a turn ran; the state now only picks the line's wording (a turn the user
                        // stopped at the PC meanwhile reads "No turn was running").
                        const auto s = _registry->GetWithoutQueue(tok.sessionId);
                        _pressEscRemote(tok.sessionId, threadId, /*fromCard*/ false, s ? s->state : SessionState::Running);
                    }
                }
                else
                {
                    // A host without the window round trip (the m5 harness): one Esc through the registry's injector, from here.
                    const bool ok = _registry->Inject(tok.sessionId, L"\x1b");
                    _reply(threadId, ok ? L"\x25A0 Interrupt sent." : L"Could not reach the session (no input bound).", false);
                    if (ok)
                    {
                        _awaitCardChange(tok.sessionId); // §21.28: until the card shows the stopped turn
                    }
                    LogNav(L"remote telegram /interrupt " + ShortId(tok.sessionId) + (ok ? L" -> sent" : L" -> refused:no-injector"));
                }
            }
            else if (k == L"wake" || k == L"wake-pin")
            {
                _wake(tok.sessionId, k == L"wake-pin");
                _reply(threadId, L"Woken: its topic appears in a moment.", false);
            }
            else if (IsSearchPickActionKind(k))
            {
                // A Search pick's action menu (_showSearchPick; its buttons fit where the session stood, §21.38): it acts on the
                // session's state NOW, and edits or deletes the menu it sits on.
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
        // HOOKS.md "Interrupted turns": a fork the topic asked for, of a session in the middle of a turn, is asked about first —
        // the buttons send this request again with the answer. No question: it forks as it always did.
        if (req.kind == RemoteLaunchRequest::Kind::Fork && req.forkTurn == ForkTurnChoice::Ask)
        {
            if (_askForkTurnBeforeLaunch(req, threadId))
            {
                return;
            }
            req.forkTurn = ForkTurnChoice::Uninterrupted;
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
            // A turn runs: the question /restartsession asks goes in the thread — "Restart interrupted / uninterrupted" when the
            // restarted claude would continue the turn by itself (HOOKS.md "Interrupted turns", the third question), else the
            // confirm that the turn is cut off — each with its single-use buttons.
            RemoteCommand cmd;
            cmd.kind = RemoteKind::AgentmasterCommand;
            cmd.name = L"restartsession";
            _runSessionCommand(sessionId, cmd, json::Value::MkObj(), threadId);
            return L"A turn is running \x2014 answer the question below to restart.";
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
        // HOOKS.md "Interrupted turns": a turn the USER interrupted is asked about in the thread first (the buttons restart).
        if (_askUninterruptBeforeRestart(sessionId, *s, threadId))
        {
            LogNav(L"remote telegram restart (card) " + ShortId(sessionId) + L" -> asked (an interrupted turn)");
            return L"Your last turn was interrupted \x2014 choose below whether the restart continues it.";
        }
        // ...else a turn the restarted claude would continue by itself (Needs approval, a call in flight, a turn a crash cut
        // off) gets the third question in the thread: restart interrupted or uninterrupted (the buttons restart).
        if (_askRestartTurnBeforeRestart(sessionId, *s, threadId))
        {
            LogNav(L"remote telegram restart (card) " + ShortId(sessionId) + L" -> asked (a turn that would go on)");
            return L"Its turn would go on after the restart \x2014 choose below whether it does.";
        }
        try
        {
            _hooks.restartSession(sessionId, RestartTurnChoice::KeepInterrupted);
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

    // ===== §21.41: the card's ⎋ Esc and 🌙 Deactivate (and the typed /interrupt and /deactivate they share a path with) ======

    std::wstring MessagingService::_escFromCard(const std::wstring& sessionId, int64_t threadId)
    {
        // The lean read: nothing here needs the queue.
        const auto s = _registry->GetWithoutQueue(sessionId);
        if (!s || !s->live)
        {
            LogNav(L"remote telegram esc (card) " + ShortId(sessionId) + L" -> closed");
            return L"This session is closed. Resume it from the \U0001F50E Search topic.";
        }
        // The service's own reading first, with the box counted as free (only the hosting window can see it): a tap that can press
        // nothing - a parked tab, an exited claude, a Codex session - is answered at once, with no round trip to a window.
        if (const RemoteEscResult early = DecideRemoteEsc(*s, /*boxHeld*/ false); early != RemoteEscResult::Pressed)
        {
            LogNav(L"remote telegram esc (card) " + ShortId(sessionId) + L" -> " + std::wstring{ RemoteEscResultWord(early) });
            return RemoteEscOutcomeText(early, s->state);
        }
        if (!_hooks.sessionAction)
        {
            return L"Not available here.";
        }
        // One Esc per kRemoteEscGapMs: a double tap must not become the double Esc that opens Claude Code's Rewind menu.
        if (!_claimEscPress(sessionId))
        {
            LogNav(L"remote telegram esc (card) " + ShortId(sessionId) + L" -> repeated (within " + std::to_wstring(kRemoteEscGapMs / 1000) + L" s of the last Esc)");
            return std::wstring{ kRemoteEscRepeatedText };
        }
        _pressEscRemote(sessionId, threadId, /*fromCard*/ true, s->state);
        return {}; // the window's answer sets the toast (_onEscAnswered), and the caller must leave the tap alone
    }

    std::wstring MessagingService::_deactivateFromCard(const std::wstring& sessionId, int64_t threadId)
    {
        const auto s = _registry->GetWithoutQueue(sessionId);
        if (!s || !s->live)
        {
            LogNav(L"remote telegram deactivate (card) " + ShortId(sessionId) + L" -> closed");
            return RemoteParkOutcomeText(RemoteParkResult::Closed);
        }
        // The service's own reading, with the box counted as free (the window judges it): it settles everything but a park itself.
        const RemoteParkResult early = DecideRemotePark(*s, /*boxHeld*/ false, /*cutOffTurn*/ false);
        if (early == RemoteParkResult::TurnRunning)
        {
            // A turn would be cut off (and read Error on its resume): /deactivate's own question, with its single-use [Park now]
            // [Cancel], goes into the thread, and the toast points at it - 🔄 Restart Session's rule for a running turn.
            RemoteCommand cmd;
            cmd.kind = RemoteKind::AgentmasterCommand;
            cmd.name = L"deactivate";
            _runSessionCommand(sessionId, cmd, json::Value::MkObj(), threadId);
            return RemoteParkOutcomeText(RemoteParkResult::TurnRunning);
        }
        if (early != RemoteParkResult::Parked)
        {
            LogNav(L"remote telegram deactivate (card) " + ShortId(sessionId) + L" -> " + std::wstring{ RemoteParkResultWord(early) });
            return RemoteParkOutcomeText(early);
        }
        if (!_hooks.sessionAction)
        {
            return L"Not available here.";
        }
        _deactivateRemote(sessionId, threadId, /*cutOffTurn*/ false, /*fromCard*/ true);
        return {}; // the window's answer sets the toast (_onParkAnswered), and the caller must leave the tap alone
    }

    bool MessagingService::_claimEscPress(const std::wstring& sessionId)
    {
        // The MONOTONIC clock: a wall-clock step (a time sync, a manual change) must neither block an Esc for minutes nor let two
        // through at once.
        const int64_t now = static_cast<int64_t>(::GetTickCount64());
        std::lock_guard lk{ _mtx };
        auto& last = _escPressMs[sessionId];
        if (RemoteEscTooSoon(last, now))
        {
            return false;
        }
        last = now;
        return true;
    }

    void MessagingService::_releaseEscPress(const std::wstring& sessionId) noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            _escPressMs.erase(sessionId);
        }
        catch (...)
        {
            // Rule #18. Recovery: the stamp stays and expires by itself after kRemoteEscGapMs; a retry inside that window is
            // answered with kRemoteEscRepeatedText, which says to tap again in a moment.
            LogSwallowedException(L"MessagingService::_releaseEscPress");
        }
    }

    void MessagingService::_tellRemoteOutcome(int64_t threadId, bool fromCard, const std::wstring& text, const std::vector<TapHoldRef>& taps)
    {
        if (fromCard)
        {
            // The tapped button's toast, set through the request's own holds: they keep the tap unanswered until the caller lets them
            // go, so the answer the last hold triggers carries this text (TapAnswer::SetToast takes its own lock).
            for (const auto& hold : taps)
            {
                if (hold && hold->Tap())
                {
                    hold->Tap()->SetToast(text);
                }
            }
            return;
        }
        // A typed command: a silent line in the topic (inside the caller's tap scope, so a confirm button's tap waits for it).
        _reply(threadId, TgEscapeHtml(text), false);
    }

    void MessagingService::_pressEscRemote(const std::wstring& sessionId, int64_t threadId, bool fromCard, SessionState stateAtTap)
    {
        const auto self = weak_from_this();
        // §21.28: the tapped ⎋ Esc (or /interrupt's "Interrupt now") stays busy until the window answered: the reply and its guard
        // carry the holds, which go with the last copy of the reply (every window's sink copies it once).
        const std::vector<TapHoldRef> taps = _holdTaps();
        const std::wstring via = fromCard ? L" (card) " : L" (/interrupt) ";
        // The guard reports "no window answered" when every copy of the reply goes without a call (no window hosts the tab): the
        // stamp is taken back (nothing confirmed a press), and the user is told rather than left with a button that just stops.
        const auto once = std::make_shared<SessionActionAnswer>([self, sessionId, threadId, fromCard, taps, via]() {
            if (const auto me = self.lock())
            {
                const TapScope tapScope{ taps }; // a line it posts holds a confirm button's tap until Telegram answered it
                me->_releaseEscPress(sessionId);
                me->_tellRemoteOutcome(threadId, fromCard, std::wstring{ kRemoteEscUnansweredText }, taps);
                LogNav(L"remote telegram esc" + via + ShortId(sessionId) + L" -> no-answer (no window hosts its tab)");
            }
        });
        LogNav(L"remote telegram esc" + via + ShortId(sessionId) + L" -> asked the hosting window");
        _hooks.sessionAction(sessionId, std::wstring{ kPressEscAction }, std::wstring{}, [self, sessionId, threadId, fromCard, stateAtTap, taps, once](std::wstring word) {
            // The hosting window's UI thread. The claim comes first, so the guard stays silent once a window has answered.
            if (!once->Claim())
            {
                return;
            }
            if (const auto me = self.lock())
            {
                const TapScope tapScope{ taps }; // _awaitCardChange and a posted line hold the tap from this scope
                me->_onEscAnswered(sessionId, threadId, fromCard, stateAtTap, ParseRemoteEscResult(word), taps);
            }
        });
    }

    void MessagingService::_onEscAnswered(const std::wstring& sessionId, int64_t threadId, bool fromCard, SessionState stateAtTap, RemoteEscResult result, const std::vector<TapHoldRef>& taps) noexcept
    {
        try
        {
            if (result != RemoteEscResult::Pressed)
            {
                _releaseEscPress(sessionId); // nothing was written: the retry the toast invites (a busy box) may go at once
            }
            _tellRemoteOutcome(threadId, fromCard, RemoteEscOutcomeText(result, stateAtTap), taps);
            if (result == RemoteEscResult::Pressed && TurnInFlightState(stateAtTap))
            {
                // §21.28: the turn stops - the scanner reads Claude Code's interrupt row, and the card's state line changes (and its ⎋ Esc
                // goes); the button stays busy until that edit is in, or until the hold's limit, which then carries the toast above.
                _awaitCardChange(sessionId);
            }
            LogNav(L"remote telegram esc" + std::wstring{ fromCard ? L" (card) " : L" (/interrupt) " } + ShortId(sessionId) + L" -> " + std::wstring{ RemoteEscResultWord(result) });
        }
        catch (...)
        {
            // Rule #18. Recovery: the tap is answered with what was set so far (its hold's limit at worst), and an Esc the window
            // wrote stands - nothing is retried.
            LogSwallowedException(L"MessagingService::_onEscAnswered");
        }
    }

    void MessagingService::_deactivateRemote(const std::wstring& sessionId, int64_t threadId, bool cutOffTurn, bool fromCard)
    {
        const auto self = weak_from_this();
        // §21.28: as for the Esc - the reply and its guard carry the tapped button's holds.
        const std::vector<TapHoldRef> taps = _holdTaps();
        const std::wstring via = fromCard ? L" (card) " : (cutOffTurn ? L" (/deactivate, confirmed) " : L" (/deactivate) ");
        const auto once = std::make_shared<SessionActionAnswer>([self, sessionId, threadId, fromCard, taps, via]() {
            if (const auto me = self.lock())
            {
                const TapScope tapScope{ taps };
                me->_tellRemoteOutcome(threadId, fromCard, std::wstring{ kRemoteParkUnansweredText }, taps);
                LogNav(L"remote telegram deactivate" + via + ShortId(sessionId) + L" -> no-answer (no window hosts its tab)");
            }
        });
        LogNav(L"remote telegram deactivate" + via + ShortId(sessionId) + L" -> asked the hosting window" + (cutOffTurn ? L" (a turn in flight may be cut off)" : L""));
        // ⚠ kDeactivateCutOffTurnArg only after the user's "Park now": the window reads any other argument as "at rest only".
        _hooks.sessionAction(sessionId, std::wstring{ kDeactivateAction }, cutOffTurn ? std::wstring{ kDeactivateCutOffTurnArg } : std::wstring{}, [self, sessionId, threadId, fromCard, taps, once](std::wstring word) {
            if (!once->Claim())
            {
                return;
            }
            if (const auto me = self.lock())
            {
                const TapScope tapScope{ taps };
                me->_onParkAnswered(sessionId, threadId, fromCard, ParseRemoteParkResult(word), taps);
            }
        });
    }

    void MessagingService::_onParkAnswered(const std::wstring& sessionId, int64_t threadId, bool fromCard, RemoteParkResult result, const std::vector<TapHoldRef>& taps) noexcept
    {
        try
        {
            if (result == RemoteParkResult::TurnRunning)
            {
                // A turn started between the tap and the park (the user typed at the PC, the runner sent a queued prompt): nothing was
                // parked, and the user decides - /deactivate's question goes into the thread (it reads the state again, so a turn that
                // ended meanwhile is parked by it instead), and a tapped button's toast points at the question.
                RemoteCommand cmd;
                cmd.kind = RemoteKind::AgentmasterCommand;
                cmd.name = L"deactivate";
                _runSessionCommand(sessionId, cmd, json::Value::MkObj(), threadId);
                if (fromCard)
                {
                    _tellRemoteOutcome(threadId, true, RemoteParkOutcomeText(result), taps);
                }
            }
            else
            {
                _tellRemoteOutcome(threadId, fromCard, RemoteParkOutcomeText(result), taps);
                if (result == RemoteParkResult::Parked)
                {
                    _awaitCardChange(sessionId); // §21.28: until the card shows the parked tab (its ▶ Reactivate)
                }
            }
            LogNav(L"remote telegram deactivate" + std::wstring{ fromCard ? L" (card) " : L" (/deactivate) " } + ShortId(sessionId) + L" -> " + std::wstring{ RemoteParkResultWord(result) });
        }
        catch (...)
        {
            // Rule #18. Recovery: the tap is answered with what was set so far; a park the window made stands (the card shows it).
            LogSwallowedException(L"MessagingService::_onParkAnswered");
        }
    }

    // ===== §21.47: /enter - ONE Enter in the terminal, submitting the prompt that sits in its box ==========================

    void MessagingService::_enterRemote(const std::wstring& sessionId, const RemoteCommand& cmd, const json::Value& msg, int64_t threadId, bool held)
    {
        const int64_t msgId = msg.I64At(L"message_id", 0);
        const int64_t dateMs = msg.I64At(L"date", 0) * 1000;
        const auto nav = [&](std::wstring_view outcome) {
            LogNav(L"remote telegram /enter " + ShortId(sessionId) + L" -> " + std::wstring{ outcome });
        };
        // Every answer below replies to the /enter message itself: in a busy topic the line then says which /enter it answers.
        const auto answer = [&](const std::wstring& html) { _reply(threadId, html, false, json::Value{}, msgId); };
        if (!cmd.args.empty())
        {
            // "/enter fix it" would press Enter on whatever the box holds while the words went nowhere: never pressed, the usage
            // line instead (ParseRemoteCommand trims the args, so blanks alone read as none).
            answer(RemoteEnterUsageHtml());
            nav(L"usage");
            return;
        }
        if (!held && !msg.BoolAt(L"am_voice_live") && IsBacklogUpdate(dateMs, WallMs()))
        {
            // D2: written while Agentmaster was closed. Unlike a message it is not offered for later: an Enter belongs to the screen
            // of its moment, and the box may hold a different prompt by now (a restart types the remembered draft back). A voice
            // note's transcript that was live when the note ARRIVED is no backlog however long its transcription took (§21.43's
            // flag, the plain message's rule).
            answer(RemoteEnterBacklogHtml(TgShortSpan(WallMs() - dateMs)));
            nav(L"refused:backlog");
            return;
        }
        // The lean read: nothing here needs the queue.
        const auto s = _registry->GetWithoutQueue(sessionId);
        if (!s || !s->live)
        {
            answer(std::wstring{ kRemoteEnterClosedText }); // no markup in it: posted as is
            nav(L"refused:closed");
            return;
        }
        // The record's own checks first (a parked tab, an exited claude, Codex): answered at once, with no round trip to a window.
        if (const RemoteEnterResult early = DecideRemoteEnterTarget(*s); early != RemoteEnterResult::Pressed)
        {
            answer(RemoteEnterOutcomeHtml(RemoteEnterAnswer{ early, {} }, s->state));
            nav(RemoteEnterResultWord(early));
            return;
        }
        if (!_hooks.sessionAction)
        {
            // A host without the window round trip (the m5 harness): nothing can read the box, and a blind Enter is exactly what this
            // command must never write, so there is no fallback press (unlike /interrupt's fire-and-forget Esc).
            answer(L"Not available here.");
            nav(L"refused:no-window-hook");
            return;
        }
        _pressEnterRemote(sessionId, threadId, msgId, s->state);
    }

    void MessagingService::_pressEnterRemote(const std::wstring& sessionId, int64_t threadId, int64_t replyTo, SessionState stateAtAsk)
    {
        const auto self = weak_from_this();
        // The guard reports "no window answered" when every window's copy of the reply goes without a call (no window hosts the tab),
        // so the /enter is never left without a line. The hosting window keeps its copy until its read-back ends (up to
        // kRemoteEnterVerifyMs), so the guard can only fire after that, never while the window is still reading.
        const auto once = std::make_shared<SessionActionAnswer>([self, sessionId, threadId, replyTo]() {
            if (const auto me = self.lock())
            {
                me->_reply(threadId, std::wstring{ kRemoteEnterUnansweredText }, false, json::Value{}, replyTo); // no markup in it
                LogNav(L"remote telegram /enter " + ShortId(sessionId) + L" -> no-answer (no window hosts its tab)");
            }
        });
        LogNav(L"remote telegram /enter " + ShortId(sessionId) + L" -> asked the hosting window");
        _hooks.sessionAction(sessionId, std::wstring{ kPressEnterAction }, std::wstring{}, [self, sessionId, threadId, replyTo, stateAtAsk, once](std::wstring reply) {
            // The hosting window's UI thread, once its read-back has a verdict (or at once, when it pressed nothing). The claim comes
            // first, so the guard stays silent once a window has answered.
            if (!once->Claim())
            {
                return;
            }
            if (const auto me = self.lock())
            {
                me->_onEnterAnswered(sessionId, threadId, replyTo, stateAtAsk, reply);
            }
        });
    }

    void MessagingService::_onEnterAnswered(const std::wstring& sessionId, int64_t threadId, int64_t replyTo, SessionState stateAtAsk, const std::wstring& reply) noexcept
    {
        try
        {
            const RemoteEnterAnswer answer = ParseRemoteEnterReply(reply);
            _reply(threadId, RemoteEnterOutcomeHtml(answer, stateAtAsk), false, json::Value{}, replyTo);
            LogNav(L"remote telegram /enter " + ShortId(sessionId) + L" -> " + std::wstring{ RemoteEnterResultWord(answer.result) });
        }
        catch (...)
        {
            // Rule #18. Recovery: the topic gets no line for this /enter. An Enter the window pressed stands (nothing is retried), and
            // the card's next state shows what Claude Code did with it.
            LogSwallowedException(L"MessagingService::_onEnterAnswered");
        }
    }

    bool MessagingService::_askUninterruptBeforeRestart(const std::wstring& sessionId, const SessionInfo& s, int64_t threadId)
    {
        if (s.kind != AgentKind::Claude)
        {
            return false; // a Codex rollout has no interrupt rows to take back
        }
        // The plan reads the transcript's tail (one small read in the common case — no API-error or interrupt bytes there means
        // no parse at all), on the bot's own thread.
        const auto plan = PlanInterruptRewindForSession(sessionId);
        if (plan.verdict != InterruptRewindVerdict::Rewind)
        {
            if (plan.verdict != InterruptRewindVerdict::NotApplicable)
            {
                AppendStateLog(L"hooks.log", L"[interrupt-rewind] " + ShortId(sessionId) + L" telegram restart: not offered \x2014 " + plan.reason + L"\n");
            }
            return false;
        }
        const AutoContinueSetting setting = ResolveClaudeAutoContinue(s.workingDir, ResolveSessionEnv(LoadAppSettings(), s.workingDir));
        const AutoContinueAtRestart mode = ClassifyAutoContinueAtRestart(setting.on, setting.fromSettingsFile);
        // The topic's words: the PC dialog's, shortened for a phone (UninterruptQuestionBody is the long form) — the answers named
        // as the buttons are, the words of every restart and fork question (2026-11-06).
        std::wstring html = L"\x23F8 You interrupted <b>" + TgTitleToken(TgTitleForm::Plain, _titleSquare(&s), s.title, _threadOf(sessionId)) + L"</b> ";
        html += plan.duringToolUse ? L"during a tool call." : L"in the middle of a reply.";
        html += L"\n\n<b>Restart uninterrupted</b>: before the restart, Agentmaster adds one line to the transcript that takes the interrupt back";
        if (mode == AutoContinueAtRestart::OffInSettings)
        {
            html += L"; CLAUDE_CODE_RESUME_INTERRUPTED_TURN is off in your settings, so your next message continues it.";
        }
        else if (plan.targetKind == L"prompt")
        {
            html += L", and the restarted Claude sends your prompt";
            if (!plan.targetText.empty())
            {
                html += L" \x201C" + TgEscapeHtml(plan.targetText) + L"\x201D";
            }
            html += L" again.";
        }
        else
        {
            html += L", and the restarted Claude carries on from the last step that finished.";
        }
        html += L" Nothing is deleted.\n<b>Restart interrupted</b>: the restarted session waits for you, interrupted as it is now.";
        const std::wstring yes = _tokenFor(L"restart-uninterrupt", sessionId, L"", kTokenTtlMsFor(1));
        const std::wstring keep = _tokenFor(L"restart-keep", sessionId, L"", kTokenTtlMsFor(1));
        const std::wstring no = _tokenFor(L"cancel", sessionId, L"", kTokenTtlMsFor(1));
        // The buttons in the other questions' places: ⏸ interrupted first, ▶ uninterrupted second.
        _reply(threadId, html, false, _buttons({ { { L"\x23F8 Restart interrupted", keep }, { L"\x25B6 Restart uninterrupted", yes } }, { { L"Cancel", no } } }));
        LogNav(L"remote telegram restart " + ShortId(sessionId) + L" -> asked: the turn the user interrupted (" + plan.targetKind + L", " + (setting.on ? L"auto-continue on" : L"auto-continue off") + L")");
        return true;
    }

    bool MessagingService::_askRestartTurnBeforeRestart(const std::wstring& sessionId, const SessionInfo& s, int64_t threadId)
    {
        if (s.kind != AgentKind::Claude)
        {
            return false; // a Codex restart resumes a rollout: there is no Claude resume to stop
        }
        if (!s.started && !s.external)
        {
            // A parked tab is STARTED, not restarted (_reactivate — the window's restart refuses a connection that never
            // started): there is no restart for an answer to steer. The callers send a parked tab there before asking.
            return false;
        }
        // "The turn runs right now": the session's live state (Running or Needs approval — the restart's shutdown cuts either);
        // for a never-messaged fork, which the restart RE-FORKS from its source, also the source's (the re-fork continues the
        // source's turn). The rule is the PC's (_RestartTurnQuestionFor), so the phone and the PC ask about the same restarts.
        const auto turnRunningOf = [](const SessionInfo& r) { return r.live && (r.state == SessionState::Running || r.state == SessionState::NeedsApproval); };
        bool turnRunning = turnRunningOf(s);
        if (!turnRunning && !s.forkParentId.empty() && !ClaudeConversationExists(sessionId))
        {
            const auto source = _registry->GetWithoutQueue(s.forkParentId);
            turnRunning = source && turnRunningOf(*source);
        }
        // The tail read and the settings reads run on the bot's own thread (small reads), never on a UI thread. Situation B (a turn
        // that died on an API error) is considered while Agentmaster may take API errors back (AppSettings::resumeApiErrorTurns).
        const AppSettings settings = LoadAppSettings();
        const auto outlook = AssessRestartContinuation(sessionId, s.forkParentId, turnRunning, s.workingDir, ResolveSessionEnv(settings, s.workingDir), settings.resumeApiErrorTurns);
        if (!outlook.ask)
        {
            LogNav(L"remote telegram restart " + ShortId(sessionId) + L" -> not asked about a turn that would go on: " + outlook.why);
            return false;
        }
        std::wstring html = L"\U0001F504 <b>" + TgTitleToken(TgTitleForm::Plain, _titleSquare(&s), s.title, _threadOf(sessionId)) + L"</b> ";
        if (outlook.apiError)
        {
            // Situation B, the PC dialog's words (ApiErrorTurnQuestionBody) shortened for a phone: the error quoted, the take-back
            // and what the switch makes of it.
            const AutoContinueAtRestart mode = ClassifyAutoContinueAtRestart(outlook.autoContinue.on, outlook.autoContinue.fromSettingsFile);
            html += L"stopped on an API error";
            if (!outlook.apiErrorPlan.errorText.empty())
            {
                html += L": <i>" + TgEscapeHtml(outlook.apiErrorPlan.errorText) + L"</i>";
            }
            html += L". A restarted Claude would wait at it.\n\n<b>Restart uninterrupted</b>: Agentmaster takes the error back (one line added to the transcript; nothing is deleted)";
            if (mode == AutoContinueAtRestart::OffInSettings)
            {
                html += L"; CLAUDE_CODE_RESUME_INTERRUPTED_TURN is off in your settings, so your next message continues the turn.";
            }
            else
            {
                html += outlook.apiErrorPlan.targetKind == L"prompt" ? L", and the restarted Claude sends your prompt again by itself." : L", and the restarted Claude carries on with the turn by itself.";
            }
            // "Interrupted" takes the error back too (the user's rule of 2026-11-06: "interrupted keeps but replaces error with
            // regular interrupt"); with the switch off in a settings file nothing starts to stop, and the turn simply waits.
            html += mode == AutoContinueAtRestart::OffInSettings ? L"\n<b>Restart interrupted</b>: Agentmaster takes the error back too, and the restarted Claude waits for you." :
                                                                   L"\n<b>Restart interrupted</b>: Agentmaster takes the error back too, then presses Esc the moment the restarted Claude starts the turn, so it waits for you with a regular interrupt in place of the error.";
        }
        else
        {
            // The topic's words: the PC dialog's (RestartTurnQuestionBody), shortened for a phone and with the answers in bold.
            html += outlook.turnRunning ? L"is in the middle of a turn." : L"stopped in the middle of a turn.";
            html += L" A restart resumes the conversation in a new Claude, and Claude Code continues that turn by itself as soon as it starts";
            if (!outlook.tail.droppedTool.empty())
            {
                html += L" \x2014 the <code>" + TgEscapeHtml(outlook.tail.droppedTool) + L"</code> call it was running runs again";
            }
            else if (outlook.tail.verdict == ResumeContinuation::ResendsPrompt)
            {
                html += L" \x2014 the last prompt is sent again";
            }
            html += L".\n\n<b>Restart interrupted</b>: Agentmaster locks the tab and presses Esc the moment the restarted Claude starts running, so it waits for you.\n"
                    L"<b>Restart uninterrupted</b>: the restarted Claude carries on with the turn.";
        }
        // Single-use buttons (an hour, like every question's): the answer goes to the hosting window as a RestartTurnChoice, so
        // the PC restarts with it and asks nothing. Situation B's tokens carry "api-error" so the tap's reply can say what the
        // answer does to the error (the window re-plans either way).
        const std::wstring situation = outlook.apiError ? std::wstring{ L"api-error" } : std::wstring{};
        const std::wstring stop = _tokenFor(L"restart-interrupted", sessionId, situation, kTokenTtlMsFor(1));
        const std::wstring go = _tokenFor(L"restart-uninterrupted", sessionId, situation, kTokenTtlMsFor(1));
        const std::wstring no = _tokenFor(L"cancel", sessionId, L"", kTokenTtlMsFor(1));
        _reply(threadId, html, false, _buttons({ { { L"\x23F8 Restart interrupted", stop }, { L"\x25B6 Restart uninterrupted", go } }, { { L"Cancel", no } } }));
        LogNav(L"remote telegram restart " + ShortId(sessionId) + (outlook.apiError ? (L" -> asked: a turn that stopped on an API error (" + (outlook.apiErrorPlan.errorKind.empty() ? std::wstring{ L"no kind" } : outlook.apiErrorPlan.errorKind) + L"; " + outlook.autoContinue.source + L")") :
                                                                                     (L" -> asked: the turn would go on (" + (outlook.turnRunning ? std::wstring{ L"running" } : outlook.tail.tailKind) +
                                                                                      (outlook.reforks ? std::wstring{ L"; re-forks its source" } : std::wstring{}) + (outlook.tail.droppedTool.empty() ? std::wstring{} : (L"; re-runs " + outlook.tail.droppedTool)) + L")")));
        return true;
    }

    bool MessagingService::_askForkTurnBeforeLaunch(const RemoteLaunchRequest& req, int64_t threadId)
    {
        const auto s = _registry->Get(req.sessionId);
        if (s && s->kind != AgentKind::Claude)
        {
            return false; // a Codex fork (`codex fork`) has no Claude resume to stop
        }
        const bool sourceRunning = s && s->live && (s->state == SessionState::Running || s->state == SessionState::NeedsApproval);
        // Situation B (the source's turn died on an API error) is considered while Agentmaster may take API errors back
        // (AppSettings::resumeApiErrorTurns); situation C (the user interrupted the source's turn) always — as the PC's dialog does.
        const AppSettings settings = LoadAppSettings();
        const auto outlook = AssessForkContinuation(req.sessionId, sourceRunning, req.dir, ResolveSessionEnv(settings, req.dir), settings.resumeApiErrorTurns, /*considerUserInterrupt*/ true);
        if (!outlook.ask)
        {
            LogNav(L"remote telegram fork " + ShortId(req.sessionId) + L" -> not asked: " + outlook.why);
            return false;
        }
        const std::wstring title = s ? s->title : req.title;
        std::wstring html = L"\x2442 <b>" + (s ? TgTitleToken(TgTitleForm::Plain, _titleSquare(&*s), title, _threadOf(req.sessionId)) : TgEscapeHtml(title)) + L"</b> ";
        // The request's own fields ride the tokens, so the answer forks exactly what was asked (the model a /forksession typed).
        const std::wstring payload = req.dir + L'\x1F' + req.title + L'\x1F' + req.model;
        if (outlook.apiError)
        {
            // Situation B, the PC dialog's words (ApiErrorTurnQuestionBody) shortened for a phone. Its own tokens, because both
            // answers take the error back: "interrupted" adds the stop watch (fork-take-back-and-interrupt — a regular interrupt
            // replaces the error), "uninterrupted" lets the fork carry on (fork-take-back-and-continue) — the A tokens would mean
            // neither.
            const AutoContinueAtRestart mode = ClassifyAutoContinueAtRestart(outlook.autoContinue.on, outlook.autoContinue.fromSettingsFile);
            html += L"stopped on an API error";
            if (!outlook.apiErrorPlan.errorText.empty())
            {
                html += L": <i>" + TgEscapeHtml(outlook.apiErrorPlan.errorText) + L"</i>";
            }
            html += L". A fork starts from the same conversation, so it would wait at that error too.\n\n<b>Fork uninterrupted</b>: Agentmaster takes the error back for the fork (one line added to the source's transcript; the source stays where it is)";
            if (mode == AutoContinueAtRestart::OffInSettings)
            {
                html += L"; CLAUDE_CODE_RESUME_INTERRUPTED_TURN is off in your settings, so your next message in the fork continues the turn.";
            }
            else
            {
                html += L", and the fork starts at once and carries on with the turn by itself.";
            }
            // The user's rule of 2026-11-06: "interrupted keeps but replaces error with regular interrupt". With the switch off in a
            // settings file nothing starts to stop: the error is gone and the fork simply waits.
            html += mode == AutoContinueAtRestart::OffInSettings ? L"\n<b>Fork interrupted</b>: Agentmaster takes the error back for the fork too, and the fork waits for you." :
                                                                   L"\n<b>Fork interrupted</b>: Agentmaster takes the error back for the fork too, then presses Esc the moment the fork starts the turn, so it waits for you with a regular interrupt in place of the error.";
            const std::wstring stop = _tokenFor(L"fork-take-back-and-interrupt", req.sessionId, payload, kTokenTtlMsFor(1));
            const std::wstring go = _tokenFor(L"fork-take-back-and-continue", req.sessionId, payload, kTokenTtlMsFor(1));
            const std::wstring no = _tokenFor(L"cancel", req.sessionId, L"", kTokenTtlMsFor(1));
            _reply(threadId, html, false, _buttons({ { { L"\x23F8 Fork interrupted", stop }, { L"\x25B6 Fork uninterrupted", go } }, { { L"Cancel", no } } }));
            LogNav(L"remote telegram fork " + ShortId(req.sessionId) + L" -> asked (an API error: " + (outlook.apiErrorPlan.errorKind.empty() ? std::wstring{ L"no kind" } : outlook.apiErrorPlan.errorKind) + L")");
            return true;
        }
        if (outlook.userInterrupt)
        {
            // Situation C, the PC dialog's words (ForkInterruptQuestionBody) shortened for a phone. Its own tokens: "interrupted"
            // is a plain fork that waits at the interrupt (fork-keep-interrupt — nothing would run to stop), "uninterrupted" the
            // take-back of the interrupt (fork-take-back-and-continue, B's token: the same action).
            const AutoContinueAtRestart mode = ClassifyAutoContinueAtRestart(outlook.autoContinue.on, outlook.autoContinue.fromSettingsFile);
            html += outlook.interruptPlan.duringToolUse ? L"was interrupted by you during a tool call." : L"was interrupted by you in the middle of a reply.";
            html += L" A fork starts from the same conversation, so it would wait at your interrupt too.\n\n<b>Fork uninterrupted</b>: Agentmaster takes the interrupt back for the fork (one line added to the source's transcript; the source stays where it is)";
            if (mode == AutoContinueAtRestart::OffInSettings)
            {
                html += L"; CLAUDE_CODE_RESUME_INTERRUPTED_TURN is off in your settings, so your next message in the fork continues the turn.";
            }
            else if (outlook.interruptPlan.targetKind == L"prompt")
            {
                html += L", and the fork starts at once and sends your prompt";
                if (!outlook.interruptPlan.targetText.empty())
                {
                    html += L" \x201C" + TgEscapeHtml(outlook.interruptPlan.targetText) + L"\x201D";
                }
                html += L" again.";
            }
            else
            {
                html += L", and the fork starts at once and carries on from the last step that finished.";
            }
            html += L"\n<b>Fork interrupted</b>: the fork waits for you, interrupted like its source.";
            const std::wstring keep = _tokenFor(L"fork-keep-interrupt", req.sessionId, payload, kTokenTtlMsFor(1));
            const std::wstring go = _tokenFor(L"fork-take-back-and-continue", req.sessionId, payload, kTokenTtlMsFor(1));
            const std::wstring no = _tokenFor(L"cancel", req.sessionId, L"", kTokenTtlMsFor(1));
            _reply(threadId, html, false, _buttons({ { { L"\x23F8 Fork interrupted", keep }, { L"\x25B6 Fork uninterrupted", go } }, { { L"Cancel", no } } }));
            LogNav(L"remote telegram fork " + ShortId(req.sessionId) + L" -> asked (the user's interrupt, " + outlook.interruptPlan.targetKind + L")");
            return true;
        }
        html += sourceRunning ? L"is in the middle of a turn." : L"stopped in the middle of a turn.";
        html += L" Claude Code continues that turn in the fork by itself as soon as the fork's Claude starts";
        if (!outlook.tail.droppedTool.empty())
        {
            html += L" \x2014 the <code>" + TgEscapeHtml(outlook.tail.droppedTool) + L"</code> call it was running runs again in the fork";
        }
        html += L".\n\n<b>Fork interrupted</b>: Agentmaster locks the fork's tab and presses Esc the moment it starts running, so the fork waits for you.\n<b>Fork uninterrupted</b>: the fork runs the turn on its own.";
        const std::wstring stop = _tokenFor(L"fork-interrupted", req.sessionId, payload, kTokenTtlMsFor(1));
        const std::wstring go = _tokenFor(L"fork-uninterrupted", req.sessionId, payload, kTokenTtlMsFor(1));
        const std::wstring no = _tokenFor(L"cancel", req.sessionId, L"", kTokenTtlMsFor(1));
        _reply(threadId, html, false, _buttons({ { { L"\x23F8 Fork interrupted", stop }, { L"\x25B6 Fork uninterrupted", go } }, { { L"Cancel", no } } }));
        LogNav(L"remote telegram fork " + ShortId(req.sessionId) + L" -> asked (" + (sourceRunning ? std::wstring{ L"running" } : outlook.tail.tailKind) + L")");
        return true;
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
        // The page's word (CardPageLogWord): "main", "session", and the picker's four - "allscreen", "allscreen+diffs",
        // "allscreen+last", "allscreen+diffs+last" - so the trail says which boxes the user checked.
        LogNav(L"remote telegram card-page " + ShortId(cardOwner.empty() ? sessionId : cardOwner) + L" " + CardPageLogWord(page) +
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

    const wchar_t* MessagingService::_startConversationImage(const std::wstring& sessionId, int64_t threadId, const std::wstring& transcript, int turnsLevel, bool editDiffs, bool lastTurnOnly)
    {
        // The poll thread (a typed /allscreen, a tapped 📜 All Screen): nothing here may wait for the drawing.
        const auto unmark = [this, &sessionId]() {
            std::lock_guard lk{ _mtx };
            _conversationImageSessions.erase(sessionId);
        };
        if (_conversationImageRenders.fetch_add(1) >= kConversationImageMaxRenders)
        {
            _conversationImageRenders.fetch_sub(1);
            unmark();
            _reply(threadId, L"Another conversation is being drawn right now; ask again in a moment.", false);
            return L"busy";
        }
        std::wstring title;
        // The title only (the captions, the files' names), so the lean read: no queue is copied.
        if (const auto s = _registry->GetWithoutQueue(sessionId))
        {
            title = s->title;
        }
        try
        {
            // §21.28: the worker carries the tapped 📜 All Screen along (`taps`: empty for a typed /allscreen) and queues every part
            // inside a scope of them, so the button stays busy until the last picture - or the line saying why there is none - is in.
            std::thread([self = weak_from_this(), sessionId, threadId, transcript, turnsLevel, editDiffs, lastTurnOnly, title, taps = _holdTaps()]() noexcept {
                const auto me = self.lock();
                if (!me)
                {
                    return; // the service is gone (an app quit): its counter and marks went with it
                }
                const TapScope tapScope{ taps }; // never throws; the sends below hold the tap until Telegram answered them
                try
                {
                    ConversationImageOptions options;
                    options.level = static_cast<TurnsLevel>(turnsLevel);
                    options.editDiffs = editDiffs; // §21.39 "File Diffs": every file change as a diff, whatever the level
                    options.lastTurnOnly = lastTurnOnly; // §21.39 "Last Turn": the newest turn only
                    const std::wstring levelName = TurnsLevelName(options.level);
                    // What was drawn, in every caption and file name: the level always (the default is the cog's choice, so a bare
                    // name would not say which), and "+ diffs" when File Diffs was checked. "Last turn" names the WHAT instead of
                    // "conversation" (the caption's subject, the file's first word), so the two never read alike in the thread.
                    const std::wstring drawn = levelName + (editDiffs ? L" + diffs" : L"");
                    const wchar_t* const subject = lastTurnOnly ? L"last turn" : L"conversation";
                    const std::wstring titleLine = TgTitleLine(title, 120).empty() ? ShortId(sessionId) : TgTitleLine(title, 120);
                    // "conversation-Agentmaster-full.png", "conversation-Agentmaster-compact-diffs-2of4.png", "last-turn-Agentmaster-
                    // max-diffs.png": the part only when there are several.
                    const std::wstring stem = std::wstring{ lastTurnOnly ? L"last-turn-" : L"conversation-" } + SafeFileStem(title) + L"-" + levelName + (editDiffs ? L"-diffs" : L"");
                    size_t sent = 0;
                    size_t totalBytes = 0;
                    options.onPart = [&](ConversationImagePart&& part) {
                        // "📜 Agentmaster · conversation (full + diffs) · 2/4 · turns 12–31", "📜 Agentmaster · last turn (max) · turn
                        // 31": what was drawn, which part, and which turns it shows (the sheet counts the rows above the first turn
                        // heading - the path legend, a Last Turn's own note - as that turn; the 1 below only guards a sheet with none).
                        std::wstring caption = L"\U0001F4DC " + titleLine + L" \x00B7 " + subject + L" (" + drawn + L")";
                        if (part.count > 1)
                        {
                            caption += L" \x00B7 " + std::to_wstring(part.index + 1) + L"/" + std::to_wstring(part.count);
                        }
                        if (part.lastTurn > 0)
                        {
                            const int first = (std::max)(1, part.firstTurn);
                            caption += first == part.lastTurn ? L" \x00B7 turn " + std::to_wstring(first) :
                                                                L" \x00B7 turns " + std::to_wstring(first) + L"\x2013" + std::to_wstring(part.lastTurn);
                        }
                        const std::wstring fileName = stem + (part.count > 1 ? L"-" + std::to_wstring(part.index + 1) + L"of" + std::to_wstring(part.count) : std::wstring{}) + L".png";
                        totalBytes += part.picture.png.size();
                        // A DOCUMENT, not a photo: Telegram shrinks a photo to 2,560 px on its long side (a tall picture would be
                        // unreadable) and refuses one past 10,000 px of width + height; a document keeps every pixel, and its
                        // viewer zooms. Queued now, so part 1 is on its way while part 2 is drawn (the outbox keeps their order).
                        me->_sendDocument(threadId, fileName, std::move(part.picture.png), caption, SendPriority::Final);
                        ++sent;
                    };
                    const ConversationImageResult r = RenderConversationImages(transcript, options);
                    me->_log(L"[msg] conversation pictures of " + ShortId(sessionId) + L": " + levelName + (editDiffs ? L"+diffs" : L"") + (lastTurnOnly ? L"+last" : L"") + L", " +
                             std::to_wstring(sent) + L" of " + std::to_wstring(r.parts.size()) +
                             L" part(s) sent, " + std::to_wstring(r.rows) + L" rows (" + std::to_wstring(r.droppedRows) + L" left out), " + std::to_wstring(r.grid.widthPx) + L" px wide, " +
                             std::to_wstring(totalBytes / 1024) + L" KB, " + r.fontFamily + L", turns view " + std::to_wstring(r.turnsMs) + L" ms (" +
                             std::to_wstring(r.markdownChars / 1024) + L" K chars), layout " + std::to_wstring(r.layoutMs) + L" ms, drawn in " + std::to_wstring(r.drawMs) + L" ms" +
                             (r.error.empty() ? std::wstring{} : L" - failed: " + r.error));
                    // The thread's lines name what was asked for: "the conversation" or "the last turn" (Last Turn).
                    const std::wstring what = lastTurnOnly ? L"The last turn" : L"The conversation";
                    if (!r.error.empty())
                    {
                        me->_reply(threadId,
                                   sent == 0 ? what + L" could not be drawn: " + TgEscapeHtml(r.error) :
                                               what + L"'s pictures stopped after part " + std::to_wstring(sent) + L": " + TgEscapeHtml(r.error),
                                   false);
                    }
                    else if (r.droppedRows > 0)
                    {
                        // The first picture says it too (its top row); this line adds what to do about it: a shorter level of the
                        // card's picker (§21.39 - Full, the default, keeps the most per turn, so a long session hits the cap first).
                        // With Last Turn checked the typed form keeps its "last" (the same request, shorter).
                        const std::wstring typedLast = lastTurnOnly ? L" last" : L"";
                        const std::wstring shorter = options.level >= TurnsLevel::Full ?
                                                         L" A shorter level draws more of it: Default or Compact in \U0001F4DC All Screen \x25B8 (or /allscreen compact" + typedLast + L")." :
                                                         (options.level == TurnsLevel::Default ? L" Compact draws more of it: \U0001F4DC All Screen \x25B8 Compact (or /allscreen compact" + typedLast + L")." :
                                                                                                 std::wstring{});
                        // With File Diffs checked the diffs take room too: unchecking it draws more of the conversation as well. One
                        // turn's rows are all that turn's, so Last Turn says no "through turn".
                        me->_reply(threadId,
                                   what + L" is longer than " + std::to_wstring(r.parts.size()) + L" pictures hold: its first " + std::to_wstring(r.droppedRows) +
                                       L" rows" + (r.droppedThroughTurn > 0 && !lastTurnOnly ? L" (through turn " + std::to_wstring(r.droppedThroughTurn) + L")" : std::wstring{}) +
                                       L" are left out." + shorter + (editDiffs ? L" Without File Diffs the pictures are shorter too." : L""),
                                   false);
                    }
                }
                catch (...)
                {
                    // Rule #18: an exception escaping a std::thread would end the process.
                    LogSwallowedException(L"MessagingService::_startConversationImage (worker)");
                }
                me->_conversationImageRenders.fetch_sub(1);
                try
                {
                    std::lock_guard lk{ me->_mtx };
                    me->_conversationImageSessions.erase(sessionId);
                }
                catch (...)
                {
                    // std::system_error from the lock: the mark stays, and the next request for this session hears "being drawn".
                    LogSwallowedException(L"MessagingService::_startConversationImage (unmark)");
                }
            }).detach();
            return L"ok";
        }
        catch (...)
        {
            // The thread could not start (std::system_error, std::bad_alloc copying its arguments): nothing is drawing, so the
            // count goes back and the session is unmarked.
            _conversationImageRenders.fetch_sub(1);
            unmark();
            LogSwallowedException(L"MessagingService::_startConversationImage");
            _reply(threadId, L"The pictures could not be started (hooks.log says why).", false);
            return L"failed";
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

        /// <summary>
        /// A chosen folder's check with a BOUND on the caller's wait — the poll thread must never hang on a slow folder (§21.17.4).
        /// The session's folder (§21.25, _checkNewThreadFolder) and the ➕ message's extra folders (§21.25.9,
        /// _checkNewThreadAddFolder) are checked by this one rule.
        /// </summary>
        /// <param name="path">The folder: a list entry, or the path the user typed.</param>
        /// <param name="typed">False for a LIST pick: the shared prober's FRESH answer (one still running from the picker's lists is
        /// joined, never doubled), the list's own spelling standing — Exists, Missing (gone since it was listed) or NoAnswer. True
        /// for a TYPED path: the whole case-insensitive walk (ResolveNewThreadFolder over the real file system) on a worker of its
        /// own — the disk's spelling of what exists, what a Create would make, or why not; NoAnswer when the walk outlives the wait
        /// (an offline share holds a lookup for the network's timeout; the walk goes on in the background, its answer dropped).</param>
        /// <returns>The resolution; the caller waited at most kNewThreadPickProbeWaitMs for it.</returns>
        /// <exception cref="std::bad_alloc">If a path or a reason cannot be allocated.</exception>
        FolderResolution ResolveNewThreadFolderBounded(const std::wstring& path, bool typed)
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
                return res;
            }
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
            return res;
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
            // §21.48: Telegram for Android sends the message that opened this thread from "All" WITHOUT the thread's id, right after
            // this service message: the follow takes a thread-less message it stamped within kNewThreadOpenerFollowSec of this
            // creation as the thread's (_handleMessage). A named thread follows too — harmless, as nothing else comes thread-less
            // within seconds of a creation.
            _openerFollow.OnThreadCreated(threadId, msg.I64At(L"date", 0));
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
        // §21.48: /newsession in a thread no session owns is the WIZARD itself, never words for the session — held, it was replayed
        // at the bind as the new topic's own /newsession and opened a SECOND session in the same folder.
        const RemoteCommand command = ParseRemoteCommand(text);
        const bool newSessionCommand = command.kind == RemoteKind::AgentmasterCommand && command.name == L"newsession";
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
                        if (!full && !newSessionCommand) // §21.48: /newsession is answered below, never handed to the session
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
                if (newSessionCommand)
                {
                    // §21.48: the session this thread was opened for is coming up; once it is, /newsession here is that topic's own
                    // (a new session in the same folder).
                    _reply(threadId, L"A new session is already starting in this thread \x2014 it is up in a moment. Then /newsession here opens another one in its folder.", false, json::Value{}, msgId);
                    LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + L" /newsession -> already launching");
                }
                else if (full)
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
                // The thread's topic now: as if the caller had found it. §21.43: a voice note goes to the voice path with that
                // session (a note has no text: _handleTopicMessage would drop it silently).
                if (IsVoiceNoteMessage(msg))
                {
                    _voiceTake(msg, topicSession, threadId, false, false); // an edit never gets here: answered at the top
                }
                else
                {
                    _handleTopicMessage(topicSession, msg, threadId);
                }
                return;
            }
        }
        if (newSessionCommand)
        {
            // §21.48: the wizard shows itself here (a folder and a model in the command are pre-picked) — the thread was opened by
            // the command (Telegram named it "/newsession": it gets the temporary name kNewSessionThreadName), or the user asks for
            // the steps again. A thread the bot never saw gets its record here, at the folder step, as below.
            {
                std::lock_guard lk{ _mtx };
                _newThreads[threadId].lastUseMs = now;
                _stateDirty = true;
            }
            LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + L" /newsession");
            _newSessionInThread(threadId, command.args, msgId, true);
            return;
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
        // The lists, once per picker and outside the lock (_buildNewThreadListsNow: a registry copy, one BOUNDED probe per folder).
        NewThreadLists lists;
        if (needLists)
        {
            lists = _buildNewThreadListsNow();
        }
        std::wstring html;
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
        int64_t pickerId = 0;
        int64_t replacedId = 0; // a repost: the picker above the user's message, deleted before the new one goes
        bool kickCensus = false;
        bool addFirst = false; // §21.25.9: the ➕ message is posted before the picker (it must stand above it)
        bool addFollows = false; // §21.25.9: a posted ➕ message follows the picker's step (inert at the folder steps)
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
            // §21.25.9: from the model step on, the ➕ message stands ABOVE the picker. With none posted yet (the step was just
            // reached; a refused launch deleted it with the picker), the picker is posted again after it — a picker that stands
            // already is moved below it (deleted and posted again), since Telegram orders a thread's messages by their post.
            addFirst = NewThreadAddActive(nt.wizard.step) && nt.addMessageId == 0;
            if (addFirst && nt.pickerMessageId > 0)
            {
                repost = true;
            }
            addFollows = !addFirst && nt.addMessageId != 0;
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
        if (addFirst)
        {
            // §21.25.9: the ➕ message's post goes into the outbox BEFORE the picker's (the outbox serves one priority in its queue
            // order), so it lands above the picker. addFirst always means a picker POST below (a standing picker was reposted).
            _showNewThreadAddMessage(threadId);
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
        if (addFollows)
        {
            // §21.25.9: a posted ➕ message follows the picker's step — inert at the folder steps, its add flow again at the model
            // and window steps. Its render is compared with the one last queued, so a picker render that changes nothing for it
            // (a page, a census answer) costs no call.
            _showNewThreadAddMessage(threadId);
        }
        if (kickCensus)
        {
            _collectNewThreadWindows(threadId, 0);
        }
    }

    NewThreadLists MessagingService::_buildNewThreadListsNow()
    {
        // A registry copy of where every session worked, and one probe per listed folder (a folder deleted since is not offered).
        // A UNC path is kept unprobed — an offline share would hold the poll thread for the network's timeout; the pick checks it
        // again. Every other folder is probed with a BOUND (FolderProbe.h, §21.17.4), since a drive letter mapped to a share, or a
        // removable or sleeping disk, can hold a probe just as long: each waits at most kNewThreadListProbeEachMs and all of them
        // together at most kNewThreadListProbeBudgetMs, an answer up to kNewThreadListProbeMaxAgeMs old is taken as it is, and a
        // folder that did not answer is listed like a UNC one (the pick checks it; its probe goes on in the background and leaves
        // its answer for the next picker). Moved here from _showNewThreadPicker (§21.25.9): the ➕ message's add picker shows the
        // same lists, and builds them when the session's folder was typed (the folder step never built them then).
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
            LogSwallowedException(L"MessagingService::_buildNewThreadListsNow (temp roots)");
        }
        const uint64_t probeStartTick = ::GetTickCount64(); // the list's whole budget counts from here
        return BuildNewThreadLists(facts, scratchRoots, [probeStartTick](const std::wstring& path) {
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

    void MessagingService::_showNewThreadAddMessage(int64_t threadId, bool repost)
    {
        // §21.25.9. 1. Whether there is anything to show, and whether its add picker needs the lists (built outside the lock).
        bool needLists = false;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return;
            }
            const auto& nt = it->second;
            if (nt.addMessageId == 0 && !NewThreadAddActive(nt.wizard.step))
            {
                return; // nothing to show yet: the ➕ message first appears with the model step
            }
            needLists = NewThreadAddActive(nt.wizard.step) && nt.addStep == NewThreadAddStep::Pick && !nt.listsBuilt;
        }
        NewThreadLists lists;
        if (needLists)
        {
            lists = _buildNewThreadListsNow(); // a registry copy and bounded probes: never under `_mtx`
        }
        // 2. The render, decided under the lock: posted, edited, reposted — or nothing, when it would show what it shows already.
        std::wstring html;
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
        int64_t addId = 0;
        int64_t replacedId = 0; // a repost: the ➕ message above the user's typed choice, deleted before the new one goes
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
            if (nt.addMessageId < 0)
            {
                nt.addStale = true; // its post is in flight: the post's answer renders this state
                return;
            }
            if (nt.addMessageId == 0 && !NewThreadAddActive(nt.wizard.step))
            {
                return; // the wizard went back to the folder step while the lists were built: nothing to post now
            }
            const NewThreadView view = BuildNewThreadAddView(nt, WallMs());
            std::wstring shown = NewThreadViewKey(view);
            if (!repost && nt.addMessageId > 0 && shown == nt.addShownKey)
            {
                return; // unchanged: the picker rendered for a reason of its own (the chat's flood budget is shared)
            }
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
            if (repost && nt.addMessageId > 0)
            {
                replacedId = nt.addMessageId;
                nt.addMessageId = 0;
                _stateDirty = true;
            }
            addId = nt.addMessageId;
            if (addId == 0)
            {
                nt.addMessageId = -1; // the post is in flight: a render meanwhile marks it stale instead of posting twice
            }
            nt.addStale = false;
            nt.addShownKey = std::move(shown);
        }
        if (replacedId > 0)
        {
            // Queued before the new post (one priority, one order): the thread never shows two ➕ messages for long.
            OutItem del;
            del.prio = SendPriority::Final;
            del.method = L"deleteMessage";
            del.params = json::Value::MkObj();
            del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(replacedId)));
            _enqueue(std::move(del));
        }
        // No keyboard (the inert form): no markup at all — an edit without one removes the buttons the message had.
        json::Value markup = rows.empty() ? json::Value{} : _buttons(rows);
        auto self = weak_from_this();
        if (addId > 0)
        {
            // In place, keyed per thread: a newer render replaces one still queued (_abortNewThread drops it with the thread).
            _editText(addId, html, SendPriority::Final, L"nta:" + std::to_wstring(threadId), std::move(markup), [self, threadId, addId](const TgResult& r) {
                auto me = self.lock();
                if (!me || r.ok || r.NotModified())
                {
                    return;
                }
                if (r.ThreadNotFound())
                {
                    // The user deleted the thread: nothing is left to set up (the picker's own edit says the same).
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
                bool again = false;
                {
                    std::lock_guard lk{ me->_mtx };
                    if (const auto it = me->_newThreads.find(threadId); it != me->_newThreads.end() && it->second.addMessageId == addId)
                    {
                        // Whatever the refusal, this render did not land: forget it, so the next render sends again.
                        it->second.addShownKey.clear();
                        if (HeldMessageGone(r.ok, r.httpStatus, r.description))
                        {
                            // The user deleted the ➕ message: a new one is posted — it carries what the session will be given.
                            it->second.addMessageId = 0;
                            me->_stateDirty = true;
                            again = true;
                        }
                    }
                }
                me->_log(L"[msg] new thread " + std::to_wstring(threadId) + L": the \x2795 message's edit was refused (" + r.description + L")" +
                         (again ? std::wstring{ L"; it is gone, a new one is posted" } : std::wstring{}));
                if (again)
                {
                    me->_showNewThreadAddMessage(threadId);
                }
            });
            return;
        }
        _sendText(threadId, html, true, SendPriority::Final, L"", std::move(markup), [self, threadId](const TgResult& r) {
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
                        me->_newThreads.erase(it); // the user deleted the thread: nothing is left to set up
                        threadGone = true;
                    }
                    else
                    {
                        it->second.addMessageId = id; // 0 = refused: the next render posts it again
                        if (id == 0)
                        {
                            it->second.addShownKey.clear();
                        }
                        again = id > 0 && it->second.addStale;
                    }
                    me->_stateDirty = true; // its id is persisted (its buttons work after a restart)
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
                me->_log(L"[msg] new thread " + std::to_wstring(threadId) + L": the \x2795 message was refused (" + r.description + L")");
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
                me->_showNewThreadAddMessage(threadId);
            }
        });
    }

    std::wstring MessagingService::_onNewThreadAction(const NewThreadAction& action, int64_t threadId)
    {
        // §21.25: the wizard decides (ApplyNewThreadButton: a button of an earlier step changes nothing), the dispatcher acts.
        NewThreadOutcome out;
        NewThreadStep step = NewThreadStep::Folder;
        // §21.25.9: a ➕-message tap — its add flow's state, and the extra folders around the tap (the log says which one came or went).
        const bool addTap = NewThreadIsAddKind(action.kind);
        NewThreadAddStep addStep = NewThreadAddStep::Idle;
        std::vector<NewThreadExtraFolder> foldersBefore;
        std::vector<NewThreadExtraFolder> foldersAfter;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return kNewThreadGone;
            }
            step = it->second.wizard.step;
            addStep = it->second.addStep;
            if (addTap)
            {
                foldersBefore = it->second.wizard.extraFolders; // at most kNewThreadExtraFoldersMax: a cheap copy
            }
            it->second.lastUseMs = WallMs(); // every tap counts as a use (the idle abort counts from the last one)
            out = ApplyNewThreadButton(it->second, action);
            if (addTap)
            {
                foldersAfter = it->second.wizard.extraFolders;
            }
            _stateDirty = true;
        }
        if (out.kind != NewThreadOutcome::Kind::Stale && out.kind != NewThreadOutcome::Kind::Toast)
        {
            LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) +
                   (addTap ? L" \x2795 button at its " + std::wstring{ NewThreadAddStepName(addStep) } + L" state" : L" button at the " + std::wstring{ NewThreadStepName(step) } + L" step"));
        }
        if (addTap && foldersAfter.size() != foldersBefore.size())
        {
            // One tap adds (AddScope 1, AddConfirm) or removes (AddRemove) exactly one folder: the longer list holds it.
            const bool added = foldersAfter.size() > foldersBefore.size();
            const auto& longer = added ? foldersAfter : foldersBefore;
            const auto& shorter = added ? foldersBefore : foldersAfter;
            for (const auto& f : longer)
            {
                const bool inShorter = std::any_of(shorter.begin(), shorter.end(), [&f](const NewThreadExtraFolder& g) { return g.path == f.path; });
                if (!inShorter)
                {
                    _log(L"[msg] new thread " + std::to_wstring(threadId) + L": extra folder " + f.path + (added ? L" added (" : L" taken off (") +
                         (f.option == 2 ? L"remembered" : L"for this session") + L"); " + std::to_wstring(foldersAfter.size()) + L" in all");
                    break;
                }
            }
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
        case Kind::RenderAdd:
            // §21.25.9: the ➕ message changed. A TYPED choice sits below both messages: the ➕ message and then the picker are posted
            // again below it (in that order: the ➕ message stays above the picker); a button edits the ➕ message in place.
            _showNewThreadAddMessage(threadId, replyTo != 0);
            if (replyTo != 0)
            {
                _showNewThreadPicker(threadId, true);
            }
            return out.text;
        case Kind::CheckAddFolder:
            return _checkNewThreadAddFolder(threadId, out.path, out.typed, replyTo);
        default:
            return {};
        }
    }

    std::wstring MessagingService::_checkNewThreadFolder(int64_t threadId, const std::wstring& path, bool typed, int64_t replyTo)
    {
        // The bounded check (a list pick's probe, a typed path's walk on a worker): §21.25.9 checks an extra folder the same way.
        const FolderResolution res = ResolveNewThreadFolderBounded(path, typed);
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

    std::wstring MessagingService::_checkNewThreadAddFolder(int64_t threadId, const std::wstring& path, bool typed, int64_t replyTo)
    {
        // §21.25.9: the same bounded check as the session's folder (a list pick's probe, a typed path's case-insensitive walk on a
        // worker), so a slow or offline folder never holds the poll thread past kNewThreadPickProbeWaitMs here either.
        const FolderResolution res = ResolveNewThreadFolderBounded(path, typed);
        NewThreadOutcome next;
        NewThreadAddStep addStep = NewThreadAddStep::Idle;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return kNewThreadGone; // cancelled or launched while the folder was checked
            }
            next = ApplyNewThreadAddFolderResolved(it->second, res, typed);
            addStep = it->second.addStep;
            it->second.lastUseMs = WallMs();
            _stateDirty = true;
        }
        static constexpr const wchar_t* kKinds[] = { L"exists", L"missing", L"a file", L"no root", L"invalid", L"no answer" };
        _log(L"[msg] new thread " + std::to_wstring(threadId) + L": \x2795 " + (typed ? L"typed" : L"picked") + L" extra folder " + path + L" \x2192 " +
             kKinds[static_cast<size_t>(res.kind) < std::size(kKinds) ? static_cast<size_t>(res.kind) : 4] + (res.path.empty() || res.path == path ? std::wstring{} : L" (" + res.path + L")") +
             (addStep == NewThreadAddStep::Scope ? std::wstring{ L"; its three options are up" } : (next.text.empty() ? std::wstring{} : L"; refused: " + next.text)));
        LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + L" \x2795 " + (typed ? L"typed" : L"picked") + L" a folder -> " +
               (addStep == NewThreadAddStep::Scope ? L"scope" : (next.kind == NewThreadOutcome::Kind::Stale ? L"stale" : L"refused")));
        if (next.kind == NewThreadOutcome::Kind::Stale)
        {
            return next.text; // the add flow was left while the folder was checked: nothing to render
        }
        // A TYPED folder sits below both messages: the ➕ message and then the picker are posted again below it (the ➕ message
        // stays above the picker); a button edits the ➕ message in place.
        _showNewThreadAddMessage(threadId, replyTo != 0);
        if (replyTo != 0)
        {
            _showNewThreadPicker(threadId, true);
        }
        return next.text; // "" when the three options are up; the reason when the folder was refused
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
        int64_t addId = 0; // §21.25.9: the ➕ message, deleted with the picker once a window took the launch
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
            bind.wizard = it->second.wizard; // §21.25.9: with its extra folders — the bind sends each as /add-dir once the session is live
            pickerId = it->second.pickerMessageId;
            addId = it->second.addMessageId;
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
        if (addId > 0)
        {
            // §21.25.9: the ➕ message goes with the picker — the "Opening …" line above names the folders it added, and each one's
            // /add-dir card follows once the session is up. (A post still in flight, -1, is deleted by its own answer: its record
            // is gone, so the post reads as orphaned.) A launch no window takes never gets here: its record comes back with both.
            OutItem del;
            del.prio = SendPriority::Final;
            del.method = L"deleteMessage";
            del.params = json::Value::MkObj();
            del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(addId)));
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
            const std::wstring addKey = L"nta:" + std::to_wstring(threadId); // §21.25.9: the ➕ message's queued edit
            for (auto o = _outbox.begin(); o != _outbox.end();)
            {
                const bool mine = o->key == pickerKey || o->key == addKey || NewThreadOutItemThread(o->params) == threadId;
                if (mine)
                {
                    _outboxDropLocked(*o, L"its new thread was aborted"); // §21.50: a journaled reply must not come back after a restart
                }
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
        const int64_t addId = rec.addMessageId; // §21.25.9: the ➕ message goes with the picker when the thread itself cannot
        del.onDone = [self, threadId, pickerId, addId](const TgResult& r) {
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
            if (addId > 0)
            {
                // §21.25.9: and the ➕ message — its buttons would answer the same, and it lists folders no session will get.
                OutItem delAdd;
                delAdd.prio = SendPriority::Final;
                delAdd.method = L"deleteMessage";
                delAdd.params = json::Value::MkObj();
                delAdd.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(me->_ownerId)));
                delAdd.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(addId)));
                me->_enqueue(std::move(delAdd));
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

    // ===== §21.48: a new session from anywhere ==================================================================================

    bool MessagingService::_newThreadUntouched(int64_t threadId)
    {
        std::lock_guard lk{ _mtx };
        const auto it = _newThreads.find(threadId);
        if (it == _newThreads.end())
        {
            return false; // no wizard: a thread made before §21.17, or one whose session closed — the wizard takes its message
        }
        // The three facts _abortUntouchedNewThread reads before it deletes a thread: nothing held, no picker yet, no name the user
        // gave the thread.
        const auto& nt = it->second;
        return nt.held.empty() && nt.pickerMessageId == 0 && nt.topicName.empty();
    }

    void MessagingService::_runMainViewCommand(const RemoteCommand& cmd, const json::Value& msg)
    {
        if (cmd.name == L"start" || cmd.name == L"help")
        {
            _sendMainHelp(0);
            LogNav(L"remote telegram /" + cmd.name + L" (main view) -> help");
            return;
        }
        // /status (§21.40: the bot's line, then Claude Code's account and plan usage), /idle, /board, /pauseall, /resumeall, /verbose
        // and /recreate: the 🔎 Search topic runs them, answered here.
        _runSearchCommand(cmd, msg, 0);
    }

    void MessagingService::_sendMainHelp(int64_t threadId)
    {
        // The main view's own help: before §21.48 /help here sent the Search topic's, which read as "you are in the Search topic".
        const std::wstring html =
            L"<b>All \x2014 the chat's main view</b>\n"
            L"Every session has its own topic. Writing here opens a NEW THREAD for a NEW SESSION: the thread asks for the folder, then the model and the window, and what you wrote becomes the session's first prompt.\n"
            L"/newsession \x2014 the same without a prompt (/newsession &lt;folder&gt; [model] picks them ahead)\n"
            L"/idle \x2014 the idle tabs \x00B7 /board \x2014 counts per state\n"
            L"/status \x2014 the bot, then Claude Code's account and plan usage\n"
            L"/pauseall \x00B7 /resumeall \x2014 every Prompt Queue Runner\n"
            L"/verbose on|off \x2014 the default for all topics: the work too, or the conversation only\n"
            L"/search &lt;words&gt; \x2014 ask the \U0001F50E Search agent (its answer comes in the Search topic) \x00B7 /search_tab \x2014 put the Search topic first in the thread list\n"
            L"In a session's topic, /help lists what you can do there.";
        _reply(threadId, html, false);
    }

    void MessagingService::_renameNewThread(int64_t threadId, const std::wstring& name)
    {
        OutItem item;
        item.prio = SendPriority::New; // a name, not an answer: after the answers queued now, before the cards' edits
        item.method = L"editForumTopic";
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        item.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
        item.params.Set(L"name", json::Value::MkStr(name));
        auto self = weak_from_this();
        item.onDone = [self, threadId, name](const TgResult& r) {
            auto me = self.lock();
            if (!me || r.ok || r.TopicNotModified())
            {
                return; // renamed (its "topic renamed" notice comes back from the bot and is deleted by _handleMessage)
            }
            // Cosmetic: the thread keeps the name Telegram gave it, and the bind renames it to the session's title all the same.
            me->_log(L"[msg] new thread " + std::to_wstring(threadId) + L": its temporary name \x201C" + name + L"\x201D was refused (" + r.description +
                     L"); it keeps Telegram's until the session's title replaces it");
        };
        _enqueue(std::move(item));
        _log(L"[msg] new thread " + std::to_wstring(threadId) + L" was opened with /newsession: renamed \x201C" + name + L"\x201D until the session's title replaces it");
    }

    void MessagingService::_newSessionInThread(int64_t threadId, const std::wstring& args, int64_t replyTo, bool renameImplicit)
    {
        const NewSessionArgs a = ParseNewSessionArgs(args);
        bool known = false;
        bool untouched = false;
        bool implicitName = false;
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _newThreads.find(threadId); it != _newThreads.end())
            {
                known = true;
                // Read BEFORE anything is shown: "untouched" decides whether this command opened the thread (it gets the temporary
                // name, and its picker is posted rather than moved below the command).
                untouched = it->second.held.empty() && it->second.pickerMessageId == 0;
                implicitName = it->second.topicName.empty();
                it->second.lastUseMs = WallMs();
                _stateDirty = true; // §21.25: the wizards are persisted (the idle clock counts this use)
            }
        }
        if (!known)
        {
            return; // launched or cancelled meanwhile (the caller made the record): nothing is left to show
        }
        if (renameImplicit && untouched && implicitName)
        {
            // The thread Telegram named after the command ("/newsession"): the temporary name until the session's title replaces
            // it at the bind (the user's "a new topic with a temporary name").
            _renameNewThread(threadId, std::wstring{ kNewSessionThreadName });
        }
        // A picker that stands already is posted again BELOW the command (the user looks at the bottom of the thread); a thread
        // without one gets it posted.
        const bool repost = replyTo != 0 && !untouched;
        if (a.folder.empty())
        {
            _showNewThreadPicker(threadId, repost);
            return;
        }
        const std::optional<std::wstring> folder = NewThreadTypedFolder(a.folder);
        if (!folder)
        {
            // Not an absolute folder path (a relative name, a word): said, and the wizard shown where it stands. Nothing is checked
            // or created for it.
            _reply(threadId, L"<code>" + TgEscapeHtml(a.folder) + L"</code> is not a folder's full path \x2014 send one (like <code>K:\\src\\app</code>), or pick a folder below.", false, json::Value{}, replyTo);
            _showNewThreadPicker(threadId, repost);
            return;
        }
        NewThreadOutcome out;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _newThreads.find(threadId);
            if (it == _newThreads.end())
            {
                return;
            }
            // A TYPED folder: a folder choice at any step — checked case-insensitively, and only OFFERED for creation when missing
            // (ApplyNewThreadText → CheckFolder; the user's tap on 📁 Create makes it).
            out = ApplyNewThreadText(it->second, *folder);
            _stateDirty = true;
        }
        _applyNewThreadOutcome(threadId, out, replyTo);
        if (a.model.empty())
        {
            return;
        }
        // The model after the folder: taken only once the folder step went through — the wizard stands at the model step now —,
        // as "/model <id>" typed there (the id checked before it can reach a launch: ParseNewThreadModelId). A folder that must be
        // created first leaves the model to the model step.
        NewThreadOutcome model;
        bool atModel = false;
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _newThreads.find(threadId); it != _newThreads.end() && it->second.wizard.step == NewThreadStep::Model)
            {
                atModel = true;
                model = ApplyNewThreadText(it->second, L"/model " + a.model);
                _stateDirty = true;
            }
        }
        if (atModel)
        {
            _applyNewThreadOutcome(threadId, model, replyTo);
        }
        else
        {
            _log(L"[msg] new thread " + std::to_wstring(threadId) + L": /newsession named the model " + a.model +
                 L", but the folder step did not go through (the model step asks for it once it does)");
        }
    }

    void MessagingService::_openNewSessionThread(const std::wstring& args, const json::Value& msg, int64_t fromThread, bool holdMessage)
    {
        const int64_t msgId = msg.I64At(L"message_id", 0);
        const std::wstring name = holdMessage ? NewThreadNameForMessage(msg.StrAt(L"text")) : std::wstring{ kNewSessionThreadName };
        const std::wstring where = fromThread == 0 ? std::wstring{ L"the main view" } : std::wstring{ L"the Search topic" };
        {
            // §21.51: while Telegram refuses topics for this chat (BOT_TOPIC_WRONG_HOME), no create is asked for — it would be one
            // more refusal and one more "topic … was created" notice that no bot can delete. The user hears when it may work again.
            const int64_t now = WallMs();
            int64_t until = 0;
            if (_topicCreatesPaused(now, &until))
            {
                int64_t since = 0;
                {
                    std::lock_guard lk{ _mtx };
                    since = _state.topicCreatePausedSinceMs;
                }
                _log(L"[msg] a new thread for " + std::wstring{ holdMessage ? L"words written in " : L"/newsession in " } + where + L" was not asked for: topic creates are paused until " +
                     TgClockText(until, now));
                LogNav(L"remote telegram new-thread open (" + where + L") -> paused");
                _reply(fromThread,
                       L"Telegram refuses new topics for this chat right now" + (since > 0 ? L" (since " + TgClockText(since, now) + L")" : std::wstring{}) +
                           L", so no thread can be opened. The bot tries again at " + TgClockText(until, now) + L"; send /newsession again after that.",
                       false,
                       json::Value{},
                       msgId);
                return;
            }
        }
        OutItem item;
        item.prio = SendPriority::Final; // the answer to the user's command (or words): their thread comes first
        item.method = L"createForumTopic";
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        item.params.Set(L"name", json::Value::MkStr(name));
        auto self = weak_from_this();
        // The words the thread takes (holdMessage): the copy that names the thread is made once Telegram has answered with its id.
        const json::Value held = holdMessage ? msg : json::Value{};
        item.onDone = [self, args, held, fromThread, msgId, name, where, holdMessage](const TgResult& r) {
            auto me = self.lock();
            if (!me)
            {
                return;
            }
            const int64_t thread = r.ok ? r.result.I64At(L"message_thread_id", 0) : 0;
            if (thread <= 0)
            {
                const std::wstring why = r.description.empty() ? std::wstring{ L"no answer from Telegram" } : r.description;
                if (r.TopicHomeRefused())
                {
                    me->_pauseTopicCreates(why); // §21.51: Telegram refuses topics for the whole chat — every create waits
                }
                me->_log(L"[msg] a new thread for " + std::wstring{ holdMessage ? L"words written in " : L"/newsession in " } + where + L" was refused (" + why + L")");
                LogNav(L"remote telegram new-thread open (" + where + L") -> refused");
                me->_reply(fromThread, L"A new thread for the session could not be opened (" + TgEscapeHtml(why) + L"). Try again, or open a thread yourself.", false, json::Value{}, msgId);
                return;
            }
            me->_noteTopicCreateWorked(L""); // §21.51: a pause of the chat ends with any create that works
            {
                std::lock_guard lk{ me->_mtx };
                auto& nt = me->_newThreads[thread];
                nt.topicName.clear(); // an implicit name, as a thread Telegram names: the session's title comes from its folder
                nt.lastUseMs = WallMs();
                me->_stateDirty = true; // §21.25: the wizards are persisted
                if (held.type == json::Value::Type::Obj)
                {
                    // The words stay in the main view, where they were written: their edits are routed to this thread, and no reply
                    // inside the thread quotes them (_dropQuoteOfMovedMessage).
                    me->_openerFollow.Remember(held.I64At(L"message_id", 0), thread);
                }
            }
            me->_log(L"[msg] new thread " + std::to_wstring(thread) + L" opened by the bot for " + std::wstring{ holdMessage ? L"words written in " : L"/newsession in " } + where +
                     L": \x201C" + name + L"\x201D");
            LogNav(L"remote telegram new-thread " + std::to_wstring(thread) + L" opened (" + where + L")");
            // What the thread starts with runs on a worker of its own, holding the service weakly (as the window census does): the
            // picker's lists probe every listed folder (up to kNewThreadListProbeBudgetMs) and a folder argument is walked (up to
            // kNewThreadPickProbeWaitMs), and this is the send loop's thread, which every other message waits for.
            std::thread([self, thread, args, held]() noexcept {
                try
                {
                    if (const auto service = self.lock())
                    {
                        service->_startOpenedNewThread(thread, args, held);
                    }
                }
                catch (...)
                {
                    // Rule #18. Recovery: the thread has its wizard record but no picker yet — the user's next message there posts it
                    // (_holdForNewThread), and a thread nobody uses is deleted after kNewThreadPickerIdleMs (_newThreadsTick).
                    LogSwallowedException(L"MessagingService (start a new session's thread)");
                }
            }).detach();
            me->_reply(fromThread, BuildNewSessionThreadOpenedHtml(me->_linkBot(), thread, name, holdMessage), false, json::Value{}, msgId);
        };
        _enqueue(std::move(item));
        LogNav(L"remote telegram new-thread open (" + where + L")" + (holdMessage ? L" words" : L" /newsession"));
    }

    void MessagingService::_startOpenedNewThread(int64_t threadId, const std::wstring& args, const json::Value& held)
    {
        if (held.type == json::Value::Type::Obj)
        {
            // The words written in the main view: held by the thread's wizard as if they had been written there — the 👀, the picker
            // counting them, the first prompt of the session it starts (a lone folder path is a folder choice, as there). The copy
            // names the thread and carries the moved mark (persisted with the held messages).
            _holdForNewThread(threadId,
                              WithMember(WithMember(held, L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId))), kNewThreadMovedKey, json::Value::MkBool(true)),
                              false);
            return;
        }
        // /newsession: the wizard, with what the command named pre-picked (a bare one: the folder step). The thread already has the
        // name the service gave it.
        _newSessionInThread(threadId, args, 0, false);
    }

    void MessagingService::_queueNewThreadExtraFolders(const std::wstring& sessionId, int64_t threadId, const std::vector<NewThreadExtraFolder>& folders) noexcept
    {
        if (folders.empty())
        {
            return;
        }
        size_t queued = 0;
        for (const auto& f : folders)
        {
            try
            {
                // Queued like a message the user typed in the topic (the one funnel: D30 order, the 5 s remote settle, sent whatever
                // the runner's mode, the delivery's nets), but with no Telegram message behind it (0: no reaction, no quote) and its
                // answer preset. Each lands after the one before it, and every held message the caller hands over next lands after
                // them all — so Claude Code has read every folder before the first prompt is typed: the queue sends one prompt at a
                // time and waits for each /add-dir's echo, which Claude Code writes only once the dialog is answered.
                if (_queueRemotePrompt(sessionId, NewThreadAddDirCommand(f), 0, threadId, false, f.option))
                {
                    ++queued;
                }
                else
                {
                    _log(L"[msg] new thread " + std::to_wstring(threadId) + L": the /add-dir for extra folder " + f.path + L" was not queued (the session closed meanwhile)");
                }
            }
            catch (...)
            {
                // Rule #18. Recovery: this folder is not added (the user can send /add-dir in the topic); the next one still goes.
                LogSwallowedException(L"MessagingService::_queueNewThreadExtraFolders");
            }
        }
        try
        {
            _log(L"[msg] new thread " + std::to_wstring(threadId) + L": " + std::to_wstring(queued) + L" of " + std::to_wstring(folders.size()) + L" extra folder(s) go to " +
                 ShortId(sessionId) + L" as /add-dir, ahead of its held messages");
            LogNav(L"remote telegram new-thread " + std::to_wstring(threadId) + L" extra-folders " + ShortId(sessionId) + L" queued=" + std::to_wstring(queued));
        }
        catch (...)
        {
            // Rule #18. Recovery: the log line is lost; the commands are queued all the same.
            LogSwallowedException(L"MessagingService::_queueNewThreadExtraFolders (log)");
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
            // §21.38: a pick's menu fits where the session stands, so the help names each first button. §21.43: a voice note is
            // a search too (its transcript, echoed under it, is the request).
            html = L"<b>\U0001F50E Search</b>\nWrite what you look for in plain words, or say it in a voice note; the agent reads the sessions it finds and answers with one numbered card each, quoting the evidence it saw. Tap a number to pick one: its menu fits where the session stands \x2014 \x25B6 Resume a closed one, \x25B6 Activate a deactivated tab, \x23F0 Wake an idle one, \U0001F4AC Go to topic \x2014 or \x2442 Fork it. A follow-up refines the last answer.\n"
                   L"It is one conversation for the whole app: it carries on after a restart, and /clear (or \x2298 Clear under an answer) starts it over.\n"
                   // §21.48: a bare /newsession opens a thread whose wizard asks (it answered a usage line before).
                   L"/idle \x2014 the idle tabs\n/board \x2014 counts per state\n/newsession \x2014 a new thread for a new session: it asks for the folder, the model and the window (/newsession &lt;folder&gt; [model] opens the session at once)\n/pauseall \x00B7 /resumeall\n/status \x2014 the bot, then Claude Code's account and plan usage\n/clear \x2014 forget the search conversation\n/verbose on|off \x2014 the default for all topics: the work too, or the conversation only\n/recreate \x2014 make the session topics again in MOST RECENT order, with their history, verbose or not (asks first)\n"
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
                   L"/add-dir &lt;path&gt; adds a folder to the workspace: Claude Code's question comes here as buttons, and \x201CYes, for this session\x201D is picked by itself after 60 s.\n"
                   // §21.32.4: the file links (on in the settings' FILE LINKS section).
                   L"/download &lt;path&gt; \x2014 a file's download link (absolute, or relative to the session's folder), with \U0001F4E4 Send as file\n"
                   // §21.47: /enter submits what already sits in the box at the PC (one Enter, read back; never into a menu or dialog).
                   L"/enter \x2014 press Enter at the PC: the prompt waiting in the session's input box is submitted (nothing is pressed into a menu, a dialog or an empty box)\n"
                   L"/queue \x00B7 /sendnow \x00B7 /steer &lt;text&gt; \x00B7 /unqueue n\n/runner auto|paused\n/pin \x00B7 /unpin \x2014 never idle\n/wake \x00B7 /moveidle \x00B7 /markunread\n/rename &lt;title&gt;\n/favorite \x00B7 /tags \x00B7 /tag x \x00B7 /untag x\n"
                   L"/sessionid \x00B7 /path \x00B7 /gitbranch \x00B7 /draft\n/summary \x00B7 /transcript [level] \x00B7 /screen \x00B7 /screenimage\n"
                   // §21.40: /status sends the card copy, then what Claude Code's own /status dialog shows.
                   L"/status \x2014 the card, then what Claude Code's /status shows: version, model, MCP servers, memory, account, plan usage\n"
                   // §21.39: the card's 📜 All Screen ▸ (a level picker with File Diffs and Last Turn boxes), typed with a level or bare
                   // (the cog's default level, Full), "diffs" for every file change as a diff, and "last" for the newest turn only.
                   L"/allscreen [level] [diffs] [last] \x2014 the whole conversation as tall pictures, drawn like Claude Code draws it (bare: the default level, full unless set in the cog; diffs: every file change as a diff; last: the last turn only)\n/activate \x00B7 /restartsession \x00B7 /deactivate \x00B7 /interrupt\n"
                   // §21.41: the card's two stopping buttons, the typed commands' twins (one path each, so they behave alike). Both
                   // live on the Session page only (the user's rule of 2026-10-28 for Esc: "only inside Sessions menu").
                   L"On \x2699\xFE0F Session's page the card has \x238B Esc (one Esc at once, as at the PC) and \U0001F319 Deactivate (claude stops, the tab keeps its place)\n"
                   // §21.49: the card's 🖼️ (the user's request of 2026-11-10) - the only way the card's picture changes.
                   L"The card's \U0001F5BC\xFE0F (beside \x2699\xFE0F Session) puts the terminal's screen on the card as it is now; the card takes no picture by itself\n"
                   L"/forksession [model] \x00B7 /newsession [model]\n/takedraft \x2014 queue the unsent draft at the PC\n/cc /x \x2014 force /x through to Claude Code\n/verbose on|off|default \x2014 this topic shows the work too, or the conversation only\n/recreate \x2014 make this topic (or all) again with its history, verbose or not\n"
                   // §21.19: <code> keeps "/search-tab" from becoming a "/search" link (see the Search help above).
                   L"/search_tab \x2014 jump to the \U0001F50E Search topic: it goes first in the thread list (typed, <code>/search-tab</code> works too)\n"
                   L"/search &lt;words&gt; \x2014 ask the \U0001F50E Search agent from here: the request and its answer go to the Search topic";
        }
        _reply(threadId, html, false);
    }
}
