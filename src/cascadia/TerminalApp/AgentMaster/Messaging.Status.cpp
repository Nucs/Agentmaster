// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging, /status (see Messaging.h, MESSAGING.md §21.40): the report of what Claude Code's own /status dialog
// shows, sent into Telegram. The service's sixth TU beside Messaging.cpp, Messaging.Inbound.cpp, Messaging.Questions.cpp,
// Messaging.Menus.cpp and Messaging.Files.cpp — one class, several files.
//
// The user's request (2026-10-26): "When user send in telegram /status, we need to get all the info we can see in claude code
// status and other tabs like usage to be sent to the user."
//
// Nothing is typed into the session: opening /status in its terminal would show the dialog on the PC, write two lines into the
// transcript, need an empty input box and a running claude. The report is gathered out of band instead (ClaudeStatus.h says
// from where), on a worker thread of its own, and laid out by MessagingStatus.h. The command's earlier reply stays as it was —
// a session's card copy (§21.24), the bot's own line in the 🔎 Search topic — so the user sees an answer at once, and the
// report follows when Claude Code's headless /usage has answered (~6 s; a minute's cache makes the next /status immediate).

#include "Messaging.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <thread>

#include "ClaudeSpawn.h" // LogNav, ShortId, LogSwallowedException, ResolveClaudeDisplayTranscript
#include "ClaudeStatus.h"
#include "MessagingStatus.h"
#include "SessionRegistry.h"
#include "TranscriptStore.h" // ReadSessionPresenceForPid

namespace Agentmaster::Msg
{
    namespace
    {
        /// <summary>The wall clock in Unix ms (the report's "read at" and the process's running span).</summary>
        /// <returns>Milliseconds since 1970.</returns>
        int64_t WallMs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /// <summary>The tick counter in ms (how long a report took, for its log line).</summary>
        /// <returns>GetTickCount64.</returns>
        int64_t TickMs() noexcept
        {
            return static_cast<int64_t>(::GetTickCount64());
        }

        /// <summary>
        /// A read's outcome for the report's log line: "ok", "cached 40s", or the reason it failed, cut to a line.
        /// </summary>
        /// <param name="ok">The read gave its answer.</param>
        /// <param name="cached">The answer came from the cache.</param>
        /// <param name="readAtMs">When it was read (Unix ms).</param>
        /// <param name="error">Why it failed.</param>
        /// <param name="nowMs">Now (Unix ms).</param>
        /// <returns>The word.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring ReadOutcome(bool ok, bool cached, int64_t readAtMs, const std::wstring& error, int64_t nowMs)
        {
            if (!ok)
            {
                return L"failed (" + (error.size() > 120 ? error.substr(0, 120) + L"\x2026" : error) + L")";
            }
            if (cached && readAtMs > 0)
            {
                return L"cached " + std::to_wstring((nowMs - readAtMs) / 1000) + L"s";
            }
            return L"ok";
        }
    }

