// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionTitler (see SessionTitler.h): the AI titles' hook tap, their job queue and worker, the hidden naming call
// and the guarded rename. Every decision it acts on is AiSessionTitle.h's (pure, harness-tested); this file only carries them
// out: registry reads and writes, the system prompt's file, the child process, the logs.

#include "SessionTitler.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "AiSessionTitle.h"
#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException, AgentmasterStateDir, ShortId
#include "ClaudeStatus.h" // RunClaudeCapture
#include "Engine.h" // SharedEngine().claudeExePath
#include "HookEvents.h" // HookMessage, HookEvent
#include "Persistence.h" // LoadAppSettings, NormDirKey
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8
#include "SessionRegistry.h"

namespace Agentmaster
{
    namespace
    {
        using namespace AiTitle;

        // The most naming jobs that may wait at once. A job takes 3–8 s; a burst of new sessions (a Telegram wizard's launches,
        // a user opening tabs in a row) queues a few. Past this many something is wrong (a model that never answers, a flood of
        // prompts): a new job is refused with a log line rather than growing a queue nobody drains — its session keeps the
        // folder's name, exactly as with the feature off.
        constexpr size_t kAiTitleQueueMax = 32;

        /// <summary>One session waiting for its title: what the naming call needs, copied when the mark was taken.</summary>
        struct TitleJob
        {
            std::wstring sessionId; // the session to rename
            std::wstring from; // the title the launch derived (the mark): the rename happens only while the title still IS this
            std::wstring prompt; // the first real prompt, as the hook carried it (the user prompt cuts it)
            std::wstring folder; // the session's launch folder: the examples' folder and the prompt's "Folder:" line
        };

        /// <summary>
        /// The titler's process-wide state. Reached only through TitlerStateRef(), which LEAKS it on purpose: the worker thread is
        /// detached and may still run when the process exits (an app quit ends the process without the engine's teardown — memory
        /// "app-quit-skips-engine-teardown"), and a static destroyed under it would be touched after its destructor.
        /// </summary>
        struct TitlerState
        {
            std::mutex mtx; // guards everything below
            std::weak_ptr<SessionRegistry> registry; // set by StartAiSessionTitles; weak, so a job never keeps the engine alive
            std::deque<TitleJob> jobs; // waiting, oldest first
            bool workerRunning{ false }; // a worker thread drains `jobs`; it clears this under the lock when it finds none
        };

        /// <summary>The leaked process-wide state (see TitlerState for why it is never destroyed).</summary>
        /// <returns>The state.</returns>
        TitlerState& TitlerStateRef() noexcept
        {
            static TitlerState* const state = new TitlerState{}; // ⚠ never deleted: a detached worker may outlive main
            return *state;
        }

        /// <summary>One line into hooks.log, tagged by the caller ("[ai-title] …").</summary>
        /// <param name="line">The line, without its line break.</param>
        void TitleLog(const std::wstring& line) noexcept
        {
            try
            {
                AppendStateLog(L"hooks.log", line + L"\n");
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE: the logger itself failed; nothing is left to report through (the logging-path rule).
            }
        }

        /// <summary>
        /// The system prompt's file, written when missing or different (a byte compare): claude reads it at every call, and a prompt
        /// an update changed must reach the very next call. One writer at a time (a static mutex) — the titler's single worker is
        /// the only caller today; the lock keeps a second one safe.
        /// </summary>
        /// <param name="profileDir">The active profile.</param>
        /// <returns>The file's full path, or "" when it could not be written (the job then keeps the folder's name).</returns>
        /// <exception cref="std::bad_alloc">If a path or the prompt's bytes cannot be allocated.</exception>
        std::wstring EnsureAiTitleSystemPromptFile(const std::wstring& profileDir)
        {
            static std::mutex m;
            std::lock_guard lk{ m };
            const std::filesystem::path dir = std::filesystem::path{ profileDir } / L"ai-title";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec); // an error shows as the write below failing
            const std::filesystem::path file = dir / L"system.txt";
            const std::string want = Profiles::detail::WideToUtf8(AiTitleSystemPrompt());
            {
                std::ifstream in(file, std::ios::binary);
                const std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (have == want)
                {
                    return file.wstring();
                }
            }
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(want.data(), static_cast<std::streamsize>(want.size()));
            return out ? file.wstring() : std::wstring{};
        }

