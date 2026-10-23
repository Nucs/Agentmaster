// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the CALLER SINK: the console (or pipe) of the command that started this process, and how a GUI-subsystem
// exe writes to it (doc/agentmaster/CLI.md §2b). Header-only, plain Win32 + std (no WinRT, no wil, no engine), so the app
// exe, the console launcher shim (wt/shim.cpp) and the m5 harness all compile it.
//
// WHY: every dialog our binaries open is echoed here (DialogEcho.h), so a command that makes a dialog appear also says so
// in the console that ran it, where an agent reads its output. On 2026-10-23 two `agentmaster show a77419cf` calls each
// put a "profile in use" dialog on the user's screen, and the session that typed them saw nothing of it.
//
// WHERE THE CALLER READS (Capture, first match wins):
//   1. AGENTMASTER_CALLER_SINK, set by the package's console launcher (wt/shim.cpp) for the app it starts. The launcher
//      exits at once, so the app could not find the caller otherwise. "handle:<hex>" names a duplicate of the caller's
//      stderr (a pipe or a file) that the launcher passed by inheritance; "console:<pid>[,<pid>...]" names up to
//      kMaxConsoleCandidates processes attached to the caller's console, the launcher's parent first, and the app writes
//      through the first of them still alive. One would not do: under `cmd /c agentmaster ...` the parent is a cmd.exe
//      that exits as soon as the launcher did, before the app writes (measured 2026-10-23, the dialog-echo probe), while
//      the shell that ran that cmd.exe, attached to the same console, lives on. The variable is removed from the
//      environment at once, so no child ever sees it.
//   2. The standard handles the caller passed (STARTF_USESTDHANDLES): stderr, else stdout, but only a handle this
//      process really inherited (HANDLE_FLAG_INHERIT). Measured 2026-10-23: a console handle passed to a GUI process is
//      unusable, also when the caller made it inheritable (in the child: HANDLE_FLAG_INHERIT clear, file type 0, and
//      WriteConsoleW and WriteFile fail with error 6). So PowerShell 7 with the output piped on (`& x | Out-String`:
//      stderr = the console, stdout = its pipe) gets the lines in its pipe.
//   3. The parent's console, attached for each write (AttachConsole -> CONOUT$ -> FreeConsole). Measured: a CONOUT$
//      handle opened while attached is dead after FreeConsole, so no handle can be kept.
// A captured handle is duplicated privately (never inheritable) and an inherited original from step 1 is closed, so the
// app's own children can never hold the caller's pipe open. A console sink keeps a handle on each process it may attach
// through, which stops its process id from being reused while the sink lives.
//
// HOW LONG: the sink belongs to the command, not to the app. The app exe releases it when its startup ends
// (RequestRelease, in WindowEmperor before the message loop): at once, or as soon as the last echoed dialog was answered
// (Hold). Kept longer, a caller that reads the pipe to its end (bash `$(...)`, Claude Code's Bash tool) would wait for
// the whole life of the app.
//
// MODULE-LOCAL STATE: the state is a function-local static, so each binary has its own copy. The app exe captures and
// releases it. TerminalApp.dll's copy never holds a sink, and that loses nothing: a dialog of the DLL is the running
// app's own UI, which nothing can reach before the exe's release.

