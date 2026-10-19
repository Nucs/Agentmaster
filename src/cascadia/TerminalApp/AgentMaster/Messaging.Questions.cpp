// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging, the QUESTIONS (see Messaging.h, MESSAGING.md §21.30, INTERACTIVE_TOOLS.md §6): Claude Code's
// AskUserQuestion dialogs ANSWERED FROM TELEGRAM. The service's third TU beside Messaging.cpp and Messaging.Inbound.cpp — one
// class, three files: the question registry's acceptor, the QUESTION CARD (posted, edited as the user goes, watched until the
// transcript shows the dialog's result), its buttons, and the typed answers and notes. The pure half — the card's model, its
// texts and buttons, the answers composed exactly as the PC composes them, the decision lines — is AskQuestion.h; the pipe the
// question hooks wait on is HooksBridge.h's AskBridge, and the hook itself ClaudeSpawn.cpp's BuildAskHookScript.
//
// The user's request (2026-10-19): "close the gap, enabling telegram picking a choice, including the user manual text input. Use
// telegram to the best of its ability to produce a managed message that will cover this interaction and including custom answer".

#include "Messaging.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>
#include <utility> // std::exchange

#include "ClaudeSpawn.h" // LogNav, ShortId, LogSwallowedException
#include "HooksBridge.h" // AskRegistry, AskEntryInfo, AskEntryState
#include "ScreenImage.h" // RenderScreenSnapshotPng (the previews as a picture)
#include "SessionRegistry.h"
#include "TranscriptTurns.h" // ParseTranscriptJsonl

namespace Agentmaster::Msg
{
    namespace
    {
        // A request whose session has no topic is waited for this long (a session that just turned busy gets its topic at the
        // next sync), then released: nothing can show it, and the PC keeps the dialog. An open card whose topic went away is
        // released after the same wait (a recreation makes the topic anew within it, and the card is posted there).
        constexpr int64_t kAskNoTopicReleaseMs = 30 * 1000;
        // A finished card is kept this long after its outcome: its buttons answer what happened, the mirror finds it to leave out
        // the "You ↩ answer" line of a result it reads late (a history replay holds the mirror back), and its held messages can
        // still be sent as the next prompt.
        constexpr int64_t kAskCardKeepMs = 30 * 60 * 1000;
        // The most cards kept at once; the oldest finished ones go first (an open card is never dropped for the cap).
        constexpr size_t kAskMaxCards = 64;
        // The watch's first read looks this far back for the dialog's tool_use line (written 0.10–0.25 s before the hook starts,
        // INTERACTIVE_TOOLS.md §5): a whole turn's tool output can sit between, and 512 KB is a few such turns.
        constexpr int64_t kAskWatchSeedBytes = 512 * 1024;
        // A watch reads at most this often, and at most this much per read (a busy session writes megabytes a minute).
        constexpr int64_t kAskWatchGapMs = 1000;
        constexpr int64_t kAskWatchMaxBytes = 4 * 1024 * 1024;
        // An open card whose post Telegram refused posts again this much later.
        constexpr int64_t kAskRepostAfterFailureMs = 30 * 1000;
        // A request that went GONE (the hook ended: Esc at the PC, claude exited) closes its open card only after this long, so
        // the watch reads the transcript's result first and the card says what really happened.
        constexpr int64_t kAskGoneSettleMs = 3 * 1000;
        // The most messages one card's "answer or next prompt?" question holds at once (the oldest is dropped: it stays in the
        // chat, and the user can send it again).
        constexpr size_t kAskMaxHeld = 8;
        // How many pictures are drawn at once — shared with the card's 🖼 Screen Image (Messaging.Inbound.cpp kScreenImageMaxRenders,
        // the same value): one counter, `_screenImageRenders`, bounds every picture the bot draws.
        constexpr int kAskPictureMaxRenders = 2;
        // The reaction on a typed text the card took as the answer or the note (in Telegram's list of allowed reactions).
        constexpr std::wstring_view kAskTakenReaction{ L"\U0001F44C" };

        int64_t WallMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /// <summary>
        /// A new card's callback key: 10 characters of [0-9a-z] from a per-thread generator seeded once — never guessable from the
        /// card's content, and a button of another card (or of an earlier run, whose keys were forgotten) never matches one.
        /// </summary>
        /// <returns>The key.</returns>
        /// <exception cref="std::bad_alloc">If the key cannot be allocated.</exception>
        std::wstring MakeAskCardKey()
        {
            thread_local std::mt19937_64 rng{ (static_cast<uint64_t>(std::random_device{}()) << 32) ^ static_cast<uint64_t>(::GetTickCount64()) ^
                                              static_cast<uint64_t>(::GetCurrentThreadId()) };
            static constexpr wchar_t kDigits[] = L"0123456789abcdefghijklmnopqrstuvwxyz";
            std::wstring key;
            key.reserve(10);
            for (int i = 0; i < 10; ++i)
            {
                key.push_back(kDigits[rng() % 36]);
            }
            return key;
        }

        /// <summary>What one read of a card's watch found (ReadQuestionWatch).</summary>
        struct QuestionWatchRead
        {
            bool readOk{ false }; // the file was read (false: it is gone or unreadable for now — the watch tries again)
            int64_t nextOffset{ -1 }; // where the next read starts: after the last COMPLETE line read
            std::wstring toolUseId; // the dialog's tool_use id (the one the caller knew, or the newest question of its content)
            bool resultFound{ false };
            bool answered{ false }; // the result carried answers, or no error (else: an error result — Chat about this, Esc)
            std::wstring answerText; // the answer as the mirror shows it (ClassifyTranscriptLine's answer item; "" for none)
            int64_t timeoutMs{ 0 }; // Claude Code's own idle timeout resolved the dialog (`afkTimeoutMs`): nobody answered
        };

