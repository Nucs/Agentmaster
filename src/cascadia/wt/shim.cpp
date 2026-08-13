// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <string>
#include <string_view>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wil/stl.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>

// Agentmaster: this launcher shim is the target of the agentmaster[dev].exe execution alias, and it
// is now DUAL-MODE (doc/agentmaster/CLI.md §2):
//   * a known CLI verb (show/list/sessions/...) -> exec agentmaster-cli.exe on THIS console, so
//     `agentmaster show ...` prints to the caller's console and the shell waits for it. That REQUIRES
//     this launcher to be CONSOLE subsystem (set in wt.vcxproj) — a GUI-subsystem exe cannot own
//     stdout (the reason `wt.exe` never returned output);
//   * ANY other commandline -> forward to Agentmaster.exe (the GUI binary) EXACTLY as the original shim did (bare
//     launch, `-w`/`-s` reopen, etc.). Defterm/COM handoff and Start-menu activation never hit the
//     alias (they target Agentmaster.exe directly), so they are unaffected.
// Being console subsystem, a launch from a real shell attaches to the existing console (no flash);
// the only no-console caller is our own reopen ShellExecute, where the loader allocates a console —
// we FreeConsole it before forwarding so a GUI launch never flashes a console window.

namespace
{
    bool IsCliVerb(std::wstring_view v)
    {
        // Keep in sync with the agentmaster-cli verb set (CLI.md §5).
        static constexpr std::wstring_view kVerbs[] = {
            L"show", L"list", L"sessions", L"tabs", L"windows", L"external", L"restore", L"archive"
        };
        for (const auto k : kVerbs)
        {
            if (v == k)
            {
                return true;
            }
        }
        return false;
    }

    // The commandline with argv[0] (the program path/name) stripped — i.e. the classic wWinMain
    // pCmdLine — preserving the ORIGINAL quoting/spacing of the remaining args verbatim, so the
    // GUI-forward below is byte-for-byte what the original shim forwarded.
    std::wstring_view CommandlineTail()
    {
        std::wstring_view s{ GetCommandLineW() };
        if (!s.empty() && s.front() == L'"')
        {
            const auto e = s.find(L'"', 1);
            s = (e == std::wstring_view::npos) ? std::wstring_view{} : s.substr(e + 1);
        }
        else
        {
            const auto e = s.find(L' ');
            s = (e == std::wstring_view::npos) ? std::wstring_view{} : s.substr(e);
        }
        while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
        {
            s.remove_prefix(1);
        }
        return s;
    }

    std::wstring_view FirstToken(std::wstring_view s)
    {
        const auto e = s.find_first_of(L" \t");
        return e == std::wstring_view::npos ? s : s.substr(0, e);
    }

    // A CLI invocation == the first token is a verb (the standard verb-first form,
    // `agentmaster show ...`) OR a CLI-ONLY global flag (so `agentmaster --instance dev show ...`
    // also dispatches). The listed flags are unique to the CLI — none collide with a
    // WindowsTerminal commandline token — so a GUI launch (`-w`, `-s`, `nt`, `-Embedding`, bare) is
    // never misrouted to the CLI; it forwards to the GUI as before.
    bool IsCliInvocation(std::wstring_view tail)
    {
        const auto tok = FirstToken(tail);
        if (IsCliVerb(tok))
        {
            return true;
        }
        static constexpr std::wstring_view kCliFlags[] = {
            L"--json", L"--self", L"--offline", L"--tail", L"--instance", L"--state", L"--dir"
        };
        for (const auto f : kCliFlags)
        {
            if (tok == f)
            {
                return true;
            }
        }
        return false;
    }
}

#pragma warning(suppress : 26461) // we can't change the signature of wmain
int __cdecl wmain(int /*argc*/, wchar_t** /*argv*/)
{
    std::filesystem::path module{ wil::GetModuleFileNameW<std::wstring>(nullptr) };

    // Cache our name (wt, wtd, agentmaster, agentmasterdev)
    std::wstring ourFilename{ module.filename().wstring() };

    const std::wstring_view tail = CommandlineTail();

    // --- a CLI invocation -> the console introspection tool, on this console ---
    if (IsCliInvocation(tail))
    {
        std::filesystem::path cli{ module };
        cli.replace_filename(L"agentmaster-cli.exe");

        // argv[0] then the verbatim tail (the tail already begins with the verb).
        std::wstring cmdline;
        if (FAILED(wil::str_printf_nothrow(cmdline, L"agentmaster-cli %.*s", static_cast<int>(tail.size()), tail.data())))
        {
            return 1;
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        GetStartupInfoW(&si);

        wil::unique_process_information pi;
        if (!CreateProcessW(cli.c_str(), cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
        {
            return 1;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        return static_cast<int>(code);
    }

    // --- anything else -> forward to the GUI, EXACTLY as the original shim did ---
    // If we are the ONLY process attached to this console, we allocated it (launched with no parent
    // console — e.g. our reopen ShellExecute) — drop it so the GUI launch doesn't flash a console.
    {
        DWORD pids[2]{};
        if (GetConsoleProcessList(pids, 2) <= 1)
        {
            FreeConsole();
        }
    }

    // Swap wt[d].exe / agentmaster[dev].exe for Agentmaster.exe (the GUI binary)
    module.replace_filename(L"Agentmaster.exe");

    // Append the rest of the commandline to the saved name (== the original `%s %s` with pCmdLine).
    std::wstring cmdline;
    if (FAILED(wil::str_printf_nothrow(cmdline, L"%s %.*s", ourFilename.c_str(), static_cast<int>(tail.size()), tail.data())))
    {
        return 1;
    }

    // Get our startup info so it can be forwarded
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    GetStartupInfoW(&si);

    // Go!
    wil::unique_process_information pi;
    return !CreateProcessW(module.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
}
