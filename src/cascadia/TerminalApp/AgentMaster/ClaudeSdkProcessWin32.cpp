// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the Windows process backend of the Claude Agent SDK (SDK_HARNESS.md §3).
//
// The SDK (refs/claude-agent-sdk-cpp, 0xeb/claude-agent-sdk-cpp) starts Claude Code as a child
// process through a small internal interface: claude::subprocess::Process, ReadPipe, WritePipe and
// find_executable, declared in its src/internal/subprocess/process.hpp. The SDK's own Windows
// implementation of that interface (src/internal/subprocess/process_win32.cpp) is NOT compiled into
// Agentmaster. This file implements the SAME interface instead, because the SDK's implementation
// has three defects that a long-lived conversation inside a GUI application cannot accept:
//
//   1. A BUSY LOOP. Its ReadPipe::has_data ignores the timeout and returns at once (one
//      PeekNamedPipe). The SDK's transport reads the child's output with
//      `while (running) { if (!has_data(100)) continue; ... }`, so one thread uses 100% of a CPU
//      core for as long as the child lives (measured with the SDK's own file on this machine; see
//      SDK_HARNESS.md §3). Here has_data WAITS for data: the read end is an OVERLAPPED named pipe,
//      and the wait is a WaitForSingleObject on the pending read's event.
//   2. ANSI ONLY. It calls CreateProcessA and GetEnvironmentStringsA, so every path, argument and
//      environment value is converted through the system code page, and a character outside that
//      code page is lost. Here every string is UTF-16 at the OS boundary (see "Text encoding").
//   3. NO WAY TO REMOVE A VARIABLE. Its environment block is the parent's variables plus
//      overrides. A harness child must not inherit the variables of a parent Claude Code session
//      (CLAUDECODE, CLAUDE_CODE_SESSION_ID, ...) or of a terminal tab (WT_SESSION). Here an
//      override with an EMPTY value removes the variable from the child's block.
//
// Text encoding — the contract of every std::string that enters this file:
//   - The SDK holds text as std::string. JSON payloads and the arguments it builds from them
//     (--mcp-config, --settings, a system prompt) are UTF-8.
//   - The SDK validates ONE string with std::filesystem before it reaches us: the executable path
//     (`fs::exists(cli_path)`), which the C++ library decodes in the ANSI code page.
//   - So each string is decoded as UTF-8 when it is VALID UTF-8, and in the ANSI code page
//     otherwise (FromSdkText). ASCII is the same in both. A caller that must pass a non-ASCII
//     executable path passes it in the ANSI code page (see NarrowPathForSdk in Harness.cpp); every
//     other string is passed as UTF-8.
//
// Other deliberate differences from the SDK's file:
//   - The child is created SUSPENDED, put into a kill-on-close job object, and then resumed, so a
//     child can never outlive Agentmaster, not even when Agentmaster is terminated between the two
//     calls. (Agentmaster ends with TerminateProcess, so no atexit handler ever runs — the job
//     object is the ONLY cleanup that works; see the unity file's note on std::atexit.)
//   - WritePipe::flush does nothing. A Win32 pipe has no user-mode buffer, and FlushFileBuffers on
//     a pipe BLOCKS until the other side has read every byte — a hang when the child stops reading.
//   - A batch file (.cmd / .bat) is refused. Agentmaster launches the native claude.exe only
//     (CLAUDE.md, "Native-exe-only policy").
//
// The interface is INTERNAL to the SDK. It is safe to implement here only because the SDK is a
// submodule pinned to one commit: after every move of the pin, compare process.hpp and
// process_win32.cpp with this file (SDK_HARNESS.md §9 lists the steps).
//
// Plain C++ + Win32, no WinRT (the vcxproj marks it <PrecompiledHeader>NotUsing</PrecompiledHeader>).

#include "internal/subprocess/process.hpp" // the SDK's interface (include path: refs/claude-agent-sdk-cpp/src)