        /// <summary>
        /// One read of a card's WATCH: the transcript from `offset` (the first read, `offset` &lt; 0, looks back kAskWatchSeedBytes
        /// and drops the line the window cuts), COMPLETE lines only, classified as the mirror classifies them. Finds the dialog's
        /// tool_use — the NEWEST question item whose text is `signature` when the card does not know it yet (an earlier dialog
        /// of the same content may be in the window; its tool result names its own id) — and the tool result that answers it: an
        /// answer item (the answers, with their notes), a result without an error (no answers), or an error (Chat about this, or
        /// Esc at the PC).
        /// </summary>
        /// <param name="path">The transcript (the request's transcript_path).</param>
        /// <param name="offset">Where the last read ended (-1 = the first read).</param>
        /// <param name="signature">The card's questions (AskQuestionsSignature).</param>
        /// <param name="knownToolUseId">The dialog's tool_use id when the card knows it ("" = find it).</param>
        /// <returns>What was found. Never throws: a failure is logged and reads as "not read" (the next tick tries again).</returns>
        QuestionWatchRead ReadQuestionWatch(const std::wstring& path, int64_t offset, const std::wstring& signature, const std::wstring& knownToolUseId) noexcept
        {
            QuestionWatchRead w;
            try
            {
                HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                {
                    return w; // not there (yet, or any more): the next tick looks again
                }
                std::string bytes;
                int64_t from = 0;
                LARGE_INTEGER size{};
                if (::GetFileSizeEx(h, &size))
                {
                    from = offset < 0 ? (std::max<int64_t>)(0, size.QuadPart - kAskWatchSeedBytes) : offset;
                    if (from > size.QuadPart)
                    {
                        from = size.QuadPart; // the file was replaced by a shorter one: read on from its end
                    }
                    const int64_t want = (std::min<int64_t>)(size.QuadPart - from, kAskWatchMaxBytes);
                    LARGE_INTEGER pos{};
                    pos.QuadPart = from;
                    if (want > 0 && ::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
                    {
                        bytes.resize(static_cast<size_t>(want));
                        DWORD read = 0;
                        if (!::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
                        {
                            bytes.clear();
                        }
                        else
                        {
                            bytes.resize(read);
                        }
                    }
                    w.readOk = true;
                }
                ::CloseHandle(h);
                if (!w.readOk)
                {
                    return w;
                }
                size_t start = 0;
                if (offset < 0 && from > 0)
                {
                    // The look-back window starts inside a line: that line is torn, and the window's first whole line follows it.
                    const size_t nl = bytes.find('\n');
                    if (nl == std::string::npos)
                    {
                        w.nextOffset = from + static_cast<int64_t>(bytes.size()); // one line longer than the window: skipped
                        w.toolUseId = knownToolUseId;
                        return w;
                    }
                    start = nl + 1;
                }
                const size_t lastNl = bytes.rfind('\n');
                if (lastNl == std::string::npos || lastNl < start)
                {
                    w.nextOffset = from + static_cast<int64_t>(start); // nothing complete yet (a line still being written)
                    w.toolUseId = knownToolUseId;
                    return w;
                }
                w.nextOffset = from + static_cast<int64_t>(lastNl) + 1;
                std::wstring toolUseId = knownToolUseId;
                for (const auto& line : ParseTranscriptJsonl(bytes.substr(start, lastNl + 1 - start)))
                {
                    for (const auto& it : ClassifyTranscriptLine(line))
                    {
                        if (it.kind == TopicItemKind::Question)
                        {
                            // The card's dialog when it does not know its id yet: the newest question of the same content (a
                            // result read before it belongs to an earlier dialog of that content, and is dropped with the old id).
                            if (knownToolUseId.empty() && !it.toolUseId.empty() && it.text == signature)
                            {
                                toolUseId = it.toolUseId;
                                w.resultFound = false;
                                w.answerText.clear();
                            }
                            continue;
                        }
                        if ((it.kind != TopicItemKind::Answer && it.kind != TopicItemKind::ToolResult) || toolUseId.empty() || it.toolUseId != toolUseId)
                        {
                            continue;
                        }
                        w.resultFound = true;
                        w.answered = it.kind == TopicItemKind::Answer || !it.isError;
                        w.answerText = it.kind == TopicItemKind::Answer ? it.text : std::wstring{};
                        w.timeoutMs = it.afkTimeoutMs;
                    }
                }
                w.toolUseId = toolUseId;
            }
            catch (...)
            {
                // Rule #18. Recovery: the read counts as not done — the offset stays, and the next tick reads the same bytes again.
                LogSwallowedException(L"Messaging ReadQuestionWatch");
                w = QuestionWatchRead{};
            }
            return w;
        }

        /// <summary>The toast of a card button pressed when the card is no longer open (it says what became of the question).</summary>
        /// <param name="phase">The card's phase.</param>
        /// <returns>The toast.</returns>
        std::wstring ClosedPhaseToast(AskCardPhase phase)
        {
            switch (phase)
            {
            case AskCardPhase::Sending:
                return L"\U0001F4E8 Your answer is on its way to Claude.";
            case AskCardPhase::Answered:
                return L"\x2705 Already answered from here.";
            case AskCardPhase::AnsweredAtPc:
                return L"\x2705 It was answered at the PC.";
            case AskCardPhase::Clarify:
                return L"\U0001F4AC You asked Claude to clarify \x2014 its next message asks what.";
            case AskCardPhase::Cancelled:
                return L"\x2716\xFE0F You cancelled this question \x2014 Claude stopped and waits for you.";
            case AskCardPhase::Replied:
                return L"\U0001F4AC You replied to Claude instead of answering.";
            case AskCardPhase::TimedOut:
                return L"\x23F1\xFE0F Claude Code went on by itself \x2014 nobody answered in time.";
            case AskCardPhase::Closed:
            default:
                return L"\x23F9\xFE0F This question is not open any more.";
            }
        }

        /// <summary>The phase's word in a hooks.log line.</summary>
        /// <param name="phase">The phase.</param>
        /// <returns>The word.</returns>
        const wchar_t* PhaseWord(AskCardPhase phase) noexcept
        {
            switch (phase)
            {
            case AskCardPhase::Open:
                return L"open";
            case AskCardPhase::Sending:
                return L"sending";
            case AskCardPhase::Answered:
                return L"answered from Telegram";
            case AskCardPhase::AnsweredAtPc:
                return L"answered at the PC";
            case AskCardPhase::Clarify:
                return L"Chat about this";
            case AskCardPhase::Cancelled:
                return L"cancelled from Telegram";
            case AskCardPhase::Replied:
                return L"replied from Telegram";
            case AskCardPhase::TimedOut:
                return L"Claude Code's idle timeout";
            case AskCardPhase::Closed:
            default:
                return L"closed";
            }
        }

        /// <summary>Is the phase an outcome (the card takes no more taps and is forgotten kAskCardKeepMs later)?</summary>
        /// <param name="phase">The phase.</param>
        /// <returns>True for Answered, AnsweredAtPc, Clarify and Closed.</returns>
        bool IsFinishedPhase(AskCardPhase phase) noexcept
        {
            return phase != AskCardPhase::Open && phase != AskCardPhase::Sending;
        }

        /// <summary>How many of a card's questions have an answer, and how many carry a note (a preview question's Notes field).</summary>
        /// <param name="m">The card's model.</param>
        /// <param name="answered">Out: the answered questions.</param>
        /// <param name="notes">Out: the questions with a note.</param>
        /// <exception cref="std::bad_alloc">If an answer cannot be computed.</exception>
        void CountAnswers(const AskCardModel& m, size_t& answered, size_t& notes)
        {
            answered = 0;
            notes = 0;
            for (size_t i = 0; i < m.questions.size() && i < m.choices.size(); ++i)
            {
                answered += AskQuestionAnswered(m.questions[i], m.choices[i]) ? 1 : 0;
                notes += m.questions[i].previewLayout && !m.choices[i].note.empty() ? 1 : 0;
            }
        }
    }

    // ===== the registry: set once, accepted while the bot runs =====================================================

    void MessagingService::SetAskRegistry(std::shared_ptr<AskRegistry> registry) noexcept
    {
        try
        {
            if (registry)
            {
                // Every change of the registry (a request opened, answered, written, released, gone) wakes the tick, so a question
                // reaches the phone within a moment instead of the tick's period. Called with no registry lock held.
                std::weak_ptr<MessagingService> weak = weak_from_this();
                registry->SetListener([weak]() {
                    if (const auto me = weak.lock())
                    {
                        {
                            std::lock_guard lk{ me->_mtx };
                            me->_tickRequested = true;
                        }
                        me->_tickCv.notify_all();
                    }
                });
            }
            std::shared_ptr<AskRegistry> previous;
            {
                std::lock_guard lk{ _mtx };
                previous = std::exchange(_asks, registry);
            }
            if (previous && previous != registry)
            {
                // A registry replaced (never in the app: it is set once) stops calling this service.
                previous->SetListener({});
                previous->SetAcceptor({});
            }
            if (registry && _running.load())
            {
                _questionsStart(); // the bot runs already: become the new registry's acceptor now
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: without the listener and the acceptor the registry refuses every question — they stay with the PC.
            LogSwallowedException(L"MessagingService::SetAskRegistry");
        }
    }

    void MessagingService::_questionsStart()
    {
        std::shared_ptr<AskRegistry> asks;
        {
            std::lock_guard lk{ _mtx };
            asks = _asks;
            // The last run's cards are gone with its buttons' meaning (their keys are forgotten: a tap answers "no longer open
            // here"); its requests were released when it stopped (_questionsStop).
            _questionCards.clear();
            _plainQuestions.clear();
            _asksTaken.clear();
        }
        if (!asks)
        {
            return;
        }
        std::weak_ptr<MessagingService> weak = weak_from_this();
        asks->SetAcceptor([weak](const std::wstring& sessionId) {
            const auto me = weak.lock();
            return me && me->_acceptsAsk(sessionId);
        });
    }

    void MessagingService::_questionsStop() noexcept
    {
        try
        {
            std::shared_ptr<AskRegistry> asks;
            {
                std::lock_guard lk{ _mtx };
                asks = _asks;
                _questionCards.clear();
                _plainQuestions.clear();
                _asksTaken.clear();
            }
            if (!asks)
            {
                return;
            }
            asks->SetAcceptor({}); // from now on every new question is refused at the door: the PC answers it
            size_t released = 0;
            for (const auto& e : asks->Snapshot())
            {
                if (e.state == AskEntryState::Open && asks->Release(e.id, L"the Telegram bot stopped"))
                {
                    ++released;
                }
                else if (e.state != AskEntryState::Open && e.state != AskEntryState::Answered)
                {
                    asks->Forget(e.id);
                }
            }
            if (released > 0)
            {
                _log(L"[ask] the bot stopped: " + std::to_wstring(released) + L" open question(s) released to the PC");
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: a request not released here is released when its hook ends (its claude exits, or a day passes);
            // its dialog at the PC works all the same.
            LogSwallowedException(L"MessagingService::_questionsStop");
        }
    }

    bool MessagingService::_acceptsAsk(const std::wstring& sessionId) const noexcept
    {
        try
        {
            if (sessionId.empty() || !_running.load() || _stop.load() || !_topicsEnabled.load())
            {
                return false; // the bot is off, stopping, or without topics: nothing could show the question
            }
            // The lean read (no queue copy): an ask worker asks this for every question of every Claude session of the machine
            // that carries the hook.
            const auto s = _registry->GetWithoutQueue(sessionId);
            return s && s->live && s->kind == AgentKind::Claude;
        }
        catch (...)
        {
            // Rule #18. Recovery: refused — the helper hears "{}" and the PC answers the dialog as before.
            LogSwallowedException(L"MessagingService::_acceptsAsk");
            return false;
        }
    }

    // ===== the tick =====================================================================================================

    void MessagingService::_questionsTick() noexcept
    {
        try
        {
            const int64_t now = WallMs();
            std::shared_ptr<AskRegistry> asks;
            {
                std::lock_guard lk{ _mtx };
                asks = _asks;
            }
            std::vector<AskEntryInfo> entries;
            if (asks)
            {
                entries = asks->Snapshot();
            }
            // 1. Every request the service has not seen becomes a card in its session's topic.
            for (const auto& e : entries)
            {
                if (e.state != AskEntryState::Open)
                {
                    continue; // answered, released or gone before a card was made: nothing to show
                }
                bool known = false;
                int64_t threadId = 0;
                {
                    std::lock_guard lk{ _mtx };
                    known = _asksTaken.count(e.id) != 0;
                    if (!known)
                    {
                        if (const auto it = _state.topics.find(e.sessionId); it != _state.topics.end())
                        {
                            threadId = it->second.threadId;
                        }
                    }
                }
                if (known)
                {
                    continue;
                }
                std::optional<AskRequest> req;
                try
                {
                    req = ParseAskRequestLine(e.requestLine);
                }
                catch (...)
                {
                    // Rule #18. Recovery: the request reads as unreadable below, and its dialog stays with the PC.
                    LogSwallowedException(L"MessagingService::_questionsTick (parse a request)");
                    req.reset();
                }
                if (!req || req->questions.empty())
                {
                    asks->Release(e.id, L"its questions could not be read");
                    {
                        std::lock_guard lk{ _mtx };
                        _asksTaken.insert(e.id);
                    }
                    _log(L"[ask] " + ShortId(e.sessionId) + L" request " + std::to_wstring(e.id) + L": its questions could not be read; the dialog stays with the PC");
                    continue;
                }
                if (threadId == 0)
                {
                    if (now - e.openedMs >= kAskNoTopicReleaseMs)
                    {
                        asks->Release(e.id, L"its session has no topic");
                        {
                            std::lock_guard lk{ _mtx };
                            _asksTaken.insert(e.id);
                        }
                        _log(L"[ask] " + ShortId(e.sessionId) + L" request " + std::to_wstring(e.id) + L": its session has no topic after " +
                             std::to_wstring(kAskNoTopicReleaseMs / 1000) + L" s; the dialog stays with the PC");
                    }
                    continue; // its topic may be in the making (a session that just turned busy is listed by the next sync)
                }
                _openQuestionCard(e, std::move(*req), threadId);
            }
            // 2. Every card: its watch, its request, its topic, its clocks.
            std::vector<std::pair<std::wstring, uint64_t>> cards;
            {
                std::lock_guard lk{ _mtx };
                cards.reserve(_questionCards.size());
                for (const auto& c : _questionCards)
                {
                    cards.emplace_back(c.key, c.askId);
                }
            }
            for (const auto& [key, askId] : cards)
            {
                const AskEntryInfo* entry = nullptr;
                for (const auto& e : entries)
                {
                    if (e.id == askId)
                    {
                        entry = &e;
                        break;
                    }
                }
                _tickQuestionCard(key, entry, now);
            }
            // 3. Forget: finished cards past their keep time (the oldest finished ones first past the cap), and the registry's
            //    finished requests that no card needs any more.
            std::vector<uint64_t> forget;
            {
                std::lock_guard lk{ _mtx };
                for (auto it = _questionCards.begin(); it != _questionCards.end();)
                {
                    if (it->finishedMs != 0 && now - it->finishedMs >= kAskCardKeepMs)
                    {
                        forget.push_back(it->askId);
                        it = _questionCards.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
                while (_questionCards.size() > kAskMaxCards)
                {
                    const auto oldest = std::find_if(_questionCards.begin(), _questionCards.end(), [](const QuestionCard& c) { return c.finishedMs != 0; });
                    if (oldest == _questionCards.end())
                    {
                        break; // every card is open: none is dropped for the cap
                    }
                    forget.push_back(oldest->askId);
                    _questionCards.erase(oldest);
                }
                for (const auto& e : entries)
                {
                    const bool finished = e.state != AskEntryState::Open && e.state != AskEntryState::Answered;
                    const bool carded = std::any_of(_questionCards.begin(), _questionCards.end(), [&e](const QuestionCard& c) { return c.askId == e.id; });
                    if (finished && !carded && std::find(forget.begin(), forget.end(), e.id) == forget.end())
                    {
                        forget.push_back(e.id);
                    }
                }
                // The ids the registry no longer has never come back (they are never reused): the "taken" set keeps the others.
                for (auto it = _asksTaken.begin(); it != _asksTaken.end();)
                {
                    const bool listed = std::any_of(entries.begin(), entries.end(), [id = *it](const AskEntryInfo& e) { return e.id == id; });
                    it = listed ? std::next(it) : _asksTaken.erase(it);
                }
            }
            if (asks)
            {
                for (const uint64_t id : forget)
                {
                    asks->Forget(id); // an open one is left alone by the registry (a card dropped at a topic's end released it first)
                }
            }
            // 4. The plain questions whose hook did not come within kAskPairWaitMs: show-only, as before §21.30.
            std::vector<PlainQuestion> due;
            {
                std::lock_guard lk{ _mtx };
                for (auto it = _plainQuestions.begin(); it != _plainQuestions.end();)
                {
                    if (now - it->seenMs >= kAskPairWaitMs)
                    {
                        due.push_back(std::move(*it));
                        it = _plainQuestions.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            for (const auto& p : due)
            {
                _postPlainQuestion(p);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the next tick runs the whole pass again (a card's state is kept; a request not seen yet is seen then).
            LogSwallowedException(L"MessagingService::_questionsTick");
        }
    }

    void MessagingService::_openQuestionCard(const AskEntryInfo& entry, AskRequest req, int64_t threadId)
    {
        QuestionCard c;
        c.askId = entry.id;
        c.key = MakeAskCardKey();
        c.sessionId = entry.sessionId;
        c.threadId = threadId;
        c.model = MakeAskCardModel(std::move(req.questions));
        c.signature = AskQuestionsSignature(c.model.questions);
        c.toolInput = std::move(req.toolInput);
        c.transcriptPath = std::move(req.transcriptPath);
        c.openedMs = entry.openedMs;
        const std::wstring key = c.key;
        const size_t questions = c.model.questions.size();
        bool paired = false;
        {
            std::lock_guard lk{ _mtx };
            _asksTaken.insert(entry.id);
            // The mirror read the dialog's question first and holds it for this request: the card takes its place (and its sound,
            // decided when the question was read, and its tool_use id).
            for (auto it = _plainQuestions.begin(); it != _plainQuestions.end(); ++it)
            {
                if (it->sessionId == c.sessionId && it->signature == c.signature)
                {
                    c.sound = it->sound;
                    c.toolUseId = it->toolUseId;
                    c.replyRef = it->replyRef;
                    _plainQuestions.erase(it);
                    paired = true;
                    break;
                }
            }
            if (!paired)
            {
                // The request came first. ONE sound per "needs you" (§21.6): the card rings unless the NeedsApproval edge already rang
                // the sticky card, and the edge that follows rings nothing more. The tick owns the mirrors, and this runs on it,
                // between the per-topic passes: no copy of this session's mirror is out.
                Mirror& m = _mirrors[c.sessionId];
                c.sound = !m.approvalPinged;
                m.awaitingAnswer = true;
                c.replyRef = m.promptRef;
            }
            _questionCards.push_back(std::move(c));
        }
        _postQuestionCard(key);
        _log(L"[ask] " + ShortId(entry.sessionId) + L" request " + std::to_wstring(entry.id) + L": question card posted in its topic (" + std::to_wstring(questions) +
             L" question(s)" + (paired ? L", after the transcript's question" : L"") + L")");
    }

    void MessagingService::_tickQuestionCard(const std::wstring& key, const AskEntryInfo* entry, int64_t now)
    {
        // What the watch needs, copied: the read is file IO and runs without the lock.
        std::wstring path;
        std::wstring signature;
        std::wstring toolUseId;
        std::wstring sessionId;
        int64_t offset = -1;
        bool watch = false;
        bool open = false;
        {
            std::lock_guard lk{ _mtx };
            QuestionCard* c = _findQuestionCardLocked(key);
            if (!c)
            {
                return;
            }
            watch = !c->resultSeen && !c->transcriptPath.empty() && now - c->lastWatchMs >= kAskWatchGapMs;
            if (watch)
            {
                c->lastWatchMs = now;
                path = c->transcriptPath;
                signature = c->signature;
                toolUseId = c->toolUseId;
                offset = c->watchOffset;
            }
            sessionId = c->sessionId;
            open = c->model.phase == AskCardPhase::Open;
        }
        QuestionWatchRead w;
        if (watch)
        {
            w = ReadQuestionWatch(path, offset, signature, toolUseId);
        }
        // The session's own end, read outside `_mtx` (the registry's lock is never taken under it): a session that is no longer
        // live has no claude to answer, and the helper — whose parent check is its own belt — is released here at once.
        bool sessionEnded = false;
        if (open)
        {
            const auto s = _registry->GetWithoutQueue(sessionId);
            sessionEnded = !s || !s->live;
        }
        QuestionEffects fx;
        fx.key = key;
        {
            std::lock_guard lk{ _mtx };
            QuestionCard* c = _findQuestionCardLocked(key);
            if (!c)
            {
                return;
            }
            AskCardModel& m = c->model;
            const AskCardPhase before = m.phase;
            if (watch && w.readOk)
            {
                c->watchOffset = w.nextOffset;
                if (c->toolUseId.empty() && !w.toolUseId.empty())
                {
                    c->toolUseId = w.toolUseId;
                }
                if (w.resultFound && !c->resultSeen)
                {
                    _applyQuestionResultLocked(*c, !w.answered ? QuestionResult::Rejected : (w.timeoutMs > 0 ? QuestionResult::TimedOut : QuestionResult::Answered), w.answerText, now,
                                               fx, w.timeoutMs);
                }
            }
            // The request's side. GONE while open: the hook ended before an answer from here — Esc at the PC (Claude Code ends
            // the hook then), or claude exited. Settled first, so the watch can name the outcome when the transcript has it.
            if (m.phase == AskCardPhase::Open)
            {
                const bool gone = entry == nullptr || (entry->state == AskEntryState::Gone && now - entry->changedMs >= kAskGoneSettleMs);
                if (gone)
                {
                    m.phase = AskCardPhase::Closed;
                    m.closedWhy = L"it was closed at the PC, or its session ended";
                    m.outcomeAtMs = now;
                    m.awaiting = AskTextTarget::None;
                    c->decided = true;
                }
                else if (sessionEnded)
                {
                    // The tab closed, or its claude exited: nothing can answer the dialog any more, and its hook is let go.
                    m.phase = AskCardPhase::Closed;
                    m.closedWhy = L"its session ended";
                    m.outcomeAtMs = now;
                    m.awaiting = AskTextTarget::None;
                    if (!c->decided)
                    {
                        c->decided = true;
                        fx.askId = c->askId;
                        fx.releaseWhy = L"its session ended";
                    }
                }
            }
            else if (m.phase == AskCardPhase::Sending && entry != nullptr && entry->state == AskEntryState::Gone)
            {
                // The decision could not be written: the hook had ended (the PC answered or closed the dialog meanwhile). The watch
                // turns this into "answered at the PC" when the transcript shows that answer.
                m.phase = AskCardPhase::Closed;
                m.closedWhy = L"your answer did not reach Claude Code \x2014 the question was closed at the PC meanwhile";
                m.outcomeAtMs = now;
            }
            if (m.phase == AskCardPhase::Sending && c->sentMs != 0 && now - c->sentMs >= kAskConfirmTimeoutMs)
            {
                // Taken by the hook, never confirmed by the transcript: a rule refused the rewritten input, or the claude ignored it.
                m.phase = AskCardPhase::Answered;
                m.unconfirmed = true;
            }
            // The topic: made again (a recreation) — the card's message went with the old topic, so the card is posted in the new
            // one while it can still be answered; gone — an open card waits kAskNoTopicReleaseMs for a new topic, then releases.
            int64_t topicThread = 0;
            if (const auto it = _state.topics.find(c->sessionId); it != _state.topics.end())
            {
                topicThread = it->second.threadId;
            }
            if (topicThread != 0 && topicThread != c->threadId)
            {
                c->threadId = topicThread;
                c->messageId = 0;
                c->writePromptMessageId = 0;
                c->noTopicSinceMs = 0;
                if (m.phase == AskCardPhase::Open || m.phase == AskCardPhase::Sending)
                {
                    fx.post = true;
                    fx.logLine = L"[ask] " + ShortId(c->sessionId) + L" request " + std::to_wstring(c->askId) + L": its topic was made again; the card is posted there";
                }
            }
            else if (topicThread == 0)
            {
                if (c->noTopicSinceMs == 0)
                {
                    c->noTopicSinceMs = now;
                }
                if (m.phase == AskCardPhase::Open && now - c->noTopicSinceMs >= kAskNoTopicReleaseMs)
                {
                    m.phase = AskCardPhase::Closed;
                    m.closedWhy = L"its topic is gone";
                    m.outcomeAtMs = now;
                    m.awaiting = AskTextTarget::None;
                    c->messageId = 0; // its message went with the topic: no edit
                    if (!c->decided)
                    {
                        c->decided = true;
                        fx.askId = c->askId;
                        fx.releaseWhy = L"its topic is gone";
                    }
                }
            }
            else
            {
                c->noTopicSinceMs = 0;
            }
            // A post Telegram refused: again, while the question can still be answered from here.
            if (!fx.post && c->messageId == 0 && c->postFailedMs != 0 && topicThread == c->threadId && topicThread != 0 &&
                (m.phase == AskCardPhase::Open || m.phase == AskCardPhase::Sending) && now - c->postFailedMs >= kAskRepostAfterFailureMs)
            {
                c->postFailedMs = 0;
                fx.post = true;
            }
            if (m.phase != before)
            {
                ++c->version;
                fx.edit = !fx.post;
                if (m.phase != AskCardPhase::Open && c->writePromptMessageId > 0)
                {
                    fx.deleteMessages.push_back(c->writePromptMessageId); // the ✍️ / 📝 prompt of an open card goes with its openness
                    c->writePromptMessageId = 0;
                }
                if (fx.logLine.empty())
                {
                    fx.logLine = L"[ask] " + ShortId(c->sessionId) + L" request " + std::to_wstring(c->askId) + L": " + PhaseWord(before) + L" -> " + PhaseWord(m.phase) +
                                 (m.phase == AskCardPhase::Answered && m.unconfirmed ? L" (not confirmed by the transcript within " + std::to_wstring(kAskConfirmTimeoutMs / 1000) + L" s)" :
                                                                                      std::wstring{}) +
                                 (m.closedWhy.empty() || m.phase != AskCardPhase::Closed ? std::wstring{} : L" (" + m.closedWhy + L")");
                }
            }
            if (c->finishedMs == 0 && IsFinishedPhase(m.phase))
            {
                c->finishedMs = now;
            }
        }
        _applyQuestionEffects(std::move(fx));
    }

    void MessagingService::_applyQuestionResultLocked(QuestionCard& c, QuestionResult result, const std::wstring& answerText, int64_t now, QuestionEffects& fx, int64_t timeoutMs)
    {
        if (c.resultSeen)
        {
            return; // the other reader (the watch, or the mirror) applied it already
        }
        c.resultSeen = true;
        AskCardModel& m = c.model;
        const AskCardPhase before = m.phase;
        const bool timedOut = result == QuestionResult::TimedOut;
        const bool answered = result == QuestionResult::Answered;
        // Claude Code's own idle timeout (`afkTimeoutMs`) resolved the dialog: nobody answered it, from the PC or from here. The card
        // says so, with what was selected before it; a hook still waiting is released like after a PC answer.
        const auto toTimedOut = [&]() {
            m.phase = AskCardPhase::TimedOut;
            m.atPcAnswer = answerText;
            m.timeoutMs = timeoutMs;
            m.closedWhy.clear();
            m.outcomeAtMs = now;
            m.awaiting = AskTextTarget::None;
            m.confirmCancel = false;
            if (!c.decided)
            {
                c.decided = true;
                fx.askId = c.askId;
                fx.releaseWhy = L"Claude Code's idle timeout resolved it";
            }
        };
        switch (m.phase)
        {
        case AskCardPhase::Open:
            if (timedOut)
            {
                toTimedOut();
                break;
            }
            // The PC was first. Claude Code does NOT end the hook when the PC answers (measured on 2.1.291): released here, it
            // prints nothing and ends — else it would wait until its claude exits.
            m.phase = answered ? AskCardPhase::AnsweredAtPc : AskCardPhase::Closed;
            m.atPcAnswer = answered ? answerText : std::wstring{};
            m.closedWhy = answered ? std::wstring{} : std::wstring{ L"it was dismissed at the PC" };
            m.outcomeAtMs = now;
            m.awaiting = AskTextTarget::None;
            m.confirmCancel = false;
            if (!c.decided)
            {
                c.decided = true;
                fx.askId = c.askId;
                fx.releaseWhy = answered ? L"answered at the PC" : L"dismissed at the PC";
            }
            break;
        case AskCardPhase::Sending:
            if (timedOut)
            {
                toTimedOut(); // the timeout came first: the phone's answer never reached the model
                break;
            }
            // The phone's answer, as Claude Code took it — or, an error, refused (a deny rule, the dialog cancelled at that moment).
            m.phase = answered ? AskCardPhase::Answered : AskCardPhase::Closed;
            if (!answered)
            {
                m.closedWhy = L"Claude Code did not take the answer from here \x2014 answer it at the PC";
                m.outcomeAtMs = now;
            }
            break;
        case AskCardPhase::Answered:
            if (answered && m.unconfirmed)
            {
                m.unconfirmed = false; // confirmed after the clock gave up on it
                ++c.version;
                fx.edit = true;
            }
            break;
        case AskCardPhase::Closed:
            // Closed on the request's side first (the hook ended); the transcript says the PC answered it — or the idle timeout.
            if (timedOut)
            {
                m.phase = AskCardPhase::TimedOut;
                m.atPcAnswer = answerText;
                m.timeoutMs = timeoutMs;
                m.closedWhy.clear();
            }
            else if (answered)
            {
                m.phase = AskCardPhase::AnsweredAtPc;
                m.atPcAnswer = answerText;
                m.closedWhy.clear();
            }
            break;
        case AskCardPhase::AnsweredAtPc:
        case AskCardPhase::Clarify:
        case AskCardPhase::Cancelled:
        case AskCardPhase::Replied:
        case AskCardPhase::TimedOut:
        default:
            break; // the phone's own decision (its deny or its reply is the result) or an outcome already shown: confirmed
        }
        if (m.phase != before)
        {
            ++c.version;
            fx.edit = true;
            fx.logLine = L"[ask] " + ShortId(c.sessionId) + L" request " + std::to_wstring(c.askId) + L": " + PhaseWord(before) + L" -> " + PhaseWord(m.phase) + L" (the transcript's result)";
            if (m.phase != AskCardPhase::Open && c.writePromptMessageId > 0)
            {
                fx.deleteMessages.push_back(c.writePromptMessageId);
                c.writePromptMessageId = 0;
            }
            if (c.finishedMs == 0 && IsFinishedPhase(m.phase))
            {
                c.finishedMs = now;
            }
        }
    }

    void MessagingService::_prepareQuestionAnswerLocked(QuestionCard& c, int64_t now, QuestionEffects& fx, std::wstring& toast)
    {
        json::Value answers;
        json::Value annotations;
        ComposeAskAnswers(c.model.questions, c.model.choices, answers, annotations);
        if (!AskAnswersWithinBounds(answers))
        {
            // Claude Code's own bounds for a remote answer (8,192 characters each, 32,768 in all): its hook path does not check
            // them, so Agentmaster does — a longer answer is the user's to shorten, never cut silently.
            toast = L"The answers are too long for Claude Code (8,192 characters each, 32,768 in all) \x2014 shorten your own answer.";
            return;
        }
        fx.decisionLine = BuildAskAllowLine(c.toolInput, answers, annotations);
        fx.askId = c.askId;
        c.decided = true;
        c.sentMs = now;
        c.model.phase = AskCardPhase::Sending;
        c.model.awaiting = AskTextTarget::None;
        c.model.outcomeAtMs = now;
        ++c.version;
        fx.edit = true;
        if (c.writePromptMessageId > 0)
        {
            fx.deleteMessages.push_back(c.writePromptMessageId);
            c.writePromptMessageId = 0;
        }
        size_t answered = 0;
        size_t notes = 0;
        CountAnswers(c.model, answered, notes);
        const std::wstring counts = std::to_wstring(answered) + L" of " + std::to_wstring(c.model.questions.size()) + L" question(s) answered, " + std::to_wstring(notes) + L" note(s)";
        // Sizes and counts only: the answers themselves are the user's words and stay out of the log.
        fx.logLine = L"[ask] " + ShortId(c.sessionId) + L" request " + std::to_wstring(c.askId) + L": answered from Telegram (" + counts + L", " +
                     std::to_wstring(fx.decisionLine.size()) + L" bytes)";
        fx.navLine = L"remote telegram question " + ShortId(c.sessionId) + L" -> answered (" + counts + L")";
        toast = L"\U0001F4E8 Sent to Claude.";
    }

    MessagingService::QuestionCard* MessagingService::_findQuestionCardLocked(std::wstring_view key)
    {
        for (auto& c : _questionCards)
        {
            if (c.key == key)
            {
                return &c;
            }
        }
        return nullptr;
    }

    // ===== the mirror's two hooks (Messaging.cpp _applyItems, tick thread) ===========================================

    bool MessagingService::_noteTranscriptQuestion(const std::wstring& sessionId, const TopicItem& it, const std::wstring& html, bool sound, const std::shared_ptr<PromptRef>& replyRef)
    {
        std::lock_guard lk{ _mtx };
        if (!_asks)
        {
            return false; // no question pipe (the m5 harness): the question is posted at once, show-only
        }
        for (auto& c : _questionCards)
        {
            if (c.sessionId != sessionId || c.signature != it.text)
            {
                continue;
            }
            if (!c.toolUseId.empty())
            {
                if (c.toolUseId == it.toolUseId)
                {
                    return true; // the card knows its dialog already (its watch read the tool_use first)
                }
                continue; // a card of an earlier dialog with the same questions
            }
            if (c.finishedMs != 0)
            {
                continue; // a finished card never takes a new dialog
            }
            c.toolUseId = it.toolUseId;
            return true;
        }
        if (!it.toolUseId.empty() && std::any_of(_plainQuestions.begin(), _plainQuestions.end(), [&](const PlainQuestion& p) {
                return p.sessionId == sessionId && p.toolUseId == it.toolUseId;
            }))
        {
            return true; // waiting already (the item was met twice)
        }
        PlainQuestion p;
        p.sessionId = sessionId;
        p.toolUseId = it.toolUseId;
        p.signature = it.text;
        p.html = html;
        p.sound = sound;
        p.seenMs = WallMs();
        p.replyRef = replyRef;
        _plainQuestions.push_back(std::move(p));
        return true;
    }

    bool MessagingService::_onTranscriptAnswer(const std::wstring& sessionId, const std::wstring& toolUseId, const std::wstring& answerText, QuestionResult result, int64_t timeoutMs)
    {
        if (toolUseId.empty())
        {
            return false;
        }
        QuestionEffects fx;
        bool shown = false;
        std::optional<PlainQuestion> flush;
        {
            std::lock_guard lk{ _mtx };
            if (_questionCards.empty() && _plainQuestions.empty())
            {
                return false; // every other tool's result: nothing to look at
            }
            for (auto& c : _questionCards)
            {
                if (c.sessionId != sessionId || c.toolUseId != toolUseId)
                {
                    continue;
                }
                fx.key = c.key;
                _applyQuestionResultLocked(c, result, answerText, WallMs(), fx, timeoutMs);
                // A card in the topic (or on its way there) shows the answer; one never posted (refused, its topic gone) does not.
                shown = c.messageId != 0;
                break;
            }
            if (!shown)
            {
                // A plain question still waiting for its hook: it goes NOW, so it lands above the answer line that follows.
                for (auto it = _plainQuestions.begin(); it != _plainQuestions.end(); ++it)
                {
                    if (it->sessionId == sessionId && it->toolUseId == toolUseId)
                    {
                        flush = std::move(*it);
                        _plainQuestions.erase(it);
                        break;
                    }
                }
            }
        }
        if (!fx.key.empty())
        {
            _applyQuestionEffects(std::move(fx));
        }
        if (flush)
        {
            _postPlainQuestion(*flush);
        }
        return shown;
    }

    // ===== the card's buttons (poll thread, inside the tap's scope — §21.28) ===========================================

    std::wstring MessagingService::_onQuestionTap(const std::wstring& data, int64_t threadId, int64_t messageId)
    {
        const std::optional<AskTap> tap = ParseAskCallbackData(data);
        if (!tap)
        {
            return {};
        }
        const int64_t now = WallMs();
        QuestionEffects fx;
        fx.key = tap->key;
        std::wstring toast;
        bool unknown = false;
        bool picture = false;
        bool decisionPrepared = false;
        std::wstring queueSession; // "h": a held message becomes the session's next prompt
        std::wstring queueText;
        int64_t queueMessageId = 0;
        {
            std::lock_guard lk{ _mtx };
            QuestionCard* c = _findQuestionCardLocked(tap->key);
            if (!c)
            {
                unknown = true;
            }
            else
            {
                AskCardModel& m = c->model;
                const std::wstring& verb = tap->verb;
                const bool open = m.phase == AskCardPhase::Open;
                // What a step decided, applied the same way for every verb.
                const auto takeStep = [&](const AskStep& step) {
                    if (step.changed)
                    {
                        ++c->version;
                        fx.edit = true;
                        if (m.awaiting == AskTextTarget::None && c->writePromptMessageId > 0)
                        {
                            fx.deleteMessages.push_back(c->writePromptMessageId); // the text came another way: its prompt goes
                            c->writePromptMessageId = 0;
                        }
                    }
                    if (!step.toast.empty())
                    {
                        toast = step.toast;
                    }
                    if (step.send)
                    {
                        _prepareQuestionAnswerLocked(*c, now, fx, toast);
                        decisionPrepared = !fx.decisionLine.empty();
                    }
                };
                const size_t n = m.questions.size();
                // The verbs that act on the tab's QUESTION (its options, its own answer, its note, its Done): the review tab has none.
                const bool questionVerb = verb == kAskVerbOption || verb == kAskVerbWrite || verb == kAskVerbNote || verb == kAskVerbNoteRemove || verb == kAskVerbNoteSend ||
                                          verb == kAskVerbDone;
                // A decision of the phone's own (Chat about this, Cancel, a reply) ends the card at once: the deny or the reply IS the
                // dialog's result, so there is no Sending phase to confirm (a decision that comes too late is said so by the effects).
                const auto finishWith = [&](AskCardPhase phase, std::string line) {
                    fx.decisionLine = std::move(line);
                    fx.askId = c->askId;
                    decisionPrepared = true;
                    c->decided = true;
                    m.phase = phase;
                    m.awaiting = AskTextTarget::None;
                    m.confirmCancel = false;
                    m.outcomeAtMs = now;
                    c->finishedMs = now;
                    ++c->version;
                    fx.edit = true;
                    if (c->writePromptMessageId > 0)
                    {
                        fx.deleteMessages.push_back(c->writePromptMessageId);
                        c->writePromptMessageId = 0;
                    }
                };
                if (verb == kAskVerbQueueHeld)
                {
                    // "📨 The next prompt": whatever the card's phase, the held message becomes the session's next prompt (D30).
                    if (const auto h = c->held.find(tap->arg); h == c->held.end())
                    {
                        toast = L"That message was decided already.";
                    }
                    else
                    {
                        queueSession = c->sessionId;
                        queueText = h->second;
                        queueMessageId = h->first;
                        c->held.erase(h);
                        fx.deleteMessages.push_back(messageId); // the "what is this message?" question has its answer
                    }
                }
                else if (verb == kAskVerbPicture)
                {
                    picture = true; // the previews can be looked at whatever the phase
                }
                else if (!open)
                {
                    toast = ClosedPhaseToast(m.phase);
                    if ((verb == kAskVerbUseHeld || verb == kAskVerbReplyHeld) && c->held.count(tap->arg) != 0)
                    {
                        toast += L" Tap \U0001F4E8 The next prompt to send your message to Claude.";
                    }
                }
                else if (n == 0)
                {
                    // A card without a question to act on (its request held none it could read): nothing to do, no toast.
                }
                else if (questionVerb && m.current >= n)
                {
                    toast = L"Open a question's tab first.";
                }
                else if (verb == kAskVerbOption)
                {
                    if (tap->arg < 0 || static_cast<size_t>(tap->arg) >= m.questions[m.current].options.size())
                    {
                        toast = L"That option is not on this question \x2014 the card changed meanwhile.";
                    }
                    else
                    {
                        takeStep(AskTapOption(m, static_cast<size_t>(tap->arg)));
                    }
                }
                else if (verb == kAskVerbWrite || verb == kAskVerbNote)
                {
                    // A preview question's only free text is its NOTE (the PC's Notes field); any other question's is the user's own
                    // answer ("Type something."). The button names the right one; an older keyboard's tap is read the same way.
                    const AskTextTarget target = AskTextTargetFor(m);
                    m.awaiting = target;
                    m.confirmCancel = false;
                    ++c->version;
                    fx.edit = true;
                    fx.writePrompt = target;
                    toast = target == AskTextTarget::Note ? L"\U0001F4DD Write your note and send it." : L"\x270D\xFE0F Write your answer and send it.";
                }
                else if (verb == kAskVerbNoteRemove)
                {
                    m.choices[m.current].note.clear();
                    m.awaiting = AskTextTarget::None;
                    ++c->version;
                    fx.edit = true;
                    toast = L"Note removed.";
                }
                else if (verb == kAskVerbNoteSend)
                {
                    AskChoice& choice = m.choices[m.current];
                    if (detail::AskJsTrim(choice.note).empty())
                    {
                        toast = L"Write a note first (\U0001F4DD Add a note).";
                    }
                    else
                    {
                        // The note ALONE is the answer: the PC sends "(notes only)" for a note without a pick (`_0t`).
                        choice.picks.clear();
                        choice.otherText.clear();
                        AskStep step;
                        step.changed = true;
                        step.send = n == 1;
                        if (n > 1)
                        {
                            detail::AskAdvance(m, m.current);
                        }
                        takeStep(step);
                    }
                }
                else if (verb == kAskVerbDone)
                {
                    if (!AskQuestionAnswered(m.questions[m.current], m.choices[m.current]))
                    {
                        toast = L"Tap at least one option first \x2014 or \x270D\xFE0F add your own.";
                    }
                    else
                    {
                        AskStep step;
                        step.changed = true;
                        step.send = n == 1;
                        if (n > 1)
                        {
                            detail::AskAdvance(m, m.current); // the next unanswered question, or the review
                        }
                        m.awaiting = AskTextTarget::None;
                        takeStep(step);
                    }
                }
                else if (verb == kAskVerbTab)
                {
                    if (tap->arg < 0)
                    {
                        toast = L"That tab is not on this card \x2014 it changed meanwhile.";
                    }
                    else
                    {
                        takeStep(AskGoTab(m, static_cast<size_t>(tap->arg)));
                    }
                }
                else if (verb == kAskVerbPrev || verb == kAskVerbNext)
                {
                    // ◀ / ▶ of a keyboard posted before the tab row: one tab back or on, the review counting as the last tab.
                    const size_t last = n > 1 ? n : 0;
                    const size_t tab = verb == kAskVerbPrev ? (m.current > 0 ? m.current - 1 : 0) : (std::min)(m.current + 1, last);
                    takeStep(AskGoTab(m, tab));
                }
                else if (verb == kAskVerbSend)
                {
                    if (AskAnsweredCount(m) == 0)
                    {
                        toast = L"Answer at least one question first.";
                    }
                    else
                    {
                        AskStep step;
                        step.send = true;
                        takeStep(step);
                    }
                }
                else if (verb == kAskVerbChat)
                {
                    // "Chat about this": the PC's rejection with its clarify request (R4e), so the model asks what to clarify — the
                    // turn goes on, and its next message comes to this topic like any reply.
                    finishWith(AskCardPhase::Clarify, BuildAskDenyLine(AskChatAboutThisMessage(m.questions, m.choices), false));
                    fx.logLine = L"[ask] " + ShortId(c->sessionId) + L" request " + std::to_wstring(c->askId) + L": Chat about this, from Telegram";
                    fx.navLine = L"remote telegram question " + ShortId(c->sessionId) + L" -> chat about this";
                    toast = L"\U0001F4AC Claude will ask what you want to clarify.";
                }
                else if (verb == kAskVerbCancel)
                {
                    // ✖ Cancel ends the turn (the PC's Esc), so a phone tap only ASKS: the card shows the confirmation.
                    takeStep(AskAskCancel(m));
                    toast = L"End Claude's turn? \x2716\xFE0F Yes, cancel \x2014 or \x25C0 No, go back.";
                }
                else if (verb == kAskVerbCancelNo)
                {
                    takeStep(AskCancelBack(m));
                }
                else if (verb == kAskVerbCancelYes)
                {
                    if (!m.confirmCancel)
                    {
                        toast = L"Tap \x2716\xFE0F Cancel first.";
                    }
                    else
                    {
                        // The PC's Esc: Claude Code's STOP rejection with `interrupt` — measured on 2.1.291, the transcript then holds
                        // exactly what an Esc leaves (the rejection, "User rejected tool use", the interrupt marker) and the turn ends.
                        finishWith(AskCardPhase::Cancelled, BuildAskDeclineLine());
                        fx.logLine = L"[ask] " + ShortId(c->sessionId) + L" request " + std::to_wstring(c->askId) + L": cancelled from Telegram (the PC's Esc: the turn ends)";
                        fx.navLine = L"remote telegram question " + ShortId(c->sessionId) + L" -> cancelled";
                        toast = L"\x2716\xFE0F Cancelled \x2014 Claude stops and waits for you.";
                    }
                }
                else if (verb == kAskVerbCancelWrite)
                {
                    m.awaiting = AskTextTarget::None;
                    AskStep step;
                    step.changed = true;
                    takeStep(step);
                }
                else if (verb == kAskVerbUseHeld)
                {
                    if (const auto h = c->held.find(tap->arg); h == c->held.end())
                    {
                        toast = L"That message was decided already.";
                    }
                    else
                    {
                        const AskTextTarget target = m.awaiting != AskTextTarget::None ? m.awaiting : AskTextTargetFor(m);
                        const std::wstring text = h->second;
                        const int64_t heldId = h->first;
                        c->held.erase(h);
                        fx.deleteMessages.push_back(messageId); // the "what is this message?" question has its answer
                        const AskStep step = AskTakeText(m, text, target);
                        if (step.changed)
                        {
                            fx.reactTo = heldId;
                            fx.navLine = L"remote telegram question " + ShortId(c->sessionId) + (target == AskTextTarget::Note ? L" -> note" : L" -> own answer") +
                                         L" chars=" + std::to_wstring(text.size());
                        }
                        takeStep(step);
                    }
                }
                else if (verb == kAskVerbReplyHeld)
                {
                    // "💬 A reply": the held message goes to Claude INSTEAD of answering — Claude Code's own `response` (its remote cards'
                    // "freeform text the user typed instead of selecting a structured option"). The model reads "The user responded:
                    // <text>" and goes on; the answers so far are not sent (the model would not read them next to a response).
                    const auto h = c->held.find(tap->arg);
                    const std::wstring text = h == c->held.end() ? std::wstring{} : detail::AskJsTrim(h->second);
                    if (h == c->held.end())
                    {
                        toast = L"That message was decided already.";
                    }
                    else if (text.empty())
                    {
                        toast = L"That message is empty \x2014 nothing was sent.";
                    }
                    else if (text.size() > kAskAnswerMaxChars)
                    {
                        toast = L"That text is longer than 8,192 characters \x2014 send a shorter one.";
                    }
                    else
                    {
                        const int64_t heldId = h->first;
                        c->held.erase(h);
                        fx.deleteMessages.push_back(messageId); // the "what is this message?" question has its answer
                        m.reply = text;
                        finishWith(AskCardPhase::Replied, BuildAskAllowLine(c->toolInput, json::Value::MkObj(), json::Value::MkObj(), text));
                        fx.reactTo = heldId;
                        // Sizes only: the reply is the user's words and stays out of the log.
                        fx.logLine = L"[ask] " + ShortId(c->sessionId) + L" request " + std::to_wstring(c->askId) + L": replied from Telegram instead of answering (" +
                                     std::to_wstring(text.size()) + L" chars)";
                        fx.navLine = L"remote telegram question " + ShortId(c->sessionId) + L" -> replied instead chars=" + std::to_wstring(text.size());
                        toast = L"\U0001F4AC Sent to Claude as your reply.";
                    }
                }
                // A tap that only changed the card writes no hooks.log line of its own: the tap's "[msg] tap …" line (§21.28) and the
                // [nav] lines of the decisions cover it.
            }
        }
        if (unknown)
        {
            // A card of an earlier run (or one forgotten after its keep time): its buttons do nothing any more — the keyboard goes.
            if (messageId > 0)
            {
                OutItem item;
                item.prio = SendPriority::Final;
                item.method = L"editMessageReplyMarkup";
                item.params = json::Value::MkObj();
                item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
                item.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
                _enqueue(std::move(item));
            }
            return L"This question is no longer open here.";
        }
        if (!queueText.empty())
        {
            _queueRemotePrompt(queueSession, queueText, queueMessageId, threadId, false);
            LogNav(L"remote telegram question " + ShortId(queueSession) + L" -> a held message queued as the next prompt");
        }
        if (picture)
        {
            toast = _startQuestionPicture(tap->key);
        }
        const bool delivered = _applyQuestionEffects(std::move(fx));
        if (decisionPrepared && !delivered)
        {
            toast = L"Too late \x2014 the question was answered or closed at the PC.";
        }
        return toast;
    }

    // ===== typed text while a card is open (poll thread) ===============================================================

    bool MessagingService::_takeQuestionText(const std::wstring& sessionId, const json::Value& msg, int64_t threadId)
    {
        const std::wstring text = msg.StrAt(L"text");
        const int64_t msgId = msg.I64At(L"message_id", 0);
        int64_t replyTo = 0;
        if (const auto* rt = msg.Find(L"reply_to_message"); rt && rt->type == json::Value::Type::Obj)
        {
            replyTo = rt->I64At(L"message_id", 0);
        }
        const int64_t now = WallMs();
        QuestionEffects fx;
        std::wstring toast;
        bool changed = false;
        bool decisionPrepared = false;
        bool askWhich = false;
        std::wstring whichHtml;
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> whichRows;
        {
            std::lock_guard lk{ _mtx };
            // The session's OPEN card — the newest, when a second dialog opened before the first one closed.
            QuestionCard* c = nullptr;
            for (auto& q : _questionCards)
            {
                if (q.sessionId == sessionId && q.model.phase == AskCardPhase::Open)
                {
                    c = &q;
                }
            }
            if (!c)
            {
                return false;
            }
            const bool replyToCard = replyTo > 0 && (replyTo == c->messageId || replyTo == c->writePromptMessageId);
            AskTextRoute route = RouteAskText(c->model, text, replyToCard);
            AskTextTarget target = AskTextTarget::None;
            if (route == AskTextRoute::Take)
            {
                target = c->model.awaiting != AskTextTarget::None ? c->model.awaiting : AskTextTargetFor(c->model);
                if (target == AskTextTarget::None)
                {
                    // A reply to the card on its review with every question answered: no question is left to take it — asked about.
                    route = AskTextRoute::AskWhich;
                }
            }
            switch (route)
            {
            case AskTextRoute::NotForCard:
                return false; // a /command: the topic's usual handling
            case AskTextRoute::Take:
            {
                fx.key = c->key;
                const AskStep step = AskTakeText(c->model, text, target);
                changed = step.changed;
                toast = step.toast;
                if (step.changed)
                {
                    ++c->version;
                    fx.edit = true;
                    fx.reactTo = msgId;
                    fx.navLine = L"remote telegram question " + ShortId(sessionId) + (target == AskTextTarget::Note ? L" -> note" : L" -> own answer") + L" chars=" +
                                 std::to_wstring(text.size());
                    if (c->writePromptMessageId > 0)
                    {
                        fx.deleteMessages.push_back(c->writePromptMessageId); // its text came: the ForceReply prompt goes
                        c->writePromptMessageId = 0;
                    }
                }
                if (step.send)
                {
                    _prepareQuestionAnswerLocked(*c, now, fx, toast);
                    decisionPrepared = !fx.decisionLine.empty();
                }
                break;
            }
            case AskTextRoute::AskWhich:
            default:
            {
                // An open card and a plain message: the user's answer (note), a reply instead of answering, or the session's next
                // prompt? Held, and asked.
                if (c->held.size() >= kAskMaxHeld)
                {
                    c->held.erase(c->held.begin());
                }
                c->held[msgId] = text;
                fx.key = c->key;
                askWhich = true;
                // My answer (note) when a question can take it, a reply to Claude instead of answering, or the next prompt.
                whichHtml = AskWhichHtml(c->model);
                whichRows = AskWhichButtons(c->model, c->key, msgId);
                break;
            }
            }
        }
        const bool delivered = _applyQuestionEffects(std::move(fx));
        if (askWhich)
        {
            // Silent, and outside the sticky card's clock (the user's own message just moved it): it replies to the message it asks about.
            _sendText(threadId, whichHtml, true, SendPriority::Final, L"", _buttons(whichRows), nullptr, msgId, false);
            return true;
        }
        if (decisionPrepared && !delivered)
        {
            _reply(threadId, L"Too late \x2014 the question was answered or closed at the PC.", false, json::Value{}, msgId);
        }
        else if (!changed && !toast.empty())
        {
            _reply(threadId, TgEscapeHtml(toast), false, json::Value{}, msgId); // an empty or too long text: said why nothing changed
        }
        return true;
    }

    bool MessagingService::_questionTextEdited(const std::wstring& sessionId, const json::Value& msg)
    {
        const int64_t msgId = msg.I64At(L"message_id", 0);
        std::lock_guard lk{ _mtx };
        for (auto& c : _questionCards)
        {
            if (c.sessionId != sessionId)
            {
                continue;
            }
            if (const auto h = c.held.find(msgId); h != c.held.end())
            {
                h->second = msg.StrAt(L"text"); // the decision the user makes next uses the text as it reads now
                return true;
            }
        }
        return false;
    }

    // ===== carrying it out (outside `_mtx`) ============================================================================

    bool MessagingService::_applyQuestionEffects(QuestionEffects fx)
    {
        std::shared_ptr<AskRegistry> asks;
        {
            std::lock_guard lk{ _mtx };
            asks = _asks;
        }
        bool delivered = true;
        if (fx.askId != 0 && !fx.decisionLine.empty())
        {
            if (!asks || !asks->Answer(fx.askId, fx.decisionLine))
            {
                // The request was no longer open: the PC answered or closed the dialog first (or the bot stopped). The card says so
                // now; its watch names the PC's answer when the transcript shows it.
                delivered = false;
                std::lock_guard lk{ _mtx };
                if (QuestionCard* c = _findQuestionCardLocked(fx.key); c && (c->model.phase == AskCardPhase::Sending || c->model.phase == AskCardPhase::Clarify ||
                                                                             c->model.phase == AskCardPhase::Cancelled || c->model.phase == AskCardPhase::Replied))
                {
                    c->model.phase = AskCardPhase::Closed;
                    c->model.closedWhy = L"it was answered or closed at the PC first";
                    c->model.outcomeAtMs = WallMs();
                    c->finishedMs = c->model.outcomeAtMs;
                    ++c->version;
                    fx.edit = true;
                }
                fx.logLine += L" \x2014 TOO LATE: the request was no longer open (the PC answered or closed it first)";
                fx.navLine += L" (too late)";
            }
        }
        if (fx.askId != 0 && !fx.releaseWhy.empty() && asks)
        {
            asks->Release(fx.askId, fx.releaseWhy);
        }
        if (!fx.logLine.empty())
        {
            _log(fx.logLine);
        }
        if (!fx.navLine.empty())
        {
            LogNav(fx.navLine);
        }
        for (const int64_t id : fx.deleteMessages)
        {
            _deleteQuestionMessage(id);
        }
        if (fx.reactTo > 0)
        {
            _react(fx.reactTo, kAskTakenReaction);
        }
        if (!fx.key.empty())
        {
            if (fx.post)
            {
                _postQuestionCard(fx.key);
            }
            else if (fx.edit)
            {
                _queueQuestionCardEdit(fx.key);
            }
            if (fx.writePrompt != AskTextTarget::None)
            {
                _postQuestionWritePrompt(fx.key, fx.writePrompt);
            }
        }
        return delivered;
    }

    void MessagingService::_postQuestionCard(const std::wstring& key)
    {
        std::wstring html;
        json::Value markup;
        int64_t threadId = 0;
        bool silent = true;
        std::shared_ptr<PromptRef> replyRef;
        uint64_t version = 0;
        {
            std::lock_guard lk{ _mtx };
            QuestionCard* c = _findQuestionCardLocked(key);
            if (!c || c->threadId == 0 || c->messageId == -1)
            {
                return; // gone, no topic, or a post on its way (its answer catches up with any change)
            }
            c->messageId = -1;
            html = BuildAskCardHtml(c->model, WallMs());
            const auto rows = BuildAskCardButtons(c->model, key);
            if (!rows.empty())
            {
                markup = _buttons(rows);
            }
            threadId = c->threadId;
            silent = !c->sound;
            c->sound = false; // a re-post (a recreated topic, a refused post) never rings again
            replyRef = std::move(c->replyRef); // only the first post quotes the turn's prompt
            version = c->version;
        }
        auto self = weak_from_this();
        _sendText(
            threadId,
            html,
            silent,
            SendPriority::Final, // the card IS the turn's "needs you" message (§21.6): it goes like a final reply
            L"",
            std::move(markup),
            [self, key, version, threadId](const TgResult& r) {
                const auto me = self.lock();
                if (!me)
                {
                    return;
                }
                bool again = false;
                {
                    std::lock_guard lk{ me->_mtx };
                    QuestionCard* c = me->_findQuestionCardLocked(key);
                    if (!c || c->threadId != threadId)
                    {
                        return; // forgotten, or moved to a new topic meanwhile (its own post follows)
                    }
                    if (r.ok)
                    {
                        const int64_t id = r.result.I64At(L"message_id", 0);
                        c->messageId = id > 0 ? id : 0;
                        c->shownVersion = version;
                        again = c->messageId > 0 && c->version > version; // changed while the post was on its way
                    }
                    else
                    {
                        c->messageId = 0;
                        c->postFailedMs = WallMs();
                    }
                }
                if (!r.ok)
                {
                    me->_log(L"[ask] a question card could not be posted (" + (r.description.empty() ? std::wstring{ L"no response" } : r.description) +
                             L"); it is posted again in " + std::to_wstring(kAskRepostAfterFailureMs / 1000) + L" s while the question is open");
                }
                if (again)
                {
                    me->_queueQuestionCardEdit(key);
                }
            },
            0,
            true,
            replyRef);
    }

    void MessagingService::_queueQuestionCardEdit(const std::wstring& key)
    {
        int64_t messageId = 0;
        std::wstring html;
        json::Value markup;
        {
            std::lock_guard lk{ _mtx };
            QuestionCard* c = _findQuestionCardLocked(key);
            if (!c || c->messageId <= 0)
            {
                return; // not posted (or on its way: the post's answer queues this edit when it is needed)
            }
            messageId = c->messageId;
            html = BuildAskCardHtml(c->model, WallMs());
            const auto rows = BuildAskCardButtons(c->model, key);
            if (!rows.empty())
            {
                markup = _buttons(rows);
            }
        }
        // One edit per card in the outbox (the key replaces a queued one; §21.28 moves its tap holds to the newer item).
        OutItem item = _classicEditItem(messageId, html, SendPriority::Final, L"qcard:" + key, markup);
        auto self = weak_from_this();
        item.beforeSend = [self, key](json::Value& params) {
            // Rendered again right before the call: the edit shows the card as it is NOW, however many changes came while it
            // waited (two quick taps, a typed answer). Without the card (forgotten), the text queued with the item goes.
            const auto me = self.lock();
            if (!me)
            {
                return;
            }
            std::wstring latest;
            json::Value latestMarkup;
            {
                std::lock_guard lk{ me->_mtx };
                QuestionCard* c = me->_findQuestionCardLocked(key);
                if (!c || c->messageId <= 0)
                {
                    return;
                }
                latest = BuildAskCardHtml(c->model, WallMs());
                const auto rows = BuildAskCardButtons(c->model, key);
                if (!rows.empty())
                {
                    latestMarkup = me->_buttons(rows);
                }
                c->sendingVersion = c->version;
            }
            detail::AskSetMember(params, L"text", json::Value::MkStr(latest.size() > kTelegramMessageMaxChars ? TgChunkHtml(latest).front() : latest));
            if (latestMarkup.type == json::Value::Type::Obj)
            {
                detail::AskSetMember(params, L"reply_markup", std::move(latestMarkup));
            }
            else
            {
                detail::AskRemoveMember(params, L"reply_markup"); // an edit without a keyboard removes the message's (a finished card)
            }
        };
        item.onDone = [self, key](const TgResult& r) {
            const auto me = self.lock();
            if (!me)
            {
                return;
            }
            bool again = false;
            std::wstring problem;
            {
                std::lock_guard lk{ me->_mtx };
                QuestionCard* c = me->_findQuestionCardLocked(key);
                if (!c)
                {
                    return;
                }
                if (r.ok || r.NotModified())
                {
                    c->shownVersion = (std::max)(c->shownVersion, c->sendingVersion);
                    again = c->version > c->shownVersion;
                }
                else if (r.description.find(L"message to edit not found") != std::wstring::npos)
                {
                    c->messageId = 0; // the user deleted the card (or its topic went): nothing more is edited
                    problem = L"its message is gone";
                }
                else
                {
                    problem = r.description.empty() ? std::wstring{ L"no response" } : r.description;
                }
            }
            if (!problem.empty())
            {
                me->_log(L"[ask] a question card could not be edited (" + problem + L")");
            }
            if (again)
            {
                me->_queueQuestionCardEdit(key);
            }
        };
        _enqueue(std::move(item));
    }

    void MessagingService::_postQuestionWritePrompt(const std::wstring& key, AskTextTarget target)
    {
        std::wstring html;
        int64_t threadId = 0;
        int64_t previous = 0;
        std::wstring placeholder;
        {
            std::lock_guard lk{ _mtx };
            QuestionCard* c = _findQuestionCardLocked(key);
            if (!c || c->threadId == 0 || c->model.phase != AskCardPhase::Open || c->model.awaiting != target || c->model.current >= c->model.questions.size())
            {
                return; // the card moved on before the prompt could go
            }
            if (c->writePromptMessageId != 0 && c->writePromptFor == target)
            {
                return; // the same prompt is there (or on its way)
            }
            previous = c->writePromptMessageId > 0 ? c->writePromptMessageId : 0; // a prompt for the other text goes
            c->writePromptMessageId = -1;
            c->writePromptFor = target;
            threadId = c->threadId;
            const AskQuestion& q = c->model.questions[c->model.current];
            html = (target == AskTextTarget::Note ? std::wstring{ L"\U0001F4DD <b>Your note</b> on: <i>" } : std::wstring{ L"\x270D\xFE0F <b>Your answer</b> to: <i>" }) +
                   TgEscapeHtml(detail::AskCut(q.question, 200)) + L"</i>\n<i>Send it as a reply to this message (or as your next message).</i>";
            placeholder = target == AskTextTarget::Note ? L"Your note" : L"Your answer";
        }
        if (previous > 0)
        {
            _deleteQuestionMessage(previous);
        }
        // ForceReply: the phone opens its keyboard as a reply to this message, with the placeholder in the input field.
        json::Value markup = json::Value::MkObj();
        markup.Set(L"force_reply", json::Value::MkBool(true));
        markup.Set(L"input_field_placeholder", json::Value::MkStr(placeholder));
        auto self = weak_from_this();
        _sendText(
            threadId,
            html,
            true,
            SendPriority::Final,
            L"",
            std::move(markup),
            [self, key, target](const TgResult& r) {
                const auto me = self.lock();
                if (!me)
                {
                    return;
                }
                const int64_t id = r.ok ? r.result.I64At(L"message_id", 0) : 0;
                bool orphan = false;
                {
                    std::lock_guard lk{ me->_mtx };
                    QuestionCard* c = me->_findQuestionCardLocked(key);
                    if (c && c->writePromptMessageId == -1 && c->writePromptFor == target && c->model.phase == AskCardPhase::Open && c->model.awaiting == target)
                    {
                        c->writePromptMessageId = id > 0 ? id : 0;
                    }
                    else
                    {
                        // The text came (or the writing was cancelled) while the prompt was on its way: it goes at once.
                        if (c && c->writePromptMessageId == -1)
                        {
                            c->writePromptMessageId = 0;
                        }
                        orphan = id > 0;
                    }
                }
                if (orphan)
                {
                    me->_deleteQuestionMessage(id);
                }
            },
            0,
            false); // outside the sticky card's clock: a transient prompt, deleted when it is done
    }

    void MessagingService::_deleteQuestionMessage(int64_t messageId)
    {
        if (messageId <= 0)
        {
            return;
        }
        OutItem del;
        // The answers' priority: a tapped button that asked for it (Cancel, the "answer or next prompt?" question) waits for this
        // call before its answer goes (§21.28), and the delete is one cheap call.
        del.prio = SendPriority::Final;
        del.method = L"deleteMessage";
        del.params = json::Value::MkObj();
        del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        auto self = weak_from_this();
        del.onDone = [self, messageId](const TgResult& r) {
            if (const auto me = self.lock(); me && !r.ok)
            {
                me->_log(L"[ask] a question's transient message (" + std::to_wstring(messageId) + L") could not be deleted (" +
                         (r.description.empty() ? std::wstring{ L"no response" } : r.description) + L"); it stays in the topic");
            }
        };
        _enqueue(std::move(del));
    }

    void MessagingService::_postPlainQuestion(const PlainQuestion& p)
    {
        int64_t threadId = 0;
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _state.topics.find(p.sessionId); it != _state.topics.end())
            {
                threadId = it->second.threadId;
            }
        }
        if (threadId == 0)
        {
            return; // the session lost its topic meanwhile: nowhere to show it
        }
        _sendText(threadId, p.html, !p.sound, SendPriority::Final, L"", json::Value{}, nullptr, 0, true, p.replyRef);
        _log(L"[msg] " + ShortId(p.sessionId) + L" final message (a question, show-only: no question hook reached Agentmaster within " +
             std::to_wstring(kAskPairWaitMs / 1000) + L" s)" + (p.sound ? L"" : L", silent"));
    }

    std::wstring MessagingService::_startQuestionPicture(const std::wstring& key)
    {
        ScreenSnapshot sheet;
        int64_t threadId = 0;
        std::wstring caption;
        std::wstring sessionId;
        {
            std::lock_guard lk{ _mtx };
            QuestionCard* c = _findQuestionCardLocked(key);
            if (!c)
            {
                return L"This question is no longer open here.";
            }
            if (c->pictureDrawing)
            {
                return L"The previews are being drawn already.";
            }
            if (c->threadId == 0 || c->model.current >= c->model.questions.size())
            {
                return L"";
            }
            const AskQuestion& q = c->model.questions[c->model.current];
            if (std::none_of(q.options.begin(), q.options.end(), [](const AskOption& o) { return o.previewKind == ClaudePreviewKind::Full; }))
            {
                return L"This question has no previews to draw.";
            }
            sheet = BuildAskPreviewSheet(q);
            caption = L"\U0001F5BC " + (q.header.empty() ? L"Question " + std::to_wstring(c->model.current + 1) : q.header) + L" \x2014 the previews of " +
                      std::to_wstring(q.options.size()) + L" option(s)";
            threadId = c->threadId;
            sessionId = c->sessionId;
            c->pictureDrawing = true;
        }
        const auto clearDrawing = [this, &key]() noexcept {
            try
            {
                std::lock_guard lk{ _mtx };
                if (QuestionCard* c = _findQuestionCardLocked(key))
                {
                    c->pictureDrawing = false;
                }
            }
            catch (...)
            {
                // Rule #18. Recovery: the card keeps saying a picture is being drawn until it is forgotten (no new picture of it).
                LogSwallowedException(L"MessagingService::_startQuestionPicture (clear the mark)");
            }
        };
        if (_screenImageRenders.fetch_add(1) >= kAskPictureMaxRenders)
        {
            _screenImageRenders.fetch_sub(1);
            clearDrawing();
            return L"Other pictures are being drawn right now; tap again in a moment.";
        }
        try
        {
            // The tap's holds ride along (§21.28): the button stays busy until the photo — or the line saying why there is none —
            // is in the topic.
            std::thread([self = weak_from_this(), key, threadId, sessionId, sheet = std::move(sheet), caption, taps = _holdTaps()]() noexcept {
                const auto me = self.lock();
                if (!me)
                {
                    return; // the service is gone (an app quit): nothing to send to
                }
                const TapScope tapScope{ taps }; // never throws; the sends below hold the tap until Telegram answered them
                try
                {
                    const int64_t started = static_cast<int64_t>(::GetTickCount64());
                    const ScreenImage image = RenderScreenSnapshotPng(sheet);
                    if (image.png.empty())
                    {
                        me->_reply(threadId, L"The previews could not be drawn: " + TgEscapeHtml(image.error), false);
                        me->_log(L"[ask] " + ShortId(sessionId) + L" previews picture failed: " + image.error);
                    }
                    else
                    {
                        me->_log(L"[ask] " + ShortId(sessionId) + L" previews picture: " + std::to_wstring(image.grid.widthPx) + L"x" + std::to_wstring(image.grid.heightPx) + L" px, " +
                                 std::to_wstring(image.png.size() / 1024) + L" KB, drawn in " + std::to_wstring(static_cast<int64_t>(::GetTickCount64()) - started) + L" ms");
                        me->_sendPhoto(threadId, L"previews.png", image.png, caption, SendPriority::Final);
                    }
                }
                catch (...)
                {
                    // Rule #18: an exception escaping a std::thread would end the process.
                    LogSwallowedException(L"MessagingService::_startQuestionPicture (worker)");
                }
                me->_screenImageRenders.fetch_sub(1);
                try
                {
                    std::lock_guard lk{ me->_mtx };
                    if (QuestionCard* c = me->_findQuestionCardLocked(key))
                    {
                        c->pictureDrawing = false;
                    }
                }
                catch (...)
                {
                    // Rule #18. Recovery: the card keeps its "being drawn" mark until it is forgotten.
                    LogSwallowedException(L"MessagingService::_startQuestionPicture (worker, clear the mark)");
                }
            }).detach();
        }
        catch (...)
        {
            // The thread could not start (std::system_error, std::bad_alloc): nothing is drawing, so the count and the mark go back.
            _screenImageRenders.fetch_sub(1);
            clearDrawing();
            LogSwallowedException(L"MessagingService::_startQuestionPicture");
            return L"The picture could not be started (hooks.log says why).";
        }
        return {};
    }
}
