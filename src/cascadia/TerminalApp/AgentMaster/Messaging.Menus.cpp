// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging, the MENUS (see Messaging.h, MESSAGING.md §21.34): a LOCAL command's dialog answered from Telegram —
// /add-dir's "Add directory to workspace". The service's fourth TU beside Messaging.cpp, Messaging.Inbound.cpp and
// Messaging.Questions.cpp — one class, four files: the WATCH of a /add-dir a topic queued, the screen reads that find its
// dialog, the MENU CARD (posted when the dialog opens, edited with the outcome), its buttons, the default pressed by itself a
// minute after the dialog opened, and the reply for a command that opened no dialog. The pure half — the dialog's texts and
// parser, the window's press decision, the transcript's result, the card's text and buttons — is TuiMenus.h; the window's half
// is TerminalPage.AgentMessaging.cpp (kMenuScreenAction, kMenuAnswerAction).
//
// The user's request (2026-10-19): "Can you support /add-dir from here and also picker for is supporting the menu that comes
// after of in this session or this folder or whatever that asks you. You can see how we deal with agentmaster's
// AskUserQuestion ... The default is the 1st option and we should support in agentmaster auto-answer after 60s in a clean way".
//
// Unlike a question card (§21.30), nothing here goes through a hook: Claude Code runs /add-dir itself, asks in its own TUI, and
// records nothing until the dialog is answered. So the dialog is FOUND on the screen (the hosting window reads it), ANSWERED by a
// key (the window types the option's digit, but only into the very dialog the card shows), and its RESULT is read from the
// transcript (the echo and the message Claude Code writes once the command ran). The PC's dialog stays fully usable: whoever
// answers first wins, and the card says who it was.

#include "Messaging.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <random>

#include "ClaudeSpawn.h" // LogNav, ShortId, LogSwallowedException, ResolveClaudeTranscriptPath
#include "SessionRegistry.h"
#include "TranscriptTurns.h" // ParseTranscriptJsonl

namespace Agentmaster::Msg
{
    namespace
    {
        // The first screen read waits this long after the command reached the terminal: a command that opens no dialog has
        // normally written its result by then, and that result ends the watch without any read.
        constexpr int64_t kMenuProbeFirstMs = 1000;
        // Screen reads at most this often per watch, and at most this many in all: a dialog opens within a moment of the
        // command, and a command whose Enter the TUI ate is submitted by the Enter rescue within ~15 s, so fifteen reads (about
        // 30 s) that never find it mean there is none to find (the result, if any, still comes). A command with no dialog
        // writes its result before the first read is due, and the watch then reads no screen at all.
        constexpr int64_t kMenuProbeGapMs = 2000;
        constexpr int kMenuProbeMax = 15;
        // A window that does not answer a read or a press within this is taken as no answer (its tab may have closed meanwhile;
        // a window that does not host the session never answers at all).
        constexpr int64_t kMenuReplyTimeoutMs = 6000;
        // After a press the window confirmed, or one that found the dialog gone, the transcript's result may take this long
        // before the card presses again (a digit the TUI ate) or closes (the dialog was answered at the PC, and its result is
        // somehow not readable).
        constexpr int64_t kMenuOutcomeWaitMs = 10 * 1000;
        // Presses per card, and the pause before a press repeats (one that found the box held by a delivery, or failed).
        constexpr int kMenuPressMaxAttempts = 3;
        constexpr int64_t kMenuPressRetryGapMs = 2000;
        // A press the window answers BUSY (a delivery or a draft swap owns the input box) is not an attempt: it is asked again
        // every kMenuPressRetryGapMs for as long as the box can legitimately be held — the delivery gate's own bound. The common
        // case is the /add-dir's own delivery: when the user had an unsent draft at the PC, the draft swap that delivered the
        // command waits up to 18 s for a turn the command never starts, and a tap made meanwhile must not run out of attempts.
        constexpr int64_t kMenuBusyMaxMs = kDeliveryGateTimeoutMs;
        // An open card whose post Telegram refused posts again this much later.
        constexpr int64_t kMenuRepostAfterFailureMs = 30 * 1000;
        // A finished watch is kept this long: its card's buttons answer what happened (an earlier run's card loses its keyboard).
        constexpr int64_t kMenuWatchKeepMs = 30 * 60 * 1000;
        // A command still QUEUED after this long (a session busy for a day) is no longer watched: its delivery is the queue's own
        // business, and a dialog then opens at the PC as before §21.34.
        constexpr int64_t kMenuPendingMaxMs = 24ll * 3600 * 1000;
        // A DELIVERED command whose result was never read (its transcript gone, a dialog nobody answered at the PC with no card)
        // stops being watched after this long, without a word.
        constexpr int64_t kMenuDeliveredMaxMs = 30 * 60 * 1000;
        // An open card whose tab lost its claude (parked, restarted) for this long closes: the dialog went with that claude.
        constexpr int64_t kMenuNotStartedCloseMs = 5 * 1000;
        // The most watches kept at once; the oldest finished ones go first, then the oldest open one (logged).
        constexpr size_t kMenuMaxWatches = 32;
        // The transcript reader reads at most this much per tick, and at most once a second per watch.
        constexpr int64_t kMenuReadMaxBytes = 4 * 1024 * 1024;
        constexpr int64_t kMenuReadGapMs = 1000;

