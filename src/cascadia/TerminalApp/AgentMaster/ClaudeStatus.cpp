// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the OS half of ClaudeStatus.h (MESSAGING.md §21.40): the short-lived Claude Code children that print the
// account (`claude auth status --text`) and the plan usage (`claude -p /usage`), their process-wide cache, and the transcript
// FILE read behind the session's status facts.
//
// Why children, and why these flags (each one measured on Claude Code 2.1.310, 2026-10-26):
//   * Claude Code holds the account's OAuth login and refreshes it itself; Agentmaster never reads a credential. Asking Claude
//     Code to print what its own dialog shows keeps every credential inside Claude Code, and keeps the wording its own.
//   * `-p /usage` runs the LOCAL command: no model call (num_turns 0, total_cost_usd 0, local_command "usage"). Below the
//     version gate (ClaudeVersionRunsHeadlessUsage) it is never asked, and a claude that answers with a model turn anyway is
//     never asked again (the latch) — "/usage" sent to a model would be a paid call for nothing.
//   * `--setting-sources ""` + `--strict-mcp-config` + `--tools ""`: no hooks, no plugins, no MCP server is started, no status
//     line runs; `--settings {"verbose":false}` keeps the answer one JSON object (the user's verbose setting turns it into an
//     array of every message); `--no-session-persistence` writes no transcript.
//   * CLAUDE_CODE_DISABLE_AUTO_MEMORY=1: without it the headless run creates an empty projects\<folder>\memory\ for its working
//     folder (seen on 2.1.310); with it nothing is left under ~/.claude. DISABLE_AUTOUPDATER=1: a child must never update the
//     user's claude.exe (an environment without it once replaced a pinned 2.1.291 with 2.1.308 within minutes).
//   * AM_HARNESS=claude-status: the Fleet Observer and the CLI skip the child (it never appears as an External session).
//
// Plain C++ + Win32, no WinRT (the vcxproj marks it <PrecompiledHeader>NotUsing</PrecompiledHeader>).

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ClaudeStatus.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <thread>

#include "AgentHarnessCore.h" // BuildHarnessEnvironment: the variables a Claude Code child must not inherit, + AM_HARNESS
#include "ClaudeSpawn.h" // AgentmasterStateDir, AppendStateLog, LogSwallowedException, ReadClaudeFileVersion

namespace Agentmaster
{
    namespace
    {
        /// <summary>The name the probe children carry in AM_HARNESS (what the Fleet Observer and the CLI skip).</summary>
        constexpr wchar_t kClaudeStatusHarnessName[] = L"claude-status";

        /// <summary>The bytes one read of a child's pipe or of a transcript takes.</summary>
        constexpr DWORD kReadChunkBytes = 64 * 1024;

        /// <summary>The bytes one read of a transcript takes (a big sequential read: the file may be 100 MB).</summary>
        constexpr DWORD kTranscriptChunkBytes = 1024 * 1024;

        /// <summary>The tick counter in ms (the caches' age: immune to a change of the wall clock).</summary>
        /// <returns>GetTickCount64.</returns>
        int64_t TickMs() noexcept
        {
            return static_cast<int64_t>(::GetTickCount64());
        }

