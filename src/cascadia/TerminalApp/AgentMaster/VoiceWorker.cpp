// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceWorker (see VoiceWorker.h): the voice worker process — CreateProcessW with three pipes, a kill-on-close
// job, two reader threads, and JSON-line requests with deadlines.

#include "VoiceWorker.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <vector>

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8 / Utf8ToWide

namespace Agentmaster::Voice
{
    namespace
    {
        /// <summary>One line into the voice log (best effort; the logger never throws).</summary>
        /// <param name="line">The line, without a line break.</param>
        void VoiceLog(const std::wstring& line) noexcept
        {
            try
            {
                AppendStateLog(L"voice.log", line + L"\n");
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE: the logger itself failed; nothing left to report through.
            }
        }

        /// <summary>A Win32 error as "text (code)" for a log line or a topic reply.</summary>
        /// <param name="error">The error code.</param>
        /// <returns>The text.</returns>
        std::wstring ErrorText(DWORD error)
        {
            wchar_t* buffer = nullptr;
            const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                                             reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
            std::wstring text = n && buffer ? std::wstring{ buffer, n } : std::wstring{};
            if (buffer)
            {
                ::LocalFree(buffer);
            }
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.'))
            {
                text.pop_back();
            }
            return (text.empty() ? std::wstring{ L"error" } : text) + L" (" + std::to_wstring(error) + L")";
        }

        /// <summary>
        /// The worker's environment block: Agentmaster's own, plus the switches a Python worker needs to speak the protocol
        /// cleanly (unbuffered, UTF-8 standard streams) and the AM_HARNESS mark (the Fleet Observer and the CLI skip a process
        /// that carries it, as they skip the AI search's child).
        /// </summary>
        /// <returns>A sorted, double-terminated block for CREATE_UNICODE_ENVIRONMENT.</returns>
        std::vector<wchar_t> BuildWorkerEnvironment()
        {
            std::map<std::wstring, std::wstring, bool (*)(const std::wstring&, const std::wstring&)> vars{ [](const std::wstring& a, const std::wstring& b) {
                // Windows sorts an environment block case-insensitively; the map keeps one entry per name the same way.
                return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
            } };
            if (wchar_t* block = ::GetEnvironmentStringsW())
            {
                for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
                {
                    const std::wstring entry{ p };
                    const size_t eq = entry.find(L'=', 1); // a leading '=' belongs to a drive variable's name ("=C:")
                    if (eq != std::wstring::npos)
                    {
                        vars[entry.substr(0, eq)] = entry.substr(eq + 1);
                    }
                }
                ::FreeEnvironmentStringsW(block);
            }
            vars[L"PYTHONUNBUFFERED"] = L"1";
            vars[L"PYTHONIOENCODING"] = L"utf-8";
            vars[L"AM_HARNESS"] = L"voice-worker";
            std::vector<wchar_t> out;
            for (const auto& [k, v] : vars)
            {
                out.insert(out.end(), k.begin(), k.end());
                out.push_back(L'=');
                out.insert(out.end(), v.begin(), v.end());
                out.push_back(L'\0');
            }
            out.push_back(L'\0');
            return out;
        }

        /// <summary>Closes a HANDLE stored as void* and nulls it.</summary>
        /// <param name="h">The handle slot.</param>
        void CloseSlot(void*& h) noexcept
        {
            if (h)
            {
                ::CloseHandle(static_cast<HANDLE>(h));
                h = nullptr;
            }
        }
    }

    VoiceWorkerProcess::VoiceWorkerProcess(std::wstring commandLine, std::wstring workDir) :
        _commandLine(std::move(commandLine)),
        _workDir(std::move(workDir))
    {
    }

    VoiceWorkerProcess::~VoiceWorkerProcess()
    {
        Stop();
    }

    bool VoiceWorkerProcess::Running() const noexcept
    {
        std::lock_guard lk{ _handleMtx };
        return _process && ::WaitForSingleObject(static_cast<HANDLE>(_process), 0) == WAIT_TIMEOUT;
    }

    void VoiceWorkerProcess::Kill() noexcept
    {
        // Only the two handles Kill needs, under their own lock: _closeHandles nulls them under the same lock before it closes
        // them, so a Kill racing a cleanup never terminates through a closed (or reused) handle value.
        std::lock_guard lk{ _handleMtx };
        if (_job)
        {
            ::TerminateJobObject(static_cast<HANDLE>(_job), 1);
        }
        if (_process)
        {
            ::TerminateProcess(static_cast<HANDLE>(_process), 1);
        }
    }