#include "ClaudeSpawn.h" // AppendStateLog — [harness-proc] lines in hooks.log

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // How many bytes one overlapped read asks for. One read-ahead buffer per pipe; a Claude Code
    // stream-json line is usually well below this, and a larger line simply takes several reads.
    constexpr DWORD kReadAheadBytes = 64 * 1024;

    // The size of the child's stdin pipe. The default (4 KiB) fills up with ONE large tool result,
    // and a write to a full pipe blocks until the child reads. 1 MiB keeps every normal message a
    // non-blocking write.
    constexpr DWORD kStdinPipeBytes = 1024 * 1024;

    // Decode a string that came from the SDK: UTF-8 when the bytes are valid UTF-8, the ANSI code
    // page otherwise (see "Text encoding" in the file header).
    //   text    — the SDK's bytes; may be empty.
    //   returns — the UTF-16 text; empty for empty input or when both decodes fail.
    //   throws  — std::bad_alloc.
    std::wstring FromSdkText(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int len = static_cast<int>(text.size());
        // MB_ERR_INVALID_CHARS makes the UTF-8 decode FAIL on an invalid sequence instead of
        // substituting U+FFFD, which is what lets ANSI bytes fall through to the second decode.
        int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), len, nullptr, 0);
        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (n <= 0)
        {
            codePage = CP_ACP;
            flags = 0;
            n = ::MultiByteToWideChar(codePage, flags, text.data(), len, nullptr, 0);
            if (n <= 0)
            {
                return {};
            }
        }
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(codePage, flags, text.data(), len, out.data(), n);
        return out;
    }

    // UTF-16 -> UTF-8, for error messages and log lines that carry OS text.
    //   returns — the UTF-8 bytes; empty for empty input or on a conversion failure.
    //   throws  — std::bad_alloc.
    std::string ToUtf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0)
        {
            return {};
        }
        std::string out(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    // The system's text for a Win32 error code, as UTF-8, without the trailing line break.
    //   error   — a GetLastError value.
    //   returns — e.g. "The system cannot find the file specified. (2)"; never empty.
    //   throws  — std::bad_alloc.
    std::string Win32ErrorText(DWORD error)
    {
        wchar_t* buffer = nullptr;
        const DWORD size = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<wchar_t*>(&buffer),
            0,
            nullptr);
        std::wstring message = (buffer && size) ? std::wstring(buffer, size) : std::wstring{ L"unknown error" };
        if (buffer)
        {
            ::LocalFree(buffer);
        }
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
        {
            message.pop_back();
        }
        return ToUtf8(message) + " (" + std::to_string(error) + ")";
    }

    // Quote one command-line argument so that the Microsoft C runtime's parser (and every parser
    // that follows the same rules, CommandLineToArgvW included) gives the child exactly `arg`.
    // The rules: backslashes are literal UNLESS they come before a double quote; 2n backslashes
    // + a quote = n backslashes and the quote ends/starts the quoted part; 2n+1 backslashes + a
    // quote = n backslashes and a literal quote.
    //   arg     — the argument's exact text; may be empty or hold quotes, backslashes, line breaks.
    //   returns — the text to put on the command line.
    //   throws  — std::bad_alloc.
    // (The SDK's own quoting doubles EVERY backslash inside a quoted argument, which changes an
    // argument such as a JSON string that holds a Windows path.)
    std::wstring QuoteArgument(std::wstring_view arg)
    {
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        {
            return std::wstring{ arg }; // nothing the parser treats specially
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
            if (c == L'"')
            {
                out.append(backslashes * 2 + 1, L'\\'); // escape the run AND the quote
            }
            else
            {
                out.append(backslashes, L'\\'); // a run before an ordinary character is literal
            }
            backslashes = 0;
            out.push_back(c);
        }
        out.append(backslashes * 2, L'\\'); // a trailing run comes before OUR closing quote
        out.push_back(L'"');
        return out;
    }

    // Orders environment variable names the way Windows does: case-insensitive, ordinal.
    struct EnvNameLess
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const noexcept
        {
            return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
        }
    };

    // Build the child's UTF-16 environment block.
    //   inherit   — true: start from this process's variables; false: start empty.
    //   overrides — the SDK's (name -> value) map. A non-empty value sets the variable; an EMPTY
    //               value REMOVES it (this backend's convention — see the file header, defect 3).
    //   returns   — "NAME=VALUE\0...\0\0", sorted by name; at least the terminating "\0\0".
    //   throws    — std::bad_alloc.
    std::vector<wchar_t> BuildEnvironmentBlock(bool inherit, const std::map<std::string, std::string>& overrides)
    {
        std::map<std::wstring, std::wstring, EnvNameLess> vars;
        if (inherit)
        {
            if (wchar_t* const block = ::GetEnvironmentStringsW())
            {
                for (const wchar_t* p = block; *p != L'\0';)
                {
                    const std::wstring_view entry{ p };
                    // The name may START with '=' (the per-drive current-directory entries cmd.exe
                    // keeps, e.g. "=C:=C:\dir"), so the separator is the first '=' AFTER index 0.
                    const size_t eq = entry.find(L'=', 1);
                    if (eq != std::wstring_view::npos)
                    {
                        vars[std::wstring{ entry.substr(0, eq) }] = std::wstring{ entry.substr(eq + 1) };
                    }
                    p += entry.size() + 1;
                }
                ::FreeEnvironmentStringsW(block);
            }
        }
        for (const auto& [name, value] : overrides)
        {
            const std::wstring wideName = FromSdkText(name);
            if (wideName.empty())
            {
                continue;
            }
            if (value.empty())
            {
                vars.erase(wideName); // empty == "the child must not have this variable"
            }
            else
            {
                vars[wideName] = FromSdkText(value);
            }
        }
        std::vector<wchar_t> block;
        for (const auto& [name, value] : vars)
        {
            block.insert(block.end(), name.begin(), name.end());
            block.push_back(L'=');
            block.insert(block.end(), value.begin(), value.end());
            block.push_back(L'\0');
        }
        if (block.empty())
        {
            block.push_back(L'\0'); // an empty block still needs its double terminator
        }
        block.push_back(L'\0');
        return block;
    }

    // The ONE job object every SDK child is put into. JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE ends the
    // children when the last handle to the job closes, which happens when this process ends — by a
    // normal exit, by TerminateProcess (how Agentmaster always ends) or by a crash.
    //   returns — the job handle, or nullptr when the job could not be created (the child then
    //             runs outside a job; the caller logs it).
    HANDLE ChildJob() noexcept
    {
        // A function-local static: created once, on first use, thread-safe. Never closed on
        // purpose — closing it is exactly the "kill every child" action.
        static const HANDLE job = []() noexcept -> HANDLE {
            const HANDLE h = ::CreateJobObjectW(nullptr, nullptr);
            if (h)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                ::SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
            }
            return h;
        }();
        return job;
    }

    // One [harness-proc] line in hooks.log. Never throws: a logging failure must not turn a
    // contained process error into a new one.
    void LogProc(const std::wstring& message) noexcept
    {
        try
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[harness-proc] " + message + L"\n");
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (CLAUDE.md Rule #18, the logging-path exemption): this IS the
            // logging path; reporting its own failure would call AppendStateLog again.
        }
    }

    // Closes a handle on scope exit unless it was released. For the child-side pipe ends and the
    // other temporaries of Process::spawn, so every early throw closes what was opened so far.
    struct ScopedHandle
    {
        HANDLE h{ INVALID_HANDLE_VALUE };
        ScopedHandle() = default;
        explicit ScopedHandle(HANDLE handle) noexcept :
            h{ handle } {}
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;
        ~ScopedHandle() { reset(); }
        bool valid() const noexcept { return h != INVALID_HANDLE_VALUE && h != nullptr; }
        void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        {
            if (valid())
            {
                ::CloseHandle(h);
            }
            h = handle;
        }
        HANDLE release() noexcept
        {
            const HANDLE out = h;
            h = INVALID_HANDLE_VALUE;
            return out;
        }
    };

    std::atomic<unsigned long> g_pipeSerial{ 0 }; // makes each named pipe's name unique in this process
}

