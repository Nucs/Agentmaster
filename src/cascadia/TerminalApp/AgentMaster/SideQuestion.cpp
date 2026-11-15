// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SIDE QUESTIONS, the OS half (SideQuestion.h, MESSAGING.md §21.56): the btw thread files, the headless
// fork, and the router that asks the session's own claude first.
//
// The headless way, and why each flag (Claude Code 2.1.322, measured on this PC 2026-11-15):
//   * `-p --input-format stream-json --output-format stream-json --verbose`: the SDK's mode, the only one with the
//     `side_question` control request — Claude Code's /btw runner itself (its framing, its history as message pairs).
//   * `--resume <id> --fork-session --no-session-persistence`: the session's conversation under a NEW id that writes
//     nothing (no transcript, no line in the session's own file; a second claude resuming the same id would re-stamp the
//     live session's leaf at its exit).
//   * `--settings {"disableAllHooks":true,"env":{CLAUDE_CODE_RESUME_INTERRUPTED_TURN:"0", …_MAX_AGE_MS:"1", <preset>}}`:
//     with the user's resume-interrupted-turn switch on, the first fork of a BUSY session re-ran its unfinished turn and
//     called a tool before it was stopped; hooks would run the user's SessionStart / SessionEnd for a session nobody
//     opened (and Agentmaster's would report it). Neither changes the request, so the cache still serves the conversation.
//   * `--permission-mode dontAsk`: nothing the fork could reach runs without a rule (the side question denies every tool
//     itself; this is the belt).
//   * no `--plugin-dir`: the bundled mods only draw in a terminal, and the link mod must not dial in from a child.
// Measured with these flags on a live 335k-token session: one request (the side question), 334,769 tokens read from the
// cache, 2,590 written, the answer in 6 s, the child gone 2.4 s later.
//
// Plain C++ + Win32, no WinRT (the vcxproj marks it <PrecompiledHeader>NotUsing</PrecompiledHeader>); the m5 harness
// links it.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "SideQuestion.h"

#include <atomic>
#include <chrono>
#include <mutex>

#include "ClaudeSpawn.h" // AgentmasterStateDir, AppendStateLog, LogSwallowedException, ShortId, ResolveClaudeTranscriptPath
#include "ClaudeStatus.h" // RunClaudeStreamJson, QuoteClaudeArgument, ReadClaudeTranscriptStatusFile
#include "Persistence.h" // WriteStateDocument, ForgetStateDocument
#include "SessionLink.h" // SharedSessionLink, the UTF-8 codec
#include "StateFileGuard.h" // ReadStateFileUtf8

namespace Agentmaster
{
    namespace
    {
        /// <summary>The headless children's AM_HARNESS value (the Fleet Observer and the CLI skip them).</summary>
        constexpr wchar_t kSideQuestionHarnessName[] = L"claude-btw";

        /// <summary>The app's preset lookup (SetSideQuestionLaunchProvider); null in every program but the app.</summary>
        std::atomic<SideQuestionLaunchProvider> g_launchProvider{ nullptr };

        /// <summary>Guards every thread file's read-modify-write in this process (writes are rare: one per answer).</summary>
        std::mutex g_threadFileMtx;

        /// <summary>The tick counter in ms (how long a way took).</summary>
        /// <returns>GetTickCount64.</returns>
        int64_t TickMs() noexcept
        {
            return static_cast<int64_t>(::GetTickCount64());
        }

