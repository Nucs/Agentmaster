// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — "a user's own install always wins for that user" (header-only, plain Win32; no WinRT, no engine
// deps — the ProfileBootstrap.h idiom, so the app exe's prelude (through PortableInstall.h) and the console
// agentmaster-cli.exe share one rule).
//
// WHY: an install FOR ALL USERS (Program Files, `install.path` = `machine`) is reached through things a user
// without administrator rights cannot redirect: Windows puts the MACHINE PATH before the user's own, so
// `agentmaster` / `agentmaster-cli` typed in any terminal run the shared copy; a Start pin can only be changed by
// hand (an app that asks gets "Access is denied"); an old taskbar pin or a double-click in Program Files leads
// straight to it. All measured as a standard user in a Windows 11 25H2 test desktop on 2026-10-22
// (PROFILES.md §2c). A user who installed a copy of their OWN next to it (the per-user install, registered in
// HKCU — for example the one the all-users update's "install the new version just for you" makes when no
// administrator is at hand) would otherwise keep landing in the older shared copy, and the two would run on the
// same data folder at once (the second start warns "profile in use").
//
// THE RULE: when the shared copy starts for a user who has an install of their own, it passes the launch on to
// that copy, with the same arguments, and exits — the app (PortableInstall.h EnsureInstallDecidedAtStartup,
// StartupPlan::HandoffToUserCopy) and the CLI (cli/agentcli.cpp, ForwardCliToUserInstall). It always prefers the
// user's own copy, also when an administrator later updates the shared one past it: that copy offers the same
// update itself, without rights (the user's decision of 2026-10-22).
//
// LOOPS ARE IMPOSSIBLE BY CONSTRUCTION: the target must hold the exe, must not be the running folder, and must not
// be a shared copy itself (Profiles::IsMachineCopyIn) — a per-user install never hands off.
//
// THE WAY OUT: an administrator who wants to run the shared copy anyway (to test it) sets AGENTMASTER_NO_HANDOFF=1.

#pragma once