namespace claude
{
    namespace subprocess
    {
        // The parent's end of one pipe. The SDK declares this type and leaves its definition to
        // the platform file; ReadPipe and WritePipe each own one through a unique_ptr.
        //
        // For a READ pipe it is also the read-ahead state: at most ONE overlapped ReadFile is in
        // flight, into `buffer`; `event` is signaled when it completes. The OVERLAPPED and the
        // buffer live INSIDE this heap object, so they keep their address when the owning
        // ReadPipe is moved, and the destructor cancels and WAITS for a pending read before the
        // memory goes away (the kernel writes into both until the read has completed).
        //
        // Not thread-safe: the SDK calls a pipe from one thread at a time (the transport's reader
        // thread for stdout, its stderr thread for stderr, and writes under the transport's mutex).
        struct PipeHandle
        {
            HANDLE handle = INVALID_HANDLE_VALUE; // the parent's end; INVALID_HANDLE_VALUE == closed
            HANDLE event = nullptr; // read pipes only: manual-reset, signaled when the pending read completed
            OVERLAPPED overlapped{}; // read pipes only: the pending read's state (hEvent == event)
            std::vector<char> buffer; // read pipes only: the read-ahead bytes
            size_t position = 0; // next unread byte in buffer
            size_t available = 0; // bytes of buffer that hold data
            bool pending = false; // an overlapped ReadFile is in flight
            bool eof = false; // the writer closed its end (or the read failed for good)
            DWORD lastError = ERROR_SUCCESS; // the error that set `eof`, when it was not a clean close

            PipeHandle() = default;
            PipeHandle(const PipeHandle&) = delete;
            PipeHandle& operator=(const PipeHandle&) = delete;
            ~PipeHandle() { Close(); }

