// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging, /btw (see Messaging.h, MESSAGING.md §21.56): a SIDE QUESTION about a session, asked in its
// Telegram topic and answered there while the session keeps working. The service's seventh TU beside Messaging.cpp,
// Messaging.Inbound.cpp, Messaging.Questions.cpp, Messaging.Menus.cpp, Messaging.Files.cpp and Messaging.Status.cpp — one
// class, several files.
//
// The user's request (2026-11-15): /btw <question>, or a message that starts with "btw" / "by the way", answered as a reply
// that quotes it; "/btw clear" and "/clear btw" start the btw conversation over; and it must not waste tokens — the fork
// must read the conversation from the prompt cache.
//
// How it is answered (SideQuestion.h): in the session's own claude when its agentmaster-link mod is linked — Claude Code's
// `$.model.fork`, the snapshot its own /btw forks — else by a headless fork running Claude Code's own side_question. Nothing
// is typed into the session and nothing is written to its transcript. The btw thread (the earlier exchanges each question
// carries) is Agentmaster's: <profile>\btw\<session id>.json.

#include "Messaging.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "ClaudeSpawn.h" // LogNav, ShortId, LogSwallowedException, ResolveClaudeTranscriptPath
#include "MessagingBtw.h"
#include "SessionRegistry.h"
#include "SideQuestion.h"

namespace Agentmaster::Msg
{
    namespace
    {
        /// <summary>The wall clock in Unix ms.</summary>
        /// <returns>Milliseconds since 1970.</returns>
        int64_t WallMs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /// <summary>The longest answer posted (characters of Markdown): a runaway answer is cut with a note, never a flood.</summary>
        constexpr size_t kBtwMaxAnswerChars = 30000;
    }

    bool MessagingService::_takeBtwMessage(const std::wstring& sessionId, const json::Value& msg, int64_t threadId, bool held)
    {
        const BtwMessage m = ParseBtwMessage(msg.StrAt(L"text"), !held);
        if (m.verb == BtwVerb::None)
        {
            return false;
        }
        const int64_t msgId = msg.I64At(L"message_id", 0);
        switch (m.verb)
        {
        case BtwVerb::Usage:
        {
            bool readable = true;
            const SideQuestionThread thread = LoadSideQuestionThread(sessionId, readable);
            _reply(threadId, BtwUsageHtml(thread.exchanges.size()), false, json::Value{}, msgId);
            LogNav(L"remote telegram /btw " + ShortId(sessionId) + L" -> usage (" + std::to_wstring(thread.exchanges.size()) + L" in the thread)");
            return true;
        }
        case BtwVerb::Clear:
        {
            const int forgotten = ClearSideQuestionThread(sessionId);
            _reply(threadId, BtwClearedHtml(forgotten), false, json::Value{}, msgId);
            _log(L"[msg] " + ShortId(sessionId) + L" btw thread cleared from topic (" + (forgotten < 0 ? std::wstring{ L"failed" } : std::to_wstring(forgotten) + L" exchange(s)") + L")");
            LogNav(L"remote telegram /btw clear " + ShortId(sessionId) + L" -> " + (forgotten < 0 ? std::wstring{ L"failed" } : L"cleared " + std::to_wstring(forgotten)));
            return true;
        }
        default:
            _askBtw(sessionId, m.question, msgId, threadId, false);
            return true;
        }
    }

    void MessagingService::_askBtw(const std::wstring& sessionId, const std::wstring& question, int64_t messageId, int64_t threadId, bool confirmed)
    {
        const auto s = _registry->Get(sessionId);
        const auto nav = [&](const std::wstring& outcome) {
            LogNav(L"remote telegram /btw " + ShortId(sessionId) + L" chars=" + std::to_wstring(question.size()) + L" -> " + outcome);
        };
        if (!s)
        {
            _reply(threadId, L"Unknown session.", false, json::Value{}, messageId);
            nav(L"refused:unknown");
            return;
        }
        if (s->kind != AgentKind::Claude)
        {
            _reply(threadId, std::wstring{ kBtwNotClaudeHtml }, false, json::Value{}, messageId);
            nav(L"refused:not-claude");
            return;
        }
        if (!confirmed && !SideQuestionCacheLikelyWarm(*s, WallMs()))
        {
            // The cache most likely lapsed: the fork would write the whole conversation again. Ask first (the user's "so we don't
            // waste tokens"); the payload carries the message id and the question to the button.
            const int64_t last = (std::max)(s->convApiActivityUnixMs, s->lastTurnUnixMs);
            const std::wstring ask = _tokenFor(L"btw-ask", sessionId, std::to_wstring(messageId) + L"|" + question, kTokenTtlMsFor(2));
            const std::wstring cancel = _tokenFor(L"cancel", sessionId, L"", kTokenTtlMsFor(2));
            _reply(threadId, BtwColdCacheQuestionHtml(WallMs() - last, s->contextTokens), false,
                   _buttons({ { { L"\x2601\xFE0F Ask anyway", ask }, { L"Cancel", cancel } } }), messageId);
            nav(L"asked:cold-cache");
            return;
        }
        BtwJob job;
        job.sessionId = sessionId;
        job.question = question;
        job.messageId = messageId;
        job.threadId = threadId;
        job.askedAtMs = WallMs();
        _startBtw(std::move(job));
        nav(confirmed ? L"queued (asked anyway)" : L"queued");
    }