        /// <summary>The wall clock in Unix ms (what a report prints as "read at").</summary>
        /// <returns>Milliseconds since 1970.</returns>
        int64_t WallMs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /// <summary>
        /// One [claude-status] line in hooks.log. Never throws: a logging failure must not turn a contained probe error into a
        /// new one.
        /// </summary>
        /// <param name="line">The line (without the tag and the line break).</param>
        void LogStatus(const std::wstring& line) noexcept
        {
            try
            {
                AppendStateLog(L"hooks.log", L"[claude-status] " + line + L"\n");
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE (CLAUDE.md Rule #18, the logging-path exemption): this IS a log write; reporting its own
                // failure would write to the same log again. Degradation: the probe's log line is lost, nothing else.
            }
        }

        /// <summary>
        /// UTF-8 bytes as UTF-16 (MultiByteToWideChar; an invalid sequence becomes U+FFFD, as Claude Code's own output never has
        /// one). Empty for empty input or a conversion the system refuses.
        /// </summary>
        /// <param name="bytes">The bytes.</param>
        /// <returns>The text.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring Utf8ToWideText(std::string_view bytes)
        {
            if (bytes.empty())
            {
                return {};
            }
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (n <= 0)
            {
                return {};
            }
            std::wstring out(static_cast<size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
            return out;
        }

        /// <summary>
        /// The system's text for a Win32 error code, without its trailing line break and full stop, with the code after it
        /// ("The system cannot find the file specified (2)") — what a report or a log line says when a child cannot start.
        /// </summary>
        /// <param name="error">A GetLastError value.</param>
        /// <returns>The text; never empty ("unknown error (N)" when the system has none).</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring Win32ErrorText(DWORD error)
        {
            wchar_t* buffer = nullptr;
            const DWORD size = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
                                                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
            std::wstring message = (buffer && size) ? std::wstring(buffer, size) : std::wstring{ L"unknown error" };
            if (buffer)
            {
                ::LocalFree(buffer);
            }
            while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ' || message.back() == L'.'))
            {
                message.pop_back();
            }
            return message + L" (" + std::to_wstring(error) + L")";
        }

        /// <summary>
        /// Quotes one argument so the Microsoft C runtime's parser gives the child exactly `arg` (the rules of
        /// ClaudeSdkProcessWin32.cpp's QuoteArgument): backslashes are literal unless a quote follows them; an empty argument
        /// becomes "" (so `--setting-sources ""` reaches claude as an EMPTY value, not as a missing one).
        /// </summary>
        /// <param name="arg">The argument's exact text.</param>
        /// <returns>The text to put on the command line.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring QuoteArg(std::wstring_view arg)
        {
            if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
            {
                return std::wstring{ arg };
            }
            std::wstring out;
            out.reserve(arg.size() + 2);
            out.push_back(L'"');
            size_t backslashes = 0;
            for (const wchar_t c : arg)
            {
                if (c == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
                backslashes = 0;
                out.push_back(c);
            }
            out.append(backslashes * 2, L'\\');
            out.push_back(L'"');
            return out;
        }

        /// <summary>Orders environment variable names the way Windows does: case-insensitive, ordinal.</summary>
        struct EnvNameLess
        {
            /// <summary>a &lt; b, ignoring case.</summary>
            /// <param name="a">One name.</param>
            /// <param name="b">The other.</param>
            /// <returns>True when a sorts first.</returns>
            bool operator()(const std::wstring& a, const std::wstring& b) const noexcept
            {
                return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
            }
        };

        /// <summary>
        /// The child's UTF-16 environment block: this process's variables with `changes` applied — a non-empty value sets a
        /// variable, an EMPTY value REMOVES it (BuildHarnessEnvironment's convention) — sorted by name, double-terminated.
        /// </summary>
        /// <param name="changes">Name → UTF-8 value.</param>
        /// <returns>The block for CreateProcessW (CREATE_UNICODE_ENVIRONMENT).</returns>
        /// <exception cref="std::bad_alloc">If the block cannot be allocated.</exception>
        std::vector<wchar_t> BuildChildEnvironment(const std::map<std::string, std::string>& changes)
        {
            std::map<std::wstring, std::wstring, EnvNameLess> vars;
            if (wchar_t* const block = ::GetEnvironmentStringsW())
            {
                for (const wchar_t* p = block; *p != L'\0';)
                {
                    const std::wstring_view entry{ p };
                    // A name may START with '=' (cmd.exe's per-drive "=C:=C:\dir" entries): the separator is the first '=' after 0.
                    const size_t eq = entry.find(L'=', 1);
                    if (eq != std::wstring_view::npos)
                    {
                        vars[std::wstring{ entry.substr(0, eq) }] = std::wstring{ entry.substr(eq + 1) };
                    }
                    p += entry.size() + 1;
                }
                ::FreeEnvironmentStringsW(block);
            }
            for (const auto& [name, value] : changes)
            {
                const std::wstring wideName = Utf8ToWideText(name);
                if (wideName.empty())
                {
                    continue;
                }
                if (value.empty())
                {
                    vars.erase(wideName);
                }
                else
                {
                    vars[wideName] = Utf8ToWideText(value);
                }
            }
            std::vector<wchar_t> out;
            for (const auto& [name, value] : vars)
            {
                out.insert(out.end(), name.begin(), name.end());
                out.push_back(L'=');
                out.insert(out.end(), value.begin(), value.end());
                out.push_back(L'\0');
            }
            if (out.empty())
            {
                out.push_back(L'\0');
            }
            out.push_back(L'\0');
            return out;
        }

        /// <summary>Closes a handle on scope exit unless it was released (every early return closes what was opened so far).</summary>
        struct OwnedHandle
        {
            HANDLE h{ nullptr };
            OwnedHandle() = default;
            explicit OwnedHandle(HANDLE handle) noexcept :
                h{ handle == INVALID_HANDLE_VALUE ? nullptr : handle } {}
            OwnedHandle(const OwnedHandle&) = delete;
            OwnedHandle& operator=(const OwnedHandle&) = delete;
            ~OwnedHandle() { reset(); }
            /// <summary>Closes the handle now (if any) and takes `handle` instead.</summary>
            /// <param name="handle">The new handle (INVALID_HANDLE_VALUE reads as none).</param>
            void reset(HANDLE handle = nullptr) noexcept
            {
                if (h)
                {
                    ::CloseHandle(h);
                }
                h = handle == INVALID_HANDLE_VALUE ? nullptr : handle;
            }
        };

        /// <summary>
        /// Reads a pipe to its end (the child and everything it started closed their write ends), keeping at most
        /// kClaudeCaptureMaxBytes: past that the bytes are read and dropped, so a runaway child never blocks on a full pipe.
        /// </summary>
        /// <param name="pipe">The read end.</param>
        /// <param name="out">Receives the bytes.</param>
        void DrainPipe(HANDLE pipe, std::string& out) noexcept
        {
            try
            {
                std::vector<char> buffer(kReadChunkBytes);
                for (;;)
                {
                    DWORD got = 0;
                    if (!::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) || got == 0)
                    {
                        return; // ERROR_BROKEN_PIPE: every writer is gone
                    }
                    if (out.size() < kClaudeCaptureMaxBytes)
                    {
                        out.append(buffer.data(), (std::min)(static_cast<size_t>(got), kClaudeCaptureMaxBytes - out.size()));
                    }
                }
            }
            catch (...)
            {
                // Rule #18. Recovery: the bytes read so far are kept; the caller reads the child's answer from them (a cut JSON
                // simply fails to parse and the report says the read failed).
                LogSwallowedException(L"ClaudeStatus DrainPipe");
            }
        }

        /// <summary>
        /// A thread that drains one pipe, joined by its destructor — so no early return can leave a joinable std::thread behind
        /// (its destructor would end the process). The pipe must reach its end for the join to return: the caller ends the
        /// child (and its job) before this goes out of scope on every path.
        /// </summary>
        struct PipeReader
        {
            std::string bytes; // what the pipe gave
            std::thread thread; // the reader (not joinable when it never started)
            /// <summary>Starts draining `pipe` on a new thread.</summary>
            /// <param name="pipe">The read end (owned by the caller, closed after the join).</param>
            /// <exception cref="std::system_error">If the thread cannot start.</exception>
            void Start(HANDLE pipe)
            {
                thread = std::thread([this, pipe]() noexcept { DrainPipe(pipe, bytes); });
            }
            /// <summary>Waits for the reader to end (the pipe reached its end). Never throws.</summary>
            void Join() noexcept
            {
                try
                {
                    if (thread.joinable())
                    {
                        thread.join();
                    }
                }
                catch (...)
                {
                    // std::system_error from join: it is thrown only for a thread that is the caller or not joinable, which this
                    // reader never is. Should it happen anyway, the thread is detached, because a joinable std::thread's
                    // destructor would end the whole process; the caller then reads whatever the bytes hold so far.
                    LogSwallowedException(L"ClaudeStatus PipeReader::Join");
                    if (thread.joinable())
                    {
                        thread.detach();
                    }
                }
            }
            ~PipeReader() { Join(); }
        };

        /// <summary>
        /// The identity of an executable for the /usage latch: its path, size and last write time, so a claude.exe replaced by
        /// an update (a new size or time at the same path) is asked again.
        /// </summary>
        /// <param name="exe">The executable's path.</param>
        /// <returns>"&lt;path&gt;|&lt;size&gt;|&lt;write time&gt;", or the path alone when its attributes cannot be read.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring ExeIdentity(const std::wstring& exe)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!::GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &data))
            {
                return exe;
            }
            const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            const uint64_t written = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            return exe + L"|" + std::to_wstring(size) + L"|" + std::to_wstring(written);
        }

        /// <summary>The first line of a child's output, at most `maxChars` (for a log line or an error that names the cause).</summary>
        /// <param name="text">The output.</param>
        /// <param name="maxChars">The longest result.</param>
        /// <returns>The first non-blank line, trimmed and cut.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring FirstOutputLine(std::wstring_view text, size_t maxChars)
        {
            for (const auto& line : ParseClaudeStatusLines(text))
            {
                std::wstring whole = line.label.empty() ? line.value : line.label + L": " + line.value;
                if (whole.size() > maxChars)
                {
                    whole.resize(maxChars);
                    whole += L"\x2026";
                }
                return whole;
            }
            return {};
        }

        /// <summary>
        /// Why a child gave no usable answer, in a few words: it did not start, ran out of time, or ended with an exit code
        /// (with the first line it printed, standard error first).
        /// </summary>
        /// <param name="r">The child's result.</param>
        /// <param name="what">The command's name for the text ("auth status", "/usage").</param>
        /// <param name="timeoutMs">Its time limit (named when it ran out).</param>
        /// <returns>The reason.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring CaptureFailureText(const ClaudeCaptureResult& r, const wchar_t* what, uint32_t timeoutMs)
        {
            if (!r.started)
            {
                return std::wstring{ L"claude " } + what + L" could not start: " + r.error;
            }
            if (r.timedOut)
            {
                return std::wstring{ L"claude " } + what + L" did not answer within " + std::to_wstring(timeoutMs / 1000) + L" s";
            }
            std::wstring said = FirstOutputLine(r.err, 160);
            if (said.empty())
            {
                said = FirstOutputLine(r.out, 160);
            }
            return std::wstring{ L"claude " } + what + L" ended with exit code " + std::to_wstring(r.exitCode) + (said.empty() ? std::wstring{} : L": " + said);
        }
    }

