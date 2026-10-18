// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — FolderProbe: "is this path an existing folder?" asked with a BOUND on the wait (header-only, plain Win32 +
// the standard library; MESSAGING.md §21.17.4).
//
// WHY: the Telegram bot checks the folders a user picks — a new thread's picker buttons and lists, a path typed into a new
// thread, the 🔎 Search topic's /newsession <folder> — on its POLL thread, the one thread that reads Telegram.
// GetFileAttributesW on an offline network share (\\nas\share\…, or a drive letter mapped to one) waits for the network's
// timeout, tens of seconds, and the bot read nothing meanwhile: every topic, the Search topic and every button froze. A
// Win32 file call cannot be cancelled, so the probe runs on a thread of its own and the caller waits only as long as it
// chooses; a probe that did not answer in time goes on in the background and leaves its answer for the next ask.
//
// THE RULES (FolderProber::Probe):
//   * One probe per path at a time: an ask while a probe of that path is in flight waits on THAT probe, so an offline share
//     never collects a thread per tap.
//   * At most `maxInFlight` probes at once (kFolderProbeMaxInFlight): past it an ask answers NoAnswer at once and starts
//     nothing, which bounds the threads an unreachable server can hold.
//   * An answer is kept: an ask that accepts an answer up to `maxAgeMs` old takes it without a probe (the picker's lists, with
//     dozens of folders); maxAgeMs 0 always probes again (a pick, which must see the folder as it is now).
//   * Every probe thread holds the prober's STATE by a shared pointer, so the prober may go away while a probe still waits on
//     the network; the thread ends when the OS call returns.

