// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Starts fakegui.exe the two ways a shell cannot be made to from Git Bash, and prints its exit code:
//   launcher console  <full exe path> <args...>  no standard handles passed (like an interactive cmd.exe); the
//                                                launcher's console is shared, so the CLI writes to that console
//   launcher detached <full exe path> <args...>  FreeConsole first (a caller without a console, like pythonw or a
//                                                scheduled task); stdout and stderr = out-detached.txt beside the exe
// Never opens a window: run from Claude Code's Bash tool, the launcher's console is the tool's hidden one, and the
// detached case has no console at all (the CLI then gets CREATE_NO_WINDOW from CliDispatch.h).

#include <windows.h>

#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::printf("usage: launcher console|detached <full path of fakegui.exe> <args...>\n");
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

    // lpApplicationName = the full path: never a search of PATH or of the current folder.
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(argv[2], cmdline.data(), nullptr, nullptr, inherit, 0, nullptr, nullptr, &si, &pi))
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
