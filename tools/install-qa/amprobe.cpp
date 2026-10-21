// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster install QA probe (2026-10-21; a test tool, not part of the product — see README.md here).
//
// A GUI exe that is NAMED Agentmaster.exe and runs the app's startup prelude exactly as
// WindowEmperor::HandleCommandlineArgs does — the --uninstall-portable flag, the first-launch install question
// (PortableInstall.h), the profile resolution (ProfileBootstrap.h) and the registration refresh — then, instead of
// starting the terminal UI, appends one line per step to C:\amqa\probe.log. The embedded am-update.ps1 (the
// AM_UPDATE_PS1 resource, amprobe.rc) is the repository's real script, so every elevated / per-user / uninstall
// hop the C++ makes runs the real thing. Build it with build.cmd (into Q:) and run it ONLY inside a disposable
// test desktop: it installs, registers and uninstalls for real.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "PortableInstall.h"

#include <cstdio>

// TaskDialogIndirect (the install question, the profile picker) exists only in comctl32 v6: without this
// dependency the loader binds comctl32 5.82 and the exe dies at start with a missing entry point.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
    // The UTF-8 bytes of a UTF-16 string ("" for an empty or unconvertible one).
    //   s       - the text.
    //   returns - its UTF-8 encoding.
    //   throws  - std::bad_alloc only.
    std::string ToUtf8(const std::wstring& s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0)
        {
            return {};
        }
        std::string out(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    // Append one line to the QA log: C:\amqa\probe.log (the test desktop makes that folder writable for every
    // account), else %TEMP%\amprobe.log. Best-effort: a failed write loses only that line, and the probe goes on.
    //   line   - the text (a timestamp and the process id are prefixed).
    //   throws - std::bad_alloc only (string building).
    void ProbeLog(const std::wstring& line)
    {
        SYSTEMTIME t{};
        ::GetLocalTime(&t);
        wchar_t stamp[64]{};
        swprintf_s(stamp, L"[%02u:%02u:%02u.%03u] pid=%lu ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, ::GetCurrentProcessId());
        const std::string bytes = ToUtf8(std::wstring{ stamp } + line + L"\r\n");
        const std::wstring fallback = Agentmaster::Profiles::detail::GetEnvVar(L"TEMP") + L"\\amprobe.log";
        const wchar_t* paths[] = { L"C:\\amqa\\probe.log", fallback.c_str() };
        for (const wchar_t* p : paths)
        {
            // FILE_APPEND_DATA: every writer appends at the end, also when several probe processes (two
            // accounts, a relaunch) write at once.
            HANDLE h = ::CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                continue;
            }
            DWORD written = 0;
            ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
            ::CloseHandle(h);
            return;
        }
    }

    // The account plan's name, for the log.
    //   p       - the plan.
    //   returns - its enumerator name.
    //   throws  - nothing.
    const wchar_t* PlanName(Agentmaster::PortableInstall::AccountPlan p) noexcept
    {
        using Agentmaster::PortableInstall::AccountPlan;
        switch (p)
        {
        case AccountPlan::AsIs:
            return L"AsIs";
        case AccountPlan::AsIsElevated:
            return L"AsIsElevated";
        case AccountPlan::OtherAccountRelaunch:
            return L"OtherAccountRelaunch";
        case AccountPlan::OtherAccountExplain:
            return L"OtherAccountExplain";
        case AccountPlan::NoSessionUser:
            return L"NoSessionUser";
        }
        return L"?";
    }

    // A stand-in for WindowEmperor's unpackagedAumidForImagePath (WT's til::hash is not available here): the
    // probe only needs a stable per-path id for the Start-menu shortcut's AppUserModelID stamp.
    //   exe     - the image path.
    //   returns - "AgentmasterProbe.<FNV-1a 64 of the lower-cased path, hex>".
    //   throws  - std::bad_alloc only.
    std::wstring ProbeAumid(const std::wstring& exe)
    {
        uint64_t h = 1469598103934665603ull;
        for (const wchar_t c : exe)
        {
            h = (h ^ static_cast<uint64_t>(::towlower(c))) * 1099511628211ull;
        }
        wchar_t buf[40]{};
        swprintf_s(buf, L"AgentmasterProbe.%016llx", static_cast<unsigned long long>(h));
        return buf;
    }

    // The probe's whole run: the prelude steps, each logged.
    //   returns - 0 always (the probe reports through its log, never through an exit code).
    //   throws  - std::bad_alloc only (string building); wWinMain catches it.
    int RunProbe()
    {
        namespace PI = Agentmaster::PortableInstall;
        namespace PB = Agentmaster::Profiles;
        const std::wstring exeDir = PB::detail::ExeDirPath();
        const PI::InstallIdentity who = PI::DetectInstallIdentity();
        const PI::AccountPlan plan = PI::DecideAccountPlan(who);
        ProbeLog(L"START user=" + who.runningUser + L" elevated=" + (who.elevated ? L"1" : L"0") + L" system=" + (who.system ? L"1" : L"0") +
                 L" session=" + std::to_wstring(who.sessionId) + L" sessionUser=" + who.sessionUser + L" known=" + (who.sessionUserKnown ? L"1" : L"0") +
                 L" isSessionUser=" + (who.isSessionUser ? L"1" : L"0") + L" plan=" + PlanName(plan) + L" exeDir=" + exeDir + L" cmdline=" + ::GetCommandLineW());

        // 1. --uninstall-portable (before the single-instance handoff in the app).
        if (PI::HandleUninstallFlagAtStartup())
        {
            ProbeLog(L"uninstall flag handled -> exit");
            return 0;
        }

        // 2. The first-launch install question.
        const std::wstring_view cmdline{ ::GetCommandLineW() };
        const bool embedding = cmdline.find(L"-Embedding") != std::wstring_view::npos;
        bool deferPortablePicker = false;
        switch (PI::EnsureInstallDecidedAtStartup(!embedding, [](const std::wstring& exe) { return ProbeAumid(exe); }))
        {
        case PI::StartupAction::Exit:
            ProbeLog(L"install step -> Exit (handed off / installed and relaunched)");
            return 0;
        case PI::StartupAction::ContinueDeferProfile:
            ProbeLog(L"install step -> ContinueDeferProfile");
            deferPortablePicker = true;
            break;
        default:
            ProbeLog(L"install step -> Continue");
            break;
        }

        // 3. The profile.
        if (!PB::EnsureProfileResolvedAtStartup(!embedding, deferPortablePicker))
        {
            ProbeLog(L"profile step -> declined (profile in use elsewhere) -> exit");
            return 0;
        }

        // 4. The registration refresh of an installed copy.
        PI::RefreshInstalledRegistration();

        // 5. Where this run ended up.
        const PI::InstallDecision d = PI::ReadInstallDecisionIn(exeDir);
        std::wstring decision = PI::EncodeInstallDecision(d);
        if (decision.empty())
        {
            decision = L"none";
        }
        ProbeLog(L"RESULT decision=" + decision + L" machineCopy=" + (PB::IsMachineCopy() ? L"1" : L"0") + L" installedPortable=" +
                 (PI::IsInstalledPortable() ? L"1" : L"0") + L" machineInstall=" + (PI::IsMachineInstall() ? L"1" : L"0") + L" pointer=" +
                 (PB::LocalProfilePointerExistsIn(exeDir) ? L"1" : L"0") + L" profile=" + PB::ResolveProfileDir());
        return 0;
    }
}

// The GUI entry point (the app is a GUI exe too, so ShellExecute / Explorer / task launches behave the same).
//   returns - the probe's exit code: 0, or 3 when the run threw.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        return RunProbe();
    }
    catch (...)
    {
        // The probe's only sink is its own log; an allocation failure inside it ends the run quietly.
        try
        {
            ProbeLog(L"probe CRASHED (exception)");
        }
        catch (...)
        {
            // logger-failed: nothing left to report through
        }
        return 3;
    }
}