#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster::CallerSink
{
    // The variable through which the console launcher names the caller's sink for the app it starts (see the header).
    inline constexpr const wchar_t* kEnvVariable = L"AGENTMASTER_CALLER_SINK";

    // At most this many processes in a "console:" value: the launcher's parent and the other processes attached to the
    // caller's console. A console with more simply hands over the first ones; any one alive is enough.
    inline constexpr std::size_t kMaxConsoleCandidates = 8;

    // Where a captured sink writes.
    enum class Kind
    {
        None, // nothing to write to: the command had no console and passed no handle
        Handle, // a pipe or a file (the caller's stderr / stdout, or the launcher's duplicate of it)
        Console, // a console, reached by attaching to a process that is attached to it
    };

    // AGENTMASTER_CALLER_SINK decoded (ParseEnvValue).
    struct EnvSink
    {
        Kind kind{ Kind::None };
        std::uint64_t handle{}; // Kind::Handle: the inherited handle's value in THIS process
        // Kind::Console: processes attached to the caller's console, in the launcher's order (its parent first); the
        // first pidCount entries are set, each nonzero and distinct.
        std::array<DWORD, kMaxConsoleCandidates> pids{};
        std::size_t pidCount{};
    };

    // PURE: decodes AGENTMASTER_CALLER_SINK. Accepts exactly "handle:<hex>" (with or without 0x) and "console:<decimal
    // pid>" followed by up to kMaxConsoleCandidates - 1 more ",<decimal pid>"; anything else (empty, malformed, an empty
    // or zero element, a pid past 32 bits, too many pids) is Kind::None, so a stray or hostile value can only switch the
    // echo off. A pid named twice is kept once.
    //   value   — the variable's value ("" when it is not set).
    //   returns — the decoded sink.
    //   throws  — nothing.
    inline EnvSink ParseEnvValue(std::wstring_view value) noexcept
    {
        EnvSink out;
        const auto digitsOf = [](std::wstring_view s, unsigned base, std::uint64_t& number) noexcept {
            if (s.empty() || s.size() > 16)
            {
                return false;
            }
            number = 0;
            for (const wchar_t c : s)
            {
                unsigned d = 0;
                if (c >= L'0' && c <= L'9')
                {
                    d = static_cast<unsigned>(c - L'0');
                }
                else if (base == 16 && c >= L'a' && c <= L'f')
                {
                    d = static_cast<unsigned>(c - L'a') + 10u;
                }
                else if (base == 16 && c >= L'A' && c <= L'F')
                {
                    d = static_cast<unsigned>(c - L'A') + 10u;
                }
                else
                {
                    return false;
                }
                number = number * base + d;
            }
            return true;
        };
        if (value.starts_with(L"handle:"))
        {
            auto digits = value.substr(7);
            if (digits.starts_with(L"0x") || digits.starts_with(L"0X"))
            {
                digits.remove_prefix(2);
            }
            std::uint64_t h = 0;
            if (digitsOf(digits, 16, h) && h != 0)
            {
                out.kind = Kind::Handle;
                out.handle = h;
            }
        }
        else if (value.starts_with(L"console:"))
        {
            EnvSink list;
            auto rest = value.substr(8);
            // Element by element: every one must be a nonzero 32-bit decimal, or the whole value is refused.
            while (true)
            {
                const size_t comma = rest.find(L',');
                const auto element = rest.substr(0, comma);
                std::uint64_t pid = 0;
                if (!digitsOf(element, 10, pid) || pid == 0 || pid > 0xFFFFFFFFull)
                {
                    return out;
                }
                bool seen = false;
                for (size_t i = 0; i < list.pidCount; ++i)
                {
                    seen = seen || list.pids[i] == static_cast<DWORD>(pid);
                }
                if (!seen)
                {
                    if (list.pidCount == kMaxConsoleCandidates)
                    {
                        return out; // more than the launcher ever writes
                    }
                    list.pids[list.pidCount++] = static_cast<DWORD>(pid);
                }
                if (comma == std::wstring_view::npos)
                {
                    break;
                }
                rest.remove_prefix(comma + 1);
            }
            list.kind = Kind::Console;
            out = list;
        }
        return out;
    }

    // PURE: the launcher's "handle:<hex>" value for a handle it passes to the app by inheritance.
    //   handle  — the inheritable duplicate's value (the same value in the child: inherited handles keep it).
    //   returns — e.g. "handle:0x1a4".
    //   throws  — std::bad_alloc only.
    inline std::wstring FormatHandleEnvValue(std::uint64_t handle)
    {
        wchar_t buf[40]{};
        swprintf_s(buf, L"handle:0x%llx", static_cast<unsigned long long>(handle));
        return buf;
    }

    // PURE: the launcher's "console:<pid>[,<pid>...]" value for the processes attached to the caller's console.
    //   pids    — those processes, the launcher's parent first; only the first kMaxConsoleCandidates are written (the
    //             launcher never passes more), zeros are skipped.
    //   returns — e.g. "console:1234,5678"; "" when no nonzero pid was given (the launcher then sets nothing).
    //   throws  — std::bad_alloc only.
    inline std::wstring FormatConsoleEnvValue(std::span<const DWORD> pids)
    {
        std::wstring out;
        size_t written = 0;
        for (const DWORD pid : pids)
        {
            if (pid == 0 || written == kMaxConsoleCandidates)
            {
                continue;
            }
            out += written == 0 ? L"console:" : L",";
            out += std::to_wstring(pid);
            ++written;
        }
        return out;
    }

    // PURE: the "console:<pid>" value for ONE process attached to the caller's console (the tests' shorthand).
    //   pid     — that process.
    //   returns — e.g. "console:1234"; "" for 0.
    //   throws  — std::bad_alloc only.
    inline std::wstring FormatConsoleEnvValue(DWORD pid)
    {
        return FormatConsoleEnvValue(std::span<const DWORD>{ &pid, 1 });
    }

    namespace detail
    {
        // The process-wide (module-wide, see the header) sink. Guarded by `lock`; every field is read and written only
        // under it.
        struct State
        {
            SRWLOCK lock = SRWLOCK_INIT;
            bool captured{ false }; // Capture ran (it runs once)
            Kind kind{ Kind::None };
            HANDLE handle{ nullptr }; // Kind::Handle: OUR private, non-inheritable duplicate
            // Kind::Console: the processes to attach through, in order, each with a SYNCHRONIZE handle that keeps its pid
            // from being reused (`count` entries are set; a candidate found dead is dropped from the list).
            HANDLE pins[kMaxConsoleCandidates]{};
            DWORD pids[kMaxConsoleCandidates]{};
            size_t count{ 0 };
            int open{ 0 }; // Holds alive (echoed dialogs not answered yet)
            bool releaseRequested{ false }; // RequestRelease ran; the sink goes when `open` reaches 0
        };

        // The one State of this module.
        //   returns — it (created on first use; never destroyed: a dialog may echo during shutdown).
        //   throws  — nothing.
        inline State& Get() noexcept
        {
            static State* state = new State{};
            return *state;
        }

        // Whether `h` is a handle this process INHERITED and can write text to: open (GetHandleInformation), flagged
        // inheritable (an inherited handle keeps HANDLE_FLAG_INHERIT; a stale value that names one of our own handles
        // usually lacks it), and a pipe, a disk file or a character device such as NUL.
        //   h       — the candidate (null and INVALID_HANDLE_VALUE are refused).
        //   returns — true when the caller's text can go there.
        //   throws  — nothing.
        inline bool IsInheritedWritable(HANDLE h) noexcept
        {
            if (!h || h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD flags = 0;
            if (!::GetHandleInformation(h, &flags) || (flags & HANDLE_FLAG_INHERIT) == 0)
            {
                return false;
            }
            const DWORD type = ::GetFileType(h);
            return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK || type == FILE_TYPE_CHAR;
        }

        // The id of the process that created this one, from NtQueryInformationProcess (ProcessBasicInformation; the
        // struct is declared here so no winternl.h is needed). The process may be gone already: the caller checks.
        //   returns — the parent's id, or 0 when Windows does not say.
        //   throws  — nothing.
        inline DWORD ParentProcessId() noexcept
        {
            struct BasicInformation
            {
                void* reserved1;
                void* peb;
                void* reserved2[2];
                ULONG_PTR uniqueProcessId;
                ULONG_PTR inheritedFromUniqueProcessId;
            };
            using QueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
            const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            const auto query = ntdll ? reinterpret_cast<QueryFn>(::GetProcAddress(ntdll, "NtQueryInformationProcess")) : nullptr;
            if (!query)
            {
                return 0;
            }
            BasicInformation info{};
            ULONG got = 0;
            if (query(::GetCurrentProcess(), 0 /* ProcessBasicInformation */, &info, sizeof(info), &got) < 0)
            {
                return 0;
            }
            return static_cast<DWORD>(info.inheritedFromUniqueProcessId);
        }

        // A SYNCHRONIZE handle on process `pid`, but only when that process started BEFORE this one: a process id that
        // Windows gave again to a newer process (the original caller having exited) must never receive our text. While
        // the handle is open, the id cannot be reused.
        //   pid     — the candidate.
        //   returns — the handle (the caller closes it), or nullptr when the process is gone, younger or unreadable.
        //   throws  — nothing.
        inline HANDLE PinOlderProcess(DWORD pid) noexcept
        {
            if (pid == 0 || pid == ::GetCurrentProcessId())
            {
                return nullptr;
            }
            const HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process)
            {
                return nullptr;
            }
            FILETIME theirs{}, ours{}, exitTime{}, kernel{}, user{};
            const bool older = ::GetProcessTimes(process, &theirs, &exitTime, &kernel, &user) &&
                               ::GetProcessTimes(::GetCurrentProcess(), &ours, &exitTime, &kernel, &user) &&
                               ::CompareFileTime(&theirs, &ours) <= 0;
            if (!older || ::WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
            {
                ::CloseHandle(process);
                return nullptr;
            }
            return process;
        }

        // Duplicates `h` as a private, NON-inheritable handle of this process: the app's own children (tabs, helpers)
        // can then never inherit the caller's pipe and keep it open.
        //   h       — a handle of this process.
        //   returns — the duplicate, or nullptr when it cannot be made.
        //   throws  — nothing.
        inline HANDLE DuplicatePrivate(HANDLE h) noexcept
        {
            HANDLE dup = nullptr;
            if (!::DuplicateHandle(::GetCurrentProcess(), h, ::GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            {
                return nullptr;
            }
            return dup;
        }

        // Closes every handle of the sink and forgets it. Caller holds the lock.
        //   s       — the state.
        //   throws  — nothing.
        inline void ReleaseLocked(State& s) noexcept
        {
            if (s.handle)
            {
                ::CloseHandle(s.handle);
                s.handle = nullptr;
            }
            for (size_t i = 0; i < s.count; ++i)
            {
                ::CloseHandle(s.pins[i]);
                s.pins[i] = nullptr;
                s.pids[i] = 0;
            }
            s.count = 0;
            s.kind = Kind::None;
        }

        // Adds `pid` to the console candidates when PinOlderProcess accepts it. Caller holds the lock.
        //   s       — the state.
        //   pid     — a process attached to the caller's console.
        //   throws  — nothing.
        inline void AddConsoleCandidateLocked(State& s, DWORD pid) noexcept
        {
            if (s.count == kMaxConsoleCandidates)
            {
                return;
            }
            if (const HANDLE pin = PinOlderProcess(pid))
            {
                s.pins[s.count] = pin;
                s.pids[s.count] = pid;
                ++s.count;
            }
        }

        // The console control handler installed for the few milliseconds this GUI process is attached to the caller's
        // console: a Ctrl+C or Ctrl+Break pressed there at that moment must not end the app (the default handler would
        // call ExitProcess). Other events (the console closing, a logoff) keep their default handling.
        //   ctrlType — the event.
        //   returns  — TRUE (handled) for Ctrl+C and Ctrl+Break, FALSE otherwise.
        //   throws   — nothing.
        inline BOOL WINAPI SwallowCtrl(DWORD ctrlType) noexcept
        {
            return ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT;
        }

        // Writes `text` (lines joined with "\n") to a pipe or a file as UTF-8, the way agentmaster-cli writes.
        //   h       — the handle.
        //   text    — the text; a final "\n" is added.
        //   returns — false when the write failed (a closed pipe: the caller is gone).
        //   throws  — std::bad_alloc only.
        inline bool WriteUtf8(HANDLE h, std::wstring_view text)
        {
            std::string utf8;
            if (!text.empty())
            {
                const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
                if (bytes > 0)
                {
                    utf8.resize(static_cast<size_t>(bytes));
                    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
                }
            }
            utf8.push_back('\n');
            DWORD wrote = 0;
            return ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr) != FALSE;
        }

        // Writes `text` to the console that process `pid` is attached to: attach, write through a CONOUT$ handle, detach.
        // WriteConsoleW is right in any console code page, and every "\n" becomes "\r\n" for a console. The Ctrl
        // handler above covers the attach. Fails when this process is attached to a console already, or when `pid` has
        // none any more.
        //   pid     — a process attached to the caller's console.
        //   text    — the text; a final line break is added.
        //   returns — false when nothing could be written.
        //   throws  — std::bad_alloc only.
        inline bool WriteToConsoleOf(DWORD pid, std::wstring_view text)
        {
            std::wstring crlf;
            crlf.reserve(text.size() + 16);
            for (const wchar_t c : text)
            {
                if (c == L'\n')
                {
                    crlf += L"\r\n";
                }
                else
                {
                    crlf.push_back(c);
                }
            }
            crlf += L"\r\n";
            ::SetConsoleCtrlHandler(SwallowCtrl, TRUE);
            bool wrote = false;
            if (::AttachConsole(pid))
            {
                const HANDLE out = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (out != INVALID_HANDLE_VALUE)
                {
                    DWORD count = 0;
                    wrote = ::WriteConsoleW(out, crlf.data(), static_cast<DWORD>(crlf.size()), &count, nullptr) != FALSE;
                    ::CloseHandle(out);
                }
                ::FreeConsole();
            }
            ::SetConsoleCtrlHandler(SwallowCtrl, FALSE);
            return wrote;
        }

        // TEST HOOK (the m5 harness): makes `h` this module's sink (duplicated privately), as if Capture had found it.
        // Never called by a product binary.
        //   h       — a handle to write to (a pipe's write end in the tests).
        //   throws  — nothing.
        inline void UseHandleForTest(HANDLE h) noexcept
        {
            auto& s = Get();
            ::AcquireSRWLockExclusive(&s.lock);
            ReleaseLocked(s);
            s.captured = true;
            s.releaseRequested = false;
            s.open = 0;
            s.handle = DuplicatePrivate(h);
            s.kind = s.handle ? Kind::Handle : Kind::None;
            ::ReleaseSRWLockExclusive(&s.lock);
        }
    }

    // Finds where the command that started this process reads (the three steps of the header) and keeps it for Write.
    // The app exe calls it ONCE, first thing after TryRunCliCall in wWinMain; later calls do nothing. Removes
    // AGENTMASTER_CALLER_SINK from the environment whatever it held, and closes the inherited handle it named.
    //   throws  — nothing (a sink that cannot be set up is simply Kind::None).
    inline void Capture() noexcept
    {
        auto& s = detail::Get();
        ::AcquireSRWLockExclusive(&s.lock);
        if (s.captured)
        {
            ::ReleaseSRWLockExclusive(&s.lock);
            return;
        }
        s.captured = true;

        // 1. the launcher's hand-over. Read and removed in one go, so the variable never reaches a child of the app. The
        //    longest valid value ("console:" + 8 pids of 10 digits + 7 commas) is 95 characters; a longer one is refused.
        wchar_t value[128]{};
        const DWORD n = ::GetEnvironmentVariableW(kEnvVariable, value, ARRAYSIZE(value));
        if (n > 0)
        {
            ::SetEnvironmentVariableW(kEnvVariable, nullptr);
        }
        const EnvSink handedOver = (n > 0 && n < ARRAYSIZE(value)) ? ParseEnvValue(value) : EnvSink{};
        if (handedOver.kind == Kind::Handle)
        {
            const HANDLE inherited = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(handedOver.handle));
            if (detail::IsInheritedWritable(inherited))
            {
                s.handle = detail::DuplicatePrivate(inherited);
                // The original is ours alone (the launcher passed it for this purpose) and inheritable: closing it is
                // what keeps every later child of the app from inheriting the caller's pipe.
                ::CloseHandle(inherited);
                s.kind = s.handle ? Kind::Handle : Kind::None;
            }
        }
        else if (handedOver.kind == Kind::Console)
        {
            // Every candidate that is still alive now and older than this process; one that exits later is skipped when
            // a write finds it gone.
            for (size_t i = 0; i < handedOver.pidCount; ++i)
            {
                detail::AddConsoleCandidateLocked(s, handedOver.pids[i]);
            }
            s.kind = s.count > 0 ? Kind::Console : Kind::None;
        }

        // 2. the caller's own standard handles, when it passed real ones. Read before anything could attach a console
        //    (AttachConsole may fill this process's empty standard handles).
        if (s.kind == Kind::None && handedOver.kind == Kind::None)
        {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            ::GetStartupInfoW(&si);
            if ((si.dwFlags & STARTF_USESTDHANDLES) != 0)
            {
                const HANDLE pick = detail::IsInheritedWritable(si.hStdError)  ? si.hStdError :
                                    detail::IsInheritedWritable(si.hStdOutput) ? si.hStdOutput :
                                                                                 nullptr;
                if (pick)
                {
                    s.handle = detail::DuplicatePrivate(pick);
                    s.kind = s.handle ? Kind::Handle : Kind::None;
                }
            }
        }

        // 3. the parent's console. Not attached here: Write attaches for each echo, and only while the parent lives. The
        //    parent is the one candidate: this process is not attached to that console, so it cannot list the others,
        //    and a shell that runs a GUI program directly either waits for it (cmd /c, bash, Python) or lives on.
        if (s.kind == Kind::None && handedOver.kind == Kind::None)
        {
            detail::AddConsoleCandidateLocked(s, detail::ParentProcessId());
            s.kind = s.count > 0 ? Kind::Console : Kind::None;
        }
        ::ReleaseSRWLockExclusive(&s.lock);
    }

    // Whether a sink is held right now (a quick check that saves the formatting work when nobody can read it).
    //   returns — true while Capture found a sink and it was not released.
    //   throws  — nothing.
    inline bool Active() noexcept
    {
        auto& s = detail::Get();
        ::AcquireSRWLockShared(&s.lock);
        const bool active = s.kind != Kind::None;
        ::ReleaseSRWLockShared(&s.lock);
        return active;
    }

    // Writes `text` to the caller: a pipe or a file as UTF-8, a console through a short attach. A sink that fails (the
    // caller went away) is dropped, so later echoes cost nothing. Serialized, so the lines of two dialogs never mix.
    //   text    — the text, lines joined with "\n"; a final line break is added.
    //   throws  — nothing.
    inline void Write(std::wstring_view text) noexcept
    {
        auto& s = detail::Get();
        ::AcquireSRWLockExclusive(&s.lock);
        try
        {
            if (s.kind == Kind::Handle)
            {
                if (!detail::WriteUtf8(s.handle, text))
                {
                    detail::ReleaseLocked(s);
                }
            }
            else if (s.kind == Kind::Console)
            {
                // Through the first candidate still alive. Every candidate is attached to the same console, so which one
                // carries the text changes nothing for the reader. A candidate that ended, or whose attach fails (it left
                // the console, or Windows refused it), is dropped, so it costs nothing at the next echo.
                bool wrote = false;
                while (s.count > 0)
                {
                    if (::WaitForSingleObject(s.pins[0], 0) == WAIT_TIMEOUT && detail::WriteToConsoleOf(s.pids[0], text))
                    {
                        wrote = true;
                        break;
                    }
                    ::CloseHandle(s.pins[0]);
                    for (size_t i = 1; i < s.count; ++i)
                    {
                        s.pins[i - 1] = s.pins[i];
                        s.pids[i - 1] = s.pids[i];
                    }
                    --s.count;
                    s.pins[s.count] = nullptr;
                    s.pids[s.count] = 0;
                }
                if (!wrote)
                {
                    detail::ReleaseLocked(s); // no process of the caller's console is left: the console is gone
                }
            }
        }
        catch (...)
        {
            // Only std::bad_alloc can land here (the UTF-8 / CRLF copies). The echo is lost; the dialog it describes is
            // shown regardless by the caller. Nothing to log to: the app has no log before its profile resolves.
        }
        ::ReleaseSRWLockExclusive(&s.lock);
    }

    // The app's startup is over: release the sink now, or, while an echoed dialog is still open (Hold), when the last
    // one was answered. Called by the app exe before its message loop. Later Captures do nothing (the command is over).
    //   throws  — nothing.
    inline void RequestRelease() noexcept
    {
        auto& s = detail::Get();
        ::AcquireSRWLockExclusive(&s.lock);
        s.captured = true;
        s.releaseRequested = true;
        if (s.open <= 0)
        {
            detail::ReleaseLocked(s);
        }
        ::ReleaseSRWLockExclusive(&s.lock);
    }

    // Keeps the sink alive while it exists: taken for every echoed dialog, so a RequestRelease made while the dialog is
    // open (the app's startup ends while WindowEmperor's message box waits on another thread) leaves the sink until the
    // dialog's answer is written. Movable, not copyable.
    class Hold
    {
    public:
        // Counts one more open dialog.
        //   throws  — nothing.
        Hold() noexcept
        {
            auto& s = detail::Get();
            ::AcquireSRWLockExclusive(&s.lock);
            ++s.open;
            ::ReleaseSRWLockExclusive(&s.lock);
        }

        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;

        // Takes over `other`'s count (a coroutine moves its Hold to the thread that shows the dialog).
        //   other   — left empty: its destructor counts nothing.
        //   throws  — nothing.
        Hold(Hold&& other) noexcept :
            _armed{ other._armed }
        {
            other._armed = false;
        }

        Hold& operator=(Hold&&) = delete;

        // Counts the dialog as answered; releases the sink when a release was requested and nothing else is open.
        //   throws  — nothing.
        ~Hold()
        {
            if (!_armed)
            {
                return;
            }
            auto& s = detail::Get();
            ::AcquireSRWLockExclusive(&s.lock);
            if (--s.open <= 0)
            {
                s.open = 0;
                if (s.releaseRequested)
                {
                    detail::ReleaseLocked(s);
                }
            }
            ::ReleaseSRWLockExclusive(&s.lock);
        }

    private:
        bool _armed{ true };
    };

    // What the console LAUNCHER (wt/shim.cpp) hands to the app it starts (PrepareForChild).
    struct Forward
    {
        // An inheritable duplicate of the launcher's stderr when that is a pipe or a file: it must be the ONLY handle
        // in the child's PROC_THREAD_ATTRIBUTE_HANDLE_LIST. nullptr when the caller's sink is a console (or none).
        HANDLE inheritable{ nullptr };
        // Whether AGENTMASTER_CALLER_SINK was set in the launcher's environment for the child to inherit.
        bool envSet{ false };
    };

    // PURE: the "console:" candidates the launcher hands over: its parent first when the parent is attached to the same
    // console (the shell that ran the command, the usual case), then the console's other processes in the order Windows
    // listed them, never the launcher itself, never 0, each once, at most kMaxConsoleCandidates.
    //   attached — GetConsoleProcessList's answer (it lists the launcher too).
    //   self     — the launcher's own id.
    //   parent   — the launcher's parent's id (0 when unknown).
    //   returns  — the candidates, in order.
    //   throws   — std::bad_alloc only.
    inline std::vector<DWORD> ConsoleCandidates(std::span<const DWORD> attached, DWORD self, DWORD parent)
    {
        std::vector<DWORD> out;
        const auto add = [&](DWORD pid) {
            if (pid != 0 && pid != self && out.size() < kMaxConsoleCandidates && std::find(out.begin(), out.end(), pid) == out.end())
            {
                out.push_back(pid);
            }
        };
        if (std::find(attached.begin(), attached.end(), parent) != attached.end())
        {
            add(parent);
        }
        for (const DWORD pid : attached)
        {
            add(pid);
        }
        return out;
    }

    // The launcher's side of step 1: describes the caller's sink to the app it is about to start, in the launcher's OWN
    // environment (the child inherits it; the launcher exits right after). A pipe or a file is duplicated as an
    // inheritable handle; a console is named by the processes attached to it (ConsoleCandidates), so the app can still
    // reach it when the launcher's parent exits first (`cmd /c agentmaster ...`). A launcher with no console and no
    // handle (the reopen ShellExecute) hands over nothing.
    //   returns — the handle to inherit (if any) and whether the variable was set; CloseForward cleans up.
    //   throws  — nothing.
    inline Forward PrepareForChild() noexcept
    {
        Forward f;
        try
        {
            const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
            DWORD mode = 0;
            if (err && err != INVALID_HANDLE_VALUE && ::GetConsoleMode(err, &mode))
            {
                // A list longer than the buffer is not stored at all (the function only returns the count needed), so
                // the buffer is generous; a console with more processes than that hands over nothing.
                DWORD pids[128]{};
                const DWORD count = ::GetConsoleProcessList(pids, ARRAYSIZE(pids));
                if (count > 0 && count <= ARRAYSIZE(pids))
                {
                    const auto candidates = ConsoleCandidates(std::span<const DWORD>{ pids, count }, ::GetCurrentProcessId(), detail::ParentProcessId());
                    const auto value = FormatConsoleEnvValue(candidates);
                    if (!value.empty() && ::SetEnvironmentVariableW(kEnvVariable, value.c_str()))
                    {
                        f.envSet = true;
                    }
                }
            }
            else if (err && err != INVALID_HANDLE_VALUE)
            {
                const DWORD type = ::GetFileType(err);
                if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK || type == FILE_TYPE_CHAR)
                {
                    HANDLE dup = nullptr;
                    if (::DuplicateHandle(::GetCurrentProcess(), err, ::GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS) && dup)
                    {
                        if (::SetEnvironmentVariableW(kEnvVariable, FormatHandleEnvValue(reinterpret_cast<ULONG_PTR>(dup)).c_str()))
                        {
                            f.inheritable = dup;
                            f.envSet = true;
                        }
                        else
                        {
                            ::CloseHandle(dup);
                        }
                    }
                }
            }
        }
        catch (...)
        {
            // std::bad_alloc only (the variable's text): the app is started without a sink, as before this existed.
        }
        return f;
    }

    // Undoes PrepareForChild in the launcher after the child was started (or failed to start).
    //   f       — what PrepareForChild returned; emptied.
    //   throws  — nothing.
    inline void CloseForward(Forward& f) noexcept
    {
        if (f.inheritable)
        {
            ::CloseHandle(f.inheritable);
            f.inheritable = nullptr;
        }
        if (f.envSet)
        {
            ::SetEnvironmentVariableW(kEnvVariable, nullptr);
            f.envSet = false;
        }
    }
}