        /// <summary>One [btw] line in hooks.log. Never throws.</summary>
        /// <param name="line">The line, without the tag and the line break.</param>
        void LogBtw(const std::wstring& line) noexcept
        {
            try
            {
                AppendStateLog(L"hooks.log", L"[btw] " + line + L"\n");
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE (CLAUDE.md Rule #18, the logging-path exemption): this IS a log write; reporting its own
                // failure would write to the same log again. Degradation: the line is lost, nothing else.
            }
        }

        /// <summary>The thread file's round trip (PERSISTENCE.md §17): what this build makes of a parsed file.</summary>
        /// <param name="disk">The parsed file.</param>
        /// <returns>Its document as this build writes it.</returns>
        /// <exception cref="std::bad_alloc">If the document cannot be allocated.</exception>
        json::Value SideQuestionThreadRoundTrip(const json::Value& disk)
        {
            return SideQuestionThreadDocument(SideQuestionThreadFromDocument(disk));
        }

        /// <summary>Whether a session id can name a file: 1..64 of [0-9A-Za-z-] (a UUID is; nothing else reaches the profile).</summary>
        /// <param name="id">The id.</param>
        /// <returns>True when it is safe as a file name.</returns>
        bool IsFileSafeId(const std::wstring& id) noexcept
        {
            if (id.empty() || id.size() > 64)
            {
                return false;
            }
            return std::all_of(id.begin(), id.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-'; });
        }

        /// <summary>
        /// The model the session's newest response ASKED for (its `requestedModel`, which keeps a "[1m]" suffix and follows a
        /// /model switch), else the model the API named: what the headless fork must run, or the cache entry is the wrong one
        /// (and a 1M-context conversation would not fit a 200k model at all).
        /// </summary>
        /// <param name="sessionId">The conversation whose transcript to read.</param>
        /// <returns>The model id, or "" when the transcript names none.</returns>
        std::wstring TranscriptModel(const std::wstring& sessionId) noexcept
        {
            try
            {
                const std::wstring path = ResolveClaudeTranscriptPath(sessionId);
                if (path.empty())
                {
                    return {};
                }
                ClaudeTranscriptStatus status;
                std::wstring error;
                if (!ReadClaudeTranscriptStatusFile(path, status, error))
                {
                    LogBtw(L"the transcript of " + ShortId(sessionId) + L" could not be read for its model: " + error);
                    return {};
                }
                return status.requestedModel.empty() ? status.responseModel : status.requestedModel;
            }
            catch (...)
            {
                LogSwallowedException(L"SideQuestion TranscriptModel");
                return {};
            }
        }

        /// <summary>
        /// The headless way: a hidden headless fork of the session's transcript asked Claude Code's own side_question (the file
        /// header says why each flag).
        /// </summary>
        /// <param name="r">The request.</param>
        /// <param name="answer">Receives the answer or the error (via Headless).</param>
        void AskHeadless(const SideQuestionRequest& r, SideQuestionAnswer& answer) noexcept
        {
            answer.via = SideQuestionVia::Headless;
            const int64_t started = TickMs();
            try
            {
                if (r.claudeExe.empty())
                {
                    answer.error = L"Claude Code was not found on this PC";
                    return;
                }
                const std::wstring resumeId = r.resumeSessionId.empty() ? r.sessionId : r.resumeSessionId;
                // The session's preset (provider, model, key): the fork must reach the same endpoint and model as the session.
                SideQuestionLaunchExtras extras;
                if (const auto provider = g_launchProvider.load())
                {
                    extras = provider(r.sessionId, r.workDir);
                }
                if (!extras.error.empty())
                {
                    answer.error = L"the session's preset cannot launch: " + extras.error;
                    return;
                }
                std::wstring model = TranscriptModel(resumeId);
                if (model.empty())
                {
                    model = extras.model;
                }
                std::wstring args = L"-p --input-format stream-json --output-format stream-json --verbose";
                args += L" --resume " + QuoteClaudeArgument(resumeId) + L" --fork-session --no-session-persistence --permission-mode dontAsk";
                if (!model.empty())
                {
                    args += L" --model " + QuoteClaudeArgument(model);
                }
                if (!extras.effort.empty())
                {
                    args += L" --effort " + QuoteClaudeArgument(extras.effort);
                }
                if (!extras.appendSystemPrompt.empty())
                {
                    args += L" --append-system-prompt " + QuoteClaudeArgument(extras.appendSystemPrompt);
                }
                if (!extras.extraArgs.empty())
                {
                    args += L" " + extras.extraArgs;
                }
                args += L" --settings " + QuoteClaudeArgument(BuildSideQuestionSettingsJson(extras.settingsEnv));

                ClaudeStreamRun run;
                run.exe = r.claudeExe;
                run.args = std::move(args);
                run.workDir = r.workDir;
                run.harnessName = kSideQuestionHarnessName;
                for (const auto& [k, v] : extras.processEnv)
                {
                    run.envChanges.emplace_back(Link::detail::WideToUtf8(k), Link::detail::WideToUtf8(v));
                    if (!extras.authTokenEnv.empty() && k == extras.authTokenEnv)
                    {
                        // A preset's key reaches claude only as ANTHROPIC_AUTH_TOKEN (the pwsh host moves it for a tab; here
                        // the child is claude itself). Never logged.
                        run.envChanges.emplace_back("ANTHROPIC_AUTH_TOKEN", Link::detail::WideToUtf8(v));
                    }
                }
                run.stdinUtf8 = Link::detail::WideToUtf8(BuildSideQuestionInputLines(r.thread, r.question));
                run.timeoutMs = r.headlessTimeoutMs;
                SideQuestionLine got;
                run.onLine = [&got, &r](std::string_view line) noexcept -> bool {
                    try
                    {
                        // Only the side question's lines matter; the initialize answer (one long line of every command)
                        // and the hook lines are read as Other and passed by.
                        SideQuestionLine read = ReadSideQuestionLine(Link::detail::Utf8ToWide(line));
                        if (read.kind == SideQuestionLineKind::Progress && read.status == L"api_retry")
                        {
                            LogBtw(L"headless fork of " + ShortId(r.sessionId) + L": API retry " + std::to_wstring(read.retryAttempt) +
                                   (read.errorStatus ? L" (HTTP " + std::to_wstring(read.errorStatus) + L")" : std::wstring{}));
                        }
                        if (read.kind == SideQuestionLineKind::Answer || read.kind == SideQuestionLineKind::Error)
                        {
                            got = std::move(read);
                            return true;
                        }
                        return false;
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"SideQuestion AskHeadless (a line)");
                        return false;
                    }
                };
                const ClaudeStreamResult result = RunClaudeStreamJson(run);
                answer.tookMs = TickMs() - started;
                if (got.kind == SideQuestionLineKind::Answer)
                {
                    if (got.hasResponse && !got.response.empty())
                    {
                        answer.ok = true;
                        answer.text = std::move(got.response);
                        answer.synthetic = got.synthetic;
                        answer.fallbackNotice = std::move(got.fallbackNotice);
                    }
                    else
                    {
                        answer.error = L"Claude Code gave no answer (the side question was cut off or refused)";
                    }
                }
                else if (got.kind == SideQuestionLineKind::Error)
                {
                    answer.error = L"Claude Code refused the side question: " + got.error;
                }
                else if (!result.started)
                {
                    answer.error = L"claude could not start: " + result.error;
                }
                else if (result.timedOut)
                {
                    answer.error = L"no answer within " + std::to_wstring(r.headlessTimeoutMs / 60000) + L" min";
                }
                else
                {
                    // It ended before it answered: its last words on standard error name the cause (an unknown flag, no login).
                    std::wstring said = result.errTail;
                    while (!said.empty() && (said.back() == L'\n' || said.back() == L'\r' || said.back() == L' '))
                    {
                        said.pop_back();
                    }
                    const size_t lastLine = said.find_last_of(L'\n');
                    said = lastLine == std::wstring::npos ? said : said.substr(lastLine + 1);
                    if (said.size() > 200)
                    {
                        said = said.substr(0, 200) + L"\x2026";
                    }
                    answer.error = L"claude ended without an answer (exit " + std::to_wstring(result.exitCode) + L")" + (said.empty() ? std::wstring{} : L": " + said);
                }
                LogBtw(L"headless fork of " + ShortId(r.sessionId) + (resumeId != r.sessionId ? L" (its source " + ShortId(resumeId) + L")" : std::wstring{}) + L": " +
                       (answer.ok ? std::to_wstring(answer.text.size()) + L" chars" + (answer.synthetic ? L" (synthetic)" : L"") : L"no answer, " + answer.error) + L" in " +
                       std::to_wstring(answer.tookMs) + L" ms; model " + (model.empty() ? std::wstring{ L"(the session's default)" } : model) +
                       (extras.hasPreset ? L", preset" : L"") + L"; child " + (result.exitedByItself ? L"ended by itself" : L"ended by us"));
            }
            catch (...)
            {
                LogSwallowedException(L"SideQuestion AskHeadless");
                answer.ok = false;
                answer.error = L"the headless fork failed (hooks.log says why)";
                answer.tookMs = TickMs() - started;
            }
        }
    }