    bool VoiceWorkerProcess::_launch(std::wstring& error) noexcept
    {
        try
        {
            SECURITY_ATTRIBUTES inheritable{ sizeof(inheritable), nullptr, TRUE };
            HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
            // Three pipes; the child's ends are inheritable, ours are not (a child holding our end would keep a pipe from ever
            // reaching its end, and our reader threads would never return).
            const auto fail = [&](const wchar_t* what) {
                error = std::wstring{ what } + L": " + ErrorText(::GetLastError());
                for (HANDLE h : { inRead, inWrite, outRead, outWrite, errRead, errWrite })
                {
                    if (h)
                    {
                        ::CloseHandle(h);
                    }
                }
                return false;
            };
            if (!::CreatePipe(&inRead, &inWrite, &inheritable, 0) || !::CreatePipe(&outRead, &outWrite, &inheritable, 0) ||
                !::CreatePipe(&errRead, &errWrite, &inheritable, 0))
            {
                return fail(L"the worker's pipes could not be made");
            }
            ::SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
            ::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
            ::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

            // Only the three child ends go to the worker (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): with plain inheritance every
            // other inheritable handle of this busy process (a tab's ConPTY pipe) would leak into it and outlive our use.
            SIZE_T attributeBytes = 0;
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            std::vector<unsigned char> attributeBuffer(attributeBytes);
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
            const bool attributesMade = ::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes) != FALSE;
            HANDLE inherited[3] = { inRead, outWrite, errWrite };
            const bool listed = attributesMade &&
                                ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
            STARTUPINFOEXW si{};
            si.StartupInfo.cb = static_cast<DWORD>(listed ? sizeof(si) : sizeof(si.StartupInfo));
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = inRead;
            si.StartupInfo.hStdOutput = outWrite;
            si.StartupInfo.hStdError = errWrite;
            si.lpAttributeList = listed ? attributes : nullptr;
            std::vector<wchar_t> environment = BuildWorkerEnvironment();
            std::wstring commandLine = _commandLine; // CreateProcessW may write into this buffer
            PROCESS_INFORMATION pi{};
            // SUSPENDED: the worker joins its kill-on-close job before it runs one instruction, so nothing it starts escapes.
            const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0);
            const BOOL created = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, flags, environment.data(),
                                                  _workDir.empty() ? nullptr : _workDir.c_str(), &si.StartupInfo, &pi);
            const DWORD createError = created ? ERROR_SUCCESS : ::GetLastError();
            if (attributesMade)
            {
                ::DeleteProcThreadAttributeList(attributes);
            }
            // The child holds its own copies now (or never will): ours would keep the pipes open past its end.
            ::CloseHandle(inRead);
            ::CloseHandle(outWrite);
            ::CloseHandle(errWrite);
            inRead = outWrite = errWrite = nullptr;
            if (!created)
            {
                ::SetLastError(createError);
                return fail(L"the voice worker could not start");
            }
            HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
            if (job)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
                if (!::AssignProcessToJobObject(job, pi.hProcess))
                {
                    // Without the job only the worker itself can be ended; a child it starts could outlive the app.
                    VoiceLog(L"[voice] the worker could not join its job (" + ErrorText(::GetLastError()) + L"); a stop ends only the worker itself");
                    ::CloseHandle(job);
                    job = nullptr;
                }
            }
            {
                std::lock_guard hk{ _handleMtx }; // Kill may read them from another thread from now on
                _process = pi.hProcess;
                _job = job;
            }
            _stdinWrite = inWrite;
            _stdoutRead = outRead;
            _stderrRead = errRead;
            {
                std::lock_guard lk{ _mtx };
                _answers.clear();
                _stdoutEnded = false;
            }
            try
            {
                _outThread = std::thread([this]() noexcept { _readStdout(); });
                _errThread = std::thread([this]() noexcept { _readStderr(); });
            }
            catch (...)
            {
                // A reader could not start (std::system_error): the suspended worker is ended, so a reader that did start reaches
                // the end of its pipe and the joins in _closeHandles return.
                LogSwallowedException(L"VoiceWorkerProcess::_launch (readers)");
                ::TerminateProcess(pi.hProcess, 1);
                ::ResumeThread(pi.hThread);
                ::CloseHandle(pi.hThread);
                _closeHandles();
                error = L"the voice worker's output could not be read";
                return false;
            }
            ::ResumeThread(pi.hThread);
            ::CloseHandle(pi.hThread);
            _lastActiveTick = ::GetTickCount64();
            VoiceLog(L"[voice] worker started (pid " + std::to_wstring(pi.dwProcessId) + L"): " + _commandLine);
            return true;
        }
        catch (...)
        {
            LogSwallowedException(L"VoiceWorkerProcess::_launch");
            error = L"the voice worker could not start (an exception; see hooks.log)";
            return false;
        }
    }

    void VoiceWorkerProcess::_readStdout() noexcept
    {
        try
        {
            std::string pending;
            char buffer[4096];
            DWORD read = 0;
            while (::ReadFile(static_cast<HANDLE>(_stdoutRead), buffer, sizeof(buffer), &read, nullptr) && read > 0)
            {
                pending.append(buffer, read);
                size_t nl = 0;
                while ((nl = pending.find('\n')) != std::string::npos)
                {
                    std::string raw = pending.substr(0, nl);
                    pending.erase(0, nl + 1);
                    if (!raw.empty() && raw.back() == '\r')
                    {
                        raw.pop_back();
                    }
                    if (raw.empty())
                    {
                        continue;
                    }
                    const std::wstring line = Profiles::detail::Utf8ToWide(raw);
                    if (auto answer = ParseWorkerAnswer(line))
                    {
                        std::lock_guard lk{ _mtx };
                        _answers.push_back(std::move(*answer));
                        _cv.notify_all();
                    }
                    else
                    {
                        // Not a protocol line (a library printed to stdout after all): kept for the log, never an answer.
                        VoiceLog(L"[voice] worker stdout (not a protocol line): " + line.substr(0, 400));
                    }
                }
                // A line longer than 1 MB is not a protocol answer anyone needs whole: the reader keeps memory bounded.
                if (pending.size() > (1u << 20))
                {
                    VoiceLog(L"[voice] worker stdout: a line over 1 MB was dropped");
                    pending.clear();
                }
            }
        }
        catch (...)
        {
            LogSwallowedException(L"VoiceWorkerProcess::_readStdout");
        }
        std::lock_guard lk{ _mtx };
        _stdoutEnded = true;
        _cv.notify_all();
    }

    void VoiceWorkerProcess::_readStderr() noexcept
    {
        try
        {
            std::string pending;
            char buffer[4096];
            DWORD read = 0;
            while (::ReadFile(static_cast<HANDLE>(_stderrRead), buffer, sizeof(buffer), &read, nullptr) && read > 0)
            {
                pending.append(buffer, read);
                size_t nl = 0;
                // A progress bar redraws with '\r': each redraw is a line of its own, so a download's progress is readable.
                while ((nl = pending.find_first_of("\r\n")) != std::string::npos)
                {
                    const std::string raw = pending.substr(0, nl);
                    pending.erase(0, nl + 1);
                    if (!raw.empty())
                    {
                        VoiceLog(L"[voice-worker] " + Profiles::detail::Utf8ToWide(raw).substr(0, 1000));
                    }
                }
                if (pending.size() > (64u << 10))
                {
                    VoiceLog(L"[voice-worker] " + Profiles::detail::Utf8ToWide(pending.substr(0, 1000)) + L" …");
                    pending.clear();
                }
            }
            if (!pending.empty())
            {
                VoiceLog(L"[voice-worker] " + Profiles::detail::Utf8ToWide(pending).substr(0, 1000));
            }
        }
        catch (...)
        {
            LogSwallowedException(L"VoiceWorkerProcess::_readStderr");
        }
    }

    std::optional<VoiceWorkerAnswer> VoiceWorkerProcess::_call(const std::wstring& line, int64_t id, uint32_t timeoutMs, std::wstring& error) noexcept
    {
        try
        {
            const std::string bytes = Profiles::detail::WideToUtf8(line) + "\n";
            DWORD written = 0;
            if (!::WriteFile(static_cast<HANDLE>(_stdinWrite), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size())
            {
                error = L"the voice worker stopped taking requests: " + ErrorText(::GetLastError());
                return std::nullopt;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            std::unique_lock lk{ _mtx };
            for (;;)
            {
                // Answers to an earlier request (one that timed out) are dropped here: only this id is the answer.
                while (!_answers.empty())
                {
                    VoiceWorkerAnswer a = std::move(_answers.front());
                    _answers.pop_front();
                    if (a.id == id)
                    {
                        _lastActiveTick = ::GetTickCount64();
                        return a;
                    }
                    if (a.id == 0 && !a.ok)
                    {
                        // An answer without an id is the worker refusing a line it could not read — ours.
                        _lastActiveTick = ::GetTickCount64();
                        return a;
                    }
                }
                if (_stdoutEnded)
                {
                    error = L"the voice worker exited (see voice.log)";
                    return std::nullopt;
                }
                if (_cv.wait_until(lk, deadline) == std::cv_status::timeout && _answers.empty())
                {
                    error = L"the voice worker did not answer within " + std::to_wstring(timeoutMs / 1000) + L" s";
                    return std::nullopt;
                }
            }
        }
        catch (...)
        {
            LogSwallowedException(L"VoiceWorkerProcess::_call");
            error = L"the voice request failed (an exception; see hooks.log)";
            return std::nullopt;
        }
    }

    std::optional<VoiceWorkerAnswer> VoiceWorkerProcess::EnsureReady(uint32_t timeoutMs, std::wstring& error) noexcept
    {
        std::lock_guard callLk{ _callMtx };
        if (Running() && _hello)
        {
            return _hello;
        }
        if (_process)
        {
            // A worker that exited by itself (a crash): its handles and readers go before a new one starts.
            VoiceLog(L"[voice] the worker had exited; starting it again");
            _closeHandles();
        }
        _hello.reset();
        if (!_launch(error))
        {
            VoiceLog(L"[voice] " + error);
            return std::nullopt;
        }
        const int64_t id = _nextId++;
        auto hello = _call(BuildHelloRequest(id), id, timeoutMs, error);
        if (!hello || !hello->ok)
        {
            if (hello && !hello->ok)
            {
                error = L"the voice worker could not load its engine: " + hello->error;
            }
            VoiceLog(L"[voice] hello failed: " + error);
            // A worker that cannot say hello is of no use: ended, so the next note starts a fresh one.
            Kill();
            _closeHandles();
            return std::nullopt;
        }
        VoiceLog(L"[voice] worker ready: " + hello->engine + (hello->takesPrompt ? L" (takes a prompt)" : L"") + (hello->takesHotwords ? L" (takes hotwords)" : L""));
        _hello = hello;
        return _hello;
    }

    std::optional<VoiceWorkerAnswer> VoiceWorkerProcess::Transcribe(const VoiceWorkerRequest& request, uint32_t timeoutMs, std::wstring& error) noexcept
    {
        // The hello's own deadline is generous (a first start loads, or downloads, the model); the transcription's is the
        // caller's.
        if (!EnsureReady((std::max)(timeoutMs, 180000u), error))
        {
            return std::nullopt;
        }
        std::lock_guard callLk{ _callMtx };
        const int64_t id = _nextId++;
        auto answer = _call(BuildTranscribeRequest(id, request), id, timeoutMs, error);
        if (!answer)
        {
            // No answer: the engine is stuck (still running past the deadline) or the worker died. Either way it is ended and
            // its handles closed, so the next note starts a fresh worker instead of waiting behind this one.
            VoiceLog(L"[voice] " + error + (Running() ? L"; the worker is ended" : L""));
            Kill();
            _closeHandles();
            _hello.reset();
        }
        return answer;
    }

    void VoiceWorkerProcess::Stop() noexcept
    {
        try
        {
            std::lock_guard callLk{ _callMtx };
            if (!_process)
            {
                return;
            }
            if (Running() && _stdinWrite)
            {
                // A polite "quit" first (a Python worker frees the GPU cleanly), then closing stdin, then the job ends it.
                // _process is only ever written on this (the requesting) thread, under _callMtx held here, so the wait reads
                // a value no one can change meanwhile (Kill only terminates through it).
                const int64_t id = _nextId++;
                const std::string bytes = Profiles::detail::WideToUtf8(BuildQuitRequest(id)) + "\n";
                DWORD written = 0;
                ::WriteFile(static_cast<HANDLE>(_stdinWrite), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
                CloseSlot(_stdinWrite);
                ::WaitForSingleObject(static_cast<HANDLE>(_process), 2000);
            }
            Kill();
            _closeHandles();
            _hello.reset();
            VoiceLog(L"[voice] worker stopped");
        }
        catch (...)
        {
            LogSwallowedException(L"VoiceWorkerProcess::Stop");
        }
    }

    void VoiceWorkerProcess::_closeHandles() noexcept
    {
        // Runs on the requesting thread only (under _callMtx, or from _launch / the destructor). The process must be gone before
        // the joins: its pipe ends close with it, and only then do the readers return. The job goes first: a grandchild that
        // inherited the worker's stdout (a Python helper process) would hold the pipe open, and a join would then wait forever;
        // ending the job ends every descendant at once.
        {
            std::lock_guard hk{ _handleMtx };
            if (_job)
            {
                ::TerminateJobObject(static_cast<HANDLE>(_job), 0);
            }
        }
        if (_process)
        {
            ::WaitForSingleObject(static_cast<HANDLE>(_process), 5000);
        }
        CloseSlot(_stdinWrite);
        if (_outThread.joinable())
        {
            _outThread.join();
        }
        if (_errThread.joinable())
        {
            _errThread.join();
        }
        CloseSlot(_stdoutRead);
        CloseSlot(_stderrRead);
        // Nulled under the lock Kill takes: a Kill from another thread sees either the live handles or none.
        std::lock_guard hk{ _handleMtx };
        CloseSlot(_process);
        CloseSlot(_job);
    }
}
