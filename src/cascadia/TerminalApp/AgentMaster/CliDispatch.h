// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — which command lines are a CLI call, and how the app exe answers one (doc/agentmaster/CLI.md §2, §2a).
// Header-only, plain Win32 + std (no WinRT, no wil, no engine), so the console launcher shim (wt/shim.cpp), the app
// exe's wWinMain (WindowsTerminal/main.cpp) and the m5 harness all compile the SAME rule and can never drift apart.
//
// WHY THE APP EXE ASKS TOO: `agentmaster <verb>` reaches agentmaster-cli.exe through the PACKAGE's execution alias,
// which targets the console launcher shim (shipped as wtd.exe). An UNPACKAGED install has no alias: the per-user one
// (~\.agentmaster\bin), the one for all users (Program Files) and a portable folder on PATH put their folder on PATH,
// and in that folder the name `agentmaster` finds Agentmaster.exe, the app itself. The machine PATH also comes before
// the user PATH that holds the package's alias, so an all-users copy hides the package's `agentmaster` from every user.
// Until 2026-10-23 the app read `agentmaster show <id>` as a Windows Terminal command line. An unpackaged copy has a
// single-instance identity of its own, so it did not hand the line to a running package: it started a SECOND app,
// which stopped at "profile in use" (two such dialogs reached the user's screen that day; a Yes runs two instances on
// one profile, a data risk) and would then have opened a tab running a program named "show". Now wWinMain's first step
// is TryRunCliCall: a CLI call runs the agentmaster-cli.exe beside the exe and exits with its exit code, and the app
// never starts for it.
//
// THE LIMIT: the app exe is GUI-subsystem, and some callers do not wait for a GUI program (measured 2026-10-23 with a
// GUI stand-in built on this header): an interactive cmd.exe, and PowerShell unless the output is piped on (`& x |
// Out-String` waits; a plain call and `$o = & x` return at once, with no output and no $LASTEXITCODE). There the CLI
// still runs, and its output reaches the console after the prompt. bash (Git Bash, Claude Code's Bash tool), Python,
// Node and `cmd /c` wait, and get the output and the exit code. `agentmaster-cli <verb>`, a console program in the same
// folder, has no such limit.

