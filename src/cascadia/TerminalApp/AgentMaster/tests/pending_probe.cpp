// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the OUT-OF-BAND pending-input probe (PENDING_INPUT.md §6 "LIVE" verification — the
// ad-hoc ground-truth oracle, the uia_probe.cpp precedent: standalone, NOT in the msbuild).
//
// Reads a live claude.exe's ConPTY screen buffer from a SEPARATE process — AttachConsole(pid) ->
// CONOUT$ -> ReadConsoleOutputCharacterW — and runs the app's REAL detector (PendingInput.h) plus the
// REAL paste resolver (PendingPaste.h) over it, so what the shipped scan would extract is verifiable
// with the app running, stopped, or not even installed. Strictly read-only (repeatedly verified
// against live sessions); same-user only; stdout dies on AttachConsole, so the report goes to a FILE.
//
// This is the instrument the 2026-07-23 hardening pass was built on: the whole-fleet capture sweep
// (76 pids, 2.1.211..2.1.218 — every attachable screen detected 17/17), the NBSP-defect live proof,
// and the wow-sess truncated-paste resolution (bf8eefafa3e80676.txt, expanded 1,036 -> 18,647 chars).
//
// Usage: pending_probe.exe <claude-pid> <outfile> [tailRows]
//   Resolve a session id -> pid via claude's own presence heartbeat first:
//     grep -l "<session-id>" ~/.claude/sessions/*.json   ->  <pid>.json
//   The report: the detector verdict + the extracted draft + leading code points + the paste-marker
//   resolution against the REAL <claude home>\paste-cache + the raw tail rows, each escaped as a C++
//   wide-literal body (fixture-ready — the §6 REAL-capture test rows came from exactly this output).
// Exit code: 0 = box found, 1 = no box / attach failed, 2 = usage.

#include <windows.h>

#include <fstream>
#include <string>
#include <vector>

#include "../PendingInput.h"
#include "../PendingPaste.h"

