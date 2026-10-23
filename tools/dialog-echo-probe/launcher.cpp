// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Starts a program on a HIDDEN desktop (WinSta0\AgentmasterDialogProbe: windows there never reach the user's screen)
// the way a command's caller would, and prints what that program echoed for its dialogs:
//   launcher pipe       <full exe path> <args...>  stdout + stderr = a pipe this launcher reads to its end (bash, Python,
//                                                  Claude Code's Bash tool)
//   launcher console    <full exe path> <args...>  an interactive cmd.exe: this launcher's standard handles become its
//                                                  console's, and Windows copies them into a CONSOLE child by itself (a
//                                                  GUI child gets none)
//   launcher conhandles <full exe path> <args...>  a caller that passes the console's handles explicitly
//                                                  (STARTF_USESTDHANDLES, inheritable, every inheritable handle inherited)
//   launcher pspiped    <full exe path> <args...>  PowerShell 7 running a program whose output is piped on
//                                                  (`& x | Out-String`; measured 2026-10-23): stdin and stderr = the
//                                                  console's handles, stdout = a pipe this launcher reads to its end.
//                                                  (Unpiped, PowerShell 7 passes no handles at all, like cmd.exe.)
// In the console modes the launcher first blanks its console, then reads back the rows that hold "[agentmaster
// dialog]" once the child ended (the console is the Bash tool's hidden one, never on the user's screen).
// The program is started by its FULL path only: a bare name could find the installed Agentmaster on PATH.
// Exit code: the child's.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t kDesktop[] = L"AgentmasterDialogProbe";

    // The rows of this process's console that hold a dialog echo (the console is the caller's in the console modes), as
    // UTF-8 for the reader of THIS launcher's stdout.
    //   returns — the rows, trailing blanks removed; empty when this launcher has no console.
    std::vector<std::string> EchoRows()
    {
        std::vector<std::string> rows;
        const HANDLE out = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (out == INVALID_HANDLE_VALUE || !::GetConsoleScreenBufferInfo(out, &info))
        {
            if (out != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(out);
            }
            return rows;
        }
        for (SHORT y = 0; y <= info.dwCursorPosition.Y; ++y)
        {
            std::wstring row(info.dwSize.X, L' ');
            DWORD read = 0;
            ::ReadConsoleOutputCharacterW(out, row.data(), info.dwSize.X, COORD{ 0, y }, &read);
            if (row.find(L"[agentmaster dialog]") != std::wstring::npos)
            {
                while (!row.empty() && row.back() == L' ')
                {
                    row.pop_back();
                }
                const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, row.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string utf8(bytes > 0 ? bytes - 1 : 0, '\0');
                ::WideCharToMultiByte(CP_UTF8, 0, row.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
                rows.push_back(std::move(utf8));
            }
        }
        ::CloseHandle(out);
        return rows;
    }

    // Blanks this launcher's console and puts its cursor home. The Bash tool's hidden console lives across commands, so
    // without this EchoRows would also find the echo of an EARLIER run and pass a check on it.
    //   returns — false when this launcher has no console.
    bool ClearConsole()
    {
        const HANDLE out = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (out == INVALID_HANDLE_VALUE || !::GetConsoleScreenBufferInfo(out, &info))
        {
            if (out != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(out);
            }
            return false;
        }
        const DWORD cells = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
        DWORD done = 0;
        ::FillConsoleOutputCharacterW(out, L' ', cells, COORD{ 0, 0 }, &done);
        ::FillConsoleOutputAttribute(out, info.wAttributes, cells, COORD{ 0, 0 }, &done);
        ::SetConsoleCursorPosition(out, COORD{ 0, 0 });
        ::CloseHandle(out);
        return true;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::printf("usage: launcher pipe|console|conhandles|pspiped <full exe path> <args...>\n");
        return 2;
    }
    const std::wstring mode = argv[1];
    const bool consoleMode = mode == L"console" || mode == L"conhandles" || mode == L"pspiped";
    if (!consoleMode && mode != L"pipe")
    {
        std::printf("LAUNCHER unknown mode\n");
        return 2;
    }
    std::wstring cmdline = L"\"" + std::wstring{ argv[2] } + L"\"";
    for (int i = 3; i < argc; ++i)
    {
        cmdline += L" ";
        cmdline += argv[i];
    }

    const HDESK desktop = ::CreateDesktopW(kDesktop, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop)
    {
        std::printf("LAUNCHER CreateDesktop failed %lu\n", ::GetLastError());
        return 3;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    wchar_t desktopName[64] = L"WinSta0\\AgentmasterDialogProbe";
    si.lpDesktop = desktopName;
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    BOOL inherit = FALSE;
    if (mode == L"pipe")
    {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        ::CreatePipe(&read, &write, &sa, 0);
        ::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0); // only the write end goes to the child
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write;
        si.hStdError = write;
        inherit = TRUE;
    }
    else
    {
        // The console the Bash tool gave this launcher (a hidden one): its input and output, as a shell running in it
        // holds them. The launcher's own standard handles are the Bash tool's pipes, and Windows would otherwise copy
        // THOSE into a console child (measured 2026-10-23: the launcher shim then handed the app "handle:..." and the
        // echo went to the pipe, not to the console). Inheritable, as a real console's handles are: cmd.exe passes its
        // standard handles to its children by inheritance, and a non-inheritable copy reached a program run by
        // `cmd /c` as a dead value (type 0, GetConsoleMode error 6; measured 2026-10-23).
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        const HANDLE conin = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        const HANDLE conout = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (conin == INVALID_HANDLE_VALUE || conout == INVALID_HANDLE_VALUE || !ClearConsole())
        {
            std::printf("LAUNCHER no console %lu\n", ::GetLastError());
            return 3;
        }
        if (mode == L"console")
        {
            // An interactive cmd.exe holds the console's handles as its standard handles, and a CONSOLE program it starts
            // gets copies of them without inheritance (Windows duplicates them; a GUI program gets none). The C runtime's
            // stdout keeps the pipe it found at startup, so this launcher's printf still reaches its reader.
            ::SetStdHandle(STD_INPUT_HANDLE, conin);
            ::SetStdHandle(STD_OUTPUT_HANDLE, conout);
            ::SetStdHandle(STD_ERROR_HANDLE, conout);
        }
        else if (mode == L"conhandles")
        {
            // The console's handles passed explicitly, every inheritable handle inherited.
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = conin;
            si.hStdOutput = conout;
            si.hStdError = conout;
            inherit = TRUE;
        }
        else
        {
            // PowerShell 7 with the output piped on: stdin and stderr stay the console's, stdout is PowerShell's pipe.
            SECURITY_ATTRIBUTES pipeSa{ sizeof(pipeSa), nullptr, TRUE };
            ::CreatePipe(&read, &write, &pipeSa, 0);
            ::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = conin;
            si.hStdOutput = write;
            si.hStdError = conout;
            inherit = TRUE;
        }
    }

    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(argv[2], cmdline.data(), nullptr, nullptr, inherit, 0, nullptr, nullptr, &si, &pi))
    {
        std::printf("LAUNCHER CreateProcess failed %lu\n", ::GetLastError());
        return 3;
    }
    ::CloseHandle(pi.hThread);

    if (read)
    {
        // Our copy of the write end goes, so the pipe ends when every process that holds it closed it: the child, and
        // (through the launcher shim) the app that shim started, once its startup released the caller's sink.
        ::CloseHandle(write);
        std::string all;
        char buf[4096];
        DWORD got = 0;
        while (::ReadFile(read, buf, sizeof(buf), &got, nullptr) && got > 0)
        {
            all.append(buf, got);
        }
        ::CloseHandle(read);
        std::printf("%s", all.c_str());
        std::printf("LAUNCHER pipe ended\n");
    }

    ::WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    if (consoleMode)
    {
        // A program started through the launcher shim (or a cmd.exe that ran the shim) runs on after the process this
        // launcher started exited: wait (up to 6 s) for its last echo, the "answered:" line; a run with no dialog simply
        // waits the 6 s out. In pspiped mode the pipe already ended (the program released everything), so one look
        // tells whether anything reached the console.
        std::vector<std::string> rows;
        for (int i = 0; i < (mode == L"pspiped" ? 1 : 60); ++i)
        {
            rows = EchoRows();
            bool answered = false;
            for (const auto& row : rows)
            {
                answered = answered || row.find("answered:") != std::string::npos;
            }
            if (answered)
            {
                break;
            }
            ::Sleep(100);
        }
        for (const auto& row : rows)
        {
            std::printf("CONSOLE %s\n", row.c_str());
        }
    }
    std::printf("LAUNCHER child exit=%lu\n", code);
    ::CloseDesktop(desktop);
    return static_cast<int>(code);
}
