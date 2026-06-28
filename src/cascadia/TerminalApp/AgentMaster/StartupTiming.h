// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — startup phase timing (header-only, plain Win32; no WinRT, no engine deps).
//
// WHY: "Agentmaster launch is very slow" — we couldn't SEE where the time went. This header lets
// BOTH the WindowsTerminal EXE (WindowEmperor — links TerminalApp.dll, NOT the TerminalAppLib
// static lib, so it can't call the engine's AppendStateLog) AND the DLL (TerminalPage / Engine)
// drop `[startup]` timing lines into the SAME <profile>\hooks.log, on ONE shared clock.
//
// The clock is the PROCESS CREATION time (GetProcessTimes), so every line's `(+<N>ms)` anchor is
// "milliseconds since the process was created" — comparable across the exe/dll boundary and across
// every phase. Reconstruct the whole launch timeline with:  grep '\[startup\]' hooks.log
//
// FORMAT (matches AppendStateLog's [HH:MM:SS.mmm] line stamp so the lines interleave cleanly):
//   [12:34:56.789] [startup] <phase>: <durationMs>ms (+<sinceProcessStartMs>ms)   <- a completed phase
//   [12:34:56.789] [startup] <label> (+<sinceProcessStartMs>ms)                   <- an instant milestone
//
// USAGE:
//   Agentmaster::Startup::ScopedPhase _t{ L"engine-init" };   // logs duration when the scope exits
//   Agentmaster::Startup::Mark(L"entering message loop");     // logs an instantaneous +Nms anchor
//   const auto t0 = ::GetTickCount64(); /* ...work... */
//   Agentmaster::Startup::Phase(L"reopen-scan", ::GetTickCount64() - t0);
//
// SAFETY: Log() resolves the active profile via Agentmaster::Profiles::ResolveProfileDir() (which
// caches), so callers MUST NOT emit a line before the profile is resolved — doing so would cache the
// profile dir prematurely (Rule #15). On the EXE side that means: only after EnsureProfileResolved-
// AtStartup. On the DLL side the profile is always resolved by the time any window lays out. Timing
// itself (GetTickCount64 / GetProcessTimes) touches no profile state and is safe anywhere.

#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio> // swprintf (timestamp prefix)
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#include "ProfileBootstrap.h" // Agentmaster::Profiles::ResolveProfileDir (the active <profile> dir)

namespace Agentmaster::Startup
{
    // Milliseconds since THIS process was created. The same value in the exe and the dll (one
    // process), so it stitches the EXE-side and DLL-side [startup] lines onto one timeline.
    inline uint64_t SinceProcessStartMs()
    {
        FILETIME create{}, exit{}, kernel{}, user{};
        if (!::GetProcessTimes(::GetCurrentProcess(), &create, &exit, &kernel, &user))
        {
            return 0;
        }
        ULARGE_INTEGER c{};
        c.LowPart = create.dwLowDateTime;
        c.HighPart = create.dwHighDateTime;
        FILETIME nowFt{};
        ::GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER n{};
        n.LowPart = nowFt.dwLowDateTime;
        n.HighPart = nowFt.dwHighDateTime;
        if (n.QuadPart <= c.QuadPart)
        {
            return 0;
        }
        return (n.QuadPart - c.QuadPart) / 10000ULL; // 100-ns FILETIME ticks -> ms
    }

    namespace detail
    {
        // The same [HH:MM:SS.mmm] prefix AppendStateLog writes, so our lines line up with the hook
        // event stream in the same file.
        inline std::wstring TimestampPrefix()
        {
            SYSTEMTIME st{};
            ::GetLocalTime(&st);
            wchar_t buf[24];
            ::swprintf(buf,
                       24,
                       L"[%02u:%02u:%02u.%03u] ",
                       static_cast<unsigned>(st.wHour),
                       static_cast<unsigned>(st.wMinute),
                       static_cast<unsigned>(st.wSecond),
                       static_cast<unsigned>(st.wMilliseconds));
            return std::wstring{ buf };
        }

        inline void Write(std::wstring line)
        {
            // Best-effort, thread-safe (the exe main thread and the dll window threads can both call
            // this). A separate ofstream/mutex from the lib's AppendStateLog is fine: each record is a
            // whole '\n'-terminated line in append mode, so an occasional interleave can't corrupt one.
            static std::mutex mtx;
            std::lock_guard<std::mutex> lk{ mtx };
            try
            {
                if (line.empty() || line.back() != L'\n')
                {
                    line.push_back(L'\n');
                }
                line = TimestampPrefix() + line;
                ::OutputDebugStringW(line.c_str()); // also surface in the debugger / DebugView

                const auto& dir = ::Agentmaster::Profiles::ResolveProfileDir();
                if (dir.empty())
                {
                    return;
                }
                const auto path = std::filesystem::path{ dir } / L"hooks.log";
                std::ofstream f(path, std::ios::binary | std::ios::app);
                if (f)
                {
                    const int n = ::WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
                    if (n > 0)
                    {
                        std::string bytes(static_cast<size_t>(n), '\0');
                        ::WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), bytes.data(), n, nullptr, nullptr);
                        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                    }
                }
            }
            catch (...)
            {
            }
        }
    }

    // Log a COMPLETED phase and how long it took: "[startup] <phase>: <durationMs>ms (+<total>ms)".
    inline void Phase(std::wstring_view phase, uint64_t durationMs)
    {
        detail::Write(L"[startup] " + std::wstring{ phase } + L": " + std::to_wstring(durationMs) +
                      L"ms (+" + std::to_wstring(SinceProcessStartMs()) + L"ms)");
    }

    // Log an INSTANTANEOUS milestone: "[startup] <label> (+<total>ms)".
    inline void Mark(std::wstring_view label)
    {
        detail::Write(L"[startup] " + std::wstring{ label } + L" (+" + std::to_wstring(SinceProcessStartMs()) + L"ms)");
    }

    // RAII: measure a scope's wall-clock duration and Phase()-log it when the scope exits (including
    // an early return or an exception unwinding through it).
    struct ScopedPhase
    {
        explicit ScopedPhase(std::wstring_view phase) :
            _phase{ phase }, _start{ ::GetTickCount64() } {}
        ~ScopedPhase() { Phase(_phase, ::GetTickCount64() - _start); }

        ScopedPhase(const ScopedPhase&) = delete;
        ScopedPhase& operator=(const ScopedPhase&) = delete;
        ScopedPhase(ScopedPhase&&) = delete;
        ScopedPhase& operator=(ScopedPhase&&) = delete;

    private:
        std::wstring _phase;
        uint64_t _start;
    };
}