            // Close the parent's end. A pending read is cancelled and waited for first.
            void Close() noexcept
            {
                if (handle != INVALID_HANDLE_VALUE)
                {
                    if (pending)
                    {
                        ::CancelIoEx(handle, &overlapped);
                        DWORD ignored = 0;
                        // bWait TRUE: the kernel may still write into `overlapped` and `buffer`
                        // until the cancelled read has completed.
                        ::GetOverlappedResult(handle, &overlapped, &ignored, TRUE);
                        pending = false;
                    }
                    ::CloseHandle(handle);
                    handle = INVALID_HANDLE_VALUE;
                }
                if (event)
                {
                    ::CloseHandle(event);
                    event = nullptr;
                }
            }

            // Record the result of a completed read.
            //   ok    — the read's success flag (ReadFile's or GetOverlappedResult's).
            //   bytes — the byte count it reported.
            //   error — GetLastError when !ok.
            void Complete(bool ok, DWORD bytes, DWORD error) noexcept
            {
                pending = false;
                position = 0;
                if (ok && bytes > 0)
                {
                    available = bytes;
                    return;
                }
                available = 0;
                eof = true;
                // A closed writer reports ERROR_BROKEN_PIPE (or a 0-byte success): a clean end.
                // Anything else is kept so read() can report it.
                lastError = (ok || error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_OPERATION_ABORTED) ? ERROR_SUCCESS : error;
            }

            // Start an overlapped read when none is in flight and the buffer is empty.
            // Never allocates: Process::spawn sized `buffer` when it created the pipe.
            void StartRead() noexcept
            {
                if (pending || eof || position < available || handle == INVALID_HANDLE_VALUE)
                {
                    return;
                }
                if (buffer.size() != kReadAheadBytes || !event)
                {
                    // Not a pipe that spawn() set up for reading. Report the end of the stream
                    // with an error instead of reading into a buffer that is not there.
                    Complete(false, 0, ERROR_INVALID_HANDLE);
                    return;
                }
                ::ResetEvent(event);
                overlapped = OVERLAPPED{};
                overlapped.hEvent = event;
                DWORD bytes = 0;
                if (::ReadFile(handle, buffer.data(), kReadAheadBytes, &bytes, &overlapped))
                {
                    Complete(true, bytes, ERROR_SUCCESS); // data was already in the pipe
                    return;
                }
                const DWORD error = ::GetLastError();
                if (error == ERROR_IO_PENDING)
                {
                    pending = true;
                    return;
                }
                Complete(false, 0, error);
            }

            // Wait for the pending read.
            //   timeoutMs — INFINITE, or how long to wait.
            //   returns   — true when no read is pending any more (data, end of stream or error).
            bool WaitRead(DWORD timeoutMs) noexcept
            {
                if (!pending)
                {
                    return true;
                }
                if (::WaitForSingleObject(event, timeoutMs) != WAIT_OBJECT_0)
                {
                    return false;
                }
                DWORD bytes = 0;
                const BOOL ok = ::GetOverlappedResult(handle, &overlapped, &bytes, FALSE);
                Complete(ok != FALSE, bytes, ok ? ERROR_SUCCESS : ::GetLastError());
                return true;
            }
        };

        // The child process. The SDK declares this type and leaves its definition to the platform file.
        struct ProcessHandle
        {
            HANDLE process = nullptr; // nullptr until spawn succeeded
            DWORD processId = 0;
            bool running = false; // true from a successful spawn until a wait saw the exit
            int exitCode = -1; // valid once running went false after a spawn

            ProcessHandle() = default;
            ProcessHandle(const ProcessHandle&) = delete;
            ProcessHandle& operator=(const ProcessHandle&) = delete;
            ~ProcessHandle()
            {
                if (process)
                {
                    ::CloseHandle(process);
                }
            }
        };

        // ===== ReadPipe =====================================================================

        ReadPipe::ReadPipe() :
            handle_(std::make_unique<PipeHandle>())
        {
        }

        ReadPipe::~ReadPipe()
        {
            close();
        }

        ReadPipe::ReadPipe(ReadPipe&& other) noexcept = default;
        ReadPipe& ReadPipe::operator=(ReadPipe&& other) noexcept = default;

        // Read up to `size` bytes, blocking until at least one byte or the end of the stream.
        //   buffer  — where the bytes go; at least `size` bytes.
        //   size    — the most bytes to return; 0 returns 0 at once.
        //   returns — the byte count; 0 == end of stream (the child closed its end or exited).
        //   throws  — std::runtime_error when the pipe is not open, or when the read failed with
        //             an error other than a closed pipe.
        size_t ReadPipe::read(char* buffer, size_t size)
        {
            if (!is_open())
            {
                throw std::runtime_error("Pipe is not open");
            }
            if (size == 0)
            {
                return 0;
            }
            PipeHandle& h = *handle_;
            for (;;)
            {
                if (h.position < h.available)
                {
                    const size_t n = (std::min)(size, h.available - h.position);
                    std::memcpy(buffer, h.buffer.data() + h.position, n);
                    h.position += n;
                    return n;
                }
                if (h.eof)
                {
                    if (h.lastError != ERROR_SUCCESS)
                    {
                        throw std::runtime_error("Read failed: " + Win32ErrorText(h.lastError));
                    }
                    return 0;
                }
                h.StartRead();
                h.WaitRead(INFINITE);
            }
        }