    void MessagingService::_startBtw(BtwJob job)
    {
        _react(job.messageId, kBtwThinkingReaction);
        {
            std::lock_guard lk{ _btwMtx };
            _btwQueue.push_back(std::move(job));
        }
        _pumpBtw();
    }

    void MessagingService::_pumpBtw() noexcept
    {
        try
        {
            // The engine's claude.exe, read here (the hook is the service's; the worker never touches the service's fields but
            // through `me`).
            const std::wstring claudeExe = _hooks.claudeExe ? _hooks.claudeExe() : std::wstring{};
            for (;;)
            {
                BtwJob job;
                {
                    std::lock_guard lk{ _btwMtx };
                    if (_btwRunning.size() >= kBtwMaxRunning)
                    {
                        return;
                    }
                    // The oldest question whose session runs none now (one per session keeps its thread in order).
                    const auto it = std::find_if(_btwQueue.begin(), _btwQueue.end(), [&](const BtwJob& j) { return _btwRunning.count(j.sessionId) == 0; });
                    if (it == _btwQueue.end())
                    {
                        return;
                    }
                    job = std::move(*it);
                    _btwQueue.erase(it);
                    _btwRunning.insert(job.sessionId);
                }
                try
                {
                    // §21.28: a question asked from a button (Ask anyway) keeps the button busy while the answer is read.
                    std::thread([self = weak_from_this(), job, claudeExe, taps = _holdTaps()]() noexcept {
                        const auto me = self.lock();
                        if (!me)
                        {
                            return; // the service is gone (an app quit): nothing to answer to
                        }
                        const TapScope tapScope{ taps };
                        me->_runBtw(job, claudeExe);
                        {
                            std::lock_guard lk{ me->_btwMtx };
                            me->_btwRunning.erase(job.sessionId);
                        }
                        me->_pumpBtw();
                    }).detach();
                }
                catch (...)
                {
                    // The worker could not start (std::system_error): the question is answered with the failure, its slot freed.
                    LogSwallowedException(L"MessagingService::_pumpBtw (worker)");
                    {
                        std::lock_guard lk{ _btwMtx };
                        _btwRunning.erase(job.sessionId);
                    }
                    _react(job.messageId, L"");
                    _reply(job.threadId, BtwFailedHtml(L"its worker could not start (hooks.log says why)"), false, json::Value{}, job.messageId);
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the questions still queued wait for the next pump (the next question asked, or another ending).
            LogSwallowedException(L"MessagingService::_pumpBtw");
        }
    }

    void MessagingService::_runBtw(const BtwJob& job, const std::wstring& claudeExe) noexcept
    {
        try
        {
            const auto s = _registry->Get(job.sessionId);
            if (!s)
            {
                _react(job.messageId, L"");
                _reply(job.threadId, BtwFailedHtml(L"the session is gone"), false, json::Value{}, job.messageId);
                return;
            }
            // The transcript to fork: the session's own, else — a fork that has no message of its own yet — its source's (the
            // conversation is the same; ResolveClaudeDisplayTranscript's rule).
            std::wstring resumeId = job.sessionId;
            if (ResolveClaudeTranscriptPath(job.sessionId).empty())
            {
                if (!s->forkParentId.empty() && !ResolveClaudeTranscriptPath(s->forkParentId).empty())
                {
                    resumeId = s->forkParentId;
                }
                else
                {
                    _react(job.messageId, L"");
                    _reply(job.threadId, std::wstring{ kBtwNoConversationHtml }, false, json::Value{}, job.messageId);
                    LogNav(L"remote telegram /btw " + ShortId(job.sessionId) + L" -> refused:no-conversation");
                    return;
                }
            }
            bool readable = true;
            SideQuestionRequest req;
            req.sessionId = job.sessionId;
            req.resumeSessionId = resumeId == job.sessionId ? std::wstring{} : resumeId;
            req.workDir = s->workingDir.empty() ? s->liveCwd : s->workingDir;
            req.claudeExe = claudeExe;
            req.question = job.question;
            req.thread = LoadSideQuestionThread(job.sessionId, readable);
            const SideQuestionAnswer answer = AskSideQuestion(req);
            const int64_t answeredAtMs = WallMs();
            _react(job.messageId, L"");
            if (!answer.ok)
            {
                _reply(job.threadId, BtwFailedHtml(answer.error), false, json::Value{}, job.messageId);
                _log(L"[msg] " + ShortId(job.sessionId) + L" btw from topic (message " + std::to_wstring(job.messageId) + L"): no answer via " +
                     SideQuestionViaWord(answer.via) + L" in " + std::to_wstring(answer.tookMs) + L" ms: " + answer.error);
                LogNav(L"remote telegram /btw " + ShortId(job.sessionId) + L" -> failed");
                return;
            }
            // The thread keeps real answers only (Claude Code's own rule: a synthetic placeholder is shown, never sent back).
            const size_t number = req.thread.exchanges.size() + 1;
            if (!answer.synthetic)
            {
                SideQuestionExchange x;
                x.question = job.question;
                x.response = answer.text;
                x.fallbackNotice = answer.fallbackNotice;
                x.askedAtMs = job.askedAtMs;
                x.answeredAtMs = answeredAtMs;
                x.via = answer.via;
                x.cacheReadTokens = answer.usageKnown ? answer.cacheReadTokens : -1;
                x.cacheWriteTokens = answer.usageKnown ? answer.cacheWriteTokens : -1;
                if (!readable || !SaveSideQuestionExchange(job.sessionId, x))
                {
                    _log(L"[msg] " + ShortId(job.sessionId) + L" btw answer not kept in the btw thread (its file could not be read or written)");
                }
            }
            // The reply: the header, the title row (§21.54: line 2 of a header-led message), the answer, the footer.
            std::wstring body = answer.text;
            if (body.size() > kBtwMaxAnswerChars)
            {
                body = body.substr(0, kBtwMaxAnswerChars) + L"\n\n\x2026 (cut: the answer was " + std::to_wstring(answer.text.size()) + L" characters)";
            }
            if (!answer.fallbackNotice.empty())
            {
                body = L"\x26A0\xFE0F " + answer.fallbackNotice + L"\n\n" + body;
            }
            const std::wstring footer = BtwFooterText(answer.via == SideQuestionVia::Link, answer.usageKnown, answer.cacheReadTokens, answer.cacheWriteTokens,
                                                      answer.inputTokens, answer.tookMs, answer.via == SideQuestionVia::Link ? std::wstring{} : answer.linkSkipped);
            std::wstring markdown = BtwHeaderMarkdown(answeredAtMs, number) + L"\n\n" + body + L"\n\n_" + TgMdEscapeText(footer) +
                                    (answer.synthetic ? L" \x00B7 Claude Code's placeholder, not kept in the btw thread" : L"") + L"_";
            markdown = MarkdownTitleRowAfterHeader(markdown);
            std::wstring html = BtwHeaderHtml(answeredAtMs, number) + L"\n" + TgMarkdownToHtml(body) + L"\n\n<i>" + TgEscapeHtml(footer) + L"</i>";
            html = WithTopicTitleRowAfterHeader(html, _titleRowToken(job.threadId));
            _sendRich(job.threadId, markdown, html, false, SendPriority::Final, nullptr, job.messageId);
            _log(L"[msg] " + ShortId(job.sessionId) + L" btw from topic (message " + std::to_wstring(job.messageId) + L") answered via " + SideQuestionViaWord(answer.via) +
                 L" in " + std::to_wstring(answer.tookMs) + L" ms (" + std::to_wstring(answer.text.size()) + L" chars, #" + std::to_wstring(number) +
                 (answer.usageKnown ? L"; cache read " + std::to_wstring(answer.cacheReadTokens) + L", written " + std::to_wstring(answer.cacheWriteTokens) : std::wstring{}) +
                 (answer.via == SideQuestionVia::Headless && !answer.linkSkipped.empty() ? L"; not in the session: " + answer.linkSkipped : std::wstring{}) + L")");
            LogNav(L"remote telegram /btw " + ShortId(job.sessionId) + L" -> answered:" + SideQuestionViaWord(answer.via));
        }
        catch (...)
        {
            // Rule #18: an exception escaping the worker's thread would end the process. Recovery: one line in the topic.
            LogSwallowedException(L"MessagingService::_runBtw");
            try
            {
                _react(job.messageId, L"");
                _reply(job.threadId, BtwFailedHtml(L"it failed (hooks.log says why)"), false, json::Value{}, job.messageId);
            }
            catch (...)
            {
                // std::bad_alloc queuing the line: the answer is lost; the log above says why.
                LogSwallowedException(L"MessagingService::_runBtw (reply)");
            }
        }
    }
}