// The reply of a SINGLE-INSTANCE HAND-OFF (doc/agentmaster/CLI.md §2b): when a new process hands its command line to the
// running app (WindowEmperor's WM_COPYDATA), a dialog that line causes (Windows Terminal's --help / --version / command
// line error box) opens in the RUNNING app, which has no access to the new process's caller. The sender creates a small
// named section, names it in the payload, and the receiver writes the dialog's title and text into it before its
// WM_COPYDATA handler returns; the sender then echoes it to its caller (DialogEcho::EchoForeignDialog). An older
// receiver ignores the extra payload field, and the section then stays empty.
namespace Agentmaster::HandoffReply
{
    // Every reply section's name starts with this (the receiver refuses any other name from a payload).
    inline constexpr std::wstring_view kNamePrefix = L"Local\\AgentmasterHandoffReply-";
    // The section's size; a longer text is cut to fit (with a marker), so the receiver never writes past it.
    inline constexpr DWORD kBytes = 64 * 1024;
    // The section's first 4 bytes once a reply was written ('AMRP').
    inline constexpr std::uint32_t kMagic = 0x50524D41u;

    // A reply as written by the receiver.
    struct Message
    {
        std::wstring title; // the dialog's title
        std::wstring text; // the dialog's text
        DWORD pid{}; // the receiver (the running app) that shows it
    };