        // Read one line, the line break included. Same contract as the SDK's implementation.
        //   max_size — stop after this many bytes even without a line break.
        //   returns  — the bytes read; empty == end of stream.
        //   throws   — what read() throws; std::bad_alloc.
        std::string ReadPipe::read_line(size_t max_size)
        {
            std::string line;
            line.reserve(256);
            char ch = '\0';
            while (line.size() < max_size)
            {
                if (read(&ch, 1) == 0)
                {
                    break; // end of stream
                }
                line.push_back(ch);
                if (ch == '\n')
                {
                    break;
                }
            }
            return line;
        }

        // Wait until a read() would return without blocking.
        //   timeout_ms — how long to wait; 0 or less == look only.
        //   returns    — true when data is buffered OR the stream has ended (so the caller's next
        //                read() returns 0 and its loop can stop); false when the time ran out.
        // ⚠ At the end of the stream the call still WAITS for timeout_ms before it answers true.
        //   The SDK's stderr loop does `if (line.empty() && !running) break; ... continue;`, so an
        //   immediate "true" there would spin on a child that closed stderr but still runs.
        // Never throws (the SDK's loops do not expect it to).
        bool ReadPipe::has_data(int timeout_ms)
        {
            if (!is_open())
            {
                return false;
            }
            PipeHandle& h = *handle_;
            if (h.position < h.available)
            {
                return true;
            }
            const DWORD wait = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 0;
            if (h.eof)
            {
                if (wait > 0)
                {
                    ::Sleep(wait);
                }
                return true;
            }
            h.StartRead();
            if (!h.WaitRead(wait))
            {
                return false;
            }
            return h.position < h.available || h.eof;
        }

        void ReadPipe::close()
        {
            if (handle_)
            {
                handle_->Close();
            }
        }

        bool ReadPipe::is_open() const
        {
            return handle_ && handle_->handle != INVALID_HANDLE_VALUE;
        }

        // ===== WritePipe ====================================================================

        WritePipe::WritePipe() :
            handle_(std::make_unique<PipeHandle>())
        {
        }

        WritePipe::~WritePipe()
        {
            close();
        }

        WritePipe::WritePipe(WritePipe&& other) noexcept = default;
        WritePipe& WritePipe::operator=(WritePipe&& other) noexcept = default;

        // Write every byte. Blocks only while the pipe (1 MiB) is full, that is, while the child
        // does not read its input.
        //   data, size — the bytes.
        //   returns    — `size`.
        //   throws     — std::runtime_error when the pipe is not open or the write failed (the
        //                usual cause: the child exited).
        size_t WritePipe::write(const char* data, size_t size)
        {
            if (!is_open())
            {
                throw std::runtime_error("Pipe is not open");
            }
            size_t done = 0;
            while (done < size)
            {
                // WriteFile takes a DWORD count; a message is far below 4 GiB, but the loop keeps
                // the function correct for any size and for a short write.
                const DWORD chunk = static_cast<DWORD>((std::min)(size - done, static_cast<size_t>(0x40000000)));
                DWORD written = 0;
                if (!::WriteFile(handle_->handle, data + done, chunk, &written, nullptr))
                {
                    throw std::runtime_error("Write failed: " + Win32ErrorText(::GetLastError()));
                }
                done += written;
            }
            return done;
        }

        size_t WritePipe::write(const std::string& data)
        {
            return write(data.data(), data.size());
        }

        // Nothing to flush: a Win32 pipe write goes straight into the pipe. (FlushFileBuffers on a
        // pipe waits until the reader has read everything — see the file header.)
        void WritePipe::flush()
        {
        }

        void WritePipe::close()
        {
            if (handle_)
            {
                handle_->Close();
            }
        }

        bool WritePipe::is_open() const
        {
            return handle_ && handle_->handle != INVALID_HANDLE_VALUE;
        }

        // ===== Process ======================================================================

        Process::Process() :
            handle_(std::make_unique<ProcessHandle>())
        {
        }

        Process::~Process()
        {
            // Same rule as the SDK's file: a Process object never leaves its child running.
            if (handle_ && is_running())
            {
                terminate();
                wait();
            }
        }

        Process::Process(Process&& other) noexcept = default;
        Process& Process::operator=(Process&& other) noexcept = default;