namespace
{
    std::string Utf8(const std::wstring& w)
    {
        if (w.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }

    // Escape a row as a C++ wide-string-literal body: printable ASCII stays, everything else \uXXXX —
    // which is exactly what makes an invisible byte (the NBSP separator!) visible in a fixture.
    std::string EscapeRow(const std::wstring& w)
    {
        std::string s;
        s.reserve(w.size() + 16);
        for (const wchar_t c : w)
        {
            if (c == L'\\')
            {
                s += "\\\\";
            }
            else if (c == L'"')
            {
                s += "\\\"";
            }
            else if (c >= 0x20 && c < 0x7F)
            {
                s += (char)c;
            }
            else
            {
                char b[8];
                ::sprintf_s(b, "\\u%04X", (unsigned)c);
                s += b;
            }
        }
        return s;
    }

    std::wstring RTrimW(const std::wstring& w)
    {
        size_t e = w.size();
        while (e > 0 && (w[e - 1] == L' ' || w[e - 1] == L'\t'))
        {
            --e;
        }
        return w.substr(0, e);
    }

    // The paste cache dir, resolved like ClaudeSpawn's ClaudePasteCacheDir (kept local so the probe
    // links against only the two pure headers — no engine TU list to keep in lockstep).
    std::wstring PasteCacheDir()
    {
        wchar_t buf[4096];
        DWORD n = ::GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", buf, 4096);
        std::wstring base = (n > 0 && n < 4096) ? std::wstring{ buf, n } : std::wstring{};
        if (base.empty())
        {
            n = ::GetEnvironmentVariableW(L"USERPROFILE", buf, 4096);
            if (n == 0 || n >= 4096)
            {
                return {};
            }
            base = std::wstring{ buf, n } + L"\\.claude";
        }
        return base + L"\\paste-cache";
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        return 2;
    }
    const DWORD pid = (DWORD)_wtoi(argv[1]);
    const std::wstring outPath = argv[2];
    const int tailRows = (argc >= 4) ? _wtoi(argv[3]) : 60;

    std::string report;
    ::FreeConsole();
    const bool attached = ::AttachConsole(pid) != FALSE;
    report += "pid=" + std::to_string(pid) + " attached=" + (attached ? "1" : "0") + "\n";

    std::vector<std::wstring> rows;
    if (attached)
    {
        const HANDLE h = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (h != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(h, &csbi))
        {
            report += "buffer=" + std::to_string(csbi.dwSize.X) + "x" + std::to_string(csbi.dwSize.Y) + "\n";
            std::vector<wchar_t> line(csbi.dwSize.X);
            for (SHORT y = 0; y < csbi.dwSize.Y; ++y)
            {
                DWORD read = 0;
                COORD c{ 0, y };
                if (::ReadConsoleOutputCharacterW(h, line.data(), (DWORD)line.size(), c, &read))
                {
                    rows.emplace_back(line.data(), read);
                }
                else
                {
                    rows.emplace_back();
                }
            }
        }
        if (h != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(h);
        }
        ::FreeConsole();
    }

    const auto d = Agentmaster::DetectPendingInput(rows);
    report += "boxFound=" + std::string(d.boxFound ? "1" : "0") +
              " caretRow=" + std::to_string(d.caretRow) +
              " bottomRuleRow=" + std::to_string(d.bottomRuleRow) +
              " chars=" + std::to_string(d.text.size()) + "\ncps=";
    for (size_t i = 0; i < d.text.size() && i < 10; ++i)
    {
        char b[16];
        ::sprintf_s(b, "%04X ", (unsigned)d.text[i]);
        report += b;
    }
    report += "\n---- DRAFT ----\n" + Utf8(d.text) + "\n---- END DRAFT ----\n";

    // Resolve any paste markers against the REAL paste-cache (mirrors ResolvePendingPasteRefsIn).
    const auto markers = Agentmaster::FindPasteMarkers(d.text);
    report += "paste markers=" + std::to_string(markers.size()) + "\n";
    if (!markers.empty())
    {
        std::vector<Agentmaster::PasteFileText> files;
        const std::wstring cacheDir = PasteCacheDir();
        WIN32_FIND_DATAW fd{};
        HANDLE fh = ::FindFirstFileW((cacheDir + L"/*.txt").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    continue;
                }
                std::ifstream f((cacheDir + L"/" + fd.cFileName).c_str(), std::ios::binary);
                if (!f)
                {
                    continue;
                }
                std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (bytes.empty() || bytes.size() > 2u * 1024 * 1024)
                {
                    continue;
                }
                const int need = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
                if (need <= 0)
                {
                    continue;
                }
                std::wstring wide((size_t)need, L'\0');
                ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), wide.data(), need);
                files.push_back({ fd.cFileName, std::move(wide) });
            } while (::FindNextFileW(fh, &fd));
            ::FindClose(fh);
        }
        report += "cache files read=" + std::to_string(files.size()) + " (" + Utf8(cacheDir) + ")\n";
        const auto res = Agentmaster::ResolvePasteMarkers(markers, files);
        for (size_t i = 0; i < markers.size(); ++i)
        {
            report += std::string(markers[i].truncated ? "truncated" : "paste") + " #" + std::to_string(markers[i].index) +
                      " (+" + std::to_string(markers[i].lines) + " lines) -> " +
                      (res[i].resolved ? Utf8(res[i].fileName) : "unresolved") +
                      (res[i].resolved && !markers[i].truncated ? " (by line count)" : "") + "\n";
        }
    }

    // Raw tail rows, escaped fixture-ready.
    const int n = (int)rows.size();
    const int start = n > tailRows ? n - tailRows : 0;
    report += "---- ROWS " + std::to_string(start) + ".." + std::to_string(n - 1) + " (escaped) ----\n";
    for (int y = start; y < n; ++y)
    {
        report += "R" + std::to_string(y) + ": \"" + EscapeRow(RTrimW(rows[y])) + "\"\n";
    }
    report += "---- END ROWS ----\n";

    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    f.write(report.data(), (std::streamsize)report.size());
    return d.boxFound ? 0 : 1;
}