    // The fixed header at the start of the section, followed by `titleChars` then `textChars` UTF-16 units.
    struct Header
    {
        std::uint32_t magic;
        std::uint32_t pid;
        std::uint32_t titleChars;
        std::uint32_t textChars;
    };

    // PURE: the section's bytes for a reply, cut to `capacity` bytes. The text keeps its start and gets a marker when
    // it does not fit; the title is cut to 512 characters.
    //   m        — the reply.
    //   capacity — the section's size in bytes (kBytes in the product).
    //   returns  — the bytes to copy to the start of the section (never more than capacity).
    //   throws   — std::bad_alloc only.
    inline std::vector<std::uint8_t> Encode(const Message& m, size_t capacity)
    {
        static constexpr std::wstring_view kCut = L"\n[... cut: the dialog shows the rest]";
        std::wstring title = m.title.substr(0, 512);
        std::wstring text = m.text;
        const size_t room = capacity > sizeof(Header) ? (capacity - sizeof(Header)) / sizeof(wchar_t) : 0;
        if (title.size() > room)
        {
            title.resize(room);
        }
        if (title.size() + text.size() > room)
        {
            const size_t keep = room - title.size() > kCut.size() ? room - title.size() - kCut.size() : 0;
            text.resize(keep);
            if (room - title.size() >= kCut.size())
            {
                text.append(kCut);
            }
        }
        const Header h{ kMagic, m.pid, static_cast<std::uint32_t>(title.size()), static_cast<std::uint32_t>(text.size()) };
        std::vector<std::uint8_t> bytes(sizeof(Header) + (title.size() + text.size()) * sizeof(wchar_t));
        std::memcpy(bytes.data(), &h, sizeof(h));
        std::memcpy(bytes.data() + sizeof(h), title.data(), title.size() * sizeof(wchar_t));
        std::memcpy(bytes.data() + sizeof(h) + title.size() * sizeof(wchar_t), text.data(), text.size() * sizeof(wchar_t));
        return bytes;
    }