    const wchar_t* MessagingService::_startClaudeStatusReport(const std::wstring& sessionId, int64_t threadId, int64_t replyTo) noexcept
    {
        // The poll thread: nothing here may wait for a child process or read a large file. It decides, claims a slot and starts
        // the worker; the worker does the rest.
        bool slotClaimed = false;
        try
        {
            std::optional<SessionInfo> session;
            if (!sessionId.empty())
            {
                session = _registry->Get(sessionId);
                if (!session)
                {
                    return L"failed"; // closed in between: the card copy already said what there is to say
                }
                if (session->kind != AgentKind::Claude)
                {
                    // A Codex session has its own /status (in Codex's TUI); Claude Code's account and plan usage are not its.
                    _reply(threadId, L"\U0001F4CA Claude Code's status, account and plan usage are read for Claude Code sessions; this one runs Codex.", false, json::Value{}, replyTo);
                    return L"not-claude";
                }
            }
            if (!SharedClaudeStatusProbe().TryBeginReport())
            {
                _reply(threadId, L"Two status reports are being read right now; ask again in a moment.", false, json::Value{}, replyTo);
                return L"busy";
            }
            slotClaimed = true;
            std::wstring titleToken;
            if (session)
            {
                // §21.13 / §21.17: the title as the tab shows it, a link to its topic (the send layer renders the token).
                titleToken = TgTitleToken(TgTitleForm::Plain, _titleSquare(&*session), TgTitleLine(session->title, 120), _threadOf(sessionId));
            }
            // The engine's claude.exe: the one every session runs, so the account and the usage are those sessions' own. Read here,
            // where the hook was set; the worker never touches the service's fields but through `me`.
            const std::wstring claudeExe = _hooks.claudeExe ? _hooks.claudeExe() : std::wstring{};
            // §21.28: the worker carries the calling scope's taps (none for a typed /status), so a button that asks for a report
            // stays busy until it is in.
            std::thread([self = weak_from_this(), session = std::move(session), threadId, replyTo, titleToken = std::move(titleToken), claudeExe,
                         taps = _holdTaps()]() noexcept {
                // The slot is given back on every path out of the worker, the service gone included.
                struct SlotRelease
                {
                    ~SlotRelease() { SharedClaudeStatusProbe().EndReport(); }
                } release;
                const auto me = self.lock();
                if (!me)
                {
                    return; // the service is gone (an app quit): nothing to send to
                }
                const TapScope tapScope{ taps }; // never throws; the replies below hold the tap until Telegram answered them
                const int64_t startedTick = TickMs();
                try
                {
                    ClaudeStatusReportInput in;
                    in.session = session ? &*session : nullptr;
                    in.titleToken = titleToken;
                    // The plan usage is the slowest read (~6 s: a Claude Code start, the usage API, a scan of this PC's recent
                    // sessions for "What's contributing"), so it runs on a helper thread while the account, the heartbeat and the
                    // transcript are read here. The helper is always joined before `usage` goes out of scope.
                    ClaudeUsageRead usage;
                    std::thread usageThread;
                    try
                    {
                        usageThread = std::thread([&usage, &claudeExe]() noexcept {
                            try
                            {
                                usage = SharedClaudeStatusProbe().Usage(claudeExe);
                            }
                            catch (...)
                            {
                                // Rule #18. Recovery: the report says the usage could not be read.
                                LogSwallowedException(L"MessagingService::_startClaudeStatusReport (usage)");
                                usage = ClaudeUsageRead{};
                                usage.error = L"the usage read failed (hooks.log says why)";
                            }
                        });
                    }
                    catch (...)
                    {
                        // The helper could not start (std::system_error): the usage is read on this thread, after the rest.
                        LogSwallowedException(L"MessagingService::_startClaudeStatusReport (usage thread)");
                    }
                    struct JoinUsage
                    {
                        std::thread& t;
                        ~JoinUsage()
                        {
                            if (t.joinable())
                            {
                                t.join(); // the usage read is bounded by its child's time limit (kClaudeUsageTimeoutMs)
                            }
                        }
                    } joinUsage{ usageThread };

                    in.account = SharedClaudeStatusProbe().Account(claudeExe);
                    if (session)
                    {
                        // The heartbeat of the session's claude, trusted only when it names THIS conversation: a pid Windows gave to
                        // another claude since never matches (and its row would describe another session).
                        SessionPresenceRow row;
                        if (session->pid != 0 && ReadSessionPresenceForPid(session->pid, row) && row.sessionId == session->id)
                        {
                            in.process.running = true;
                            in.process.pid = row.pid;
                            in.process.version = row.version;
                            in.process.status = row.status;
                            in.process.waitingFor = row.waitingFor;
                            in.process.startedAtMs = row.startedAtMs;
                        }
                        // The transcript: the session's own, or — for a fork with no message of its own yet — its source's
                        // (ResolveClaudeDisplayTranscript; the same rule as every other view of a session's conversation).
                        const ClaudeDisplayTranscript display = ResolveClaudeDisplayTranscript(session->id, session->forkParentId);
                        if (display.path.empty())
                        {
                            in.transcriptNote = L"No transcript yet: the session has no message, so the rows its transcript gives are not shown.";
                        }
                        else
                        {
                            std::wstring error;
                            if (ReadClaudeTranscriptStatusFile(display.path, in.transcript, error))
                            {
                                in.transcriptRead = true;
                                if (!display.forkSourceId.empty())
                                {
                                    in.transcriptNote = L"A fork with no message of its own yet: the rows from its transcript are its source's (" + ShortId(display.forkSourceId) + L").";
                                }
                            }
                            else
                            {
                                in.transcriptNote = L"Its transcript could not be read: " + error;
                            }
                        }
                    }
                    if (usageThread.joinable())
                    {
                        usageThread.join();
                    }
                    else
                    {
                        usage = SharedClaudeStatusProbe().Usage(claudeExe);
                    }
                    in.usage = std::move(usage);
                    in.nowMs = WallMs();
                    const std::vector<std::wstring> messages = BuildClaudeStatusReport(in);
                    for (size_t i = 0; i < messages.size(); ++i)
                    {
                        // The first message answers the /status (its quote shows what was asked); a second one follows it.
                        me->_reply(threadId, messages[i], false, json::Value{}, i == 0 ? replyTo : 0);
                    }
                    me->_log(L"[msg] status report of " + (session ? ShortId(session->id) : std::wstring{ L"the fleet" }) + L": " + std::to_wstring(messages.size()) +
                             L" message(s) in " + std::to_wstring(TickMs() - startedTick) + L" ms; account " +
                             ReadOutcome(in.account.ok, in.account.cached, in.account.readAtMs, in.account.error, in.nowMs) + L", usage " +
                             ReadOutcome(in.usage.ok, in.usage.cached, in.usage.readAtMs, in.usage.error, in.nowMs) +
                             (session ? L", transcript " + (in.transcriptRead ? std::to_wstring(in.transcript.lines) + L" lines (" + std::to_wstring(in.transcript.parsedLines) + L" parsed)" :
                                                                                std::wstring{ L"not read" }) :
                                        std::wstring{}));
                }
                catch (...)
                {
                    // Rule #18: an exception escaping a std::thread would end the process. Recovery: one line in the thread.
                    LogSwallowedException(L"MessagingService::_startClaudeStatusReport (worker)");
                    try
                    {
                        me->_reply(threadId, L"The status could not be read (hooks.log says why).", false);
                    }
                    catch (...)
                    {
                        // std::bad_alloc queuing the line: the report is lost; the log above says why.
                        LogSwallowedException(L"MessagingService::_startClaudeStatusReport (worker reply)");
                    }
                }
            }).detach();
            return L"report";
        }
        catch (...)
        {
            // The worker could not start (std::system_error, std::bad_alloc copying its arguments): nothing reads, so the slot goes
            // back, and the thread is told.
            LogSwallowedException(L"MessagingService::_startClaudeStatusReport");
            if (slotClaimed)
            {
                SharedClaudeStatusProbe().EndReport();
            }
            try
            {
                _reply(threadId, L"The status could not be read (hooks.log says why).", false);
            }
            catch (...)
            {
                // std::bad_alloc again: the log above is all that is left.
                LogSwallowedException(L"MessagingService::_startClaudeStatusReport (reply)");
            }
            return L"failed";
        }
    }
}
