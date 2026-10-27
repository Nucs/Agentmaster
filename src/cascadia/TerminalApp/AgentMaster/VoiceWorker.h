// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceWorker: the ONE voice worker process of the app (VOICE.md §6.2) — started on the first voice note,
// spoken to in VoiceProtocol.h's JSON lines over its stdin / stdout, ended when idle, when the settings change it, or with
// the app. Plain C++ + Win32 (no WinRT, no PCH).
//
// Lifetime: the process is created SUSPENDED, put into a kill-on-close job, then resumed — so nothing it starts (a Python
// worker's CUDA helpers) can outlive Agentmaster, and a stuck worker is ended with everything it started. Its stderr goes
// to <profile>\voice.log line by line (a Python traceback, a model download's progress); its stdout must carry only
// protocol lines — any other line is logged and skipped.
//
// Threads: one reader per pipe (stdout → the answer queue, stderr → the log); every request runs on the caller's thread
// (the service's voice thread) and waits on the answer queue with a deadline. One request at a time (a mutex): the
// protocol has no pipelining.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "VoiceProtocol.h"

namespace Agentmaster::Voice
{
    /// <summary>The voice worker process (one instance per command line; VoiceTranscriber keeps one).</summary>
    class VoiceWorkerProcess
    {
    public:
        /// <summary>Remembers the command line; starts nothing (the first Hello or Transcribe does).</summary>
        /// <param name="commandLine">The whole command line ("\"C:\\…\\python.exe\" \"…\\voice_worker.py\" --engine …").</param>
        /// <param name="workDir">The process's current folder ("" = Agentmaster's own).</param>
        VoiceWorkerProcess(std::wstring commandLine, std::wstring workDir);
        /// <summary>Ends the process (Stop).</summary>
        ~VoiceWorkerProcess();
        VoiceWorkerProcess(const VoiceWorkerProcess&) = delete;
        VoiceWorkerProcess& operator=(const VoiceWorkerProcess&) = delete;

        /// <summary>Whether the process runs right now.</summary>
        /// <returns>True while it runs.</returns>
        bool Running() const noexcept;

        /// <summary>
        /// Starts the process when it does not run and asks "hello" (the engine loads its model here: the first call can take
        /// minutes when a worker downloads a model). A process that runs already is asked nothing: the last hello is kept.
        /// </summary>
        /// <param name="timeoutMs">How long the hello may take.</param>
        /// <param name="error">Receives why it failed.</param>
        /// <returns>The hello answer, or nullopt on a failure (the process is ended then).</returns>
        std::optional<VoiceWorkerAnswer> EnsureReady(uint32_t timeoutMs, std::wstring& error) noexcept;

        /// <summary>One transcription (EnsureReady first). A timeout ENDS the worker: a stuck engine must not hold the next note.</summary>
        /// <param name="request">The request.</param>
        /// <param name="timeoutMs">How long the answer may take.</param>
        /// <param name="error">Receives why it failed.</param>
        /// <returns>The answer (its `ok` may be false: the worker's own failure), or nullopt when no answer came.</returns>
        std::optional<VoiceWorkerAnswer> Transcribe(const VoiceWorkerRequest& request, uint32_t timeoutMs, std::wstring& error) noexcept;

        /// <summary>
        /// Ends the process: "quit", then up to 2 s to leave, then the job ends it; joins the readers and closes every handle.
        /// Safe when nothing runs. ⚠ It waits for a request in flight (one request at a time): call it from the thread that
        /// makes the requests, or Kill first from another.
        /// </summary>
        void Stop() noexcept;

        /// <summary>
        /// Ends the process NOW, from any thread, without waiting for a request in flight: the job (and with it every process
        /// the worker started) is terminated, so a waiting request sees the worker's output end and returns at once. The
        /// handles and readers are cleaned up later by Stop (or the destructor) on the requesting thread.
        /// </summary>
        void Kill() noexcept;

        /// <summary>The command line this worker runs.</summary>
        /// <returns>The command line.</returns>
        const std::wstring& CommandLine() const noexcept
        {
            return _commandLine;
        }

        /// <summary>The GetTickCount64 of the last answer (or the start): the idle clock VoiceTranscriber::Tick reads.</summary>
        /// <returns>The tick; 0 before the first start.</returns>
        uint64_t LastActiveTick() const noexcept
        {
            return _lastActiveTick;
        }

    private:
        /// <summary>
        /// Starts the process (suspended → its kill-on-close job → resumed) with its three pipes and the two readers. Called under
        /// `_callMtx` by EnsureReady only.
        /// </summary>
        /// <param name="error">Receives why it failed (a CreatePipe / CreateProcessW error in words).</param>
        /// <returns>True when the worker runs.</returns>
        bool _launch(std::wstring& error) noexcept;
        /// <summary>Writes one request line and waits for the answer with its id (or the worker's end, or the deadline).</summary>
        /// <param name="line">The request line (no line break).</param>
        /// <param name="id">The request's id.</param>
        /// <param name="timeoutMs">The deadline.</param>
        /// <param name="error">Receives why no answer came.</param>
        /// <returns>The answer, or nullopt (a write failure, the worker's exit, the deadline).</returns>
        std::optional<VoiceWorkerAnswer> _call(const std::wstring& line, int64_t id, uint32_t timeoutMs, std::wstring& error) noexcept;
        /// <summary>The stdout reader thread: protocol lines into `_answers`, any other line into voice.log; flags the pipe's end.</summary>
        void _readStdout() noexcept;
        /// <summary>The stderr reader thread: every line (a '\r' redraw included) into voice.log, bounded per line.</summary>
        void _readStderr() noexcept;
        /// <summary>
        /// Ends the job, waits for the process, joins the readers and closes every handle (the process and job values nulled
        /// under `_handleMtx`, for Kill). On the requesting thread only.
        /// </summary>
        void _closeHandles() noexcept;

        std::wstring _commandLine;
        std::wstring _workDir;
        std::mutex _callMtx; // one request at a time (and Stop against a request)
        mutable std::mutex _mtx; // guards _answers, _stdoutEnded
        mutable std::mutex _handleMtx; // guards the VALUES of _process and _job against Kill (another thread) while they close
        std::condition_variable _cv;
        std::deque<VoiceWorkerAnswer> _answers;
        bool _stdoutEnded{ false };
        void* _process{ nullptr }; // HANDLE
        void* _job{ nullptr }; // HANDLE (kill on close)
        void* _stdinWrite{ nullptr }; // HANDLE
        void* _stdoutRead{ nullptr }; // HANDLE
        void* _stderrRead{ nullptr }; // HANDLE
        std::thread _outThread;
        std::thread _errThread;
        int64_t _nextId{ 1 };
        std::optional<VoiceWorkerAnswer> _hello;
        uint64_t _lastActiveTick{ 0 };
    };
}