        // Start `executable` with `args`, its standard handles connected as `options` asks.
        //   executable — the full path of a native .exe (see "Text encoding" for its encoding).
        //   args       — the arguments, without the program name; each is passed EXACTLY.
        //   options    — working directory, environment (an empty override value removes the
        //                variable), and which of stdin / stdout / stderr get a pipe. A stream
        //                without a pipe is connected to NUL (the child is windowless).
        //   throws     — std::runtime_error with the Win32 error text when a pipe or the process
        //                cannot be created, or when `executable` is a batch file; std::bad_alloc.
        //                Nothing is left open or running after a throw.
        void Process::spawn(const std::string& executable, const std::vector<std::string>& args, const ProcessOptions& options)
        {
            const std::wstring exe = FromSdkText(executable);
            if (exe.empty())
            {
                throw std::runtime_error("Failed to create process: empty executable path");
            }
            {
                // CreateProcess cannot run a batch file by lpApplicationName, and running one
                // through cmd.exe would re-parse every argument. Agentmaster never needs it.
                const std::wstring ext = std::filesystem::path{ exe }.extension().wstring();
                if (::CompareStringOrdinal(ext.c_str(), -1, L".cmd", -1, TRUE) == CSTR_EQUAL || ::CompareStringOrdinal(ext.c_str(), -1, L".bat", -1, TRUE) == CSTR_EQUAL)
                {
                    throw std::runtime_error("Failed to create process: a batch launcher is not supported; pass the native claude.exe (" + ToUtf8(exe) + ")");
                }
            }

            SECURITY_ATTRIBUTES inheritable{};
            inheritable.nLength = sizeof(inheritable);
            inheritable.bInheritHandle = TRUE;

            // The child-side ends. ScopedHandle closes them on every exit from this function: the
            // parent must not keep them, or the pipes would never report "the child closed it".
            ScopedHandle childStdin, childStdout, childStderr;
            auto parentStdin = std::make_unique<WritePipe>();
            auto parentStdout = std::make_unique<ReadPipe>();
            auto parentStderr = std::make_unique<ReadPipe>();

            if (options.redirect_stdin)
            {
                HANDLE readEnd = nullptr, writeEnd = nullptr;
                if (!::CreatePipe(&readEnd, &writeEnd, &inheritable, kStdinPipeBytes))
                {
                    throw std::runtime_error("Failed to create stdin pipe: " + Win32ErrorText(::GetLastError()));
                }
                childStdin.reset(readEnd);
                parentStdin->handle_->handle = writeEnd;
                ::SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0); // the parent's end stays here
            }

            // A read pipe the parent can WAIT on: a named pipe whose server (parent) end is
            // overlapped. An anonymous pipe (CreatePipe) cannot be opened overlapped.
            const auto makeReadPipe = [&inheritable](ReadPipe& parentEnd, ScopedHandle& childEnd, const char* what) {
                wchar_t name[96]{};
                ::swprintf_s(name, L"\\\\.\\pipe\\agentmaster-sdk.%lu.%lu.%llu", ::GetCurrentProcessId(), g_pipeSerial.fetch_add(1) + 1, static_cast<unsigned long long>(::GetTickCount64()));
                // FIRST_PIPE_INSTANCE + one instance: nobody else can own or join this name.
                // REJECT_REMOTE_CLIENTS: local only. Inbound: the server only reads.
                const HANDLE server = ::CreateNamedPipeW(
                    name,
                    PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    1,
                    0,
                    kReadAheadBytes,
                    0,
                    nullptr);
                if (server == INVALID_HANDLE_VALUE)
                {
                    throw std::runtime_error(std::string{ "Failed to create " } + what + " pipe: " + Win32ErrorText(::GetLastError()));
                }
                parentEnd.handle_->handle = server; // owned from here on (closed by the ReadPipe)
                parentEnd.handle_->buffer.assign(kReadAheadBytes, '\0'); // the read-ahead buffer; StartRead never allocates
                // The child's end: an ordinary synchronous handle, inheritable. Opening it also
                // connects the pipe, so no ConnectNamedPipe call is needed.
                const HANDLE client = ::CreateFileW(name, GENERIC_WRITE, 0, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (client == INVALID_HANDLE_VALUE)
                {
                    throw std::runtime_error(std::string{ "Failed to open " } + what + " pipe: " + Win32ErrorText(::GetLastError()));
                }
                childEnd.reset(client);
                parentEnd.handle_->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!parentEnd.handle_->event)
                {
                    throw std::runtime_error(std::string{ "Failed to create " } + what + " event: " + Win32ErrorText(::GetLastError()));
                }
            };
            if (options.redirect_stdout)
            {
                makeReadPipe(*parentStdout, childStdout, "stdout");
            }
            if (options.redirect_stderr)
            {
                makeReadPipe(*parentStderr, childStderr, "stderr");
            }