        int64_t WallMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /// <summary>
        /// A new card's callback key: 10 characters of [0-9a-z] from a per-thread generator seeded once (the question card's recipe,
        /// Messaging.Questions.cpp) — never guessable from the card, and a button of another card or of an earlier run never matches.
        /// </summary>
        /// <returns>The key.</returns>
        /// <exception cref="std::bad_alloc">If the key cannot be allocated.</exception>
        std::wstring MakeMenuCardKey()
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

        /// <summary>The size of a file now (0 when it does not exist or cannot be read).</summary>
        /// <param name="path">The file.</param>
        /// <returns>Its size in bytes.</returns>
        int64_t FileSizeNow(const std::wstring& path) noexcept
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (path.empty() || !::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                return 0;
            }
            return (static_cast<int64_t>(data.nFileSizeHigh) << 32) | static_cast<int64_t>(data.nFileSizeLow);
        }

        /// <summary>What one read of a watch's transcript found (ReadMenuTranscript).</summary>
        struct MenuTranscriptRead
        {
            bool readOk{ false }; // the file was read (false: it is gone or unreadable for now — the next tick tries again)
            int64_t nextOffset{ 0 }; // where the next read starts: after the last COMPLETE line read
            bool echoFound{ false }; // this read met the command's echo
            int64_t echoTsMs{ 0 }; // the echo line's time
            bool outputFound{ false }; // the command's result line was met (after its echo, in this read or an earlier one)
            std::wstring outputText; // the result, terminal controls stripped
        };

        /// <summary>
        /// One read of a watch's transcript from `offset`: the COMPLETE lines only (a line still being written waits for the next
        /// read), at most kMenuReadMaxBytes. Looks for the watched command's ECHO — a user line that ClassifyCommandTranscriptText reads
        /// as /add-dir with the queued text's arguments (CommandEchoMatchesPromptText, the scanner's own rule) — and the RESULT line
        /// that follows it. A fork's copies of its source's history (forkedFrom) and meta, sidechain lines are skipped: none of them
        /// can be this command's run.
        /// </summary>
        /// <param name="path">The session's transcript.</param>
        /// <param name="offset">Where the last read ended.</param>
        /// <param name="promptText">The queued command ("/add-dir &lt;path&gt;").</param>
        /// <param name="echoSeen">True when an earlier read met the echo already (the result is the next result line).</param>
        /// <returns>What was found. Never throws: a failure is logged and reads as "not read" (the next tick reads the same bytes).</returns>
        MenuTranscriptRead ReadMenuTranscript(const std::wstring& path, int64_t offset, const std::wstring& promptText, bool echoSeen) noexcept
        {
            MenuTranscriptRead r;
            try
            {
                HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                {
                    return r;
                }
                std::string bytes;
                int64_t from = offset < 0 ? 0 : offset;
                LARGE_INTEGER size{};
                const bool sized = ::GetFileSizeEx(h, &size) != FALSE;
                if (sized)
                {
                    if (from > size.QuadPart)
                    {
                        from = size.QuadPart; // replaced by a shorter file: read on from its end
                    }
                    const int64_t want = (std::min<int64_t>)(size.QuadPart - from, kMenuReadMaxBytes);
                    LARGE_INTEGER pos{};
                    pos.QuadPart = from;
                    if (want > 0 && ::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
                    {
                        bytes.resize(static_cast<size_t>(want));
                        DWORD got = 0;
                        if (!::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr))
                        {
                            bytes.clear();
                        }
                        else
                        {
                            bytes.resize(got);
                        }
                    }
                }
                ::CloseHandle(h);
                if (!sized)
                {
                    return r;
                }
                r.readOk = true;
                const size_t lastNl = bytes.rfind('\n');
                if (lastNl == std::string::npos)
                {
                    // Nothing complete. A window that is FULL and holds no line end is one line longer than the window (a tool's
                    // huge output, never a command's echo): skipped, or the reader would read the same bytes forever.
                    r.nextOffset = bytes.size() >= static_cast<size_t>(kMenuReadMaxBytes) ? from + static_cast<int64_t>(bytes.size()) : from;
                    return r;
                }
                r.nextOffset = from + static_cast<int64_t>(lastNl) + 1;
                bool echo = echoSeen;
                for (const auto& line : ParseTranscriptJsonl(bytes.substr(0, lastNl + 1)))
                {
                    if (line.type != json::Value::Type::Obj || line.StrAt(L"type") != L"user" || line.BoolAt(L"isMeta", false) || line.BoolAt(L"isSidechain", false) ||
                        line.Find(L"forkedFrom"))
                    {
                        continue;
                    }
                    const json::Value* msg = line.Find(L"message");
                    const CommandTranscriptLine c = ClassifyCommandTranscriptText(detail::ContentText(msg ? msg->Find(L"content") : nullptr));
                    if (c.kind == CommandTranscriptLine::Kind::Echo)
                    {
                        if (!echo && c.name == kAddDirCommand && CommandEchoMatchesPromptText(c.name, c.args, promptText))
                        {
                            echo = true;
                            r.echoFound = true;
                            r.echoTsMs = detail::IsoToUnixMs(line.StrAt(L"timestamp"));
                        }
                    }
                    else if (c.kind == CommandTranscriptLine::Kind::Output && echo)
                    {
                        r.outputFound = true;
                        r.outputText = c.text;
                        break; // the watch ends with this result: the rest of the window is not its business
                    }
                }
            }
            catch (...)
            {
                // Rule #18. Recovery: the read counts as not done — the offset stays, and the next tick reads the same bytes again.
                LogSwallowedException(L"Messaging ReadMenuTranscript");
                r = MenuTranscriptRead{};
            }
            return r;
        }

