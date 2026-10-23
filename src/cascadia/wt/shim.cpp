// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstddef>
#include <string>
#include <string_view>
#include <filesystem>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wil/stl.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>

// Agentmaster: the ONE rule that tells a CLI call from a launch (IsCliInvocation, CommandlineTail),
// shared with the app exe's own wWinMain, which answers the same command lines when the name
// `agentmaster` finds Agentmaster.exe itself (an unpackaged install on PATH, CLI.md §2a).
#include "../TerminalApp/AgentMaster/CliDispatch.h"
// Agentmaster: the caller's console or stderr, handed to the app this shim starts, so every dialog the app opens can
// be echoed where the command ran (CLI.md §2b). Header-only, plain Win32.
#include "../TerminalApp/AgentMaster/CallerSink.h"

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
// Which first tokens are a CLI call (the verbs and the CLI-only flags) is CliDispatch.h's to say:
// add a new CLI verb THERE, and this shim and the app exe both route it.

#pragma warning(suppress : 26461) // we can't change the signature of wmain
int __cdecl wmain(int /*argc*/, wchar_t** /*argv*/)
{
    std::filesystem::path module{ wil::GetModuleFileNameW<std::wstring>(nullptr) };

    // Cache our name (wt, wtd, agentmaster, agentmasterdev)
    std::wstring ourFilename{ module.filename().wstring() };

    // The command line after argv[0], quoting and spacing kept: the classic wWinMain lpCmdLine, so the
    // GUI forward below passes on exactly what the caller typed.
    const std::wstring_view tail = ::Agentmaster::CliDispatch::CommandlineTail(GetCommandLineW());

    // --- a CLI invocation -> the console introspection tool, on this console ---
    if (::Agentmaster::CliDispatch::IsCliInvocation(tail))
    {
        std::filesystem::path cli{ module };
        cli.replace_filename(::Agentmaster::CliDispatch::kCliExeLeaf);

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

    // Agentmaster (CallerSink.h; doc/agentmaster/CLI.md §2b): this shim exits as soon as the app is started, so the app
    // cannot find the caller's console or pipe by itself (its parent is gone), and a dialog it opens (the update prompt,
    // "Reopen your N windows?", Windows Terminal's --help box) would never reach the command's output. Hand it over:
    // the caller's stderr pipe or file as ONE inheritable handle (PROC_THREAD_ATTRIBUTE_HANDLE_LIST: nothing else is
    // inherited, exactly as with bInheritHandles=FALSE before), or the caller's console by a process attached to it,
    // named in AGENTMASTER_CALLER_SINK. Nothing to hand over (our reopen ShellExecute: the console was freed above)
    // leaves the start below exactly as it always was. The app releases the handle when its startup ends.
    auto forward = ::Agentmaster::CallerSink::PrepareForChild();
    const auto closeForward = wil::scope_exit([&]() noexcept { ::Agentmaster::CallerSink::CloseForward(forward); });

    // Go!
    wil::unique_process_information pi;
    if (forward.inheritable)
    {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<std::byte> storage(bytes);
        const auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (InitializeProcThreadAttributeList(list, 1, 0, &bytes))
        {
            const auto deleteList = wil::scope_exit([&]() noexcept { DeleteProcThreadAttributeList(list); });
            if (UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &forward.inheritable, sizeof(forward.inheritable), nullptr, nullptr))
            {
                STARTUPINFOEXW six{};
                six.StartupInfo = si;
                six.StartupInfo.cb = sizeof(six);
                // The caller's own standard handles are not in the list, so they could not reach the app anyway (they
                // never did: the start used bInheritHandles=FALSE); don't name them.
                six.StartupInfo.dwFlags &= ~STARTF_USESTDHANDLES;
                six.StartupInfo.hStdInput = six.StartupInfo.hStdOutput = six.StartupInfo.hStdError = nullptr;
                six.lpAttributeList = list;
                if (CreateProcessW(module.c_str(), cmdline.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &six.StartupInfo, &pi))
                {
                    return 0;
                }
            }
        }
        // The hand-over could not be set up: start the app without it, as before.
        ::Agentmaster::CallerSink::CloseForward(forward);
    }
    return !CreateProcessW(module.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
}
