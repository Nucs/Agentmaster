// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Starts fakegui.exe (or the real launcher shim, shim\wtd.exe) the ways a shell cannot be made to from Git Bash, and
// prints its exit code:
//   launcher console  <full exe path> <args...>  no standard handles passed (like an interactive cmd.exe); the
//                                                launcher's console is shared, so the CLI writes to that console
//   launcher detached <full exe path> <args...>  FreeConsole first (a caller without a console, like pythonw or a
//                                                scheduled task); stdout and stderr = out-detached.txt beside the exe
//   launcher none     <full exe path> <args...>  FreeConsole first and NO handles (the Run box, a shortcut): nobody can
//                                                read the CLI's text. ⚠ Only for lines the app answers (help, the
//                                                version): a CLI verb would get a console window of its own
//                                                (CREATE_NEW_CONSOLE from CliDispatch.h) on the user's screen.
// Never opens a window: run from Claude Code's Bash tool, the launcher's console is the tool's hidden one; detached and
// none have no console at all, and their child is started with CREATE_NO_WINDOW, so a CONSOLE child (the shim) gets a
// console without a window, as the Run box's would have one nobody reads (a GUI child ignores the flag; the CLI it
// starts gets CREATE_NO_WINDOW from CliDispatch.h).

#include <windows.h>

#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::printf("usage: launcher console|detached|none <full path of fakegui.exe or shim\\wtd.exe> <args...>\n");
        return 2;
    }
    const std::wstring mode = argv[1];
    std::wstring cmdline = L"\"" + std::wstring{ argv[2] } + L"\"";
    for (int i = 3; i < argc; ++i)
    {
        cmdline += L" ";
        cmdline += argv[i];
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    BOOL inherit = FALSE;
    HANDLE file = nullptr;
    if (mode == L"detached")
    {
        std::wstring path{ argv[2] };
        path = path.substr(0, path.find_last_of(L'\\') + 1) + L"out-detached.txt";
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE }; // inheritable: it must reach the CLI through fakegui
        file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = file;
        si.hStdError = file;
        inherit = TRUE;
        ::FreeConsole();
    }
    else if (mode == L"none")
    {
        // No handles and no console to attach to: the child's TryRunCliCall plans OwnConsole, and the shim finds itself
        // alone on its console. The launcher's own stdout is still the pipe Git Bash gave it, so its result line arrives.
        ::FreeConsole();
    }
    else if (mode != L"console")
    {
        std::printf("LAUNCHER unknown mode %ls\n", mode.c_str());
        return 2;
    }
    // Without a console of the launcher's own, a console child (the shim) would get a NEW one, with a window on the
    // user's screen: CREATE_NO_WINDOW gives it one without. Ignored for a GUI child (fakegui.exe).
    const DWORD flags = mode == L"console" ? 0u : static_cast<DWORD>(CREATE_NO_WINDOW);

    // lpApplicationName = the full path: never a search of PATH or of the current folder.
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(argv[2], cmdline.data(), nullptr, nullptr, inherit, flags, nullptr, nullptr, &si, &pi))
    {
        std::printf("LAUNCHER CreateProcess failed %lu\n", ::GetLastError());
        return 3;
    }
    ::CloseHandle(pi.hThread);
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    std::printf("LAUNCHER %ls child exit=%lu\n", mode.c_str(), code);
    if (file)
    {
        ::CloseHandle(file);
    }
    return 0;
}