#pragma once

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Agentmaster
{
    // Rule #18's reporter, defined in ClaudeSpawn.cpp (declared in ClaudeSpawn.h). Declared here instead of included so this
    // small header does not pull the spawn header into every user; every binary that compiles a user links ClaudeSpawn.cpp.
    void LogSwallowedException(const wchar_t* context) noexcept;

    /// <summary>What a bounded folder probe found.</summary>
    enum class FolderProbeAnswer
    {
        Folder, // the path is an existing directory
        NotFolder, // the path does not exist, is a file, or cannot be read (an access refusal reads as not a folder, as GetFileAttributesW has it)
        NoAnswer, // the probe did not answer within the wait (it goes on in the background), it failed, or no probe could be started
    };

    /// <summary>
    /// The most probes one prober runs at once. A probe of an unreachable share holds its thread for the network's timeout, so
    /// the cap bounds those threads; an ask past it answers NoAnswer without starting a probe.
    /// </summary>
    inline constexpr size_t kFolderProbeMaxInFlight = 16;

    /// <summary>
    /// The real probe: whether `path` is an existing directory, by GetFileAttributesW. It BLOCKS for as long as the file system
    /// takes — on an offline network share, the network's timeout — so call it through a FolderProber on any thread that must
    /// stay responsive.
    /// </summary>
    /// <param name="path">The folder path, as the user or the registry has it.</param>
    /// <returns>True when the path exists and is a directory; false otherwise (missing, a file, unreadable).</returns>
    inline bool IsExistingFolderNow(const std::wstring& path) noexcept
    {
        const DWORD attrs = ::GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    /// <summary>
    /// Asks "is this an existing folder?" with a bound on the wait (the header's rules): each probe runs on a detached thread of
    /// its own, the caller waits at most the time it passes, a probe still running is shared by every ask for its path, the
    /// answers are kept for asks that accept an older one, and at most a fixed number of probes run at once. Thread-safe: any
    /// thread may ask at any time. Copyable cheaply (a copy shares the state, the answers and the running probes).
    /// </summary>
    class FolderProber
    {
    public:
        /// <summary>The probe a prober runs: true when the path is an existing directory. It may block; it should not throw.</summary>
        using ProbeFn = std::function<bool(const std::wstring& path)>;

        /// <summary>A prober over `probe` (the real GetFileAttributesW probe by default; tests pass one that blocks on cue).</summary>
        /// <param name="probe">The probe to run; an empty function means IsExistingFolderNow.</param>
        /// <param name="maxInFlight">The most probes at once (kFolderProbeMaxInFlight by default; 0 reads as 1).</param>
        /// <exception cref="std::bad_alloc">If the shared state cannot be allocated.</exception>
        explicit FolderProber(ProbeFn probe = ProbeFn{}, size_t maxInFlight = kFolderProbeMaxInFlight) :
            _state{ std::make_shared<State>() }
        {
            _state->probe = probe ? std::move(probe) : ProbeFn{ &IsExistingFolderNow };
            _state->maxInFlight = maxInFlight == 0 ? 1 : maxInFlight;
        }

        /// <summary>
        /// Whether `path` is an existing folder, waiting at most `waitMs` for the answer. An answer at most `maxAgeMs` old is
        /// taken as it is; a probe of `path` that is already running is waited on (never a second one); else a probe starts on a
        /// thread of its own, unless `maxInFlight` probes are running already. A probe that outlasts the wait keeps running and
        /// records its answer for the next ask.
        /// </summary>
        /// <param name="path">The folder path. An empty path is NotFolder at once (nothing is asked of the OS).</param>
        /// <param name="waitMs">How long the caller waits for a running or new probe: 0 never waits (a running probe reads
        /// NoAnswer, a new one is only started).</param>
        /// <param name="maxAgeMs">The oldest kept answer the caller accepts: 0 accepts none (the folder is asked again, a running
        /// probe of it still shared), a list that shows dozens of folders passes a minute.</param>
        /// <returns>Folder or NotFolder from a probe that answered (now or within maxAgeMs); NoAnswer when none answered within the
        /// wait, the probe failed, the cap was reached, or a thread could not be started.</returns>
        /// <remarks>Never throws: an internal failure is logged (Rule #18) and reads NoAnswer, the answer that tells the caller
        /// "not known" without claiming the folder is gone.</remarks>
        FolderProbeAnswer Probe(const std::wstring& path, uint32_t waitMs, uint32_t maxAgeMs = 0) noexcept
        {
            if (path.empty())
            {
                return FolderProbeAnswer::NotFolder; // nothing to ask the OS (GetFileAttributesW(L"") fails anyway)
            }
            try
            {
                const std::shared_ptr<State> st = _state;
                std::unique_lock lk{ st->m };
                std::shared_ptr<Entry> entry;
                if (const auto it = st->byPath.find(path); it != st->byPath.end())
                {
                    const std::shared_ptr<Entry>& known = it->second;
                    if (!known->done)
                    {
                        entry = known; // running: wait on THAT probe — never a second thread for one path
                    }
                    else if (!known->failed && maxAgeMs > 0 && ::GetTickCount64() - known->answeredTick <= maxAgeMs)
                    {
                        return known->isFolder ? FolderProbeAnswer::Folder : FolderProbeAnswer::NotFolder;
                    }
                }
                if (!entry)
                {
                    if (st->inFlight >= st->maxInFlight)
                    {
                        return FolderProbeAnswer::NoAnswer; // the cap: unreachable servers hold enough threads already
                    }
                    entry = std::make_shared<Entry>();
                    // Recorded and counted BEFORE the thread starts: the thread's end takes this lock to record the answer and
                    // uncount itself, so it can only run after this scope released the lock (the wait below releases it).
                    st->byPath[path] = entry;
                    ++st->inFlight;
                    try
                    {
                        std::thread([st, entry, path]() noexcept { RunProbe(st, entry, path); }).detach();
                    }
                    catch (...)
                    {
                        // Rule #18. Recovery: undo the record and the count (no thread will), log after the lock, read NoAnswer.
                        --st->inFlight;
                        st->byPath.erase(path);
                        lk.unlock();
                        LogSwallowedException(L"FolderProber::Probe (start a probe thread)");
                        return FolderProbeAnswer::NoAnswer;
                    }
                }
                if (waitMs > 0 && !entry->done)
                {
                    // The wait releases the lock, so the probe thread can record its answer and wake every asker.
                    st->cv.wait_for(lk, std::chrono::milliseconds{ waitMs }, [&entry] { return entry->done; });
                }
                if (!entry->done || entry->failed)
                {
                    return FolderProbeAnswer::NoAnswer;
                }
                return entry->isFolder ? FolderProbeAnswer::Folder : FolderProbeAnswer::NotFolder;
            }
            catch (...)
            {
                // Rule #18. Recovery: NoAnswer ("not known") — the lock went with the unwinding; nothing was half recorded but a
                // thread that did start, which still records its own answer.
                LogSwallowedException(L"FolderProber::Probe");
                return FolderProbeAnswer::NoAnswer;
            }
        }

        /// <summary>How many probes are running now (each one a thread, possibly waiting on a network share).</summary>
        /// <returns>The count, at most the prober's maxInFlight.</returns>
        size_t InFlight() const noexcept
        {
            try
            {
                std::lock_guard lk{ _state->m };
                return _state->inFlight;
            }
            catch (...)
            {
                LogSwallowedException(L"FolderProber::InFlight"); // Rule #18. Recovery: 0 (the count is only for tests and logs)
                return 0;
            }
        }

    private:
        // One path's probe: running (done false) or answered (done true, with the answer and when it came).
        struct Entry
        {
            bool done{ false };
            bool isFolder{ false };
            bool failed{ false }; // the probe threw: no answer, and the next ask probes again
            uint64_t answeredTick{ 0 }; // GetTickCount64 when the answer came (the age maxAgeMs is measured against)
        };

        // Everything the probe threads share with the askers, under `m`; it lives as long as any prober copy or probe thread.
        struct State
        {
            mutable std::mutex m;
            std::condition_variable cv; // notified whenever any probe answers (each waiter re-checks its own entry)
            std::unordered_map<std::wstring, std::shared_ptr<Entry>> byPath; // the last probe of each path asked about
            size_t inFlight{ 0 };
            size_t maxInFlight{ kFolderProbeMaxInFlight };
            ProbeFn probe;
        };

        // A probe thread's body: run the probe (it may block for the network's timeout), record the answer, wake the askers.
        static void RunProbe(const std::shared_ptr<State>& st, const std::shared_ptr<Entry>& entry, const std::wstring& path) noexcept
        {
            bool isFolder = false;
            bool failed = false;
            try
            {
                isFolder = st->probe(path);
            }
            catch (...)
            {
                // Rule #18. Recovery: the probe reads "failed" — NoAnswer for the askers, and the next ask probes again.
                failed = true;
                LogSwallowedException(L"FolderProber (a probe)");
            }
            {
                std::lock_guard lk{ st->m };
                entry->isFolder = isFolder;
                entry->failed = failed;
                entry->answeredTick = ::GetTickCount64();
                entry->done = true;
                --st->inFlight;
            }
            st->cv.notify_all();
        }

        std::shared_ptr<State> _state;
    };

    /// <summary>
    /// The process's prober, the one the Telegram bot's folder checks share, so a probe one check started is joined by the next.
    /// Created on first use and never destroyed: a probe thread can still be waiting on the network when the process ends, and
    /// a static's destructor must not run under it (the threads keep the state alive either way).
    /// </summary>
    /// <returns>The shared prober (thread-safe).</returns>
    /// <exception cref="std::bad_alloc">On the first call only, if the prober cannot be allocated.</exception>
    inline FolderProber& SharedFolderProber()
    {
        static FolderProber* const prober = new FolderProber();
        return *prober;
    }
}
