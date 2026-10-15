// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database service's log (see SessionDbLog.h).

#include "SessionDbLog.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <mutex>

namespace Agentmaster::SessionDb
{
    namespace
    {
        std::mutex g_logMtx;
        std::wstring g_logPath;
        bool g_echo = false;
    }

    void InitServiceLog(const std::wstring& path, bool echo)
    {
        std::lock_guard lk{ g_logMtx };
        g_logPath = path;
        g_echo = echo;
    }

    void ServiceLog(const std::wstring& line) noexcept
    {
        try
        {
            SYSTEMTIME st{};
            ::GetLocalTime(&st);
            wchar_t stamp[32];
            swprintf_s(stamp, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            std::wstring text = stamp;
            text += line;
            if (text.empty() || text.back() != L'\n')
            {
                text.push_back(L'\n');
            }
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (n <= 0)
            {
                return;
            }
            std::string utf8(static_cast<size_t>(n), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
            std::lock_guard lk{ g_logMtx };
            if (g_echo)
            {
                std::fwrite(utf8.data(), 1, utf8.size(), stderr);
                std::fflush(stderr);
            }
            if (g_logPath.empty())
            {
                return;
            }
            // One FILE_APPEND_DATA write per line, so two processes on one profile (a one-shot verify beside a
            // running service) interleave whole lines.
            const HANDLE h = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            DWORD written = 0;
            ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            ::CloseHandle(h);
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE: this IS the service's logging path (an allocation failure while formatting a
            // line); reporting it would recurse into the logger itself.
        }
    }
}