    // PURE: reads a reply back from the section's bytes. Refuses anything malformed (no magic, lengths past the end).
    //   data    — the section's start.
    //   size    — its size in bytes.
    //   returns — the reply, or nullopt when none was written (an older receiver, or no dialog).
    //   throws  — std::bad_alloc only.
    inline std::optional<Message> Decode(const std::uint8_t* data, size_t size)
    {
        if (!data || size < sizeof(Header))
        {
            return std::nullopt;
        }
        Header h{};
        std::memcpy(&h, data, sizeof(h));
        const size_t chars = static_cast<size_t>(h.titleChars) + h.textChars;
        if (h.magic != kMagic || chars > (size - sizeof(Header)) / sizeof(wchar_t))
        {
            return std::nullopt;
        }
        Message m;
        m.pid = h.pid;
        m.title.resize(h.titleChars);
        m.text.resize(h.textChars);
        std::memcpy(m.title.data(), data + sizeof(Header), h.titleChars * sizeof(wchar_t));
        std::memcpy(m.text.data(), data + sizeof(Header) + h.titleChars * sizeof(wchar_t), h.textChars * sizeof(wchar_t));
        return m;
    }

    // The sender's side of one hand-off.
    struct Channel
    {
        HANDLE mapping{ nullptr }; // the section (zero-filled at creation)
        std::wstring name; // its name, sent in the payload ("" = no reply wanted)
    };