        /// <summary>
        /// Names ONE session (the worker's body for a job): the examples from the registry, the hidden naming call, the answer
        /// cleaned and kept apart from the open tabs' titles, and the guarded rename — written only while the session still
        /// carries the title it was launched with, so a rename the user made while the model thought always wins. Every failure
        /// keeps the folder's name and says why in hooks.log ("[ai-title] … kept …").
        /// </summary>
        /// <param name="job">The job.</param>
        void NameOneSession(const TitleJob& job) noexcept
        {
            const std::wstring tag = L"[ai-title] " + ShortId(job.sessionId) + L" ";
            try
            {
                std::shared_ptr<SessionRegistry> registry;
                {
                    std::lock_guard lk{ TitlerStateRef().mtx };
                    registry = TitlerStateRef().registry.lock();
                }
                if (!registry)
                {
                    return; // the engine is gone (process teardown): nothing to rename
                }
                // The settings are read fresh: a model chosen in the cog a moment ago applies to the very next title.
                const AppSettings settings = LoadAppSettings();
                const std::wstring model = AiTitleModelOrDefault(settings.aiTitleModel);
                // Copied once: the UI thread writes it only on a Settings save that changes the claude path (the Telegram /status
                // and Search workers read it the same way).
                const std::wstring claudeExe = SharedEngine().claudeExePath;
                if (claudeExe.empty())
                {
                    TitleLog(tag + L"kept \x201C" + job.from + L"\x201D: Claude Code was not found on this PC");
                    return;
                }
                // The examples: every record's name and folders (a lean copy), the folder's open tabs first, closed ones after.
                std::vector<AiTitleSample> samples;
                for (const auto& s : registry->TitleSamples())
                {
                    AiTitleSample a;
                    a.id = s.id;
                    a.title = s.title;
                    a.dirKey = s.workingDir.empty() ? std::wstring{} : NormDirKey(s.workingDir);
                    a.inferredKey = s.inferredWorkingDir.empty() ? std::wstring{} : NormDirKey(s.inferredWorkingDir);
                    a.live = s.live;
                    a.newestMs = s.newestUnixMs;
                    samples.push_back(std::move(a));
                }
                const std::vector<AiTitleExample> examples =
                    SelectAiTitleExamples(samples, job.sessionId, job.folder.empty() ? std::wstring{} : NormDirKey(job.folder), job.from);
                const std::wstring profileDir = AgentmasterStateDir();
                const std::wstring systemFile = EnsureAiTitleSystemPromptFile(profileDir);
                if (systemFile.empty())
                {
                    TitleLog(tag + L"kept \x201C" + job.from + L"\x201D: the system prompt's file could not be written");
                    return;
                }
                const std::wstring args = BuildAiTitleArgs(model, kAiTitleEffort, systemFile, BuildAiTitleUserPrompt(job.folder, examples, job.prompt));
                // The working folder is the profile's ai-title folder: no CLAUDE.md there (and --setting-sources "" loads none
                // anyway), so nothing of the session's repository reaches the model but its folder's path and titles.
                const ClaudeCaptureResult run = RunClaudeCapture(claudeExe, args, (std::filesystem::path{ profileDir } / L"ai-title").wstring(), kAiTitleTimeoutMs);
                if (!run.started)
                {
                    TitleLog(tag + L"kept \x201C" + job.from + L"\x201D: claude could not start: " + run.error);
                    return;
                }
                if (run.timedOut)
                {
                    TitleLog(tag + L"kept \x201C" + job.from + L"\x201D: the naming call did not answer within " + std::to_wstring(kAiTitleTimeoutMs / 1000) + L" s");
                    return;
                }
                const AiTitleOutput out = ParseAiTitleOutput(run.out);
                if (!out.ok)
                {
                    TitleLog(tag + L"kept \x201C" + job.from + L"\x201D: " + out.error + (run.exitCode ? L" (exit code " + std::to_wstring(run.exitCode) + L")" : std::wstring{}));
                    return;
                }
                const std::optional<std::wstring> cleaned = SanitizeAiTitle(out.text, kAiTitleMaxChars);
                if (!cleaned)
                {
                    TitleLog(tag + L"kept \x201C" + job.from + L"\x201D: the answer held no title (" + out.text.substr(0, 120) + L")");
                    return;
                }
                const std::wstring title = DedupeAiTitle(*cleaned, examples);
                if (AiTitlesSameName(title, job.from))
                {
                    TitleLog(tag + L"kept \x201C" + job.from + L"\x201D: the model answered the folder's own name");
                    return;
                }
                // The guarded rename. A lean pre-check first: Update notifies every observer (each window, the bot, the autosave)
                // even when its mutation changes nothing, and a session the user renamed meanwhile needs no notification at all.
                // The same compare runs again INSIDE the update, which is what decides: a rename that lands between the two
                // still wins.
                if (const auto now = registry->GetWithoutQueue(job.sessionId); !now || now->title != job.from)
                {
                    TitleLog(tag + L"kept the title \x201C" + (now ? now->title : std::wstring{ L"(the session is gone)" }) + L"\x201D: renamed while the model named it");
                    return;
                }
                bool applied = false;
                registry->Update(job.sessionId, [&](SessionInfo& s) {
                    if (s.title == job.from)
                    {
                        s.title = title;
                        applied = true;
                    }
                });
                if (!applied)
                {
                    TitleLog(tag + L"kept the title: renamed while the model named it");
                    return;
                }
                size_t openExamples = 0;
                for (const auto& e : examples)
                {
                    openExamples += e.open ? 1 : 0;
                }
                TitleLog(tag + L"\x201C" + job.from + L"\x201D -> \x201C" + title + L"\x201D (" + model + L", " + std::to_wstring(run.tookMs) + L" ms, " +
                         std::to_wstring(openExamples) + L" open and " + std::to_wstring(examples.size() - openExamples) + L" closed example titles)");
            }
            catch (...)
            {
                // Rule #18. Recovery: the session keeps the folder's name (its mark was taken: it is not asked again).
                LogSwallowedException(L"SessionTitler NameOneSession");
            }
        }