#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace Agentmaster::CliDispatch
{
    // agentmaster-cli's verbs (CLI.md §5). Matched exactly, case included: `Show`, a typo or a quoted verb is NOT a CLI
    // call and goes on to the app as a new-tab command line (the documented trap, CLI.md §11). Add a verb HERE when the
    // CLI gains one, or both the alias and the app exe hand it to the app.
    inline constexpr std::wstring_view kCliVerbs[] = {
        L"show", L"list", L"sessions", L"tabs", L"windows", L"external", L"restore", L"archive",
        L"search", L"sql", L"sessdb" // SESSIONS_DB.md §8.3: the session database's verbs
    };

    // The leading flags that only the CLI knows, so `agentmaster --instance dev show …` is a CLI call too. None is a
    // Windows Terminal command-line token, so a GUI launch (`-w`, `-s`, `nt`, `-Embedding`, bare) is never routed to
    // the CLI. Every OTHER leading flag (`--profile`, `--window`, `--version`, `--help`) still goes to the app.
    inline constexpr std::wstring_view kCliOnlyFlags[] = {
        L"--json", L"--self", L"--offline", L"--tail", L"--instance", L"--state", L"--dir"
    };

    // The CLI's file name. It ships beside the app exe and the launcher shim in every layout, package and zip.
    inline constexpr std::wstring_view kCliExeLeaf = L"agentmaster-cli.exe";

    // The exit code when the CLI could not be started: the launcher shim's value since the CLI shipped. It equals the
    // CLI's own "not found", so the stderr line written with it is what tells the two apart.
    inline constexpr int kCliStartFailedExitCode = 1;

    // PURE: whether a token is one of agentmaster-cli's verbs (kCliVerbs, exact match).
    //   token   — the first command-line token (FirstToken).
    //   returns — true for a verb; false for anything else, a verb in capitals included.
    //   throws  — nothing.
    inline bool IsCliVerb(std::wstring_view token) noexcept
    {
        for (const auto verb : kCliVerbs)
        {
            if (token == verb)
            {
                return true;
            }
        }
        return false;
    }

    // PURE: whether a token is one of the leading flags that only the CLI knows (kCliOnlyFlags, exact match).
    //   token   — the first command-line token (FirstToken).
    //   returns — true for a CLI-only flag; false for every other flag, the app's included.
    //   throws  — nothing.
    inline bool IsCliOnlyFlag(std::wstring_view token) noexcept
    {
        for (const auto flag : kCliOnlyFlags)
        {
            if (token == flag)
            {
                return true;
            }
        }
        return false;
    }

    // PURE: the command line after argv[0], with the blanks that separate them removed and every later character kept
    // as typed (quotes, spacing). It is exactly what the C runtime hands wWinMain as lpCmdLine
    // (_get_wide_winmain_command_line: argv[0] ends at the first character <= L' ' outside double quotes, then every
    // character <= L' ' is skipped). Both binaries pass this tail on unchanged, so a quoted argument such as a session
    // title reaches the CLI, or the app, as the user typed it.
    //   commandLine — the whole command line (GetCommandLineW()).
    //   returns     — a view into commandLine; empty when there are no arguments.
    //   throws      — nothing.
    inline std::wstring_view CommandlineTail(std::wstring_view commandLine) noexcept
    {
        size_t i = 0;
        bool inQuotes = false;
        while (i < commandLine.size() && (commandLine[i] > L' ' || (commandLine[i] != L'\0' && inQuotes)))
        {
            if (commandLine[i] == L'"')
            {
                inQuotes = !inQuotes;
            }
            ++i;
        }
        while (i < commandLine.size() && commandLine[i] != L'\0' && commandLine[i] <= L' ')
        {
            ++i;
        }
        return commandLine.substr(i);
    }

    // PURE: the first space- or tab-separated token of a tail. A quoted first token keeps its quotes, so `"show"` is no
    // verb, as it never was for the launcher shim.
    //   tail    — a command-line tail (CommandlineTail).
    //   returns — a view into tail; the whole tail when it holds no blank.
    //   throws  — nothing.
    inline std::wstring_view FirstToken(std::wstring_view tail) noexcept
    {
        const auto end = tail.find_first_of(L" \t");
        return end == std::wstring_view::npos ? tail : tail.substr(0, end);
    }

    // PURE: THE rule. A command line is a CLI call when its first token is a verb (the standard form, `agentmaster show
    // …`) or a CLI-only flag (`agentmaster --instance dev show …`). Anything else, a bare launch included, is the app's.
    //   tail    — the command line after argv[0] (CommandlineTail).
    //   returns — true when agentmaster-cli.exe must answer the command line.
    //   throws  — nothing.
    inline bool IsCliInvocation(std::wstring_view tail) noexcept
    {
        const auto token = FirstToken(tail);
        return IsCliVerb(token) || IsCliOnlyFlag(token);
    }

    // PURE: the path of the agentmaster-cli.exe that ships beside an exe: the same folder, kCliExeLeaf as the file name.
    // Never a bare file name for a full path: CreateProcessW would resolve a bare name against the CURRENT folder, where
    // anything called agentmaster-cli.exe would then run (TryRunCliCall refuses an empty exe path for that reason).
    //   exePath — the full path of the running exe (app or shim); either separator.
    //   returns — the CLI's full path; just kCliExeLeaf when exePath holds no folder.
    //   throws  — std::bad_alloc only.
    inline std::wstring CliExePathBeside(std::wstring_view exePath)
    {
        const auto slash = exePath.find_last_of(L"\\/");
        std::wstring path{ slash == std::wstring_view::npos ? std::wstring_view{} : exePath.substr(0, slash + 1) };
        path.append(kCliExeLeaf);
        return path;
    }

    // PURE: the CLI's command line: the argv[0] it has always received from the launcher shim (`agentmaster-cli`), then
    // the caller's tail unchanged, so every argument reaches the CLI as typed.
    //   tail    — the command line after argv[0] (CommandlineTail); never empty for a CLI call.
    //   returns — `agentmaster-cli <tail>`.
    //   throws  — std::bad_alloc only.
    inline std::wstring BuildCliCommandLine(std::wstring_view tail)
    {
        std::wstring cmdline{ L"agentmaster-cli " };
        cmdline.append(tail);
        return cmdline;
    }

    // Where the CLI's output goes when the APP exe starts it. The app exe is GUI-subsystem and owns no console, so the
    // output must go to the handles the caller passed, or to the caller's console, or, with neither, to a console
    // window of the CLI's own.
    enum class CliOutput
    {
        // The caller passed standard handles (STARTF_USESTDHANDLES): a pipe (bash, Python, Node, a PowerShell capture),
        // a redirected file, or its console handles. The CLI writes exactly there, so `> out.json` and `| jq` work.
        CallerHandles,
        // No handles, but the caller has a console (cmd.exe, PowerShell's own console): the CLI writes to that console
        // through handles opened on it. An interactive cmd.exe or PowerShell does not wait, so the output then follows
        // its next prompt; `cmd /c` waits.
        CallerConsole,
        // Neither (the Run box, a shortcut, Explorer): the CLI gets a console window of its own, which closes when it
        // ends, as the package's console launcher does when it is started the same way.
        OwnConsole,
    };

    // How the app exe starts the CLI: where the output goes and the CreateProcessW creation flags that go with it.
    struct CliLaunchPlan
    {
        CliOutput output{ CliOutput::OwnConsole };
        // 0 shares the console the app exe attached to; CREATE_NO_WINDOW gives a caller without a console no window
        // flash (the CLI writes to the caller's pipe, never to the hidden console); CREATE_NEW_CONSOLE opens the
        // CLI's own window when there is nothing else to write to.
        DWORD creationFlags{ CREATE_NEW_CONSOLE };
    };

    // PURE: the launch plan from the two facts about the caller (the table above; tested in TestCliDispatch).
    //   callerPassedStdHandles   — the caller started the app exe with STARTF_USESTDHANDLES.
    //   attachedToCallerConsole  — AttachConsole(ATTACH_PARENT_PROCESS) succeeded: the caller has a console.
    //   returns                  — the output target and its creation flags.
    //   throws                   — nothing.
    inline CliLaunchPlan PlanCliLaunch(bool callerPassedStdHandles, bool attachedToCallerConsole) noexcept
    {
        if (callerPassedStdHandles)
        {
            return { CliOutput::CallerHandles, attachedToCallerConsole ? 0u : static_cast<DWORD>(CREATE_NO_WINDOW) };
        }
        if (attachedToCallerConsole)
        {
            return { CliOutput::CallerConsole, 0u };
        }
        return { CliOutput::OwnConsole, static_cast<DWORD>(CREATE_NEW_CONSOLE) };
    }

    namespace detail
    {
        // Closes a handle when it leaves scope. Hand-written because this header must also compile in the launcher shim
        // and the m5 harness, which do not all use wil.
        struct ScopedHandle
        {
            HANDLE value{ nullptr };

            ScopedHandle() = default;
            ScopedHandle(const ScopedHandle&) = delete;
            ScopedHandle& operator=(const ScopedHandle&) = delete;

            // Closes the handle unless it is empty or INVALID_HANDLE_VALUE (CreateFileW's failure value).
            ~ScopedHandle()
            {
                if (value && value != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(value);
                }
            }

            // Whether the handle can be used (CreateFileW returns INVALID_HANDLE_VALUE, not null, on failure).
            //   returns — true for an open handle.
            //   throws  — nothing.
            bool Valid() const noexcept
            {
                return value && value != INVALID_HANDLE_VALUE;
            }
        };

        // Opens the console this process is attached to as an INHERITABLE handle, so the CLI receives it through
        // STARTF_USESTDHANDLES. The handles AttachConsole may give the process are not guaranteed to be inheritable.
        //   name    — L"CONIN$" (input) or L"CONOUT$" (output).
        //   returns — the handle, or INVALID_HANDLE_VALUE when it cannot be opened.
        //   throws  — nothing.
        inline HANDLE OpenInheritableConsole(const wchar_t* name) noexcept
        {
            SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
            return ::CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        }

        // Writes one line for the user to `h`: WriteConsoleW on a console (right in any code page), UTF-8 bytes through
        // WriteFile on a pipe or a file (what the CLI itself writes there). Nothing happens for an empty handle.
        //   h       — the destination, possibly null or INVALID_HANDLE_VALUE.
        //   text    — the line, without its line break.
        //   throws  — std::bad_alloc only (the UTF-8 copy).
        inline void WriteLine(HANDLE h, std::wstring_view text)
        {
            if (!h || h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            DWORD mode = 0;
            DWORD wrote = 0;
            if (::GetConsoleMode(h, &mode))
            {
                ::WriteConsoleW(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
                ::WriteConsoleW(h, L"\r\n", 2, &wrote, nullptr);
                return;
            }
            const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(bytes > 0 ? static_cast<size_t>(bytes) : 0u, '\0');
            if (bytes > 0)
            {
                ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
            }
            utf8.push_back('\n');
            ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr);
        }

        // The running exe's full path. GetModuleFileNameW truncates silently into a short buffer, so the buffer grows
        // until the path fits (a long-path install can pass MAX_PATH).
        //   returns — the path, or "" when Windows cannot give it (the caller must then not start anything).
        //   throws  — std::bad_alloc only.
        inline std::wstring ModulePath()
        {
            std::wstring buffer(MAX_PATH, L'\0');
            while (buffer.size() <= 32768)
            {
                const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length == 0)
                {
                    return {};
                }
                if (length < buffer.size())
                {
                    buffer.resize(length);
                    return buffer;
                }
                buffer.resize(buffer.size() * 2); // truncated: the path filled the whole buffer
            }
            return {};
        }
    }

    // The APP exe's first step (WindowsTerminal/main.cpp, before the architecture check, COM, the single-instance
    // hand-off, the install question and the profile): when the command line is a CLI call, run the agentmaster-cli.exe
    // beside the exe on the caller's handles or console (PlanCliLaunch), wait for it, and give back its exit code. The
    // app is NEVER started for a CLI call, also when the CLI cannot be started: then one line on the caller's error
    // handle says why, and the exit code is kCliStartFailedExitCode.
    //   returns — std::nullopt when the command line is not a CLI call (the caller goes on as the app); otherwise the
    //             exit code for this process (the CLI's, any value a crash's NTSTATUS included, or
    //             kCliStartFailedExitCode).
    //   throws  — nothing: a std::bad_alloc after the decision ends in kCliStartFailedExitCode with a fixed note.
    inline std::optional<int> TryRunCliCall() noexcept
    {
        // Views only, so nothing above the try can throw, and the decision is made before anything can fail.
        const std::wstring_view tail = CommandlineTail(::GetCommandLineW());
        if (!IsCliInvocation(tail))
        {
            return std::nullopt;
        }
        try
        {
            // Read what the caller passed BEFORE AttachConsole, which may fill this process's empty standard handles
            // with the console's: those must not be mistaken for handles the caller gave.
            STARTUPINFOW caller{};
            caller.cb = sizeof(caller);
            ::GetStartupInfoW(&caller);
            const bool callerPassedHandles = (caller.dwFlags & STARTF_USESTDHANDLES) != 0;

            // Attaching first lets the CLI share the caller's console, so it opens no window of its own. It fails
            // harmlessly when the caller has no console (pythonw, a scheduled task, Explorer).
            const bool attached = ::AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
            const CliLaunchPlan plan = PlanCliLaunch(callerPassedHandles, attached);

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            detail::ScopedHandle consoleIn;
            detail::ScopedHandle consoleOut;
            HANDLE errorSink = nullptr; // where a failure to start the CLI is reported
            switch (plan.output)
            {
            case CliOutput::CallerHandles:
                // A fresh STARTUPINFO with only the three handles: the caller's lpReserved2 (a C runtime's or Cygwin's
                // file-descriptor table) is meant for THIS process and must not reach the CLI's C runtime.
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdInput = caller.hStdInput;
                si.hStdOutput = caller.hStdOutput;
                si.hStdError = caller.hStdError;
                errorSink = caller.hStdError;
                break;
            case CliOutput::CallerConsole:
                consoleIn.value = detail::OpenInheritableConsole(L"CONIN$");
                consoleOut.value = detail::OpenInheritableConsole(L"CONOUT$");
                if (consoleIn.Valid() && consoleOut.Valid())
                {
                    si.dwFlags = STARTF_USESTDHANDLES;
                    si.hStdInput = consoleIn.value;
                    si.hStdOutput = consoleOut.value;
                    si.hStdError = consoleOut.value;
                    errorSink = consoleOut.value;
                }
                // Otherwise no handles are passed: a console child that shares the attached console then uses the
                // console's own handles, which is the best that is left.
                break;
            case CliOutput::OwnConsole:
                break; // the new console's own handles; a failure has nowhere to be shown
            }

            const std::wstring exePath = detail::ModulePath();
            if (exePath.empty())
            {
                detail::WriteLine(errorSink, L"agentmaster: this program's own path is unknown, so its agentmaster-cli.exe cannot be found; the app is not started for a CLI command.");
                return kCliStartFailedExitCode;
            }
            const std::wstring cliPath = CliExePathBeside(exePath);
            std::wstring cmdline = BuildCliCommandLine(tail);

            // TRUE: the caller's handles in `si` reach the CLI only by inheritance (the launcher shim does the same).
            PROCESS_INFORMATION pi{};
            if (!::CreateProcessW(cliPath.c_str(), cmdline.data(), nullptr, nullptr, TRUE, plan.creationFlags, nullptr, nullptr, &si, &pi))
            {
                const DWORD error = ::GetLastError();
                detail::WriteLine(errorSink, L"agentmaster: could not start " + cliPath + L" (Windows error " + std::to_wstring(error) + L"); the app is not started for a CLI command.");
                return kCliStartFailedExitCode;
            }
            ::CloseHandle(pi.hThread);
            ::WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = static_cast<DWORD>(kCliStartFailedExitCode);
            ::GetExitCodeProcess(pi.hProcess, &code);
            ::CloseHandle(pi.hProcess);
            return static_cast<int>(code);
        }
        catch (...)
        {
            // Only std::bad_alloc can land here (the paths and the command line are std::wstring), before the CLI ran.
            // This exe has no log yet (the profile is not resolved), so the caller's error handle is the one channel; a
            // fixed ASCII note goes there through WriteFile, which allocates nothing. Starting the app instead would
            // bring back the very bug this function exists for.
            static constexpr char kNote[] = "agentmaster: out of memory while starting agentmaster-cli.exe; the app is not started for a CLI command\n";
            DWORD wrote = 0;
            ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), kNote, static_cast<DWORD>(sizeof(kNote) - 1), &wrote, nullptr);
            return kCliStartFailedExitCode;
        }
    }
}