    void SetSideQuestionLaunchProvider(SideQuestionLaunchProvider provider) noexcept
    {
        g_launchProvider.store(provider);
    }

    std::wstring SideQuestionThreadFolder()
    {
        return AgentmasterStateDir() + L"\\btw";
    }

    std::wstring SideQuestionThreadPath(const std::wstring& sessionId)
    {
        return IsFileSafeId(sessionId) ? SideQuestionThreadFolder() + L"\\" + sessionId + L".json" : std::wstring{};
    }

    SideQuestionThread LoadSideQuestionThread(const std::wstring& sessionId, bool& readable) noexcept
    {
        readable = true;
        SideQuestionThread thread;
        try
        {
            thread.sessionId = sessionId;
            const std::wstring path = SideQuestionThreadPath(sessionId);
            if (path.empty())
            {
                return thread;
            }
            const StateFileRead read = ReadStateFileUtf8(path);
            if (read.status == StateFileStatus::Missing)
            {
                return thread;
            }
            if (read.status == StateFileStatus::Unreadable)
            {
                readable = false;
                LogBtw(L"the btw thread of " + ShortId(sessionId) + L" could not be read (error " + std::to_wstring(read.error) + L"): asked without it, and not written over");
                return thread;
            }
            const auto doc = json::Parse(read.text);
            if (!doc)
            {
                // Text that does not parse: asked without it, and the next save keeps a copy first (the state-file guard).
                LogBtw(L"the btw thread of " + ShortId(sessionId) + L" does not parse: asked without it");
                return thread;
            }
            thread = SideQuestionThreadFromDocument(*doc);
            thread.sessionId = sessionId;
        }
        catch (...)
        {
            LogSwallowedException(L"LoadSideQuestionThread");
            readable = false;
            thread.exchanges.clear();
        }
        return thread;
    }

