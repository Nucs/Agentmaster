// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Stand-in for agentmaster-cli.exe (console subsystem), built under that name beside fakegui.exe: prints its
// command line and what its standard output is (console / pipe / file / other), writes one line to stderr, appends
// the same facts to fakecli.log beside itself (so a run whose output went to a hidden console can still be checked),
// and exits 42, or with the number after "exit=" when its arguments hold one.

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <string>

int wmain()
{
    const wchar_t* cmd = ::GetCommandLineW();
    DWORD mode = 0;
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const bool console = ::GetConsoleMode(out, &mode) != FALSE;
    const DWORD type = ::GetFileType(out);
    const wchar_t* kind = console ? L"console" : (type == FILE_TYPE_PIPE ? L"pipe" : (type == FILE_TYPE_DISK ? L"file" : L"other"));
    std::wprintf(L"FAKECLI cmdline=[%ls] stdout=%ls\n", cmd, kind);
    std::fflush(stdout);
    std::fwprintf(stderr, L"FAKECLI stderr line\n");

    wchar_t self[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring log{ self };
    log = log.substr(0, log.find_last_of(L'\\') + 1) + L"fakecli.log";
    std::wofstream(log, std::ios::app) << L"cmdline=[" << cmd << L"] stdout=" << kind << L" consoleWindow=" << (::GetConsoleWindow() ? L"yes" : L"none") << L"\n";

    const std::wstring line{ cmd };
    const auto at = line.find(L"exit=");
    return at == std::wstring::npos ? 42 : _wtoi(line.c_str() + at + 5);
}