    ClaudeCaptureResult RunClaudeCapture(const std::wstring& exe, const std::wstring& args, const std::wstring& workDir, uint32_t timeoutMs) noexcept
    {
        ClaudeCaptureResult r;
        const int64_t startedTick = TickMs();
        try
        {
            // The child's environment: every variable of a parent Claude Code session or terminal tab removed (an Agentmaster
            // started from a claude's tool call carries them, and a child would report that session as its own), the harness
            // mark, and the two switches that keep the child from changing anything (see the file header).
            std::map<std::string, std::string> changes = BuildHarnessEnvironment(kClaudeStatusHarnessName);
            changes["DISABLE_AUTOUPDATER"] = "1";
            changes["CLAUDE_CODE_DISABLE_AUTO_MEMORY"] = "1";
            std::vector<wchar_t> environment = BuildChildEnvironment(changes);

            // Standard output and error each get a pipe and a reader thread of their own: a child that fills one pipe while
            // the other is being read would otherwise block forever. Standard input is NUL: a headless claude with an open,
            // silent stdin waits 3 s for input before it runs.
            SECURITY_ATTRIBUTES inheritable{ sizeof(inheritable), nullptr, TRUE };
            HANDLE outRead = nullptr;
            HANDLE outWrite = nullptr;
            HANDLE errRead = nullptr;
            HANDLE errWrite = nullptr;
            if (!::CreatePipe(&outRead, &outWrite, &inheritable, 0))
            {
                r.error = Win32ErrorText(::GetLastError());
                return r;
            }
            OwnedHandle outReadOwned{ outRead };
            OwnedHandle outWriteOwned{ outWrite };
            if (!::CreatePipe(&errRead, &errWrite, &inheritable, 0))
            {
                r.error = Win32ErrorText(::GetLastError());
                return r;
            }
            OwnedHandle errReadOwned{ errRead };
            OwnedHandle errWriteOwned{ errWrite };
            // Our read ends must not be inherited: a child holding one would keep the pipe from ever reaching its end.
            ::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
            ::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
            OwnedHandle nul{ ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr) };
            if (!nul.h)
            {
                r.error = Win32ErrorText(::GetLastError());
                return r;
            }

            // Only these three handles go to the child (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): with plain inheritance every other
            // inheritable handle of this busy process (another tab's pipe, a file) would leak into it and outlive our use.
            SIZE_T attributeBytes = 0;
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            std::vector<unsigned char> attributeBuffer(attributeBytes);
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
            if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
            {
                r.error = Win32ErrorText(::GetLastError());
                return r;
            }
            HANDLE inherited[3] = { nul.h, outWrite, errWrite };
            const BOOL listed = ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);

