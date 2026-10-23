// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Stand-in for Agentmaster.exe (GUI subsystem) that runs the REAL CliDispatch.h first step exactly as
// WindowsTerminal/main.cpp does (doc/agentmaster/CLI.md §2a). When the command line is not a CLI call it starts
// nothing: it writes app-would-start.txt beside itself (the command line it got) and exits 7, so a run can tell
// "the app would have started" from "the CLI answered".
//
// Deliberately NOT named Agentmaster.exe: a bare `Agentmaster.exe` typed in cmd.exe with
// NoDefaultCurrentDirectoryInExePath=1 skips the current folder and finds the INSTALLED app on PATH (it started the
// real all-users copy once on 2026-10-23). TryRunCliCall does not care about its own exe's name.

#include <windows.h>

#include "CliDispatch.h"

#include <fstream>
#include <string>

int __stdcall wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // The same two steps, in the same order, as the app's wWinMain.
    SetConsoleCtrlHandler(NULL, FALSE);
    if (const auto cliExitCode = ::Agentmaster::CliDispatch::TryRunCliCall())
    {
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(*cliExitCode));
        __assume(false);
    }

    // Not a CLI call: record what the app would have been started with, beside this exe.
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring marker{ self };
    marker = marker.substr(0, marker.find_last_of(L'\\') + 1) + L"app-would-start.txt";
    std::wofstream(marker) << GetCommandLineW() << L"\n";
    return 7;
}