    bool SaveSideQuestionExchange(const std::wstring& sessionId, const SideQuestionExchange& exchange) noexcept
    {
        try
        {
            const std::wstring path = SideQuestionThreadPath(sessionId);
            if (path.empty())
            {
                return false;
            }
            std::lock_guard lk{ g_threadFileMtx };
            // The freshest file, read now (Rule #16): never a thread held in memory since the question was asked.
            bool readable = true;
            SideQuestionThread thread = LoadSideQuestionThread(sessionId, readable);
            if (!readable)
            {
                LogBtw(L"the answer was not added to " + ShortId(sessionId) + L"'s btw thread: its file could not be read");
                return false;
            }
            AppendSideQuestionExchange(thread, exchange);
            ::CreateDirectoryW(SideQuestionThreadFolder().c_str(), nullptr); // ERROR_ALREADY_EXISTS is the normal case
            return WriteStateDocument(path, SideQuestionThreadDocument(thread), &SideQuestionThreadRoundTrip, false);
        }
        catch (...)
        {
            LogSwallowedException(L"SaveSideQuestionExchange");
            return false;
        }
    }

    int ClearSideQuestionThread(const std::wstring& sessionId) noexcept
    {
        try
        {
            const std::wstring path = SideQuestionThreadPath(sessionId);
            if (path.empty())
            {
                return 0;
            }
            std::lock_guard lk{ g_threadFileMtx };
            bool readable = true;
            const SideQuestionThread thread = LoadSideQuestionThread(sessionId, readable);
            if (!readable)
            {
                return -1;
            }
            if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                return 0;
            }
            if (!::DeleteFileW(path.c_str()))
            {
                LogBtw(L"the btw thread of " + ShortId(sessionId) + L" could not be deleted (error " + std::to_wstring(::GetLastError()) + L")");
                return -1;
            }
            ForgetStateDocument(path); // its residue cache and shadow go with it
            return static_cast<int>(thread.exchanges.size());
        }
        catch (...)
        {
            LogSwallowedException(L"ClearSideQuestionThread");
            return -1;
        }
    }