            STARTUPINFOEXW si{};
            // The extended size only together with EXTENDED_STARTUPINFO_PRESENT (when the handle list could be set); without the
            // list the child inherits every inheritable handle, the plain fallback.
            si.StartupInfo.cb = static_cast<DWORD>(listed ? sizeof(si) : sizeof(si.StartupInfo));
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = nul.h;
            si.StartupInfo.hStdOutput = outWrite;
            si.StartupInfo.hStdError = errWrite;
            si.lpAttributeList = listed ? attributes : nullptr;

            std::wstring commandLine = QuoteArg(exe) + L" " + args; // CreateProcessW may write into this buffer
            PROCESS_INFORMATION pi{};
            // SUSPENDED: the child goes into its kill-on-close job before it runs a single instruction, so nothing it starts can
            // escape the job (and with it the timeout, or Agentmaster's own end).
            const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0);
            const BOOL created = ::CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags, environment.data(),
                                                  workDir.empty() ? nullptr : workDir.c_str(), &si.StartupInfo, &pi);
            const DWORD createError = created ? ERROR_SUCCESS : ::GetLastError();
            ::DeleteProcThreadAttributeList(attributes);
            // The child holds its own copies now (or never will): ours would keep the pipes open past its end.
            outWriteOwned.reset();
            errWriteOwned.reset();
            nul.reset();
            if (!created)
            {
                r.error = Win32ErrorText(createError);
                return r;
            }
            r.started = true;
            OwnedHandle process{ pi.hProcess };
            OwnedHandle mainThread{ pi.hThread };
            OwnedHandle job{ ::CreateJobObjectW(nullptr, nullptr) };
            if (job.h)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                ::SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &info, sizeof(info));
                if (!::AssignProcessToJobObject(job.h, process.h))
                {
                    // Without the job only the child itself can be ended on a timeout; it starts nothing of its own here.
                    LogStatus(L"the child could not join its job (" + Win32ErrorText(::GetLastError()) + L"); a timeout ends only the child itself");
                    job.reset();
                }
            }
            // Ends the child (and, through the job, everything it started). Every path below that leaves early calls it first,
            // so the pipes reach their end and the readers' joins return.
            const auto endChild = [&](UINT code) noexcept {
                if (job.h)
                {
                    ::TerminateJobObject(job.h, code);
                }
                ::TerminateProcess(process.h, code);
                ::WaitForSingleObject(process.h, 5000);
            };
            PipeReader outReader;
            PipeReader errReader;
            try
            {
                outReader.Start(outRead);
                errReader.Start(errRead);
            }
            catch (...)
            {
                // A reader could not start (std::system_error): the suspended child is ended, so a reader that did start
                // reaches the end of its pipe and the destructors' joins return.
                LogSwallowedException(L"RunClaudeCapture (readers)");
                endChild(1);
                r.error = L"its output could not be read";
                return r;
            }
            ::ResumeThread(mainThread.h);
            mainThread.reset();
            if (::WaitForSingleObject(process.h, timeoutMs) != WAIT_OBJECT_0)
            {
                r.timedOut = true;
                endChild(1);
            }
            else
            {
                DWORD code = 0;
                ::GetExitCodeProcess(process.h, &code);
                r.exitCode = code;
                if (job.h)
                {
                    // Anything the child left running ends with it (none of these commands starts a process, but a pipe held
                    // open by one would keep the readers waiting forever).
                    ::TerminateJobObject(job.h, 0);
                }
            }
            outReader.Join();
            errReader.Join();
            r.out = Utf8ToWideText(outReader.bytes);
            r.err = Utf8ToWideText(errReader.bytes);
        }
        catch (...)
        {
            // Rule #18. Recovery: the run reads as failed; the RAII guards above closed every handle and ended the child.
            LogSwallowedException(L"RunClaudeCapture");
            if (r.error.empty())
            {
                r.error = L"it failed (hooks.log says why)";
            }
        }
        r.tookMs = TickMs() - startedTick;
        return r;
    }

    bool ReadClaudeTranscriptStatusFile(const std::wstring& path, ClaudeTranscriptStatus& out, std::wstring& error) noexcept
    {
        try
        {
            // Shared read, write AND delete: claude appends to the file while it runs, and must never be blocked by this read.
            OwnedHandle file{ ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
            if (!file.h)
            {
                error = Win32ErrorText(::GetLastError());
                return false;
            }
            ClaudeTranscriptStatusReader reader;
            const auto feed = [&reader](std::string_view line) {
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                // The filter runs on the raw bytes: only a line that can carry a status fact is decoded and parsed at all.
                if (ClaudeTranscriptStatusLineWanted(line))
                {
                    reader.Feed(Utf8ToWideText(line));
                }
                else
                {
                    reader.NotePassedLine();
                }
            };
            std::vector<char> buffer(kTranscriptChunkBytes);
            std::string carry; // the start of a line that continues in the next chunk
            bool first = true;
            for (;;)
            {
                DWORD got = 0;
                if (!::ReadFile(file.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
                {
                    error = Win32ErrorText(::GetLastError());
                    return false;
                }
                if (got == 0)
                {
                    break;
                }
                size_t start = 0;
                if (first)
                {
                    first = false;
                    if (got >= 3 && static_cast<unsigned char>(buffer[0]) == 0xEF && static_cast<unsigned char>(buffer[1]) == 0xBB && static_cast<unsigned char>(buffer[2]) == 0xBF)
                    {
                        start = 3; // a UTF-8 BOM (Claude Code writes none; an edited copy might)
                    }
                }
                for (size_t i = start; i < got; ++i)
                {
                    if (buffer[i] != '\n')
                    {
                        continue;
                    }
                    if (carry.empty())
                    {
                        feed(std::string_view{ buffer.data() + start, i - start });
                    }
                    else
                    {
                        carry.append(buffer.data() + start, i - start);
                        feed(carry);
                        carry.clear();
                    }
                    start = i + 1;
                }
                carry.append(buffer.data() + start, got - start);
            }
            if (!carry.empty())
            {
                feed(carry); // the last line, with no line break yet: claude may be writing it (a torn line counts as unparsed)
            }
            out = reader.Result();
            return true;
        }
        catch (...)
        {
            // Rule #18. Recovery: no transcript facts — the report says the transcript could not be read and shows the rest.
            LogSwallowedException(L"ReadClaudeTranscriptStatusFile");
            error = L"it could not be read (hooks.log says why)";
            return false;
        }
    }

    template<class Read, class Run>
    Read ClaudeStatusProbe::_read(Slot<Read>& slot, int64_t cacheMs, uint32_t runMs, Run&& run)
    {
        std::unique_lock lk{ _mtx };
        // A good answer still young enough is the answer: a burst of /status costs one run.
        if (slot.last.ok && slot.lastTick != 0 && TickMs() - slot.lastTick < cacheMs)
        {
            Read cached = slot.last;
            cached.cached = true;
            return cached;
        }
        if (slot.running)
        {
            // Another caller's run is in flight: wait for ITS answer instead of starting a second child. Bounded by that run's
            // own time limit (plus the child's end and a margin), so a runner that never comes back cannot hold a report forever.
            const uint64_t serial = slot.serial;
            if (_done.wait_for(lk, std::chrono::milliseconds{ static_cast<int64_t>(runMs) + 15000 }, [&] { return slot.serial != serial; }))
            {
                return slot.last;
            }
            Read stuck;
            stuck.error = L"an earlier read is still running";
            return stuck;
        }
        slot.running = true;
        lk.unlock();
        Read fresh;
        try
        {
            fresh = run();
        }
        catch (...)
        {
            // Rule #18. Recovery: this read fails (the next call runs again); `running` is cleared below on this path too, so the
            // slot never stays taken.
            LogSwallowedException(L"ClaudeStatusProbe::_read");
            fresh = Read{};
            fresh.error = L"the read failed (hooks.log says why)";
        }
        fresh.readAtMs = WallMs();
        lk.lock();
        slot.running = false;
        ++slot.serial;
        try
        {
            slot.last = fresh;
            slot.lastTick = TickMs();
        }
        catch (...)
        {
            // std::bad_alloc copying the answer: the slot keeps its older answer (a waiter gets that one); this caller still
            // returns its own fresh answer below.
            LogSwallowedException(L"ClaudeStatusProbe::_read (store)");
        }
        _done.notify_all();
        return fresh;
    }

    ClaudeAccountRead ClaudeStatusProbe::Account(const std::wstring& claudeExe)
    {
        if (claudeExe.empty())
        {
            ClaudeAccountRead none;
            none.error = L"Claude Code was not found on this PC";
            return none;
        }
        return _read(_account, kClaudeAccountCacheMs, kClaudeAccountTimeoutMs, [&]() {
            ClaudeAccountRead r;
            // The default setting sources: the account rows include what the user's settings add (an Anthropic base URL set
            // there, as the Status tab shows it). `auth status` starts no session, so no hook and no MCP server runs.
            const ClaudeCaptureResult c = RunClaudeCapture(claudeExe, L"auth status --text", AgentmasterStateDir(), kClaudeAccountTimeoutMs);
            r.lines = c.started && !c.timedOut ? ParseClaudeStatusLines(c.out) : std::vector<ClaudeStatusLine>{};
            // Lines are the answer whatever the exit code: "Not logged in" is Claude Code's own statement, worth showing as is.
            r.ok = !r.lines.empty();
            if (!r.ok)
            {
                r.error = CaptureFailureText(c, L"auth status", kClaudeAccountTimeoutMs);
            }
            LogStatus(L"account: " + (r.ok ? std::to_wstring(r.lines.size()) + L" line(s)" : r.error) + L" in " + std::to_wstring(c.tookMs) + L" ms (exit " +
                      std::to_wstring(c.exitCode) + L")");
            return r;
        });
    }

    ClaudeUsageRead ClaudeStatusProbe::Usage(const std::wstring& claudeExe)
    {
        if (claudeExe.empty())
        {
            ClaudeUsageRead none;
            none.error = L"Claude Code was not found on this PC";
            return none;
        }
        // The version gate and the latch come BEFORE the single flight: an answer they give costs no child and is never cached.
        const std::optional<ClaudeFileVersion> version = ReadClaudeFileVersion(claudeExe);
        if (!version || !ClaudeVersionRunsHeadlessUsage(version->major, version->minor, version->build))
        {
            ClaudeUsageRead refused;
            refused.error = version ? L"plan usage needs Claude Code 2.1.310 or newer on this PC (it has " + std::to_wstring(version->major) + L"." +
                                          std::to_wstring(version->minor) + L"." + std::to_wstring(version->build) + L")" :
                                      std::wstring{ L"this Claude Code's version could not be read, and only 2.1.310 or newer prints its usage without a model" };
            return refused;
        }
        const std::wstring identity = ExeIdentity(claudeExe);
        {
            std::lock_guard lk{ _mtx };
            if (!_usageRefusedExe.empty() && _usageRefusedExe == identity)
            {
                ClaudeUsageRead refused;
                refused.error = L"this Claude Code answered /usage with a model turn earlier, so it is not asked again until it is updated";
                return refused;
            }
        }
        return _read(_usage, kClaudeUsageCacheMs, kClaudeUsageTimeoutMs, [&]() {
            ClaudeUsageRead r;
            // See the file header for each flag. The command is the prompt ARGUMENT; standard input is NUL.
            std::wstring args = L"-p /usage --output-format json --no-session-persistence --strict-mcp-config --tools " + QuoteArg(L"") +
                                L" --setting-sources " + QuoteArg(L"") + L" --settings " + QuoteArg(LR"({"verbose":false})");
            const ClaudeCaptureResult c = RunClaudeCapture(claudeExe, args, AgentmasterStateDir(), kClaudeUsageTimeoutMs);
            if (!c.started || c.timedOut)
            {
                r.error = CaptureFailureText(c, L"/usage", kClaudeUsageTimeoutMs);
            }
            else
            {
                const ClaudeUsageAnswer a = ParseClaudeUsageAnswer(c.out);
                r.ok = a.ok;
                r.text = a.text;
                if (!a.ok)
                {
                    r.error = a.error.empty() ? CaptureFailureText(c, L"/usage", kClaudeUsageTimeoutMs) : a.error;
                    if (c.exitCode != 0 && a.text.empty())
                    {
                        r.error = CaptureFailureText(c, L"/usage", kClaudeUsageTimeoutMs);
                    }
                }
                if (a.modelTurn)
                {
                    // Never again for this claude.exe in this run: each ask would be a model call (the latch is per file, so
                    // an updated claude is asked once more). The lock is free here: _read runs this outside it.
                    {
                        std::lock_guard lk{ _mtx };
                        _usageRefusedExe = identity;
                    }
                    LogStatus(L"usage: \x26A0 this claude.exe answered /usage with a MODEL turn; it is not asked again until it changes (" + identity + L")");
                }
            }
            LogStatus(L"usage: " + (r.ok ? std::to_wstring(r.text.size()) + L" chars" : r.error) + L" in " + std::to_wstring(c.tookMs) + L" ms (exit " +
                      std::to_wstring(c.exitCode) + L")");
            return r;
        });
    }

    bool ClaudeStatusProbe::TryBeginReport() noexcept
    {
        std::lock_guard lk{ _mtx };
        if (_reports >= kClaudeStatusMaxReports)
        {
            return false;
        }
        ++_reports;
        return true;
    }

    void ClaudeStatusProbe::EndReport() noexcept
    {
        std::lock_guard lk{ _mtx };
        if (_reports > 0)
        {
            --_reports;
        }
    }

    ClaudeStatusProbe& SharedClaudeStatusProbe()
    {
        // Created on first use and never destroyed: a report worker may still be inside it when the app quits (TerminateProcess
        // runs no destructor anyway, and the m5 suite's normal exit must not destroy a mutex a detached thread could hold).
        static ClaudeStatusProbe* const probe = new ClaudeStatusProbe();
        return *probe;
    }
}