    // The sender creates the section before it sends the payload. Skipped (an empty Channel) when nobody can read the
    // echo anyway (CallerSink::Active() is false): the payload then carries no name, as before this existed.
    //   wanted  — whether to create one (the caller passes CallerSink::Active()).
    //   returns — the channel; Close it when done.
    //   throws  — nothing.
    inline Channel Create(bool wanted) noexcept
    {
        Channel c;
        if (!wanted)
        {
            return c;
        }
        try
        {
            std::wstring name{ kNamePrefix };
            name += std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
            const HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kBytes, name.c_str());
            if (mapping)
            {
                c.mapping = mapping;
                c.name = std::move(name);
            }
        }
        catch (...)
        {
            // std::bad_alloc only: no reply channel; the hand-off itself goes on.
        }
        return c;
    }

    // The receiver's side: writes the dialog into the sender's section, inside the WM_COPYDATA handler (the sender
    // waits in SendMessageTimeoutW until it returns). A name that is not ours is refused.
    //   name    — the section's name from the payload.
    //   m       — the dialog (its pid = the receiver).
    //   throws  — nothing.
    inline void Write(std::wstring_view name, const Message& m) noexcept
    {
        try
        {
            if (!name.starts_with(kNamePrefix) || name.size() > kNamePrefix.size() + 48)
            {
                return;
            }
            const std::wstring z{ name };
            const HANDLE mapping = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, z.c_str());
            if (!mapping)
            {
                return;
            }
            if (void* view = ::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, kBytes))
            {
                const auto bytes = Encode(m, kBytes);
                std::memcpy(view, bytes.data(), bytes.size());
                ::UnmapViewOfFile(view);
            }
            ::CloseHandle(mapping);
        }
        catch (...)
        {
            // std::bad_alloc only: no reply; the dialog itself is shown by the caller regardless.
        }
    }

    // The sender reads the reply after the hand-off returned.
    //   c       — the channel.
    //   returns — the dialog, or nullopt when the receiver wrote none.
    //   throws  — nothing.
    inline std::optional<Message> Read(const Channel& c) noexcept
    {
        if (!c.mapping)
        {
            return std::nullopt;
        }
        try
        {
            std::optional<Message> m;
            if (const void* view = ::MapViewOfFile(c.mapping, FILE_MAP_READ, 0, 0, kBytes))
            {
                m = Decode(static_cast<const std::uint8_t*>(view), kBytes);
                ::UnmapViewOfFile(view);
            }
            return m;
        }
        catch (...)
        {
            return std::nullopt; // std::bad_alloc only
        }
    }

    // Closes the sender's section.
    //   c       — emptied.
    //   throws  — nothing.
    inline void Close(Channel& c) noexcept
    {
        if (c.mapping)
        {
            ::CloseHandle(c.mapping);
            c.mapping = nullptr;
        }
        c.name.clear();
    }
}