    SideQuestionAnswer AskSideQuestion(const SideQuestionRequest& r) noexcept
    {
        SideQuestionAnswer answer;
        const int64_t started = TickMs();
        try
        {
            bool fallBack = true;
            if (r.tryLink)
            {
                auto& link = Link::SharedSessionLink();
                if (link.HasLiveLink(r.sessionId))
                {
                    auto command = json::Value::MkObj();
                    command.Set(L"kind", json::Value::MkStr(L"fork"));
                    command.Set(L"prompt", json::Value::MkStr(BuildSideQuestionForkPrompt(r.thread, r.question)));
                    const Link::LinkRunResult run = link.Run(r.sessionId, std::move(command), r.linkTimeoutMs);
                    switch (run.outcome)
                    {
                    case Link::LinkRunOutcome::Answered:
                        ReadSideQuestionForkResult(run.result, answer, fallBack);
                        if (!fallBack && answer.ok)
                        {
                            answer.linkSkipped.clear();
                        }
                        break;
                    case Link::LinkRunOutcome::TimedOut:
                        // The fork may still run in the session: a headless one now would pay for the same answer twice.
                        fallBack = false;
                        answer.via = SideQuestionVia::Link;
                        answer.error = L"the session's claude did not answer within " + std::to_wstring(r.linkTimeoutMs / 60000) + L" min";
                        break;
                    default:
                        answer.linkSkipped = std::wstring{ L"the link " } + (run.outcome == Link::LinkRunOutcome::Lost ? L"was lost" : L"was not there") + L" when the question went";
                        break;
                    }
                    LogBtw(L"in-session fork of " + ShortId(r.sessionId) + L": " + Link::LinkRunOutcomeWord(run.outcome) + L" in " + std::to_wstring(run.tookMs) + L" ms" +
                           (answer.ok ? L", " + std::to_wstring(answer.text.size()) + L" chars" : std::wstring{}) +
                           (answer.usageKnown ? L"; cache read " + std::to_wstring(answer.cacheReadTokens) + L", cache write " + std::to_wstring(answer.cacheWriteTokens) +
                                                    L", input " + std::to_wstring(answer.inputTokens) + L", output " + std::to_wstring(answer.outputTokens) +
                                                    (answer.effort.empty() ? std::wstring{} : L", effort " + answer.effort) :
                                                std::wstring{}) +
                           (!answer.linkSkipped.empty() ? L"; " + answer.linkSkipped : std::wstring{}) + (!answer.ok && !answer.error.empty() ? L"; " + answer.error : std::wstring{}));
                }
                else
                {
                    answer.linkSkipped = L"no live link (a parked tab, a claude started before the link, or the mod turned off)";
                }
            }
            if (fallBack && !answer.ok && r.allowHeadless)
            {
                const std::wstring skipped = answer.linkSkipped;
                SideQuestionAnswer headless;
                AskHeadless(r, headless);
                headless.linkSkipped = skipped;
                answer = std::move(headless);
            }
        }
        catch (...)
        {
            LogSwallowedException(L"AskSideQuestion");
            answer.ok = false;
            if (answer.error.empty())
            {
                answer.error = L"it failed (hooks.log says why)";
            }
        }
        answer.tookMs = TickMs() - started;
        return answer;
    }
}