            // A stream without a pipe goes to NUL: the child has no console window, so a handle
            // inherited from a GUI parent would be invalid.
            ScopedHandle nulIn, nulOut;
            if (!options.redirect_stdin)
            {
                nulIn.reset(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            }
            if (!options.redirect_stdout || !options.redirect_stderr)
            {
                nulOut.reset(::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            }

            STARTUPINFOEXW si{};
            si.StartupInfo.cb = sizeof(si);
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = options.redirect_stdin ? childStdin.h : nulIn.h;
            si.StartupInfo.hStdOutput = options.redirect_stdout ? childStdout.h : nulOut.h;
            si.StartupInfo.hStdError = options.redirect_stderr ? childStderr.h : nulOut.h;

            // Inherit EXACTLY these handles. Without the list, bInheritHandles would hand the child
            // every inheritable handle of this process, whatever thread created it.
            std::vector<HANDLE> inherit;
            for (const HANDLE h : { si.StartupInfo.hStdInput, si.StartupInfo.hStdOutput, si.StartupInfo.hStdError })
            {
                if (h != INVALID_HANDLE_VALUE && h != nullptr && std::find(inherit.begin(), inherit.end(), h) == inherit.end())
                {
                    inherit.push_back(h); // a handle may be listed once only
                }
            }
            SIZE_T attributeBytes = 0;
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            std::vector<BYTE> attributeStorage(attributeBytes);
            const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
            if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
            {
                throw std::runtime_error("Failed to init attribute list: " + Win32ErrorText(::GetLastError()));
            }
            struct AttributeListCleanup
            {
                LPPROC_THREAD_ATTRIBUTE_LIST list;
                ~AttributeListCleanup() { ::DeleteProcThreadAttributeList(list); }
            } attributeCleanup{ attributes };
            if (!inherit.empty() && !::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(), inherit.size() * sizeof(HANDLE), nullptr, nullptr))
            {
                throw std::runtime_error("Failed to update attribute list: " + Win32ErrorText(::GetLastError()));
            }
            si.lpAttributeList = attributes;

            // The command line: the program name, then each argument quoted for exact delivery.
            std::wstring commandLine = L"\"" + exe + L"\"";
            for (const auto& arg : args)
            {
                commandLine.push_back(L' ');
                commandLine += QuoteArgument(FromSdkText(arg));
            }

            // The environment. With nothing to change, pass nullptr: the child then inherits this
            // process's block as it is.
            std::vector<wchar_t> environment;
            const bool customEnvironment = !options.inherit_environment || !options.environment.empty();
            if (customEnvironment)
            {
                environment = BuildEnvironmentBlock(options.inherit_environment, options.environment);
            }
            const std::wstring workingDirectory = FromSdkText(options.working_directory);

            // CREATE_NO_WINDOW: Claude Code is a console program; a GUI parent would otherwise get
            // a console window. CREATE_SUSPENDED: the child must be in the job BEFORE it runs.
            PROCESS_INFORMATION pi{};
            const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
            const BOOL created = ::CreateProcessW(
                exe.c_str(),
                commandLine.data(), // CreateProcessW may write to this buffer
                nullptr,
                nullptr,
                TRUE,
                flags,
                customEnvironment ? environment.data() : nullptr,
                workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                &si.StartupInfo,
                &pi);
            if (!created)
            {
                const DWORD error = ::GetLastError();
                LogProc(L"spawn FAILED: " + exe + L" (" + FromSdkText(Win32ErrorText(error)) + L")");
                throw std::runtime_error("Failed to create process: " + Win32ErrorText(error));
            }

            if (const HANDLE job = ChildJob(); !job || !::AssignProcessToJobObject(job, pi.hProcess))
            {
                // The child still works; it just is not ended automatically with this process.
                LogProc(L"pid " + std::to_wstring(pi.dwProcessId) + L" is NOT in the kill-on-close job (" + FromSdkText(Win32ErrorText(::GetLastError())) + L")");
            }
            ::ResumeThread(pi.hThread);
            ::CloseHandle(pi.hThread);

            handle_->process = pi.hProcess;
            handle_->processId = pi.dwProcessId;
            handle_->running = true;
            handle_->exitCode = -1;
            if (options.redirect_stdin)
            {
                stdin_ = std::move(parentStdin);
            }
            if (options.redirect_stdout)
            {
                stdout_ = std::move(parentStdout);
            }
            if (options.redirect_stderr)
            {
                stderr_ = std::move(parentStderr);
            }
            // The ScopedHandles close the child-side ends here: the child holds its own copies.
        }

        WritePipe& Process::stdin_pipe()
        {
            if (!stdin_)
            {
                throw std::runtime_error("stdin not redirected");
            }
            return *stdin_;
        }