#include <windows.h>
#include <shlwapi.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "ProfileBootstrap.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace Agentmaster::InstallHandoff
{
    // The per-user install's Apps & Features key (HKCU): its InstallLocation names the user's own install. The ONE
    // definition — PortableInstall.h's kUninstallKey is this constant, and the PowerShell core writes the same key.
    inline constexpr const wchar_t* kUserUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Agentmaster";
    // Set to 1 (or true / yes) to make the shared copy run itself instead of handing off (an administrator testing it).
    inline constexpr const wchar_t* kOptOutVariable = L"AGENTMASTER_NO_HANDOFF";
    inline constexpr const wchar_t* kAppExeLeaf = L"Agentmaster.exe";
    inline constexpr const wchar_t* kCliExeLeaf = L"agentmaster-cli.exe";

    // What decides a hand-off (ShouldHandOff). Gathered by the caller so the rule itself stays pure and testable.
    struct HandoffFacts
    {
        bool sharedCopy{ false }; // this copy is the all-users install, or another folder this account cannot write
        bool allowed{ true }; // false for a launch THIS process must serve itself (a -Embedding COM activation, session 0)
        bool optOut{ false }; // AGENTMASTER_NO_HANDOFF is set
        bool userCopyUsable{ false }; // UserCopyFor found the user's own install, holding the exe to hand to
    };

    // PURE: whether this launch goes to the user's own install instead of running the shared copy.
    //   f       — the gathered facts.
    //   returns — true only for a shared copy, a launch that may be redirected, no opt-out, and a usable own install.
    //   throws  — nothing.
    inline bool ShouldHandOff(const HandoffFacts& f) noexcept
    {
        return f.sharedCopy && f.allowed && !f.optOut && f.userCopyUsable;
    }

    // PURE: whether an opt-out variable's VALUE asks the shared copy to run itself: 1, true or yes, any case. An
    // empty or other value keeps the hand-off (a stray AGENTMASTER_NO_HANDOFF=0 must not disable it).
    //   value   — the variable's value ("" when unset).
    //   returns — true when the hand-off is switched off.
    //   throws  — nothing.
    inline bool IsOptOutValue(std::wstring_view value) noexcept
    {
        std::wstring v;
        for (const wchar_t c : value)
        {
            if (c != L' ' && c != L'\t')
            {
                v.push_back(c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c);
            }
        }
        return v == L"1" || v == L"true" || v == L"yes";
    }

    // Whether THIS process was asked to skip the hand-off (kOptOutVariable).
    //   returns — IsOptOutValue of the variable.
    //   throws  — std::bad_alloc only.
    inline bool OptedOut()
    {
        return IsOptOutValue(Profiles::detail::GetEnvVar(kOptOutVariable));
    }

    // The folder of the running user's own per-user install: kUserUninstallKey's InstallLocation (HKCU), raw — no
    // check that it still holds anything (UserCopyFor checks).
    //   returns — the folder, or "" when there is no entry or it cannot be read.
    //   throws  — std::bad_alloc only.
    inline std::wstring ReadUserInstallDir()
    {
        HKEY key{};
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kUserUninstallKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return {};
        }
        std::wstring out;
        DWORD type = 0;
        DWORD size = 0;
        if (::RegQueryValueExW(key, L"InstallLocation", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size >= sizeof(wchar_t) &&
            (type == REG_SZ || type == REG_EXPAND_SZ))
        {
            std::wstring buf(size / sizeof(wchar_t), L'\0');
            if (::RegQueryValueExW(key, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &size) == ERROR_SUCCESS)
            {
                while (!buf.empty() && buf.back() == L'\0')
                {
                    buf.pop_back();
                }
                out = buf;
            }
        }
        ::RegCloseKey(key);
        return out;
    }

    // The loop guards of the header comment, applied to a registered folder: whether a launch of `leaf` from the copy
    // in `exeDir` may be handed to the install in `registeredDir`. Split from UserCopyFor so the guards can be checked
    // on scratch folders without writing the real HKCU entry (the m5 harness, TestPortableInstall).
    //   registeredDir — the folder the user's Apps & Features entry names ("" = none).
    //   exeDir        — the folder of the copy that is starting.
    //   leaf          — the exe to hand to (kAppExeLeaf for the app, kCliExeLeaf for the CLI).
    //   returns       — registeredDir when it holds `leaf`, is not `exeDir` and is not a shared copy itself; else "".
    //   throws        — std::bad_alloc only.
    inline std::wstring UsableUserCopy(const std::wstring& registeredDir, const std::wstring& exeDir, const wchar_t* leaf)
    {
        if (registeredDir.empty() || Profiles::detail::SamePath(registeredDir, exeDir))
        {
            return {}; // no own install, or the running copy IS the user's own: nothing to hand to
        }
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path{ registeredDir } / leaf, ec))
        {
            return {}; // uninstalled, or deleted by hand (PortableInstall.h cleans such leftovers up)
        }
        if (Profiles::IsMachineCopyIn(registeredDir))
        {
            return {}; // a shared copy never receives a hand-off: no chain, no loop
        }
        return registeredDir;
    }

    // The user's own install a launch of `leaf` from the copy in `exeDir` may be handed to: the folder their HKCU
    // Apps & Features entry names (ReadUserInstallDir), through the loop guards (UsableUserCopy).
    //   exeDir  — the folder of the copy that is starting.
    //   leaf    — the exe to hand to (kAppExeLeaf for the app, kCliExeLeaf for the CLI).
    //   returns — the install folder, or "" when there is none to hand to.
    //   throws  — std::bad_alloc only.
    inline std::wstring UserCopyFor(const std::wstring& exeDir, const wchar_t* leaf)
    {
        return UsableUserCopy(ReadUserInstallDir(), exeDir, leaf);
    }

    // The command line after argv[0], verbatim — what a hand-off passes on.
    //   returns — the tail ("" when there are no arguments).
    //   throws  — std::bad_alloc only.
    inline std::wstring CommandLineTail()
    {
        const wchar_t* args = ::PathGetArgsW(::GetCommandLineW());
        return args ? std::wstring{ args } : std::wstring{};
    }

    // agentmaster-cli.exe's start: when it runs from a shared copy and the user has an install of their own, run THAT
    // copy's CLI on this same console (the caller's startup info and inherited standard handles — the wt launcher
    // shim's recipe, so a redirected `> out.json` keeps working) and pass its exit code on.
    //   exitCode — receives the forwarded CLI's exit code (any value, a crash's NTSTATUS included) when it ran.
    //   returns  — true when the call was forwarded (the caller returns exitCode); false when this CLI should run
    //              itself (no hand-off, or the forwarded CLI could not be started: a running CLI beats none).
    //   throws   — nothing.
    inline bool ForwardCliToUserInstall(int& exitCode) noexcept
    {
        try
        {
            const std::wstring exeDir = Profiles::detail::ExeDirPath();
            HandoffFacts f;
            f.sharedCopy = !exeDir.empty() && Profiles::IsMachineCopyIn(exeDir);
            if (!f.sharedCopy)
            {
                return false; // the common case (a package, a per-user install, a build folder): nothing else is read
            }
            f.optOut = OptedOut();
            const std::wstring userDir = f.optOut ? std::wstring{} : UserCopyFor(exeDir, kCliExeLeaf);
            f.userCopyUsable = !userDir.empty();
            if (!ShouldHandOff(f))
            {
                return false;
            }
            const std::wstring target = userDir + L"\\" + kCliExeLeaf;
            const std::wstring tail = CommandLineTail();
            std::wstring cmdline = L"\"" + target + L"\"";
            if (!tail.empty())
            {
                cmdline += L" " + tail;
            }
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            ::GetStartupInfoW(&si);
            PROCESS_INFORMATION pi{};
            if (!::CreateProcessW(target.c_str(), cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
            {
                return false;
            }
            ::CloseHandle(pi.hThread);
            ::WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 1;
            ::GetExitCodeProcess(pi.hProcess, &code);
            ::CloseHandle(pi.hProcess);
            exitCode = static_cast<int>(code);
            return true;
        }
        catch (...)
        {
            // Only bad_alloc can land here (the paths are built in std::wstring), and only before the forwarded CLI
            // started. This CLI then runs itself — the behavior before this hand-off existed — and says so on stderr
            // (never stdout: a `--json` reader parses that), with a fixed ASCII line WriteFile writes without allocating.
            static constexpr char kNote[] = "agentmaster-cli: the check for your own Agentmaster install failed; running this copy\r\n";
            DWORD wrote = 0;
            ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), kNote, static_cast<DWORD>(sizeof(kNote) - 1), &wrote, nullptr);
            return false;
        }
    }
}