        /// <summary>
        /// The worker thread's body: drains the queue one job at a time (one naming call at a time keeps a burst of new sessions
        /// from starting a burst of claude processes), and ends when it finds the queue empty — clearing `workerRunning` under the
        /// same lock the producer checks, so a job queued at that instant either is seen here or starts a new worker.
        /// </summary>
        void TitleWorkerLoop() noexcept
        {
            for (;;)
            {
                TitleJob job;
                {
                    std::lock_guard lk{ TitlerStateRef().mtx };
                    auto& st = TitlerStateRef();
                    if (st.jobs.empty())
                    {
                        st.workerRunning = false;
                        return;
                    }
                    job = std::move(st.jobs.front());
                    st.jobs.pop_front();
                }
                NameOneSession(job); // never throws
            }
        }

        /// <summary>
        /// Queues one naming job and makes sure a worker drains it: the first job of a quiet spell starts a worker thread (detached:
        /// it ends by itself when the queue is empty), a job queued while one runs is picked up by it. A full queue refuses the
        /// job (kAiTitleQueueMax, logged): the session keeps its folder's name.
        /// </summary>
        /// <param name="job">The job.</param>
        void EnqueueTitleJob(TitleJob job) noexcept
        {
            const std::wstring id = job.sessionId;
            bool startWorker = false;
            try
            {
                {
                    std::lock_guard lk{ TitlerStateRef().mtx };
                    auto& st = TitlerStateRef();
                    if (st.jobs.size() >= kAiTitleQueueMax)
                    {
                        // Logged under the lock on purpose: AppendStateLog takes only its own leaf mutex, never this one.
                        TitleLog(L"[ai-title] " + ShortId(id) + L" kept its folder's name: " + std::to_wstring(st.jobs.size()) + L" titles are waiting already");
                        return;
                    }
                    st.jobs.push_back(std::move(job));
                    if (!st.workerRunning)
                    {
                        st.workerRunning = true;
                        startWorker = true;
                    }
                }
                if (startWorker)
                {
                    std::thread([]() noexcept { TitleWorkerLoop(); }).detach();
                }
            }
            catch (...)
            {
                // Rule #18. Recovery: the thread could not start (or the queue could not grow). The flag goes back down, so the next
                // job tries to start a worker again; a job already queued waits for that one.
                LogSwallowedException(L"SessionTitler EnqueueTitleJob");
                if (startWorker)
                {
                    std::lock_guard lk{ TitlerStateRef().mtx };
                    TitlerStateRef().workerRunning = false;
                }
            }
        }
    }

    void StartAiSessionTitles(const std::shared_ptr<SessionRegistry>& registry) noexcept
    {
        try
        {
            std::lock_guard lk{ TitlerStateRef().mtx };
            TitlerStateRef().registry = registry;
        }
        catch (...)
        {
            // Rule #18. Recovery: the titler stays without a registry and names nothing (every session keeps its folder's name).
            LogSwallowedException(L"StartAiSessionTitles");
        }
    }

    void NoteHookForAiSessionTitle(const HookMessage& m) noexcept
    {
        // Every hook of every session passes here: everything but a prompt leaves at the first compare.
        if (m.event != HookEvent::UserPromptSubmit || m.promptText.empty() || m.sessionId.empty())
        {
            return;
        }
        try
        {
            std::shared_ptr<SessionRegistry> registry;
            {
                std::lock_guard lk{ TitlerStateRef().mtx };
                registry = TitlerStateRef().registry.lock();
            }
            if (!registry)
            {
                return;
            }
            // A lean read decides whether the prompt can matter at all (no queue copied): an unmarked session — almost every
            // prompt — leaves here without a registry update, so it notifies nobody.
            const auto before = registry->GetWithoutQueue(m.sessionId);
            if (!before)
            {
                return;
            }
            const AiTitlePromptVerdict first = DecideAiTitleOnPrompt(before->aiTitlePendingFrom, before->title, m.promptText);
            if (first == AiTitlePromptVerdict::NotPending || first == AiTitlePromptVerdict::Wait)
            {
                if (first == AiTitlePromptVerdict::Wait)
                {
                    TitleLog(L"[ai-title] " + ShortId(m.sessionId) + L" waits: its prompt is a command, not a task");
                }
                return;
            }
            // The mark is taken in ONE update that judges the record as it is now (a second prompt racing this one, or a rename,
            // may have moved it since the read above): only the update that finds the mark takes it, so a session is named at
            // most once, and a mark is cleared whether the session is named or was renamed.
            AiTitlePromptVerdict verdict = AiTitlePromptVerdict::NotPending;
            TitleJob job;
            registry->Update(m.sessionId, [&](SessionInfo& s) {
                verdict = DecideAiTitleOnPrompt(s.aiTitlePendingFrom, s.title, m.promptText);
                if (verdict == AiTitlePromptVerdict::Name || verdict == AiTitlePromptVerdict::Renamed)
                {
                    job.sessionId = s.id;
                    job.from = s.aiTitlePendingFrom;
                    job.folder = s.workingDir;
                    s.aiTitlePendingFrom.clear();
                }
            });
            if (verdict == AiTitlePromptVerdict::Renamed)
            {
                TitleLog(L"[ai-title] " + ShortId(m.sessionId) + L" not named: it was renamed before its first prompt (its mark \x201C" + job.from + L"\x201D is dropped)");
                return;
            }
            if (verdict != AiTitlePromptVerdict::Name)
            {
                return; // the race: another prompt took the mark first (or it waits again)
            }
            job.prompt = m.promptText;
            TitleLog(L"[ai-title] " + ShortId(m.sessionId) + L" naming \x201C" + job.from + L"\x201D from its first prompt (" + std::to_wstring(m.promptText.size()) + L" characters)");
            EnqueueTitleJob(std::move(job));
        }
        catch (...)
        {
            // Rule #18. Recovery: this prompt names nothing; a mark not yet taken stays for the next prompt.
            LogSwallowedException(L"NoteHookForAiSessionTitle");
        }
    }
}