        ReadPipe& Process::stdout_pipe()
        {
            if (!stdout_)
            {
                throw std::runtime_error("stdout not redirected");
            }
            return *stdout_;
        }

        ReadPipe& Process::stderr_pipe()
        {
            if (!stderr_)
            {
                throw std::runtime_error("stderr not redirected");
            }
            return *stderr_;
        }

        // True while the child has not exited. One kernel call, no state change.
        bool Process::is_running() const
        {
            if (!handle_ || !handle_->running || !handle_->process)
            {
                return false;
            }
            // WAIT_TIMEOUT == still running. (GetExitCodeProcess == STILL_ACTIVE would misread a
            // child that EXITS with code 259.)
            return ::WaitForSingleObject(handle_->process, 0) == WAIT_TIMEOUT;
        }

        // The exit code when the child has exited, else nullopt. Never blocks.
        std::optional<int> Process::try_wait()
        {
            if (!handle_->running)
            {
                return handle_->exitCode;
            }
            if (::WaitForSingleObject(handle_->process, 0) != WAIT_OBJECT_0)
            {
                return std::nullopt;
            }
            return wait();
        }

        // Block until the child has exited; returns its exit code (-1 when it cannot be read).
        int Process::wait()
        {
            if (!handle_->running)
            {
                return handle_->exitCode;
            }
            ::WaitForSingleObject(handle_->process, INFINITE);
            DWORD code = 0;
            handle_->exitCode = ::GetExitCodeProcess(handle_->process, &code) ? static_cast<int>(code) : -1;
            handle_->running = false;
            return handle_->exitCode;
        }

        // End the child at once (exit code 1). Windows has no polite signal for a windowless
        // console child; the SDK closes stdin first and calls this only after its grace period.
        void Process::terminate()
        {
            if (handle_ && handle_->running && handle_->process)
            {
                ::TerminateProcess(handle_->process, 1);
            }
        }

        void Process::kill()
        {
            terminate(); // the same thing on Windows
        }

        int Process::pid() const
        {
            return handle_ ? static_cast<int>(handle_->processId) : 0;
        }

        // Find a program by name. The SDK calls this only when no explicit CLI path was given;
        // Agentmaster always gives one (Harness.cpp sets cli_path and require_explicit_cli), so
        // this is a complete but unused fallback.
        //   name    — an absolute path, or a bare name to look for in the current directory and
        //             on PATH, with the extensions .exe, .cmd, .bat and none, in that order.
        //   returns — the path in the encoding std::filesystem expects from a narrow string (the
        //             SDK validates it with fs::exists), or nullopt when nothing was found or the
        //             path found cannot be written in that encoding.
        //   throws  — std::bad_alloc.
        // ⚠ Unlike the SDK's file it prefers .exe to .cmd: spawn() refuses a batch file.
        std::optional<std::string> find_executable(const std::string& name)
        {
            namespace fs = std::filesystem;
            const auto toSdkPath = [](const fs::path& p) -> std::optional<std::string> {
                try
                {
                    return p.string(); // throws when a character has no ANSI form
                }
                catch (...)
                {
                    ::Agentmaster::LogSwallowedException(L"claude::subprocess::find_executable (a path with no ANSI form)");
                    return std::nullopt;
                }
            };
            std::error_code ec;
            const fs::path given{ FromSdkText(name) };
            if (given.is_absolute() && fs::exists(given, ec))
            {
                return toSdkPath(given);
            }
            static constexpr const wchar_t* kExtensions[] = { L".exe", L".cmd", L".bat", L"" };
            std::vector<fs::path> dirs;
            dirs.emplace_back(L"."); // the current directory first, like the SDK
            {
                std::wstring pathVar(32767, L'\0');
                const DWORD n = ::GetEnvironmentVariableW(L"PATH", pathVar.data(), static_cast<DWORD>(pathVar.size()));
                pathVar.resize(n < pathVar.size() ? n : 0);
                size_t start = 0;
                while (start <= pathVar.size())
                {
                    const size_t end = pathVar.find(L';', start);
                    const std::wstring dir = pathVar.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
                    if (!dir.empty())
                    {
                        dirs.emplace_back(dir);
                    }
                    if (end == std::wstring::npos)
                    {
                        break;
                    }
                    start = end + 1;
                }
            }
            for (const auto& dir : dirs)
            {
                for (const wchar_t* ext : kExtensions)
                {
                    const fs::path candidate = dir / (given.wstring() + ext);
                    if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec))
                    {
                        if (auto found = toSdkPath(candidate))
                        {
                            return found;
                        }
                    }
                }
            }
            return std::nullopt;
        }
    }
}