        /// <summary>The toast of a button tapped on a card that is no longer open: what became of the dialog.</summary>
        /// <param name="m">The card.</param>
        /// <returns>The toast.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring FinishedCardToast(const AddDirCardModel& m)
        {
            switch (m.phase)
            {
            case AddDirCardPhase::Answering:
            {
                const AddDirOption* o = AddDirOptionFor(m.pressed);
                return L"\x23F3 Already answering: " + std::wstring{ o ? o->label : std::wstring_view{ L"?" } } + L".";
            }
            case AddDirCardPhase::Answered:
                switch (m.outcome)
                {
                case AddDirOutcome::AddedSession:
                case AddDirOutcome::AddedNotSaved:
                    return L"\x2705 Already answered: added for this session.";
                case AddDirOutcome::AddedRemembered:
                    return L"\x2705 Already answered: added and remembered.";
                case AddDirOutcome::NotAdded:
                    return L"Already answered: not added.";
                default:
                    return L"Already answered.";
                }
            case AddDirCardPhase::Closed:
            case AddDirCardPhase::Open:
            default:
                return L"\x23F9\xFE0F This dialog is not open any more.";
            }
        }
    }

    MessagingService::MenuWatch* MessagingService::_findMenuWatchLocked(std::wstring_view key)
    {
        for (auto& w : _menuWatches)
        {
            if (w.key == key)
            {
                return &w;
            }
        }
        return nullptr;
    }

    // ===== the watch: started at the queueing (poll thread) ==========================================================

    void MessagingService::_watchAddDir(const std::wstring& sessionId, const std::wstring& promptId, const std::wstring& promptText, const std::wstring& args, int64_t messageId, int64_t threadId)
    {
        MenuWatch w;
        w.key = MakeMenuCardKey();
        w.sessionId = sessionId;
        w.promptId = promptId;
        w.promptText = promptText;
        w.args = args;
        w.messageId = messageId;
        w.threadId = threadId;
        w.createdMs = WallMs();
        // The reader starts where the transcript stands NOW, before the command can have run: a command without a dialog writes
        // its result within milliseconds of its delivery, possibly before any tick. A session with no transcript yet (its first
        // message is this command) reads its transcript from the start once it exists.
        w.transcriptPath = ResolveClaudeTranscriptPath(sessionId);
        w.offset = FileSizeNow(w.transcriptPath);
        std::wstring dropped;
        {
            std::lock_guard lk{ _mtx };
            for (const auto& existing : _menuWatches)
            {
                if (existing.promptId == promptId)
                {
                    return; // one watch per prompt (a held message queued twice is still one prompt)
                }
            }
            if (_menuWatches.size() >= kMenuMaxWatches)
            {
                // The oldest finished watch goes first; with none finished, the oldest open one (its card stays as it is, and its
                // buttons then answer "no longer open here").
                auto victim = std::find_if(_menuWatches.begin(), _menuWatches.end(), [](const MenuWatch& x) { return x.finishedMs != 0; });
                if (victim == _menuWatches.end())
                {
                    victim = _menuWatches.begin();
                    dropped = victim->sessionId;
                }
                _menuWatches.erase(victim);
            }
            _menuWatches.push_back(std::move(w));
        }
        if (!dropped.empty())
        {
            _log(L"[menu] " + ShortId(dropped) + L" an open /add-dir watch was dropped: more than " + std::to_wstring(kMenuMaxWatches) + L" at once");
        }
        _log(L"[menu] " + ShortId(sessionId) + L" /add-dir from topic (message " + std::to_wstring(messageId) + L") watched: its dialog becomes a card here, option " +
             std::to_wstring(kAddDirDefaultOption) + L" picked by itself " + std::to_wstring(kTuiMenuAutoAnswerMs / 1000) + L" s after it opens");
    }

    // ===== the tick ==================================================================================================

    void MessagingService::_menusTick() noexcept
    {
        try
        {
            const int64_t now = WallMs();
            std::vector<std::wstring> keys;
            {
                std::lock_guard lk{ _mtx };
                _menuWatches.erase(std::remove_if(_menuWatches.begin(), _menuWatches.end(),
                                                  [now](const MenuWatch& w) { return w.finishedMs != 0 && now - w.finishedMs >= kMenuWatchKeepMs; }),
                                   _menuWatches.end());
                for (const auto& w : _menuWatches)
                {
                    if (w.finishedMs == 0)
                    {
                        keys.push_back(w.key);
                    }
                }
            }
            for (const auto& key : keys)
            {
                if (_stop.load())
                {
                    break;
                }
                try
                {
                    _tickMenuWatch(key, now);
                }
                catch (...)
                {
                    // Rule #18. Recovery: this watch is stepped again at the next tick; the others go on now.
                    LogSwallowedException(L"MessagingService::_tickMenuWatch");
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the next tick runs the whole pass again (each watch keeps its state).
            LogSwallowedException(L"MessagingService::_menusTick");
        }
    }

    void MessagingService::_tickMenuWatch(const std::wstring& key, int64_t now)
    {
        // 1. What the readers need, copied: the registry and the transcript are read without `_mtx`.
        std::wstring sessionId;
        std::wstring promptId;
        std::wstring promptText;
        std::wstring path;
        int64_t offset = 0;
        bool echoSeen = false;
        bool menuSeen = false;
        bool read = false;
        {
            std::lock_guard lk{ _mtx };
            MenuWatch* w = _findMenuWatchLocked(key);
            if (!w || w->finishedMs != 0)
            {
                return;
            }
            sessionId = w->sessionId;
            promptId = w->promptId;
            promptText = w->promptText;
            path = w->transcriptPath;
            offset = w->offset;
            echoSeen = w->echoSeen;
            menuSeen = w->menuSeen;
            // A press whose reply has not come holds the read back: a result read now could not yet be told from the PC's answer
            // (the press may still find the dialog, or find it gone), so the same bytes are read again once the reply is in.
            const bool pressPending = w->pressInFlight && now - w->pressSentMs < kMenuReplyTimeoutMs;
            read = !pressPending && now - w->lastReadMs >= kMenuReadGapMs;
            if (read)
            {
                w->lastReadMs = now;
            }
        }
        // 2. The registry: the session, and the command's delivery. A full copy (the queue holds the prompt), for the few watches
        //    that are open at a time.
        const std::optional<SessionInfo> s = _registry->Get(sessionId);
        const QueuedPrompt* p = nullptr;
        if (s)
        {
            for (const auto& q : s->queue)
            {
                if (q.id == promptId)
                {
                    p = &q;
                    break;
                }
            }
        }
        const bool delivered = p && p->status == PromptStatus::Sent;
        // 3. The transcript, once the command can have run: delivered, or a card is open (its result can come any time).
        MenuTranscriptRead r;
        bool pathResolved = false;
        if (read && (delivered || menuSeen))
        {
            if (path.empty())
            {
                path = ResolveClaudeTranscriptPath(sessionId); // the transcript did not exist at the queueing: read it from its start
                pathResolved = !path.empty();
            }
            if (!path.empty())
            {
                r = ReadMenuTranscript(path, offset, promptText, echoSeen);
            }
        }
        // 4. Decide, under the lock; carry it out after it.
        bool probe = false;
        uint64_t probeSerial = 0;
        bool press = false;
        bool post = false;
        bool edit = false;
        std::wstring replyHtml;
        int64_t replyTo = 0;
        int64_t replyThread = 0;
        std::wstring log;
        std::wstring nav;
        {
            std::lock_guard lk{ _mtx };
            MenuWatch* w = _findMenuWatchLocked(key);
            if (!w || w->finishedMs != 0)
            {
                return;
            }
            AddDirCardModel& m = w->model;
            const AddDirCardPhase before = m.phase;
            if (pathResolved)
            {
                w->transcriptPath = path;
            }
            if (r.readOk)
            {
                w->offset = r.nextOffset;
                if (r.echoFound && !w->echoSeen)
                {
                    w->echoSeen = true;
                    w->echoTsMs = r.echoTsMs;
                }
            }
            int64_t topicThread = 0;
            if (const auto it = _state.topics.find(w->sessionId); it != _state.topics.end())
            {
                topicThread = it->second.threadId;
            }
            const auto close = [&](const std::wstring& why) {
                m.phase = AddDirCardPhase::Closed;
                m.closedWhy = why;
                m.outcomeAtMs = now;
                w->finishedMs = now;
                log = L"[menu] " + ShortId(w->sessionId) + L" /add-dir card closed: " + why;
            };
            if (r.outputFound)
            {
                // (a) THE RESULT: the command ran. Its outcome ends the watch — on the card when the dialog was shown, else as a
                // reply to the user's message (a command with no dialog: already readable, not found, …).
                const AddDirOutcome outcome = ClassifyAddDirOutput(r.outputText);
                if (w->menuSeen)
                {
                    m.phase = AddDirCardPhase::Answered;
                    m.outcome = outcome;
                    m.outcomeText = r.outputText;
                    m.outcomeAtMs = w->echoTsMs > 0 ? w->echoTsMs : now;
                    // Our press, when the window CONFIRMED it and the outcome is that option's; anything else was the PC's answer
                    // (a key there, Enter on the focused option, Esc).
                    m.answeredBy = (m.pressed != 0 && w->lastPress == MenuAnswerResult::Pressed && AddDirOutcomeFitsOption(outcome, m.pressed)) ? m.pressedBy : AddDirAnswerer::Pc;
                    log = L"[menu] " + ShortId(w->sessionId) + L" /add-dir dialog answered (" +
                          (m.answeredBy == AddDirAnswerer::Telegram ? L"from Telegram" : (m.answeredBy == AddDirAnswerer::Auto ? L"by itself" : L"at the PC")) +
                          L"): the card shows its result";
                }
                else
                {
                    replyHtml = BuildAddDirReplyHtml(outcome, r.outputText);
                    replyTo = w->messageId;
                    replyThread = topicThread;
                    log = L"[menu] " + ShortId(w->sessionId) + L" /add-dir ran without a dialog: its result answers the message";
                }
                w->finishedMs = now;
            }
            else if (!s || !s->live)
            {
                // (b) THE SESSION CLOSED: its tab is gone, and the dialog (if any) with its claude.
                if (w->menuSeen && (m.phase == AddDirCardPhase::Open || m.phase == AddDirCardPhase::Answering))
                {
                    close(L"its session ended");
                }
                else
                {
                    w->finishedMs = now;
                }
            }
            else if (s->state == SessionState::Done && w->menuSeen && (m.phase == AddDirCardPhase::Open || m.phase == AddDirCardPhase::Answering))
            {
                // Its claude ended while the card was open: nothing can answer the dialog any more. (A command still QUEUED in an
                // ended session keeps its watch: /reactivate starts claude again, and the command goes then.)
                close(L"its claude ended");
            }
            else if (!w->menuSeen)
            {
                // (c) NO DIALOG SEEN YET: the delivery, then screen reads until the dialog is found (or none comes).
                if (!p)
                {
                    w->finishedMs = now; // the command left the queue unsent (unqueued) or its record is gone: nothing to watch
                }
                else if (p->status == PromptStatus::Pending || p->status == PromptStatus::Held)
                {
                    if (now - w->createdMs >= kMenuPendingMaxMs)
                    {
                        w->finishedMs = now;
                        log = L"[menu] " + ShortId(w->sessionId) + L" /add-dir still queued after a day: no longer watched";
                    }
                }
                else if (p->status != PromptStatus::Sent)
                {
                    w->finishedMs = now; // Failed or Skipped: the delivery's own 👎 says so
                }
                else if (const int64_t sentAt = p->injectedAtUnixMs != 0 ? p->injectedAtUnixMs : p->sentAtUnixMs; sentAt != 0 && now - sentAt >= kMenuDeliveredMaxMs)
                {
                    // Delivered long ago, and neither a dialog nor a result was ever read (its transcript unreadable, a dialog this build
                    // cannot read that nobody answered): the watch ends without a word — the queue's own nets own the prompt.
                    w->finishedMs = now;
                    log = L"[menu] " + ShortId(w->sessionId) + L" /add-dir delivered " + std::to_wstring(kMenuDeliveredMaxMs / 60000) +
                          L" min ago with no dialog found and no result read: no longer watched";
                }
                else if (!p->echoed)
                {
                    // Typed into the terminal (the injection stamp: a prompt accepted for delivery but not typed yet has no dialog).
                    const int64_t deliveredAt = p->injectedAtUnixMs;
                    if (deliveredAt != 0 && now - deliveredAt >= kMenuProbeFirstMs && (s->started || s->external) && w->probes < kMenuProbeMax &&
                        (!w->probeInFlight || now - w->probeSentMs >= kMenuReplyTimeoutMs) && now - w->probeSentMs >= kMenuProbeGapMs)
                    {
                        probe = true;
                        w->probeInFlight = true;
                        w->probeSentMs = now;
                        probeSerial = ++w->probeSerial;
                        ++w->probes;
                    }
                    else if (w->probes >= kMenuProbeMax && !w->unreadableNoted && s->pendingBoxState == InputBoxState::MenuOpen &&
                             (!w->probeInFlight || now - w->probeSentMs >= kMenuReplyTimeoutMs))
                    {
                        // Every read came back without the dialog while the terminal shows a MENU: a dialog this build cannot read
                        // (a Claude Code update changed it) or another menu in the way. Said once; the result still answers.
                        w->unreadableNoted = true;
                        replyHtml = AddDirUnreadableHtml();
                        replyTo = w->messageId;
                        replyThread = topicThread;
                        log = L"[menu] " + ShortId(w->sessionId) + L" /add-dir: a menu is open that does not read as its dialog after " +
                              std::to_wstring(kMenuProbeMax) + L" screen reads; the PC answers it";
                    }
                }
                // Echoed with no result read yet: the result line lands with the echo — the next read takes it.
            }
            else
            {
                // (d) THE CARD: its tab, its clock, its press.
                if (!s->started && !s->external)
                {
                    if (w->notStartedSinceMs == 0)
                    {
                        w->notStartedSinceMs = now;
                    }
                    if (now - w->notStartedSinceMs >= kMenuNotStartedCloseMs && (m.phase == AddDirCardPhase::Open || m.phase == AddDirCardPhase::Answering))
                    {
                        close(L"its tab's claude stopped (the tab was parked or restarted)");
                    }
                }
                else
                {
                    w->notStartedSinceMs = 0;
                }
                if (topicThread != 0 && topicThread != w->threadId && w->cardMessageId != -1 && (m.phase == AddDirCardPhase::Open || m.phase == AddDirCardPhase::Answering))
                {
                    // The topic was made again (a recreation): the card's message went with the old one, so the card is posted in
                    // the new topic while it can still be answered (_postMenuCard moves the watch to it).
                    w->cardMessageId = 0;
                    post = true;
                }
                if (m.phase == AddDirCardPhase::Open)
                {
                    if (now >= m.deadlineMs)
                    {
                        // The user's rule: the DEFAULT, pressed by itself a minute after the dialog opened.
                        m.phase = AddDirCardPhase::Answering;
                        m.pressed = kAddDirDefaultOption;
                        m.pressedBy = AddDirAnswerer::Auto;
                        press = true;
                        const AddDirOption* o = AddDirOptionFor(kAddDirDefaultOption);
                        log = L"[menu] " + ShortId(w->sessionId) + L" /add-dir dialog unanswered for " + std::to_wstring(kTuiMenuAutoAnswerMs / 1000) + L" s: option " +
                              std::to_wstring(kAddDirDefaultOption) + L" (" + std::wstring{ o ? o->value : std::wstring_view{} } + L") is pressed by itself";
                        nav = L"remote telegram menu " + ShortId(w->sessionId) + L" /add-dir -> option " + std::to_wstring(kAddDirDefaultOption) + L" by itself after " +
                              std::to_wstring(kTuiMenuAutoAnswerMs / 1000) + L" s";
                    }
                    else if (w->cardMessageId == 0 && w->postFailedMs != 0 && now - w->postFailedMs >= kMenuRepostAfterFailureMs && topicThread != 0)
                    {
                        w->postFailedMs = 0;
                        post = true;
                    }
                }
                else if (m.phase == AddDirCardPhase::Answering)
                {
                    if (w->pressInFlight && now - w->pressSentMs >= kMenuReplyTimeoutMs)
                    {
                        // No reply from any window: its tab may be gone, or no window hosts the session. Counted as a failed press.
                        w->pressInFlight = false;
                        w->lastPress = MenuAnswerResult::Failed;
                        w->lastPressMs = now;
                    }
                    if (!w->pressInFlight && w->pressSerial == 0)
                    {
                        press = true; // chosen, but its press never went out (it failed before it was asked for): ask now
                    }
                    else if (!w->pressInFlight)
                    {
                        const int64_t since = now - w->lastPressMs;
                        switch (w->lastPress)
                        {
                        case MenuAnswerResult::Pressed:
                            // The digit went in and no result came: a key the TUI ate. Press again (the window reads the dialog
                            // again first), then give up and say so.
                            if (since >= kMenuOutcomeWaitMs)
                            {
                                if (w->pressAttempts < kMenuPressMaxAttempts)
                                {
                                    press = true;
                                }
                                else
                                {
                                    close(L"the answer did not reach Claude Code \x2014 answer it at the PC");
                                }
                            }
                            break;
                        case MenuAnswerResult::Busy:
                            // Not an attempt (_onMenuPressed gave it back): asked again until the box is free, within the bound
                            // a delivery may hold it.
                            if (w->busySinceMs != 0 && now - w->busySinceMs >= kMenuBusyMaxMs)
                            {
                                close(L"the session's input box stayed busy \x2014 answer it at the PC");
                            }
                            else if (since >= kMenuPressRetryGapMs)
                            {
                                press = true;
                            }
                            break;
                        case MenuAnswerResult::Failed:
                            if (since >= kMenuPressRetryGapMs)
                            {
                                if (w->pressAttempts < kMenuPressMaxAttempts)
                                {
                                    press = true;
                                }
                                else
                                {
                                    close(L"Agentmaster could not press the answer \x2014 answer it at the PC");
                                }
                            }
                            break;
                        case MenuAnswerResult::Gone:
                        case MenuAnswerResult::Other:
                        default:
                            // The dialog was answered or closed at the PC first (its result normally lands at once), or another
                            // /add-dir dialog replaced it.
                            if (since >= kMenuOutcomeWaitMs)
                            {
                                close(w->lastPress == MenuAnswerResult::Other ? L"another /add-dir dialog is open now" : L"the dialog was closed at the PC");
                            }
                            break;
                        }
                    }
                }
            }
            if (m.phase != before)
            {
                ++w->version;
                if (w->cardMessageId > 0)
                {
                    edit = true; // the card shown follows (a post still on its way edits itself in after: its answer sees the version)
                }
                else if (w->cardMessageId == 0 && w->postFailedMs != 0 && topicThread != 0)
                {
                    // Telegram refused the card's post, and the dialog moved on meanwhile: the card goes now, as it stands, so the
                    // user still learns what was answered. (A card the user DELETED — no refusal recorded — is not posted again.)
                    w->postFailedMs = 0;
                    post = true;
                }
            }
        }
        // 5. Carry it out.
        if (!log.empty())
        {
            _log(log);
        }
        if (!nav.empty())
        {
            LogNav(nav);
        }
        if (probe && _hooks.sessionAction)
        {
            auto self = weak_from_this();
            _hooks.sessionAction(sessionId, std::wstring{ kMenuScreenAction }, L"", [self, key, probeSerial](std::wstring screen) {
                if (const auto me = self.lock())
                {
                    try
                    {
                        me->_onMenuScreen(key, probeSerial, screen);
                    }
                    catch (...)
                    {
                        // Rule #18. Recovery: the read counts as not answered; the tick reads again after kMenuReplyTimeoutMs.
                        LogSwallowedException(L"MessagingService::_onMenuScreen");
                    }
                }
            });
        }
        if (press)
        {
            _pressMenuOption(key, {});
        }
        if (post)
        {
            _postMenuCard(key);
        }
        else if (edit)
        {
            _queueMenuCardEdit(key);
        }
        if (!replyHtml.empty() && replyThread != 0)
        {
            _reply(replyThread, replyHtml, false, json::Value{}, replyTo);
        }
    }

    // ===== the window's replies (its UI thread) ======================================================================

    void MessagingService::_onMenuScreen(const std::wstring& key, uint64_t serial, const std::wstring& screen)
    {
        // The parse is the read's whole work (pure, a few dozen rows): done before the lock, so the lock is held for bookkeeping only.
        const AddDirMenuScreen menu = ParseAddDirMenuScreen(screen);
        bool post = false;
        std::wstring log;
        {
            std::lock_guard lk{ _mtx };
            MenuWatch* w = _findMenuWatchLocked(key);
            if (!w || w->probeSerial != serial)
            {
                return; // forgotten, or an older read whose wait ran out (a newer one is on its way)
            }
            w->probeInFlight = false;
            if (w->finishedMs != 0 || w->menuSeen || !menu.found)
            {
                return; // done meanwhile, the card exists, or no dialog yet (the tick reads again)
            }
            if (!AddDirMenuBelongsTo(menu.path, w->args))
            {
                log = L"[menu] " + ShortId(w->sessionId) + L" a /add-dir dialog for another folder is open (" + menu.path + L"): not this command's, no card";
            }
            else
            {
                const int64_t now = WallMs();
                w->menuSeen = true;
                AddDirCardModel& m = w->model;
                m.phase = AddDirCardPhase::Open;
                m.path = AddDirShownPath(menu.path, w->args);
                m.deadlineMs = now + kTuiMenuAutoAnswerMs;
                ++w->version;
                post = true;
                log = L"[menu] " + ShortId(w->sessionId) + L" /add-dir dialog open (" + m.path + L"): card posted; option " + std::to_wstring(kAddDirDefaultOption) +
                      L" is pressed by itself in " + std::to_wstring(kTuiMenuAutoAnswerMs / 1000) + L" s unless answered";
            }
        }
        if (!log.empty())
        {
            _log(log);
        }
        if (post)
        {
            _postMenuCard(key);
        }
    }

    void MessagingService::_pressMenuOption(const std::wstring& key, std::vector<TapHoldRef> taps)
    {
        std::wstring sessionId;
        MenuAnswerRequest request;
        uint64_t serial = 0;
        {
            std::lock_guard lk{ _mtx };
            MenuWatch* w = _findMenuWatchLocked(key);
            if (!w || w->model.phase != AddDirCardPhase::Answering || w->model.pressed == 0)
            {
                return; // the card moved on before the press could go
            }
            sessionId = w->sessionId;
            request.command = std::wstring{ kAddDirCommand };
            request.digit = w->model.pressed;
            request.path = w->model.path; // the window presses only into a dialog that shows this folder
            w->pressInFlight = true;
            w->pressSentMs = WallMs();
            serial = ++w->pressSerial;
            ++w->pressAttempts;
        }
        if (!_hooks.sessionAction)
        {
            // The m5 harness (no window): nothing can press; the tick sees a failed press and closes the card in time.
            _onMenuPressed(key, serial, std::wstring{ MenuAnswerResultWord(MenuAnswerResult::Failed) });
            return;
        }
        auto self = weak_from_this();
        _hooks.sessionAction(sessionId, std::wstring{ kMenuAnswerAction }, BuildMenuAnswerArg(request), [self, key, serial, taps = std::move(taps)](std::wstring reply) {
            if (const auto me = self.lock())
            {
                // §21.28: a tapped option stays busy until this reply (its holds go with this function's last copy); the card's
                // edit the reply leads to is queued inside its scope, so it holds the tap too.
                const TapScope tapScope{ taps };
                try
                {
                    me->_onMenuPressed(key, serial, reply);
                }
                catch (...)
                {
                    // Rule #18. Recovery: the press counts as not answered; the tick treats it as failed after kMenuReplyTimeoutMs.
                    LogSwallowedException(L"MessagingService::_onMenuPressed");
                }
            }
        });
    }

    void MessagingService::_onMenuPressed(const std::wstring& key, uint64_t serial, const std::wstring& reply)
    {
        std::lock_guard lk{ _mtx };
        MenuWatch* w = _findMenuWatchLocked(key);
        if (!w || w->pressSerial != serial)
        {
            return; // forgotten, or an older press whose wait ran out
        }
        // The window logged the press itself ([menu] … -> pressed / gone / …); the tick acts on the word at its next step.
        w->pressInFlight = false;
        w->lastPress = ParseMenuAnswerResult(reply);
        w->lastPressMs = WallMs();
        if (w->lastPress == MenuAnswerResult::Busy)
        {
            // Nothing was typed: the box was held. Not an attempt (the press is asked again while the hold lasts, up to
            // kMenuBusyMaxMs from the first busy answer).
            w->pressAttempts = (std::max)(0, w->pressAttempts - 1);
            if (w->busySinceMs == 0)
            {
                w->busySinceMs = w->lastPressMs;
            }
        }
        else
        {
            w->busySinceMs = 0;
        }
    }

    // ===== the card's buttons (poll thread) ==========================================================================

    std::wstring MessagingService::_onMenuTap(const std::wstring& data, int64_t messageId)
    {
        const std::optional<AddDirTap> tap = ParseAddDirCallbackData(data);
        if (!tap)
        {
            return {};
        }
        bool unknown = false;
        bool press = false;
        bool edit = false;
        std::wstring toast;
        std::wstring nav;
        {
            std::lock_guard lk{ _mtx };
            MenuWatch* w = _findMenuWatchLocked(tap->key);
            if (!w)
            {
                unknown = true;
            }
            else if (w->model.phase == AddDirCardPhase::Open)
            {
                AddDirCardModel& m = w->model;
                const AddDirOption* o = AddDirOptionFor(tap->digit);
                m.phase = AddDirCardPhase::Answering;
                m.pressed = tap->digit;
                m.pressedBy = AddDirAnswerer::Telegram;
                ++w->version;
                edit = w->cardMessageId > 0;
                press = true;
                toast = L"Answering: " + std::wstring{ o ? o->label : std::wstring_view{ L"?" } } + L"\x2026";
                nav = L"remote telegram menu " + ShortId(w->sessionId) + L" /add-dir -> option " + std::to_wstring(tap->digit) + L" (" +
                      std::wstring{ o ? o->value : std::wstring_view{} } + L") from Telegram";
            }
            else
            {
                toast = FinishedCardToast(w->model);
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
            return L"This dialog is no longer open here.";
        }
        if (!nav.empty())
        {
            LogNav(nav);
        }
        if (press)
        {
            _pressMenuOption(tap->key, _holdTaps()); // the tap stays busy until the window pressed (or said why it did not)
        }
        if (edit)
        {
            _queueMenuCardEdit(tap->key);
        }
        return toast;
    }

    // ===== the card's message ========================================================================================

    void MessagingService::_postMenuCard(const std::wstring& key)
    {
        std::wstring html;
        json::Value markup;
        int64_t threadId = 0;
        int64_t replyTo = 0;
        uint64_t version = 0;
        {
            std::lock_guard lk{ _mtx };
            MenuWatch* w = _findMenuWatchLocked(key);
            if (!w || w->cardMessageId == -1)
            {
                return; // gone, or a post on its way (its answer catches up with any change)
            }
            if (const auto it = _state.topics.find(w->sessionId); it != _state.topics.end())
            {
                threadId = it->second.threadId;
            }
            if (threadId == 0)
            {
                // No topic now (it is being made again, or the session's topic went): retried like a refused post, so the card
                // goes once the session has a topic again (kMenuRepostAfterFailureMs; the tick's repost and its final-state post).
                w->postFailedMs = WallMs();
                return;
            }
            if (threadId != w->threadId)
            {
                w->threadId = threadId; // a recreated topic: the user's message stayed in the old one, so no reply to it
                w->messageId = 0;
            }
            w->cardMessageId = -1;
            html = BuildAddDirCardHtml(w->model, WallMs());
            const auto rows = BuildAddDirCardButtons(w->model, key);
            if (!rows.empty())
            {
                markup = _buttons(rows);
            }
            replyTo = w->messageId;
            version = w->version;
        }
        auto self = weak_from_this();
        _sendText(
            threadId,
            html,
            false, // it RINGS: a dialog that answers itself in a minute is a "needs you" moment
            SendPriority::Final,
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
                    MenuWatch* w = me->_findMenuWatchLocked(key);
                    if (!w || w->threadId != threadId)
                    {
                        return;
                    }
                    if (r.ok)
                    {
                        const int64_t id = r.result.I64At(L"message_id", 0);
                        w->cardMessageId = id > 0 ? id : 0;
                        w->shownVersion = version;
                        again = w->cardMessageId > 0 && w->version > version; // changed while the post was on its way
                    }
                    else
                    {
                        w->cardMessageId = 0;
                        w->postFailedMs = WallMs();
                    }
                }
                if (!r.ok)
                {
                    me->_log(L"[menu] a /add-dir card could not be posted (" + (r.description.empty() ? std::wstring{ L"no response" } : r.description) +
                             L"); it is posted again in " + std::to_wstring(kMenuRepostAfterFailureMs / 1000) + L" s while the dialog is open");
                }
                if (again)
                {
                    me->_queueMenuCardEdit(key);
                }
            },
            replyTo);
    }

    void MessagingService::_queueMenuCardEdit(const std::wstring& key)
    {
        int64_t messageId = 0;
        std::wstring html;
        json::Value markup;
        {
            std::lock_guard lk{ _mtx };
            MenuWatch* w = _findMenuWatchLocked(key);
            if (!w || w->cardMessageId <= 0)
            {
                return; // not posted (or on its way: the post's answer queues this edit when it is needed)
            }
            messageId = w->cardMessageId;
            html = BuildAddDirCardHtml(w->model, WallMs());
            const auto rows = BuildAddDirCardButtons(w->model, key);
            if (!rows.empty())
            {
                markup = _buttons(rows);
            }
        }
        // One edit per card in the outbox (the key replaces a queued one; §21.28 moves its tap holds to the newer item).
        OutItem item = _classicEditItem(messageId, html, SendPriority::Final, L"mcard:" + key, markup);
        auto self = weak_from_this();
        item.beforeSend = [self, key](json::Value& params) {
            // Rendered again right before the call: the edit shows the card as it is NOW, however many changes came while it waited.
            const auto me = self.lock();
            if (!me)
            {
                return;
            }
            std::wstring latest;
            json::Value latestMarkup;
            {
                std::lock_guard lk{ me->_mtx };
                MenuWatch* w = me->_findMenuWatchLocked(key);
                if (!w || w->cardMessageId <= 0)
                {
                    return; // forgotten: the text queued with the item goes
                }
                latest = BuildAddDirCardHtml(w->model, WallMs());
                const auto rows = BuildAddDirCardButtons(w->model, key);
                if (!rows.empty())
                {
                    latestMarkup = me->_buttons(rows);
                }
                w->sendingVersion = w->version;
            }
            detail::AskSetMember(params, L"text", json::Value::MkStr(latest.size() > kTelegramMessageMaxChars ? TgChunkHtml(latest).front() : latest));
            if (latestMarkup.type == json::Value::Type::Obj)
            {
                detail::AskSetMember(params, L"reply_markup", std::move(latestMarkup));
            }
            else
            {
                detail::AskRemoveMember(params, L"reply_markup"); // an edit without a keyboard removes the card's buttons
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
                MenuWatch* w = me->_findMenuWatchLocked(key);
                if (!w)
                {
                    return;
                }
                if (r.ok || r.NotModified())
                {
                    w->shownVersion = (std::max)(w->shownVersion, w->sendingVersion);
                    again = w->version > w->shownVersion;
                }
                else if (r.description.find(L"message to edit not found") != std::wstring::npos)
                {
                    w->cardMessageId = 0; // the user deleted the card (or its topic went): nothing more is edited
                    problem = L"its message is gone";
                }
                else
                {
                    problem = r.description.empty() ? std::wstring{ L"no response" } : r.description;
                }
            }
            if (!problem.empty())
            {
                me->_log(L"[menu] a /add-dir card could not be edited (" + problem + L")");
            }
            if (again)
            {
                me->_queueMenuCardEdit(key);
            }
        };
        _enqueue(std::move(item));
    }
}
