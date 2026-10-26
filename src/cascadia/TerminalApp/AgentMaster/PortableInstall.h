// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the PORTABLE copy's first-launch INSTALL choice (header-only, pure Win32; no WinRT,
// no engine deps — the ProfileBootstrap.h / Updater.h idiom, included by the WindowsTerminal EXE
// prelude, the Settings cog's Uninstall button, and the standalone test harness).
//
// WHAT: the very first launch of a portable copy — BEFORE anything is written anywhere — asks
// "Where should Agentmaster live?". A per-user install that needs NO administrator rights is the
// preferred, default answer (the user's rule of 2026-10-21):
//   * Install just for you           %USERPROFILE%\.agentmaster\bin   (the default, Enter)
//   * Install for all users          %ProgramFiles%\Agentmaster  (needs administrator rights; the
//                                    elevated part runs the embedded am-update.ps1 -InstallMachine)
//   * Install to a folder you pick   (Browse…; lands in <picked>\Agentmaster unless empty/ours)
//   * Install here                   the folder holding the RUNNING binary (never the cwd)
//   * Keep it portable               run from here, no shortcuts / registry — never asks again
//   * Ask me later                   run from here now, nothing written, ask again next launch
// "Install" (just for you) is an MSI-like per-user install WITHOUT an MSI: copy the binaries (or
// register them in place), a DESKTOP shortcut, a START-MENU shortcut (Start search finds it; it
// carries the unpackaged AUMID so toasts route), an "Open in Agentmaster" RIGHT-CLICK menu on folders /
// folder backgrounds / drives (the classic registry verb — what the MSIX shell extension provides for
// a packaged install, migrated to HKCU\Software\Classes), an APPS & FEATURES entry (with a working
// Uninstall), and the install dir on the USER PATH (agentmaster-cli.exe reachable from any shell).
// No administrator rights, no package registration, no certificate. The ALL-USERS install is the same
// shape one level up (HKLM verbs and Apps & Features entry, the machine PATH), done by the shared
// PowerShell install core that tools\Install-Agentmaster.ps1 uses too — except its ICONS: since
// 2026-10-22 every user gets icons of their own instead of shared ones (Windows' Active Setup runs
// `Agentmaster.exe --register-user` once per user at their next sign-in, and the shared copy's own
// start does the same; EnsureMachineCopyUserSetup), because a user without administrator rights can
// neither remove nor re-point a shared icon (measured; PROFILES.md §2c).
//
// A USER'S OWN INSTALL WINS FOR THAT USER: next to an all-users install, a user's per-user install (for
// example the one the all-users update's "install just for you" makes) writes the same icon names in
// their own folders (so it replaces the shared copy's icons), moves their taskbar pins of the shared
// copy to itself at its start (RepointSharedCopyTaskbarPins), already wins the right-click entry (HKCU
// outranks HKLM), and the shared copy hands every launch to it — the machine PATH in a terminal, a
// Start pin, a double-click in Program Files (InstallHandoff.h; StartupPlan::HandoffToUserCopy). A user
// whose own copy was deleted by hand gets its leftovers cleaned up by the shared copy
// (MachineCopyUserUpkeep).
//
// ACCOUNTS AND RIGHTS: an install sets Agentmaster up for the account that RUNS it (its HKCU, its
// Start menu, its profile folder). So the prompt first compares the process's account with the user
// signed in to this Windows session (DetectInstallIdentity / DecideAccountPlan):
//   * another account (UAC over-the-shoulder credentials, "Run as different user", a deployment agent)
//     -> the first option starts THIS copy again as the signed-in user, without administrator rights
//        (am-update.ps1 -RunAsMainUser: a one-shot scheduled task for that user), so the install lands
//        in the right account; when this process cannot do that (not elevated), the prompt says how;
//   * the signed-in user WITH administrator rights -> installs normally, then starts the installed copy
//     WITHOUT them (the same -RunAsMainUser hop), so the app never keeps rights it was not meant to run
//     with (an elevated Agentmaster would run every claude.exe elevated);
//   * session 0 (a service) -> no prompt at all (no desktop to show it on).
//
// EXISTING INSTALLS are detected, never overridden, and taken over where we can:
//   * an installed Agentmaster PACKAGE (the MSIX release family, possibly admin-trusted) — its
//     files can't and won't be touched; the install adopts its DATA (the folder that package's own
//     profile choice names, else the Default profile ~/.agentmaster), and a checkbox offers removing
//     the package afterwards (per-user removal, no admin; unchecked by default while it is running,
//     since it would be closed).
//   * an ALL-USERS install (the HKLM uninstall entry with a live app exe) — offered as "Use the
//     Agentmaster installed for all users" when it is this version or newer (no rights needed: this
//     unzip becomes a launcher stub), else as "Update the Agentmaster installed for all users"
//     (administrator rights).
//   * a previous USER install of ours (the HKCU uninstall entry) — the first option becomes
//     "Update the installed copy (vX → vY)" (an in-place upgrade preserving its state) or, when
//     it is already the same/newer version, "Use the installed copy" (this unzip then just hands
//     off to it and becomes a launcher stub).
//   * the copy Install-Agentmaster.ps1 v0.7.10 and older made (%LOCALAPPDATA%\Programs\Agentmaster,
//     LegacyScriptInstallDir: no registry entry) — offered the same way when no HKCU install exists; an
//     update registers it in place (a folder the user kept portable only gets its files) and retires the
//     script's "Agentmaster (Portable)" Start-menu entry.
//
// AN UPGRADE NEVER LEAVES A BROKEN INSTALL: the previous version is moved aside inside the install
// folder (kSetAsideLeafPrefix) before the new one is copied in, and put back when the copy fails
// (detail::SetAsideShippedEntries / RestoreSetAside; the PowerShell core's Install-AmLayout, the same).
//
// NO INSTALL EVER LOSES THE USER'S STATE (the owner's rule of 2026-10-26, after "Install just for you" into
// %USERPROFILE%\.agentmaster\bin left their profile "as if reset"): every recursive delete goes through
// detail::RemoveTreeGuarded — inside the install folder only, and never a folder that is, contains or is named like
// Agentmaster state (Profiles::PathHoldsProfileNow); an install destination that holds files of someone else's is
// renamed aside and KEPT (`<folder>.before-install-<time>`, detail::SetAsideFolderInTheWay), never mixed into, and one
// that holds a profile is refused. The PowerShell core applies the same two rules.
//
// THE DOWNLOADED COPY: once this copy handed over — installed elsewhere, or deferring to an install that was there — it
// only forwards launches, so it asks "Delete the downloaded copy?" (AskWhetherToRemoveLeftoverCopy): offered only for a
// folder that holds nothing but the download's files (JudgeLeftoverCopy), removed to the Recycle Bin by a hidden
// PowerShell after this process ended (ScheduleLeftoverCopyRemoval), and only once the installed copy started.
//
// THE DECISION FILE `<exedir>\install.path` (plain UTF-8, `#` comments, first value line wins —
// the profile.path idiom):
//   installed          this folder IS the install (registered) — in-place install, or the copy
//                      an install produced (the destination is written with this).
//   machine            this folder is the ALL-USERS install (written by the elevated installer).
//                      Profiles::IsMachineCopyIn: no prompts, each user's data in their own home.
//   portable           the user chose to stay portable — never ask again.
//   <absolute dir>     this unzip was INSTALLED TO <dir>: it is now a launcher stub that hands
//                      every launch off to <dir>\Agentmaster.exe (a vanished target re-asks).
// A portable folder this process cannot WRITE (an administrator copied it into Program Files; a
// read-only share) is a machine copy too (Profiles::IsMachineCopyIn): it never asks, because it could
// never remember an answer.
//
// THE MANIFEST `<dir>\.am-manifest` (shipped in the zip by New-UnpackagedTerminalDistribution.ps1):
// the top-level names the distribution ships. An upgrade removes the OLD install's listed names (plus
// the new layout's) and an uninstall removes only listed names, so files a user keeps in the same
// folder are never deleted (ReadManifestNamesIn; the PowerShell core applies the same rule).
// "Ask me later" (and Cancel / X / Esc) writes NOTHING: the session runs from the unzip on the
// self-contained <exedir>\profile WITHOUT persisting a profile choice either (the profile picker
// is deferred too — one "later" defers every setup question), so the next launch asks again.
//
// PROFILE OF AN INSTALLED COPY: an installed copy behaves like an installed copy — it uses the
// per-identity DEFAULT profile (~/.agentmaster) silently (the pointer profile.path is written for
// it), and any state a "later" run accumulated in <unzip>\profile is MIGRATED into it (copy-if-
// absent, the picker's migrate rule). "Keep it portable" keeps the classic §2a profile picker.
//
// The installed copy keeps its `.portable` marker: it still self-updates IN PLACE through the
// updater's zip swap (am-update.ps1 -Portable preserves install.path beside profile.path), and
// RefreshInstalledRegistration re-stamps the Apps & Features version after such an update.
//
// UNINSTALL: `Agentmaster.exe --uninstall-portable` (the Apps & Features UninstallString, also the
// cog's Uninstall button) acts only on an INSTALLED copy (DecideUninstallTarget): a per-user install
// confirms, then materializes the embedded am-update.ps1 into %TEMP% and runs it detached with
// -UninstallPortable; an all-users install runs it ELEVATED with -UninstallMachine. Shortcuts, the
// context menu, the Apps & Features entry, the PATH entry and the shipped binaries go; profile data
// (every profile folder, and the exe-side settings\ / profile\ / profile.path) is kept. Any other copy
// (a package, a plain portable, a launcher stub) is told how it is removed instead — before
// 2026-10-21 the flag ran the portable uninstaller on whatever folder the exe sat in.
//
// ORDER IN THE PRELUDE: after the single-instance handoff (a handed-off second process never
// shows UI) and BEFORE EnsureProfileResolvedAtStartup — this is the ONE question that comes before
// any file is written. Everything here is no-throw at the entry points (the prelude must never die
// on an installer hiccup): a failure is reported in a dialog and the prompt is re-shown.

#pragma once

#include <windows.h>
#include <appmodel.h>
#include <commctrl.h>
#include <propsys.h> // IPropertyStore — the Start-menu shortcut's AppUserModelID stamp
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wtsapi32.h> // WTSQuerySessionInformationW — who is signed in to this session (DetectInstallIdentity)

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "InstallHandoff.h" // the all-users copy hands a launch to the user's own install (one rule with the CLI)
#include "ProfileBootstrap.h"
#include "Updater.h" // Version parse/compare, the embedded am-update.ps1, WriteFileUtf8, CmdArg

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace Agentmaster::PortableInstall
{
    // ============================ constants ============================

    inline constexpr std::wstring_view kDecisionLeaf{ L"install.path" };
    inline constexpr std::wstring_view kManifestLeaf{ L".am-manifest" }; // the shipped-names list (ReadManifestNamesIn)
    inline constexpr const wchar_t* kAppExeLeaf = L"Agentmaster.exe";
    inline constexpr const wchar_t* kLegacyAppExeLeaf = L"WindowsTerminal.exe"; // a pre-rename zip's app exe
    inline constexpr const wchar_t* kUninstallFlag = L"uninstall-portable"; // `--uninstall-portable` (CommandLineHasFlag spelling)
    inline constexpr const wchar_t* kDisplayName = L"Agentmaster";
    inline constexpr const wchar_t* kPublisher = L"Agentmaster";
    inline constexpr const wchar_t* kShortcutLeaf = L"Agentmaster.lnk";
    inline constexpr const wchar_t* kShortcutDescription = L"Agentmaster \x2014 manage your Claude Code sessions";
    // The per-user install's Apps & Features key (HKCU; InstallHandoff.h owns the one definition, which the
    // all-users copy's hand-off reads too).
    inline constexpr const wchar_t* kUninstallKey = InstallHandoff::kUserUninstallKey;
    // The ALL-USERS install's Apps & Features key (HKLM), written by the PowerShell core. Its own NAME, not
    // kUninstallKey's: Windows lists an Uninstall key name once, and when the all-users entry shared the
    // per-user one's name, Settings -> Installed apps hid the user's own copy (a test desktop, 2026-10-21).
    // A copy registered before then still sits under kUninstallKey in HKLM, which is read second.
    inline constexpr const wchar_t* kMachineUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AgentmasterAllUsers";
    // The Start-menu entry Install-Agentmaster.ps1 v0.7.10 and older made for its portable copy in
    // %LOCALAPPDATA%\Programs\Agentmaster (LegacyScriptInstallDir); a per-user install it led to replaces it
    // with kShortcutLeaf (detail::RetireLegacyScriptShortcut). The PowerShell core retires it by the same rule.
    inline constexpr const wchar_t* kLegacyScriptShortcutLeaf = L"Agentmaster (Portable).lnk";
    // The folder an upgrade moves the previous version into while it copies the new one in, inside the install
    // folder (the same volume: every move is a rename): this prefix + a number. The PowerShell core's
    // $script:AmSetAsidePrefix is the same name, so either installer cleans up what the other left.
    inline constexpr std::wstring_view kSetAsideLeafPrefix{ L".am-previous-" };
    inline constexpr const wchar_t* kContextVerb = L"Agentmaster";
    inline constexpr const wchar_t* kContextLabel = L"Open in Agentmaster";
    inline constexpr const wchar_t* kEnvironmentKey = L"Environment";
    // `Agentmaster.exe --register-user`: the command Windows' Active Setup runs ONCE for each user at their next
    // sign-in after an install for all users (kActiveSetupKey's StubPath, which the PowerShell core's
    // Register-AmInstall writes). It gives that user their OWN "Agentmaster" desktop and Start-menu icons for the
    // shared copy (EnsureMachineCopyUserSetup) and never starts the app. Since 2026-10-22 an all-users install puts
    // no icon in the shared Start menu or on the shared (Public) desktop: a user without administrator rights can
    // delete, rename or re-point neither, and a user who installed a copy of their own then saw both (measured as a
    // standard user in a test desktop; PROFILES.md §2c). Icons of their own they control, and their own install
    // simply replaces them (the same file names).
    inline constexpr const wchar_t* kRegisterUserFlag = L"register-user";
    // The all-users install's Active Setup component: in HKLM the command (StubPath) and its Version; in HKCU the
    // Version a user already got. One fixed id for Agentmaster — the PowerShell core's $script:AmActiveSetupId.
    inline constexpr const wchar_t* kActiveSetupKey = L"Software\\Microsoft\\Active Setup\\Installed Components\\{F8D18BA4-7DD1-4F04-8EE0-C9C7342F3264}";
    // The user's taskbar pins, under %APPDATA%: one .lnk per pinned app. A pin is the user's own file, so their own
    // install may re-point the pins of the shared copy at itself (RepointPinsIn) — Windows lets no app change a
    // Start pin, but a re-pointed taskbar pin is followed (measured: the click opened the new target, whose window
    // then grouped under the pin, also when started some other way).
    inline constexpr const wchar_t* kTaskbarPinsSubdir = L"Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";

    // Sizes / timing
    inline constexpr DWORD kPackageRemoveTimeoutMs = 120000; // Remove-AppxPackage of a big package can take a while
    inline constexpr int kMarqueeTimerTickMs = 200; // TDF_CALLBACK_TIMER cadence (fixed by the control)

    // ============================ the decision file (pure) ============================

    enum class Decision
    {
        None, // no file / unparseable — a PRISTINE portable copy (ask)
        Installed, // this folder IS the install (registered): in-place install, or an install's destination
        Portable, // stay portable — never ask again
        InstalledTo, // this unzip installed elsewhere: a launcher stub for `dir`
        Machine, // this folder IS the ALL-USERS install (written by the elevated installer; Profiles::IsMachineCopyIn)
    };

    struct InstallDecision
    {
        Decision kind{ Decision::None };
        std::wstring dir; // InstalledTo only: the absolute install dir
    };

    inline std::wstring DecisionPathIn(const std::wstring& exeDir)
    {
        return exeDir.empty() ? std::wstring{} : exeDir + L"\\" + std::wstring{ kDecisionLeaf };
    }

    // PURE: the file's value line for a decision. `installed` / `machine` / `portable` / the absolute dir.
    //   d       — the decision to encode.
    //   returns — the value line; "" for None (None is never written).
    //   throws  — std::bad_alloc only.
    inline std::wstring EncodeInstallDecision(const InstallDecision& d)
    {
        switch (d.kind)
        {
        case Decision::Installed:
            return L"installed";
        case Decision::Machine:
            return L"machine";
        case Decision::Portable:
            return L"portable";
        case Decision::InstalledTo:
            return d.dir;
        default:
            return {};
        }
    }

    // PURE: file text -> decision. First non-empty non-`#` line wins; `installed` / `machine` /
    // `portable` (case-insensitive) are the keywords, an ABSOLUTE path is an InstalledTo target, anything
    // else (a relative path, garbage) reads as None so a corrupt file simply re-asks. Keep the keyword
    // spellings in step with Profiles::InstallFileKeywordIn (ProfileBootstrap.h's minimal twin).
    inline InstallDecision DecodeInstallDecision(std::wstring_view text)
    {
        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t nl = text.find(L'\n', pos);
            if (nl == std::wstring_view::npos)
            {
                nl = text.size();
            }
            std::wstring line{ text.substr(pos, nl - pos) };
            pos = nl + 1;
            while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t'))
            {
                line.pop_back();
            }
            size_t b = 0;
            while (b < line.size() && (line[b] == L' ' || line[b] == L'\t'))
            {
                ++b;
            }
            line.erase(0, b);
            if (line.empty() || line.front() == L'#')
            {
                continue;
            }
            std::wstring lower = line;
            for (auto& c : lower)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            if (lower == L"installed" || lower == L"here")
            {
                return { Decision::Installed, {} };
            }
            if (lower == L"machine")
            {
                return { Decision::Machine, {} };
            }
            if (lower == L"portable")
            {
                return { Decision::Portable, {} };
            }
            const bool absolute = (line.size() >= 3 && line[1] == L':' && (line[2] == L'\\' || line[2] == L'/')) ||
                                  (line.size() >= 2 && line[0] == L'\\' && line[1] == L'\\');
            if (absolute)
            {
                try
                {
                    return { Decision::InstalledTo, std::filesystem::path{ line }.lexically_normal().wstring() };
                }
                catch (...)
                {
                    return {};
                }
            }
            return {}; // a relative/odd value: unusable -> re-ask
        }
        return {};
    }

    inline InstallDecision ReadInstallDecisionIn(const std::wstring& exeDir)
    {
        const auto path = DecisionPathIn(exeDir);
        if (path.empty())
        {
            return {};
        }
        return DecodeInstallDecision(Profiles::detail::ReadUtf8File(path));
    }

    // Persist the decision (atomic tmp + rename, the profile.path idiom). False when the exe dir
    // refuses the write (read-only media) — the caller then simply asks again next launch.
    inline bool SaveInstallDecisionIn(const std::wstring& exeDir, const InstallDecision& d)
    {
        const auto path = DecisionPathIn(exeDir);
        const auto value = EncodeInstallDecision(d);
        if (path.empty() || value.empty())
        {
            return false;
        }
        try
        {
            std::string bytes = "# Agentmaster install decision for THIS exe folder (PortableInstall.h):\n"
                                "#   installed  = this folder is the install (registered: shortcuts, right-click menu, Apps & Features)\n"
                                "#   machine    = this folder is the install for ALL users of this PC (registered machine-wide)\n"
                                "#   portable   = stay portable, run from here, never ask again\n"
                                "#   <dir>      = installed to <dir>; this folder only hands launches off to it\n";
            bytes += Profiles::detail::WideToUtf8(value);
            bytes += '\n';
            const std::wstring tmp = path + L".tmp";
            {
                std::ofstream f{ std::filesystem::path{ tmp }, std::ios::binary | std::ios::trunc };
                if (!f)
                {
                    return false;
                }
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!f.good())
                {
                    return false;
                }
            }
            return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        }
        catch (...)
        {
            return false;
        }
    }

    // ============================ small pure helpers ============================

    // The default install dir: the Default profile's `bin` sibling folder — %USERPROFILE%\.agentmaster\bin.
    inline std::wstring DefaultUserInstallDir()
    {
        return Profiles::DefaultReleaseProfileDir() + L"\\bin";
    }

    // The default ALL-USERS install dir: `<64-bit Program Files>\Agentmaster`. %ProgramW6432% names the
    // native Program Files even from a 32-bit process (where %ProgramFiles% is the x86 one); the
    // literal is the last resort of a process with a stripped environment.
    //   returns — the folder (no trailing separator).
    //   throws  — std::bad_alloc only.
    inline std::wstring DefaultMachineInstallDir()
    {
        std::wstring pf = Profiles::detail::GetEnvVar(L"ProgramW6432");
        if (pf.empty())
        {
            pf = Profiles::detail::GetEnvVar(L"ProgramFiles");
        }
        if (pf.empty())
        {
            pf = L"C:\\Program Files";
        }
        while (!pf.empty() && (pf.back() == L'\\' || pf.back() == L'/'))
        {
            pf.pop_back();
        }
        return pf + L"\\Agentmaster";
    }

    // The profile a PER-USER install uses: the folder the MSIX RELEASE package's own profile choice
    // names (the home map's `Agentmaster_56k4f06dsfp9r=` line, written by that package's first launch or
    // its cog), else the Default profile. A per-user install shares its data with that package by
    // design (the takeover checkbox removes the package and keeps everything) — before 2026-10-21 it
    // always took the Default profile, so a user who had moved the package's profile to another folder
    // found an EMPTY Agentmaster after the switch.
    //   returns — the profile folder for the installed copy's profile.path.
    //   throws  — std::bad_alloc only.
    inline std::wstring UserInstallProfileDir()
    {
        std::wstring saved = Profiles::SavedChoiceFor(Updater::kReleaseFamily);
        return saved.empty() ? Profiles::DefaultReleaseProfileDir() : saved;
    }

    // The folder Install-Agentmaster.ps1 v0.7.10 and older put its portable copy into:
    // %LOCALAPPDATA%\Programs\Agentmaster, with a Start-menu entry "Agentmaster (Portable)" but no Apps & Features
    // entry and no install.path — so the registry never tells DetectExistingInstalls about it (the PowerShell
    // core's Find-AmUserInstallDir looks there too).
    //   returns — the folder, or "" when %LOCALAPPDATA% is not set.
    //   throws  — std::bad_alloc only.
    inline std::wstring LegacyScriptInstallDir()
    {
        const std::wstring local = Profiles::detail::GetEnvVar(L"LOCALAPPDATA");
        return local.empty() ? std::wstring{} : local + L"\\Programs\\Agentmaster";
    }

    // PURE: which copy the old script's folder stands for in the install question (DetectExistingInstalls):
    // the folder itself (never decided, kept portable, or registered in place), or — when the app's first
    // launch made it a launcher stub — the install that stub names. Never the folder THIS process runs from:
    // a copy offered to "update" or "use" itself would become a launcher stub for itself.
    //   legacyDir      — LegacyScriptInstallDir().
    //   legacyHoldsExe — that folder holds the app exe.
    //   decision       — its install.path.
    //   targetHoldsExe — decision is a launcher stub whose install still holds the app exe.
    //   exeDir         — the folder this process runs from.
    //   returns        — the folder to offer, or "" for none.
    //   throws         — std::bad_alloc only.
    inline std::wstring LegacyScriptOfferDir(const std::wstring& legacyDir, bool legacyHoldsExe, const InstallDecision& decision, bool targetHoldsExe, const std::wstring& exeDir)
    {
        if (legacyDir.empty() || !legacyHoldsExe || decision.kind == Decision::Machine)
        {
            return {}; // nothing there, or something no per-user script ever made (an all-users decision in LocalAppData)
        }
        // A stub whose install vanished re-asks like a copy that never decided (DecideStartup): offer the folder itself.
        const std::wstring offer = decision.kind == Decision::InstalledTo && targetHoldsExe ? decision.dir : legacyDir;
        return Profiles::detail::SamePath(offer, exeDir) ? std::wstring{} : offer;
    }

    // PURE: true when `name` is a plain top-level entry name a manifest may list — never a path
    // (no separator, no drive colon, no stream colon), never `.` / `..`, never empty or a wildcard,
    // never ending in a dot or a space (Windows strips those from a path's last segment, so `profile.`
    // would name the PRESERVED `profile` folder — PowerShell's Remove-Item happens to refuse it today,
    // checked 2026-10-21, but the rule must not rest on that), never a DOS device name (CON, NUL, COM1, …,
    // with or without an extension). An updater or uninstaller deletes exactly the names a manifest
    // lists, so a corrupt or planted manifest must never be able to name anything but a plain entry of
    // its own folder. The core's Test-SafeManifestName applies the same rule.
    //   name    — one manifest line, already trimmed.
    //   returns — true when the name is safe to act on inside the install folder.
    //   throws  — nothing.
    inline bool IsSafeManifestName(std::wstring_view name) noexcept
    {
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ')
        {
            return false;
        }
        for (const wchar_t c : name)
        {
            if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c < 0x20)
            {
                return false;
            }
        }
        // The part before the first dot, compared case-insensitively against the device names.
        // (std::min) in parentheses: the m5 harness includes this header without NOMINMAX.
        const std::wstring_view base = name.substr(0, (std::min)(name.find(L'.'), name.size()));
        const auto equalsNoCase = [&](std::wstring_view device) noexcept {
            if (base.size() != device.size())
            {
                return false;
            }
            for (size_t i = 0; i < base.size(); ++i)
            {
                if (::towlower(base[i]) != device[i])
                {
                    return false;
                }
            }
            return true;
        };
        constexpr std::wstring_view devices[] = { L"con",  L"prn",  L"aux",  L"nul",  L"com1", L"com2", L"com3", L"com4", L"com5", L"com6", L"com7",
                                                  L"com8", L"com9", L"lpt1", L"lpt2", L"lpt3", L"lpt4", L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9" };
        for (const auto device : devices)
        {
            if (equalsNoCase(device))
            {
                return false;
            }
        }
        return true;
    }

    // PURE: manifest text -> the top-level names it lists, in file order, without duplicates.
    // `#` lines and blank lines are skipped; each line is trimmed and may carry a trailing `/` or `\`
    // marking a folder (dropped); an unsafe name (IsSafeManifestName) is skipped, not trusted.
    //   text    — the `.am-manifest` file content.
    //   returns — the listed names; empty for an empty / comment-only / unreadable manifest.
    //   throws  — std::bad_alloc only.
    inline std::vector<std::wstring> ParseManifestNames(std::wstring_view text)
    {
        std::vector<std::wstring> names;
        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t nl = text.find(L'\n', pos);
            if (nl == std::wstring_view::npos)
            {
                nl = text.size();
            }
            std::wstring line{ text.substr(pos, nl - pos) };
            pos = nl + 1;
            while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t'))
            {
                line.pop_back();
            }
            size_t b = 0;
            while (b < line.size() && (line[b] == L' ' || line[b] == L'\t'))
            {
                ++b;
            }
            line.erase(0, b);
            if (line.empty() || line.front() == L'#')
            {
                continue;
            }
            while (!line.empty() && (line.back() == L'/' || line.back() == L'\\'))
            {
                line.pop_back(); // a folder marker, not part of the name
            }
            if (!IsSafeManifestName(line))
            {
                continue;
            }
            const auto key = Profiles::detail::NormPathKey(line);
            const bool seen = std::any_of(names.begin(), names.end(), [&](const std::wstring& n) { return Profiles::detail::NormPathKey(n) == key; });
            if (!seen)
            {
                names.push_back(std::move(line));
            }
        }
        return names;
    }

    // The names `<dir>\.am-manifest` lists (ParseManifestNames); empty when there is no manifest (an
    // install made from a zip built before the manifest existed — before 2026-10-21 — or a build folder).
    //   dir     — the install / layout folder.
    //   returns — the listed names, or empty.
    //   throws  — std::bad_alloc only.
    inline std::vector<std::wstring> ReadManifestNamesIn(const std::wstring& dir)
    {
        if (dir.empty())
        {
            return {};
        }
        return ParseManifestNames(Profiles::detail::ReadUtf8File(dir + L"\\" + std::wstring{ kManifestLeaf }));
    }

    inline std::wstring AppExeIn(const std::wstring& dir)
    {
        return dir.empty() ? std::wstring{} : dir + L"\\" + kAppExeLeaf;
    }

    // The app exe inside a folder: Agentmaster.exe, or a pre-rename zip's WindowsTerminal.exe. "" when neither.
    inline std::wstring FindAppExeIn(const std::wstring& dir)
    {
        if (dir.empty())
        {
            return {};
        }
        try
        {
            for (const wchar_t* leaf : { kAppExeLeaf, kLegacyAppExeLeaf })
            {
                const std::wstring p = dir + L"\\" + leaf;
                if (std::filesystem::exists(std::filesystem::path{ p }))
                {
                    return p;
                }
            }
        }
        catch (...)
        {
        }
        return {};
    }

    inline bool DirHoldsAppExe(const std::wstring& dir)
    {
        return !FindAppExeIn(dir).empty();
    }

    // The exe-side user state an upgrade PRESERVES — the scripts' exact list (Install-Agentmaster.ps1
    // / am-update.ps1 -Portable) plus install.path.
    inline bool IsPreservedLeaf(std::wstring_view leaf)
    {
        const auto k = Profiles::detail::NormPathKey(leaf);
        return k == L"settings" || k == L"profile" || k == L"profile.path" || k == L"install.path";
    }

    // The top-level names releases shipped BEFORE the manifest existed (zips built before 2026-10-21):
    // the PowerShell core's $script:AmLegacyShippedNames, mirrored — keep the two in step. Read from the
    // v0.7.10 x64 zip, plus the names newer layouts add. Exact names only (plus the one pattern in
    // IsLegacyShippedLeaf): never a broad *.dll / *.exe, which would also match a user's own files.
    inline constexpr std::wstring_view kLegacyShippedNames[] = {
        L".am-manifest", L".am-version", L".portable", L"agentmaster.exe", L"notice.html", L"openconsole.exe",
        L"openconsoleproxy.dll", L"terminalapp.dll", L"terminalconnection.dll", L"terminalthemehelpers.dll",
        L"windowsterminal.exe", L"windowsterminalshellext.dll", L"agentmaster-cli.exe", L"agentmaster-sessiondb.exe",
        L"duckdb.dll", L"defaults.json", L"elevate-shim.exe", L"resources.pri", L"wtd.exe", L"wt.exe", L"images",
        L"microsoft.ui.xaml", L"microsoft.ui.xaml.dll", L"profilegeneratoricons", L"profileicons",
    };

    // PURE: true when `leaf` is a name a pre-manifest release shipped — one of kLegacyShippedNames, or
    // Microsoft.Terminal.*.dll (the PowerShell core's $script:AmLegacyShippedPatterns). Case-insensitive.
    //   leaf    — a top-level entry name of an install folder.
    //   returns — whether an upgrade or uninstall of a manifest-less install treats it as ours.
    //   throws  — std::bad_alloc only.
    inline bool IsLegacyShippedLeaf(std::wstring_view leaf)
    {
        const auto k = Profiles::detail::NormPathKey(leaf);
        if (std::find(std::begin(kLegacyShippedNames), std::end(kLegacyShippedNames), std::wstring_view{ k }) != std::end(kLegacyShippedNames))
        {
            return true;
        }
        constexpr std::wstring_view prefix = L"microsoft.terminal.";
        constexpr std::wstring_view suffix = L".dll";
        return k.size() > prefix.size() + suffix.size() && k.compare(0, prefix.size(), prefix) == 0 &&
               k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // PURE: true when `leaf` is an upgrade's set-aside folder (kSetAsideLeafPrefix + anything) — Agentmaster's
    // own, never a user's: an upgrade or an uninstall deletes one that an earlier, interrupted run left, and a
    // copy never carries one along. Case-insensitive.
    //   leaf    — a top-level entry name.
    //   returns — whether it names a set-aside folder.
    //   throws  — std::bad_alloc only.
    inline bool IsSetAsideLeaf(std::wstring_view leaf)
    {
        const auto k = Profiles::detail::NormPathKey(leaf);
        return k.size() > kSetAsideLeafPrefix.size() && k.compare(0, kSetAsideLeafPrefix.size(), kSetAsideLeafPrefix) == 0;
    }

    // Entries a COPY-install never carries from the source: the source's own profile data (it is
    // MIGRATED into the destination's profile instead), its `settings` folder (upstream portable mode's Terminal
    // settings of a pre-profile copy: seeded into the profile's terminal\ instead, copy-if-absent — copied, it would
    // overwrite the destination's own preserved `settings`, which the PowerShell core never does either), its
    // pointer/decision files (the destination gets its own), and temp leftovers (an interrupted upgrade's set-aside
    // folder included). Every name here is one the destination keeps as it is.
    //   leaf    — a top-level entry name of the source layout.
    //   returns — true when a copy skips it.
    //   throws  — std::bad_alloc only.
    inline bool IsCopyExcludedLeaf(std::wstring_view leaf)
    {
        const auto k = Profiles::detail::NormPathKey(leaf);
        if (k == L"profile" || k == L"settings" || k == L"profile.path" || k == L"install.path" || IsSetAsideLeaf(k))
        {
            return true;
        }
        return k.size() > 4 && k.compare(k.size() - 4, 4, L".tmp") == 0;
    }

    // `;`-separated PATH-list membership (case-insensitive, slash/trailing-separator agnostic).
    inline bool PathListContains(std::wstring_view list, std::wstring_view dir)
    {
        const auto want = Profiles::detail::NormPathKey(dir);
        if (want.empty())
        {
            return false;
        }
        size_t pos = 0;
        while (pos <= list.size())
        {
            size_t semi = list.find(L';', pos);
            if (semi == std::wstring_view::npos)
            {
                semi = list.size();
            }
            std::wstring_view entry = list.substr(pos, semi - pos);
            while (!entry.empty() && (entry.front() == L' ' || entry.front() == L'"'))
            {
                entry.remove_prefix(1);
            }
            while (!entry.empty() && (entry.back() == L' ' || entry.back() == L'"'))
            {
                entry.remove_suffix(1);
            }
            if (!entry.empty() && Profiles::detail::NormPathKey(entry) == want)
            {
                return true;
            }
            pos = semi + 1;
        }
        return false;
    }

    // PURE: append `dir` to a PATH list unless present (no duplicate, no stray separators).
    inline std::wstring AddDirToPathList(std::wstring_view list, std::wstring_view dir)
    {
        std::wstring out{ list };
        if (dir.empty() || PathListContains(list, dir))
        {
            return out;
        }
        while (!out.empty() && (out.back() == L';' || out.back() == L' '))
        {
            out.pop_back();
        }
        if (!out.empty())
        {
            out.push_back(L';');
        }
        out += dir;
        return out;
    }

    // PURE: remove every entry equal to `dir` from a PATH list, keeping the others verbatim.
    inline std::wstring RemoveDirFromPathList(std::wstring_view list, std::wstring_view dir)
    {
        const auto want = Profiles::detail::NormPathKey(dir);
        std::wstring out;
        size_t pos = 0;
        while (pos <= list.size())
        {
            size_t semi = list.find(L';', pos);
            if (semi == std::wstring_view::npos)
            {
                semi = list.size();
            }
            const std::wstring_view entry = list.substr(pos, semi - pos);
            std::wstring_view trimmed = entry;
            while (!trimmed.empty() && (trimmed.front() == L' ' || trimmed.front() == L'"'))
            {
                trimmed.remove_prefix(1);
            }
            while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'"'))
            {
                trimmed.remove_suffix(1);
            }
            const bool drop = !trimmed.empty() && !want.empty() && Profiles::detail::NormPathKey(trimmed) == want;
            if (!drop && !entry.empty())
            {
                if (!out.empty())
                {
                    out.push_back(L';');
                }
                out += entry;
            }
            pos = semi + 1;
        }
        return out;
    }

    inline std::wstring Quoted(std::wstring_view s)
    {
        return L"\"" + std::wstring{ s } + L"\"";
    }

    // PURE: the registry command for the right-click verb: "<exe>" -d "<placeholder>" — WT's own
    // `-d <startingDirectory>` (the implicit new-tab subcommand) opens the clicked folder.
    inline std::wstring ContextMenuCommand(std::wstring_view exe, std::wstring_view placeholder)
    {
        return Quoted(exe) + L" -d " + Quoted(placeholder);
    }

    inline std::wstring FormatVersion(const Updater::Version& v)
    {
        std::wstring s = std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.patch);
        if (v.build != 0)
        {
            s += L"." + std::to_wstring(v.build);
        }
        return s;
    }

    // The `.am-version` stamp of a folder, read WITHOUT Updater::PortableVersionFromDir: that
    // helper logs through Profiles::ResolveProfileDir() on its failure path, and THIS code runs
    // BEFORE the profile is resolved — touching the cache early would pin the wrong dir (Rule #15).
    inline Updater::Version VersionStampIn(const std::wstring& dir)
    {
        try
        {
            if (dir.empty())
            {
                return {};
            }
            return Updater::ParseVersion(Profiles::detail::ReadUtf8File(dir + L"\\.am-version"));
        }
        catch (...)
        {
            return {};
        }
    }

    // True when `inner` is `outer` itself or a folder BELOW it (case-insensitive, normalized).
    inline bool IsSameOrBelow(std::wstring_view inner, std::wstring_view outer)
    {
        const auto a = Profiles::detail::NormPathKey(inner);
        const auto b = Profiles::detail::NormPathKey(outer);
        if (a.empty() || b.empty())
        {
            return false;
        }
        return a == b || (a.size() > b.size() + 1 && a.compare(0, b.size(), b) == 0 && a[b.size()] == L'\\');
    }

    // PURE: an Active Setup version — up to four comma-separated numbers ("1,2026,1022,1530"), the format Windows
    // compares before it runs a component's StubPath — as four numbers. A missing part reads 0; a part that is not
    // a number reads 0 too, and so does one past 4 294 967 295 (it saturates), so a damaged value never throws.
    //   text    — the version text (a registry value).
    //   returns — the four parts, most significant first.
    //   throws  — nothing.
    inline std::array<uint32_t, 4> ParseActiveSetupVersion(std::wstring_view text) noexcept
    {
        std::array<uint32_t, 4> parts{};
        size_t part = 0;
        uint64_t value = 0;
        bool digits = true;
        for (size_t i = 0; i <= text.size() && part < parts.size(); ++i)
        {
            const wchar_t c = i < text.size() ? text[i] : L',';
            if (c == L',')
            {
                parts[part++] = digits ? static_cast<uint32_t>(value) : 0u;
                value = 0;
                digits = true;
            }
            else if (c >= L'0' && c <= L'9')
            {
                value = std::min<uint64_t>(value * 10 + static_cast<uint64_t>(c - L'0'), 0xFFFFFFFFull);
            }
            else if (c != L' ')
            {
                digits = false; // "1.2" or "x": that part reads 0
            }
        }
        return parts;
    }

    // PURE: whether a user already got the all-users install's per-user part — Windows' own rule: the user's version
    // (HKCU) is at least the install's (HKLM). With no version in HKLM there is nothing to do (an all-users install
    // registered before 2026-10-22 had shared icons and no Active Setup component); with none in HKCU it is not done.
    //   userVersion    — the HKCU Version ("" when the user never got it).
    //   machineVersion — the HKLM Version ("" when the install has no Active Setup component).
    //   returns        — true when nothing is left to do for this user.
    //   throws         — nothing.
    inline bool ActiveSetupDone(std::wstring_view userVersion, std::wstring_view machineVersion) noexcept
    {
        if (machineVersion.empty())
        {
            return true;
        }
        if (userVersion.empty())
        {
            return false;
        }
        return ParseActiveSetupVersion(userVersion) >= ParseActiveSetupVersion(machineVersion);
    }

    // PURE: the program a StubPath / command line starts — its first token, quoted or not ("" for an empty text).
    // Used to check that the Active Setup component names THIS copy before acting on it.
    //   command — the command text (`"C:\Program Files\Agentmaster\Agentmaster.exe" --register-user`).
    //   returns — the program path, without its quotes.
    //   throws  — std::bad_alloc only.
    inline std::wstring CommandProgram(std::wstring_view command)
    {
        size_t i = 0;
        while (i < command.size() && (command[i] == L' ' || command[i] == L'\t'))
        {
            ++i;
        }
        if (i < command.size() && command[i] == L'"')
        {
            const size_t close = command.find(L'"', i + 1);
            return std::wstring{ command.substr(i + 1, close == std::wstring_view::npos ? std::wstring_view::npos : close - i - 1) };
        }
        size_t end = i;
        while (end < command.size() && command[end] != L' ' && command[end] != L'\t')
        {
            ++end;
        }
        return std::wstring{ command.substr(i, end - i) };
    }

    // ============================ startup decision (pure) ============================

    struct StartupFacts
    {
        bool portableMarker{ false }; // <exedir>\.portable
        bool envProfileSet{ false }; // AGENTMASTER_PROFILE — an explicit override skips everything
        InstallDecision decision; // <exedir>\install.path
        bool profilePointerExists{ false }; // <exedir>\profile.path — a pre-feature portable that already chose
        bool installedTargetExists{ false }; // InstalledTo: <dir>\Agentmaster.exe still there
        bool machineCopy{ false }; // Profiles::IsMachineCopyIn — the all-users install, or a folder this process cannot write
        bool allowUi{ false };
        // The shared copy's hand-off (InstallHandoff.h): this user's own install, and whether this launch goes there.
        std::wstring userCopyDir; // InstallHandoff::UserCopyFor — "" when there is none to hand to
        bool handoffToUserCopy{ false }; // InstallHandoff::ShouldHandOff for this launch
    };

    enum class StartupPlan
    {
        Continue, // nothing to do here — the normal prelude follows (incl. the §2a profile picker)
        ContinueDeferProfile, // pristine + no UI (or "later"): run self-contained, persist nothing, ask next time
        Handoff, // a launcher stub: start the installed copy, exit
        HandoffToUserCopy, // the all-users copy, started by a user with an install of their own: start that one, exit
        Prompt, // pristine + UI: ask
    };

    // PURE: the prelude's plan for this launch.
    //   f       — the gathered facts (GatherStartupFacts).
    //   returns — HandoffToUserCopy for the shared copy of a user who has their own install (checked FIRST: a launch
    //             from an Agentmaster tab carries AGENTMASTER_PROFILE, and the machine PATH puts the shared copy first
    //             in that tab too); Continue for a non-portable / overridden / decided / machine copy; Handoff for a
    //             launcher stub whose install exists; Prompt for a pristine copy with UI; ContinueDeferProfile for a
    //             pristine copy without UI.
    //   throws  — nothing.
    inline StartupPlan DecideStartup(const StartupFacts& f)
    {
        if (f.handoffToUserCopy)
        {
            return StartupPlan::HandoffToUserCopy;
        }
        if (!f.portableMarker || f.envProfileSet)
        {
            return StartupPlan::Continue;
        }
        switch (f.decision.kind)
        {
        case Decision::Installed:
        case Decision::Machine:
        case Decision::Portable:
            return StartupPlan::Continue;
        case Decision::InstalledTo:
            if (f.installedTargetExists)
            {
                return StartupPlan::Handoff;
            }
            break; // the install vanished (uninstalled / moved): pristine again
        default:
            break;
        }
        if (f.machineCopy)
        {
            // A folder this process cannot write (an administrator's copy in Program Files, a read-only
            // share): it could never remember an answer, so asking would repeat on EVERY launch for every
            // user. It runs as a machine copy instead (each user's Default profile).
            return StartupPlan::Continue;
        }
        if (f.profilePointerExists)
        {
            return StartupPlan::Continue; // it already chose a profile before this question existed — don't nag
        }
        return f.allowUi ? StartupPlan::Prompt : StartupPlan::ContinueDeferProfile;
    }

    // ============================ logging ============================

    // The install trail lands in the DEFAULT profile's hooks.log ([install] lines) — the profile
    // an installed copy uses, so the trace sits beside its own logs. Same one-append-per-line
    // recipe as Updater::LogUpdate (which this module cannot reuse: its tag is [update]). Written
    // ONLY after a decision that writes anyway — a pristine "later" never creates the folder.
    //   msg                 — the line, without its time stamp and tag.
    //   createProfileFolder — false: write only when the Default profile folder already exists. The all-users
    //                         install's per-user step (`--register-user`, run by Windows at every user's next
    //                         sign-in) passes false, so a user who never opens Agentmaster gets no .agentmaster folder.
    //   throws              — nothing.
    inline void LogInstall(const std::wstring& msg, bool createProfileFolder = true)
    {
        try
        {
            const std::wstring dir = Profiles::DefaultProfileDir();
            if (dir.empty())
            {
                return;
            }
            if (!createProfileFolder)
            {
                std::error_code ec;
                if (!std::filesystem::is_directory(std::filesystem::path{ dir }, ec))
                {
                    return;
                }
            }
            Profiles::detail::EnsureDirExists(dir);
            const std::wstring path = dir + L"\\hooks.log";
            const HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            SYSTEMTIME st{};
            ::GetLocalTime(&st);
            wchar_t stamp[24];
            ::swprintf(stamp, 24, L"[%02u:%02u:%02u.%03u] ", static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds));
            const std::string bytes = Profiles::detail::WideToUtf8(std::wstring{ stamp } + L"[install] " + msg + L"\n");
            DWORD wrote = 0;
            ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
            ::CloseHandle(h);
        }
        catch (...)
        {
            // this IS the logger — nothing left to report through
        }
    }

    // ============================ registry helpers ============================

    namespace detail
    {
        inline std::wstring RegReadString(HKEY root, const wchar_t* subKey, const wchar_t* value)
        {
            HKEY k{};
            if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
            {
                return {};
            }
            std::wstring out;
            DWORD type = 0;
            DWORD size = 0;
            if (::RegQueryValueExW(k, value, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size >= sizeof(wchar_t) && (type == REG_SZ || type == REG_EXPAND_SZ))
            {
                std::wstring buf(size / sizeof(wchar_t), L'\0');
                if (::RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &size) == ERROR_SUCCESS)
                {
                    while (!buf.empty() && buf.back() == L'\0')
                    {
                        buf.pop_back();
                    }
                    out = buf;
                }
            }
            ::RegCloseKey(k);
            return out;
        }

        inline bool RegWriteString(HKEY root, const std::wstring& subKey, const wchar_t* value, const std::wstring& data, DWORD type = REG_SZ)
        {
            HKEY k{};
            if (::RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
            {
                return false;
            }
            const LSTATUS rc = ::RegSetValueExW(k, value, 0, type, reinterpret_cast<const BYTE*>(data.c_str()), static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
            ::RegCloseKey(k);
            return rc == ERROR_SUCCESS;
        }

        inline bool RegWriteDword(HKEY root, const std::wstring& subKey, const wchar_t* value, DWORD data)
        {
            HKEY k{};
            if (::RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
            {
                return false;
            }
            const LSTATUS rc = ::RegSetValueExW(k, value, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data));
            ::RegCloseKey(k);
            return rc == ERROR_SUCCESS;
        }

        inline void RegDeleteKeyTree(HKEY root, const wchar_t* subKey)
        {
            ::RegDeleteTreeW(root, subKey);
        }

        // ---- processes ----

        // Any process whose image lives under `dir` (Agentmaster.exe, its ConPTY hosts, a
        // pre-rename WindowsTerminal.exe, the CLI): the "is this folder in use" probe. Toolhelp +
        // QueryFullProcessImageNameW (PROCESS_QUERY_LIMITED_INFORMATION — works across integrity).
        inline bool AnyProcessUnder(const std::wstring& dir)
        {
            const auto key = Profiles::detail::NormPathKey(dir);
            if (key.empty())
            {
                return false;
            }
            const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            bool found = false;
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            const DWORD self = ::GetCurrentProcessId();
            if (::Process32FirstW(snap, &pe))
            {
                do
                {
                    if (pe.th32ProcessID == self)
                    {
                        continue;
                    }
                    const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                    if (!h)
                    {
                        continue;
                    }
                    wchar_t buf[MAX_PATH * 2];
                    DWORD n = ARRAYSIZE(buf);
                    if (::QueryFullProcessImageNameW(h, 0, buf, &n) && n > 0)
                    {
                        const std::wstring_view image{ buf, n };
                        const size_t cut = image.find_last_of(L"\\/");
                        if (cut != std::wstring_view::npos && IsSameOrBelow(image.substr(0, cut), key))
                        {
                            found = true;
                        }
                    }
                    ::CloseHandle(h);
                } while (!found && ::Process32NextW(snap, &pe));
            }
            ::CloseHandle(snap);
            return found;
        }

        // ---- base64 (for powershell -EncodedCommand: UTF-16LE bytes) ----
        inline std::wstring Base64Utf16(const std::wstring& text)
        {
            static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::vector<unsigned char> bytes;
            bytes.reserve(text.size() * 2);
            for (const wchar_t c : text)
            {
                bytes.push_back(static_cast<unsigned char>(c & 0xFF));
                bytes.push_back(static_cast<unsigned char>((c >> 8) & 0xFF));
            }
            std::wstring out;
            size_t i = 0;
            while (i + 2 < bytes.size())
            {
                const uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
                out.push_back(kTable[(v >> 18) & 63]);
                out.push_back(kTable[(v >> 12) & 63]);
                out.push_back(kTable[(v >> 6) & 63]);
                out.push_back(kTable[v & 63]);
                i += 3;
            }
            if (i + 1 == bytes.size())
            {
                const uint32_t v = bytes[i] << 16;
                out.push_back(kTable[(v >> 18) & 63]);
                out.push_back(kTable[(v >> 12) & 63]);
                out += L"==";
            }
            else if (i + 2 == bytes.size())
            {
                const uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8);
                out.push_back(kTable[(v >> 18) & 63]);
                out.push_back(kTable[(v >> 12) & 63]);
                out.push_back(kTable[(v >> 6) & 63]);
                out.push_back(L'=');
            }
            return out;
        }

        // Run Windows PowerShell 5.1 (always present; the Appx cmdlets live there natively)
        // HIDDEN with an encoded command and wait. Returns the exit code, or -1 on a launch failure /
        // timeout.
        inline int RunPowerShellHidden(const std::wstring& script, DWORD timeoutMs)
        {
            std::wstring sysDir(MAX_PATH, L'\0');
            const UINT n = ::GetSystemDirectoryW(sysDir.data(), MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
            {
                return -1;
            }
            sysDir.resize(n);
            std::wstring cmd = Quoted(sysDir + L"\\WindowsPowerShell\\v1.0\\powershell.exe") +
                               L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + Base64Utf16(script);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            {
                return -1;
            }
            ::CloseHandle(pi.hThread);
            int rc = -1;
            if (::WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0)
            {
                DWORD code = 0;
                if (::GetExitCodeProcess(pi.hProcess, &code))
                {
                    rc = static_cast<int>(code);
                }
            }
            else
            {
                ::TerminateProcess(pi.hProcess, 1);
            }
            ::CloseHandle(pi.hProcess);
            return rc;
        }

        // PowerShell single-quoted literal: double every quote.
        inline std::wstring PsQuote(std::wstring_view s)
        {
            std::wstring out = L"'";
            for (const wchar_t c : s)
            {
                if (c == L'\'')
                {
                    out += L"''";
                }
                else
                {
                    out.push_back(c);
                }
            }
            out.push_back(L'\'');
            return out;
        }

        // ---- shell folders ----
        inline std::wstring ShellFolder(int csidl)
        {
            wchar_t buf[MAX_PATH];
            if (SUCCEEDED(::SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf)))
            {
                return buf;
            }
            return {};
        }

        // PKEY_AppUserModel_ID = {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, 5 — spelled out so this
        // header needs neither propkey.h's INITGUID dance nor uuid.lib in the test harness.
        inline const PROPERTYKEY& PkeyAppUserModelId()
        {
            static const PROPERTYKEY key{ { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
            return key;
        }

        // Create/overwrite a .lnk pointing at `exe` (working dir = its folder, its own icon), and
        // stamp the unpackaged AUMID on it when given (a Start-menu shortcut carrying the app's
        // AppUserModelID is what routes an unpackaged app's toasts). Caller holds COM.
        inline bool WriteShortcut(const std::wstring& lnkPath, const std::wstring& exe, const std::wstring& workDir, const std::wstring& aumid, std::wstring* error)
        {
            IShellLinkW* link = nullptr;
            HRESULT hr = ::CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
            if (FAILED(hr) || !link)
            {
                if (error)
                {
                    *error = L"CoCreateInstance(ShellLink) failed";
                }
                return false;
            }
            bool ok = false;
            link->SetPath(exe.c_str());
            link->SetWorkingDirectory(workDir.c_str());
            link->SetDescription(kShortcutDescription);
            link->SetIconLocation(exe.c_str(), 0);
            if (!aumid.empty())
            {
                IPropertyStore* store = nullptr;
                if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store))) && store)
                {
                    PROPVARIANT pv{};
                    pv.vt = VT_LPWSTR;
                    const size_t bytes = (aumid.size() + 1) * sizeof(wchar_t);
                    pv.pwszVal = static_cast<PWSTR>(::CoTaskMemAlloc(bytes));
                    if (pv.pwszVal)
                    {
                        ::memcpy(pv.pwszVal, aumid.c_str(), bytes);
                        store->SetValue(PkeyAppUserModelId(), pv);
                        store->Commit();
                        ::CoTaskMemFree(pv.pwszVal);
                    }
                    store->Release();
                }
            }
            IPersistFile* file = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file)
            {
                hr = file->Save(lnkPath.c_str(), TRUE);
                ok = SUCCEEDED(hr);
                if (!ok && error)
                {
                    *error = L"IPersistFile::Save failed for " + lnkPath;
                }
                file->Release();
            }
            else if (error)
            {
                *error = L"IPersistFile unavailable";
            }
            link->Release();
            return ok;
        }

        // The .lnk's target path ("" when unreadable). Caller holds COM.
        inline std::wstring ShortcutTarget(const std::wstring& lnkPath)
        {
            std::wstring target;
            IShellLinkW* link = nullptr;
            if (FAILED(::CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link)
            {
                return target;
            }
            IPersistFile* file = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file)
            {
                if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READ)))
                {
                    wchar_t buf[MAX_PATH * 2];
                    if (SUCCEEDED(link->GetPath(buf, ARRAYSIZE(buf), nullptr, SLGP_RAWPATH)))
                    {
                        target = buf;
                    }
                }
                file->Release();
            }
            link->Release();
            return target;
        }

        // Stamp `aumid` as the AppUserModelID of an open shell link (the property store's
        // PKEY_AppUserModel_ID). The taskbar groups a pin and the windows of the app it starts by this id, so a
        // pin re-pointed at another copy must carry that copy's id, or its window opens as a second button.
        //   link    — the link, loaded or new; the caller saves it.
        //   aumid   — the id; "" changes nothing.
        //   returns — true when the property was set (or nothing was asked).
        //   throws  — nothing.
        inline bool SetLinkAppUserModelId(IShellLinkW* link, const std::wstring& aumid) noexcept
        {
            if (aumid.empty())
            {
                return true;
            }
            IPropertyStore* store = nullptr;
            if (FAILED(link->QueryInterface(IID_PPV_ARGS(&store))) || !store)
            {
                return false;
            }
            bool ok = false;
            PROPVARIANT pv{};
            pv.vt = VT_LPWSTR;
            const size_t bytes = (aumid.size() + 1) * sizeof(wchar_t);
            pv.pwszVal = static_cast<PWSTR>(::CoTaskMemAlloc(bytes));
            if (pv.pwszVal)
            {
                ::memcpy(pv.pwszVal, aumid.c_str(), bytes);
                ok = SUCCEEDED(store->SetValue(PkeyAppUserModelId(), pv)) && SUCCEEDED(store->Commit());
                ::CoTaskMemFree(pv.pwszVal);
            }
            store->Release();
            return ok;
        }

        // Re-point an EXISTING .lnk at `exe`, IN PLACE: the file stays — for a taskbar pin that is what keeps it in
        // the taskbar's own list — and only its target, working folder, icon and AppUserModelID change. A new link
        // written over it would carry none of its other data either way, but a pin deleted and created again can
        // drop out of the taskbar, so this is the one way to move a pin. Caller holds COM.
        //   lnkPath — the shortcut to change.
        //   exe     — the new target.
        //   workDir — its working folder.
        //   aumid   — the AppUserModelID to stamp ("" leaves the shortcut's own as it is).
        //   returns — true when the link was loaded and saved.
        //   throws  — nothing.
        inline bool RepointShortcut(const std::wstring& lnkPath, const std::wstring& exe, const std::wstring& workDir, const std::wstring& aumid) noexcept
        {
            IShellLinkW* link = nullptr;
            if (FAILED(::CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link)
            {
                return false;
            }
            bool ok = false;
            IPersistFile* file = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file)
            {
                if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READWRITE)))
                {
                    link->SetPath(exe.c_str());
                    link->SetWorkingDirectory(workDir.c_str());
                    link->SetIconLocation(exe.c_str(), 0);
                    SetLinkAppUserModelId(link, aumid);
                    ok = SUCCEEDED(file->Save(nullptr, TRUE)); // nullptr: back into the file it was loaded from
                }
                file->Release();
            }
            link->Release();
            return ok;
        }

        // The AppUserModelID a shortcut carries (its property store's PKEY_AppUserModel_ID) — what the taskbar groups a
        // pin and its app's windows by, and what routes an unpackaged app's toasts through its Start-menu shortcut.
        // Caller holds COM.
        //   lnkPath — the shortcut.
        //   returns — the id, or "" when it has none or cannot be read.
        //   throws  — std::bad_alloc only.
        inline std::wstring ShortcutAumid(const std::wstring& lnkPath)
        {
            std::wstring id;
            IShellLinkW* link = nullptr;
            if (FAILED(::CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link)
            {
                return id;
            }
            IPersistFile* file = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file)
            {
                if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READ)))
                {
                    IPropertyStore* store = nullptr;
                    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store))) && store)
                    {
                        PROPVARIANT pv{};
                        if (SUCCEEDED(store->GetValue(PkeyAppUserModelId(), &pv)))
                        {
                            if (pv.vt == VT_LPWSTR && pv.pwszVal)
                            {
                                id = pv.pwszVal;
                            }
                            ::PropVariantClear(&pv);
                        }
                        store->Release();
                    }
                }
                file->Release();
            }
            link->Release();
            return id;
        }

        // The current user's taskbar pins folder (%APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\
        // TaskBar). Caller need not hold COM.
        //   returns — the folder, or "" when %APPDATA% cannot be resolved.
        //   throws  — std::bad_alloc only.
        inline std::wstring TaskbarPinsDir()
        {
            const std::wstring appData = ShellFolder(CSIDL_APPDATA);
            return appData.empty() ? std::wstring{} : appData + L"\\" + kTaskbarPinsSubdir;
        }

        struct ComScope
        {
            HRESULT hr{ E_FAIL };
            ComScope() :
                hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
            ~ComScope()
            {
                if (hr == S_OK || hr == S_FALSE)
                {
                    ::CoUninitialize();
                }
            }
        };

        inline void BroadcastEnvironmentChange()
        {
            DWORD_PTR result = 0;
            ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 3000, &result);
        }

        // The commandline tail after argv[0] (verbatim), for a launcher-stub handoff.
        inline std::wstring CommandLineTail()
        {
            const wchar_t* args = ::PathGetArgsW(::GetCommandLineW());
            return args ? std::wstring{ args } : std::wstring{};
        }

        // Start an app copy directly (same account, same rights as this process) and hand it the right
        // to come to the front. Used for a launcher stub's hand-off and for starting a copy right after
        // an install; a copy that must NOT inherit this process's rights goes through StartAsSessionUser.
        //   exe     — the app exe; its folder becomes the working directory.
        //   tail    — the commandline after the exe (verbatim; may be empty).
        //   returns — true when the process was created (it may still fail on its own afterwards).
        //   throws  — std::bad_alloc only.
        inline bool LaunchApp(const std::wstring& exe, const std::wstring& tail)
        {
            std::wstring cmd = Quoted(exe);
            if (!tail.empty())
            {
                cmd += L" " + tail;
            }
            std::wstring workDir = exe;
            if (const size_t cut = workDir.find_last_of(L"\\/"); cut != std::wstring::npos)
            {
                workDir.resize(cut);
            }
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi))
            {
                return false;
            }
            // Hand the started copy our right to the foreground: this process exits right after, and by the
            // time the copy's window exists the window the user had before (the Explorer window they
            // double-clicked in) has taken the foreground back — the installed app then opened BEHIND it,
            // only its taskbar button flashing (a test desktop, 2026-10-21, three times out of three). The
            // call succeeds while this process still holds the user's last input (the click on our
            // question); otherwise it changes nothing.
            ::AllowSetForegroundWindow(pi.dwProcessId);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            return true;
        }

        // Make `hwnd` the foreground window even when Windows' foreground lock refuses this process.
        // SetForegroundWindow is refused to a process that did not get the user's last input — and
        // that is every first-launch question started by SmartScreen's "Run anyway", by the hand-over
        // to the signed-in user (a scheduled task starts it) or by a launch whose parent already lost
        // the foreground: in a Windows 11 25H2 test desktop on 2026-10-21 each of those questions came
        // up INACTIVE (grey title, Enter typed into the window behind it), one click away but easy to
        // miss. Joining the foreground window's input queue for the call is the long-standing way past
        // that lock for a window the user is waiting for; the queues are detached again at once. Only
        // for the app's own first-launch and install dialogs — never for a window the user did not ask for.
        //   hwnd    — the window to bring forward.
        //   returns — nothing; a refusal leaves the window where it is (on top of the z-order, inactive).
        //   throws  — nothing.
        inline void ForceForeground(HWND hwnd) noexcept
        {
            const HWND fg = ::GetForegroundWindow();
            const DWORD fgThread = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
            const DWORD self = ::GetCurrentThreadId();
            // Attaching to our own thread (or to none) is meaningless; AttachThreadInput also refuses a
            // thread of another desktop or of a higher integrity level, which simply leaves `attached` false.
            const bool attached = fgThread != 0 && fgThread != self && ::AttachThreadInput(self, fgThread, TRUE);
            ::SetForegroundWindow(hwnd);
            ::BringWindowToTop(hwnd);
            if (attached)
            {
                ::AttachThreadInput(self, fgThread, FALSE);
            }
        }

        // TaskDialog callback that FOREGROUNDS the dialog on creation: the prelude has no window
        // yet, so nothing of ours can be in front, but Explorer keeps focus after a double-click and
        // an un-foregrounded first-launch question was measured to sit unnoticed for minutes (the
        // MessageBox reopen prompt had the same fix: MB_SETFOREGROUND|MB_TOPMOST). When the plain
        // request is refused (the foreground lock — see ForceForeground) it is forced.
        //   hwnd / msg — the TaskDialog callback's window and notification; only TDN_CREATED acts.
        //   returns    — S_OK (the dialog's default handling continues).
        //   throws     — nothing.
        inline HRESULT CALLBACK ForegroundOnCreate(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR)
        {
            if (msg == TDN_CREATED)
            {
                ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                if (!::SetForegroundWindow(hwnd) || ::GetForegroundWindow() != hwnd)
                {
                    ForceForeground(hwnd);
                }
            }
            return S_OK;
        }
    }

    // ============================ accounts and rights ============================

    // Who runs this process and who is signed in to its Windows session. An install writes the RUNNING
    // account's HKCU, Start menu and profile folder, so running the first launch under another account
    // (UAC over-the-shoulder credentials, "Run as different user", a deployment agent) would set
    // Agentmaster up for the wrong person.
    struct InstallIdentity
    {
        std::wstring runningUser; // DOMAIN\name of this process's token ("" when unreadable)
        std::wstring sessionUser; // DOMAIN\name signed in to this process's session ("" = none / unknown)
        bool sessionUserKnown{ false }; // the session user was read AND compared
        bool isSessionUser{ true }; // the token user IS the session user (true when unknown: never block on a probe)
        bool elevated{ false }; // the token is elevated (TokenElevation)
        bool system{ false }; // the token is LocalSystem
        DWORD sessionId{ 0 }; // this process's session (0 = services: no desktop to show a prompt on)
    };

    // What the first-launch prompt does about the account (DecideAccountPlan).
    enum class AccountPlan
    {
        AsIs, // the signed-in user, without administrator rights: install normally
        AsIsElevated, // the signed-in user WITH administrator rights: install normally, then start the copy without them
        OtherAccountRelaunch, // another account that CAN start this copy as the signed-in user (elevated / SYSTEM)
        OtherAccountExplain, // another account that cannot: the prompt says how to install for the signed-in user
        NoSessionUser, // session 0 (a service): never prompt
    };

    // PURE: the account plan for an identity.
    //   id      — the detected identity (DetectInstallIdentity).
    //   returns — NoSessionUser in session 0; AsIs / AsIsElevated for the signed-in user or when the
    //             session user is unknown (fail open: a probe failure never changes what an install does);
    //             OtherAccountRelaunch for another account that holds administrator rights (it may
    //             schedule a task for the signed-in user) or is SYSTEM; OtherAccountExplain otherwise.
    //   throws  — nothing.
    inline AccountPlan DecideAccountPlan(const InstallIdentity& id) noexcept
    {
        if (id.sessionId == 0)
        {
            return AccountPlan::NoSessionUser;
        }
        if (!id.sessionUserKnown || id.isSessionUser)
        {
            return id.elevated ? AccountPlan::AsIsElevated : AccountPlan::AsIs;
        }
        return (id.elevated || id.system) ? AccountPlan::OtherAccountRelaunch : AccountPlan::OtherAccountExplain;
    }

    namespace detail
    {
        // The DOMAIN\name of a SID ("" when it cannot be looked up).
        inline std::wstring AccountNameOfSid(PSID sid)
        {
            wchar_t name[256]{};
            wchar_t domain[256]{};
            DWORD nameLen = ARRAYSIZE(name);
            DWORD domainLen = ARRAYSIZE(domain);
            SID_NAME_USE use{};
            if (!::LookupAccountSidW(nullptr, sid, name, &nameLen, domain, &domainLen, &use))
            {
                return {};
            }
            return domainLen > 0 ? std::wstring{ domain } + L"\\" + name : std::wstring{ name };
        }

        // One WTS string of THIS process's session ("" on failure).
        inline std::wstring CurrentSessionString(WTS_INFO_CLASS info)
        {
            LPWSTR buf = nullptr;
            DWORD bytes = 0;
            if (!::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, info, &buf, &bytes) || !buf)
            {
                return {};
            }
            std::wstring out{ buf };
            ::WTSFreeMemory(buf);
            return out;
        }
    }

    // Read the process's account, elevation and session, and the user signed in to that session.
    // Every probe is best-effort: whatever cannot be read keeps the struct's fail-open default
    // (isSessionUser = true), so a lookup hiccup can never block or redirect an install.
    //   returns — the identity (see InstallIdentity).
    //   throws  — nothing (allocation failures inside read as "unknown").
    inline InstallIdentity DetectInstallIdentity() noexcept
    {
        InstallIdentity id;
        try
        {
            DWORD session = 0;
            if (::ProcessIdToSessionId(::GetCurrentProcessId(), &session))
            {
                id.sessionId = session;
            }
            HANDLE token = nullptr;
            if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            {
                return id;
            }
            // Closed on every exit from this block, the exceptional one included.
            struct TokenCloser
            {
                HANDLE h;
                ~TokenCloser() { ::CloseHandle(h); }
            } closer{ token };
            // The token user (variable-size: ask for the size first).
            std::vector<BYTE> userBuf;
            DWORD need = 0;
            ::GetTokenInformation(token, TokenUser, nullptr, 0, &need);
            if (need > 0)
            {
                userBuf.resize(need);
                if (::GetTokenInformation(token, TokenUser, userBuf.data(), need, &need))
                {
                    const PSID me = reinterpret_cast<TOKEN_USER*>(userBuf.data())->User.Sid;
                    id.runningUser = detail::AccountNameOfSid(me);
                    id.system = ::IsWellKnownSid(me, WinLocalSystemSid) != FALSE;

                    // The signed-in user of THIS session, by name, then by SID for an exact compare.
                    const std::wstring user = detail::CurrentSessionString(WTSUserName);
                    const std::wstring domain = detail::CurrentSessionString(WTSDomainName);
                    if (!user.empty())
                    {
                        id.sessionUser = domain.empty() ? user : domain + L"\\" + user;
                        BYTE sidBuf[SECURITY_MAX_SID_SIZE]{};
                        DWORD sidLen = sizeof(sidBuf);
                        wchar_t refDomain[256]{};
                        DWORD refLen = ARRAYSIZE(refDomain);
                        SID_NAME_USE use{};
                        if (::LookupAccountNameW(nullptr, id.sessionUser.c_str(), sidBuf, &sidLen, refDomain, &refLen, &use))
                        {
                            id.sessionUserKnown = true;
                            id.isSessionUser = ::EqualSid(me, reinterpret_cast<PSID>(sidBuf)) != FALSE;
                        }
                        else if (!id.runningUser.empty())
                        {
                            // An account name the lookup cannot resolve (an Azure AD / Microsoft account
                            // spelling): compare the names as a fallback, case-insensitively — the full
                            // DOMAIN\name first, then the bare names (the two APIs can spell the domain
                            // part differently for the same account).
                            const size_t cut = id.runningUser.find(L'\\');
                            const std::wstring runningName = cut == std::wstring::npos ? id.runningUser : id.runningUser.substr(cut + 1);
                            id.sessionUserKnown = true;
                            id.isSessionUser = Profiles::detail::NormPathKey(id.sessionUser) == Profiles::detail::NormPathKey(id.runningUser) ||
                                               Profiles::detail::NormPathKey(user) == Profiles::detail::NormPathKey(runningName);
                        }
                    }
                }
            }
            TOKEN_ELEVATION elevation{};
            DWORD got = 0;
            if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &got))
            {
                id.elevated = elevation.TokenIsElevated != 0;
            }
        }
        catch (...)
        {
            // Only bad_alloc can land here. The identity keeps the fail-open defaults gathered so far
            // (an install then behaves as before this check existed); the trace says so.
            LogInstall(L"identity probe CRASHED (exception) \x2014 treating this process as the signed-in user");
        }
        return id;
    }

    // ============================ existing-install detection ============================

    struct ExistingInstalls
    {
        // The installed PACKAGE (the MSIX release family — Install-Agentmaster.ps1's admin path or a
        // manual Add-AppxPackage). Its files are never touched; a checkbox offers removing it.
        bool package{ false };
        std::wstring packageFullName;
        std::wstring packagePath;
        Updater::Version packageVersion;
        bool packageRunning{ false };
        // A previous USER install of ours (HKCU Apps & Features entry with a live app exe) — or, when there is
        // none, the copy the old install script put in LegacyScriptInstallDir (userFromLegacyScript).
        std::wstring userDir;
        Updater::Version userVersion;
        bool userFromLegacyScript{ false }; // userDir came from the old script's folder, not the registry
        bool userKeptPortable{ false }; // userDir's install.path = portable: an update keeps it portable (files only)
        // A MACHINE-wide entry (HKLM — the all-users install; read-only for a normal user).
        std::wstring machineDir;
        std::wstring machineVersionText;
        bool machineHasExe{ false }; // machineDir really holds the app exe (a stale entry offers nothing)
        Updater::Version machineVersion; // its .am-version stamp, else its DisplayVersion
    };

    namespace detail
    {
        // "Agentmaster_1.2.3.0_x64__56k4f06dsfp9r" -> 1.2.3.0
        inline Updater::Version VersionFromPackageFullName(std::wstring_view full)
        {
            const size_t a = full.find(L'_');
            if (a == std::wstring_view::npos)
            {
                return {};
            }
            const size_t b = full.find(L'_', a + 1);
            return Updater::ParseVersion(full.substr(a + 1, b == std::wstring_view::npos ? std::wstring_view::npos : b - a - 1));
        }

        inline std::wstring PackagePathOf(const std::wstring& fullName)
        {
            UINT32 len = 0;
            if (::GetPackagePathByFullName(fullName.c_str(), &len, nullptr) != ERROR_INSUFFICIENT_BUFFER || len == 0)
            {
                return {};
            }
            std::wstring path(len, L'\0');
            if (::GetPackagePathByFullName(fullName.c_str(), &len, path.data()) != ERROR_SUCCESS)
            {
                return {};
            }
            while (!path.empty() && path.back() == L'\0')
            {
                path.pop_back();
            }
            return path;
        }
    }

    // The installs already on this PC, for the install question.
    //   exeDir  — the folder this process runs from: the old script's folder is never offered to itself
    //             (LegacyScriptOfferDir). "" skips nothing.
    //   returns — what was found; a probe that fails leaves its part empty.
    //   throws  — nothing.
    inline ExistingInstalls DetectExistingInstalls(const std::wstring& exeDir = {})
    {
        ExistingInstalls ex;
        try
        {
            // ---- the MSIX release family ----
            UINT32 count = 0;
            UINT32 bufLen = 0;
            const LONG rc = ::GetPackagesByPackageFamily(Updater::kReleaseFamily, &count, nullptr, &bufLen, nullptr);
            if (rc == ERROR_INSUFFICIENT_BUFFER && count > 0 && bufLen > 0)
            {
                std::vector<PWSTR> names(count);
                std::wstring buf(bufLen, L'\0');
                if (::GetPackagesByPackageFamily(Updater::kReleaseFamily, &count, names.data(), &bufLen, buf.data()) == ERROR_SUCCESS && count > 0 && names[0])
                {
                    ex.package = true;
                    ex.packageFullName = names[0];
                    ex.packageVersion = detail::VersionFromPackageFullName(ex.packageFullName);
                    ex.packagePath = detail::PackagePathOf(ex.packageFullName);
                    ex.packageRunning = !ex.packagePath.empty() && detail::AnyProcessUnder(ex.packagePath);
                }
            }
        }
        catch (...)
        {
            ex.package = false;
        }
        try
        {
            // ---- a previous user install of ours ----
            const std::wstring loc = detail::RegReadString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
            if (!loc.empty() && DirHoldsAppExe(loc))
            {
                ex.userDir = loc;
                ex.userVersion = VersionStampIn(loc);
                if (ex.userVersion.major == 0 && ex.userVersion.minor == 0 && ex.userVersion.patch == 0)
                {
                    ex.userVersion = Updater::ParseVersion(detail::RegReadString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion"));
                }
            }
            // ---- else the copy the old install script made (no registry entry; v0.7.10 and older) ----
            // Without this a new zip offered a SECOND per-user install while the script's copy, and its
            // "Agentmaster (Portable)" Start-menu entry, stayed behind.
            if (ex.userDir.empty())
            {
                const std::wstring legacy = LegacyScriptInstallDir();
                const InstallDecision d = ReadInstallDecisionIn(legacy);
                const bool targetHoldsExe = d.kind == Decision::InstalledTo && DirHoldsAppExe(d.dir);
                const std::wstring offer = LegacyScriptOfferDir(legacy, DirHoldsAppExe(legacy), d, targetHoldsExe, exeDir);
                if (!offer.empty())
                {
                    ex.userDir = offer;
                    ex.userVersion = VersionStampIn(offer);
                    ex.userFromLegacyScript = true;
                    ex.userKeptPortable = ReadInstallDecisionIn(offer).kind == Decision::Portable;
                }
            }
            // ---- a machine-wide entry (the all-users install; read-only for a normal user) ----
            // Its own key first (kMachineUninstallKey), else where a copy registered before 2026-10-21 has it.
            const wchar_t* machineKey = kMachineUninstallKey;
            std::wstring mloc = detail::RegReadString(HKEY_LOCAL_MACHINE, machineKey, L"InstallLocation");
            if (mloc.empty())
            {
                machineKey = kUninstallKey;
                mloc = detail::RegReadString(HKEY_LOCAL_MACHINE, machineKey, L"InstallLocation");
            }
            if (!mloc.empty())
            {
                ex.machineDir = mloc;
                ex.machineVersionText = detail::RegReadString(HKEY_LOCAL_MACHINE, machineKey, L"DisplayVersion");
                ex.machineHasExe = DirHoldsAppExe(mloc);
                ex.machineVersion = VersionStampIn(mloc);
                if (ex.machineVersion.major == 0 && ex.machineVersion.minor == 0 && ex.machineVersion.patch == 0)
                {
                    ex.machineVersion = Updater::ParseVersion(ex.machineVersionText);
                }
            }
        }
        catch (...)
        {
        }
        return ex;
    }

    // ============================ registration (shortcuts / menu / Apps & Features / PATH) ============================

    struct RegistrationOptions
    {
        std::wstring installDir; // where the binaries live
        std::wstring exe; // <installDir>\Agentmaster.exe
        std::wstring aumid; // the unpackaged AppUserModelID for THAT exe path ("" = don't stamp)
        std::wstring versionText; // Apps & Features DisplayVersion
        DWORD estimatedSizeKb{ 0 };
        bool desktopShortcut{ true };
        bool startMenuShortcut{ true };
        bool contextMenu{ true };
        bool appsAndFeatures{ true };
        bool userPath{ true };
    };

    // Each step best-effort; failures are collected as notes (the install is not undone for a
    // missing shortcut). Returns true when every requested step succeeded.
    inline bool RegisterInstall(const RegistrationOptions& o, std::vector<std::wstring>* notes)
    {
        bool all = true;
        const auto note = [&](const std::wstring& s) {
            all = false;
            if (notes)
            {
                notes->push_back(s);
            }
        };
        try
        {
            detail::ComScope com;
            std::wstring err;
            if (o.startMenuShortcut)
            {
                const std::wstring programs = detail::ShellFolder(CSIDL_PROGRAMS);
                if (programs.empty() || !detail::WriteShortcut(programs + L"\\" + kShortcutLeaf, o.exe, o.installDir, o.aumid, &err))
                {
                    note(L"Start-menu shortcut: " + (err.empty() ? std::wstring{ L"folder unavailable" } : err));
                }
            }
            if (o.desktopShortcut)
            {
                const std::wstring desktop = detail::ShellFolder(CSIDL_DESKTOPDIRECTORY);
                if (desktop.empty() || !detail::WriteShortcut(desktop + L"\\" + kShortcutLeaf, o.exe, o.installDir, o.aumid, &err))
                {
                    note(L"Desktop shortcut: " + (err.empty() ? std::wstring{ L"folder unavailable" } : err));
                }
            }
            if (o.contextMenu)
            {
                // The classic verb on folders (%1 = the folder), folder backgrounds (%V = the current
                // folder) and drives — HKCU\Software\Classes, per-user, no admin. Windows 11 lists it
                // under "Show more options".
                struct Verb
                {
                    const wchar_t* base;
                    const wchar_t* placeholder;
                };
                for (const Verb v : { Verb{ L"Directory", L"%1" }, Verb{ L"Directory\\Background", L"%V" }, Verb{ L"Drive", L"%1" } })
                {
                    const std::wstring key = std::wstring{ L"Software\\Classes\\" } + v.base + L"\\shell\\" + kContextVerb;
                    bool ok = detail::RegWriteString(HKEY_CURRENT_USER, key, nullptr, kContextLabel);
                    ok = detail::RegWriteString(HKEY_CURRENT_USER, key, L"Icon", Quoted(o.exe) + L",0") && ok;
                    ok = detail::RegWriteString(HKEY_CURRENT_USER, key + L"\\command", nullptr, ContextMenuCommand(o.exe, v.placeholder)) && ok;
                    if (!ok)
                    {
                        note(std::wstring{ L"Right-click menu (" } + v.base + L") could not be written");
                    }
                }
            }
            if (o.appsAndFeatures)
            {
                SYSTEMTIME st{};
                ::GetLocalTime(&st);
                wchar_t date[16];
                ::swprintf(date, 16, L"%04u%02u%02u", static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay));
                bool ok = detail::RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", kDisplayName);
                ok = detail::RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", o.versionText) && ok;
                ok = detail::RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"Publisher", kPublisher) && ok;
                ok = detail::RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", o.installDir) && ok;
                ok = detail::RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", o.exe) && ok;
                ok = detail::RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString", Quoted(o.exe) + L" --" + kUninstallFlag) && ok;
                ok = detail::RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"InstallDate", date) && ok;
                ok = detail::RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"URLInfoAbout", Updater::kReleasesPage) && ok;
                ok = detail::RegWriteDword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1) && ok;
                ok = detail::RegWriteDword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1) && ok;
                if (o.estimatedSizeKb > 0)
                {
                    ok = detail::RegWriteDword(HKEY_CURRENT_USER, kUninstallKey, L"EstimatedSize", o.estimatedSizeKb) && ok;
                }
                if (!ok)
                {
                    note(L"Apps & Features entry could not be written");
                }
            }
            if (o.userPath)
            {
                const std::wstring cur = detail::RegReadString(HKEY_CURRENT_USER, kEnvironmentKey, L"Path");
                if (!PathListContains(cur, o.installDir))
                {
                    if (detail::RegWriteString(HKEY_CURRENT_USER, kEnvironmentKey, L"Path", AddDirToPathList(cur, o.installDir), REG_EXPAND_SZ))
                    {
                        detail::BroadcastEnvironmentChange();
                    }
                    else
                    {
                        note(L"User PATH could not be updated");
                    }
                }
            }
        }
        catch (...)
        {
            note(L"registration crashed (exception)");
        }
        return all;
    }

    // ============================ the all-users install, per user ============================
    //
    // Since 2026-10-22 an install for all users gives every user icons of their OWN (EnsureMachineCopyUserSetup)
    // instead of shared ones; a user's own install takes those, and the shared copy's taskbar pins, over
    // (RegisterInstall writes the same file names; RepointSharedCopyTaskbarPins); and the shared copy passes every
    // launch to the user's own install (InstallHandoff.h). What a user WITHOUT administrator rights can change was
    // measured first, in a test desktop as a standard user (PROFILES.md §2c): the shared desktop icon and Start-menu
    // entry, HKLM and the machine PATH refuse every change; their own folders, HKCU and taskbar pins are theirs; a
    // same-named icon of their own does NOT hide a shared one (the desktop and Start show both); Windows refuses an
    // app that asks to change a Start pin.

    // The all-users install's folder, from its Apps & Features entry (HKLM: kMachineUninstallKey, else the key an
    // install registered before 2026-10-21 used), when it still holds the app exe.
    //   returns — the folder, or "".
    //   throws  — std::bad_alloc only.
    inline std::wstring MachineInstallDirFromRegistry()
    {
        for (const wchar_t* key : { kMachineUninstallKey, kUninstallKey })
        {
            const std::wstring loc = detail::RegReadString(HKEY_LOCAL_MACHINE, key, L"InstallLocation");
            if (!loc.empty() && DirHoldsAppExe(loc))
            {
                return loc;
            }
        }
        return {};
    }

    // Whether the all-users install's Active Setup component starts a program inside `dir` — the guard that keeps the
    // per-user step to the copy the component names: an unzip an administrator dropped into another protected folder
    // is a machine copy too, but gets no icons of its own.
    //   dir     — the folder to check.
    //   returns — true when the component's StubPath names a program in `dir`.
    //   throws  — std::bad_alloc only.
    inline bool ActiveSetupNamesFolder(const std::wstring& dir)
    {
        const std::wstring program = CommandProgram(detail::RegReadString(HKEY_LOCAL_MACHINE, kActiveSetupKey, L"StubPath"));
        if (program.empty() || dir.empty())
        {
            return false;
        }
        return Profiles::detail::SamePath(std::filesystem::path{ program }.parent_path().wstring(), dir);
    }

    // Make the shared copy's icons in a user's own desktop and Start-menu folders — each one only where no
    // "Agentmaster" icon exists yet: an existing one is the user's (their own install's, or one they keep), and is
    // never overwritten. A new icon carries this copy's AppUserModelID for that user. Caller holds COM.
    //   desktopDir  — the user's desktop folder ("" skips it).
    //   programsDir — the user's Start-menu Programs folder ("" skips it).
    //   exe         — the shared copy's app exe.
    //   aumid       — its AppUserModelID for this user ("" = none).
    //   returns     — the number of icons made.
    //   throws      — std::bad_alloc only.
    inline int RegisterMachineCopyUserIconsIn(const std::wstring& desktopDir, const std::wstring& programsDir, const std::wstring& exe, const std::wstring& aumid)
    {
        int made = 0;
        const std::wstring workDir = std::filesystem::path{ exe }.parent_path().wstring();
        for (const std::wstring& folder : { desktopDir, programsDir })
        {
            if (folder.empty())
            {
                continue;
            }
            const std::wstring lnk = folder + L"\\" + kShortcutLeaf;
            std::error_code ec;
            if (std::filesystem::exists(std::filesystem::path{ lnk }, ec))
            {
                continue;
            }
            Profiles::detail::EnsureDirExists(folder);
            if (detail::WriteShortcut(lnk, exe, workDir, aumid, nullptr))
            {
                ++made;
            }
        }
        return made;
    }

    // Re-point every taskbar pin whose target lies inside `fromDir` at `toExe` (detail::RepointShortcut: the pin file
    // stays in place, so the taskbar keeps it). Caller holds COM.
    //   pinsDir — the taskbar pins folder (detail::TaskbarPinsDir; tests pass a scratch folder).
    //   fromDir — the install whose pins move.
    //   toExe   — the app exe they move to.
    //   aumid   — toExe's AppUserModelID for this user ("" leaves each pin's own).
    //   returns — the number of pins re-pointed.
    //   throws  — std::bad_alloc only.
    inline int RepointPinsIn(const std::wstring& pinsDir, const std::wstring& fromDir, const std::wstring& toExe, const std::wstring& aumid)
    {
        if (pinsDir.empty() || fromDir.empty() || toExe.empty())
        {
            return 0;
        }
        int moved = 0;
        const std::wstring workDir = std::filesystem::path{ toExe }.parent_path().wstring();
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator{ std::filesystem::path{ pinsDir }, ec }; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            const std::filesystem::path& p = it->path();
            if (::_wcsicmp(p.extension().c_str(), L".lnk") != 0)
            {
                continue;
            }
            const std::wstring target = detail::ShortcutTarget(p.wstring());
            if (target.empty() || !IsSameOrBelow(std::filesystem::path{ target }.parent_path().wstring(), fromDir))
            {
                continue; // another app's pin, or one that cannot be read: left alone
            }
            if (detail::RepointShortcut(p.wstring(), toExe, workDir, aumid))
            {
                ++moved;
            }
        }
        return moved;
    }

    // Stamp this copy's AppUserModelID on the user's own "Agentmaster" icons and taskbar pins that start it but carry
    // another id or none — in place (detail::RepointShortcut keeps everything else; a link that already carries the id is
    // never rewritten). Who needs it: the icons and pins a user's own copy's uninstall hands back to this copy
    // (Move-AmUserEntriesToMachineCopy), made by a script that cannot compute the id — an icon without it does not
    // route this copy's toasts, and a pin that still names the removed copy opens this copy's first window as a second
    // taskbar button. Run ONLY by `--register-user`, a process that opens no window: the app itself defers stamping a pin
    // that names it with another id until it exits (WindowEmperor::_setupAumid, GH#20053), because a pin changed while
    // its app starts splits the taskbar button. Nothing for a user with an install of their own (their copy owns the
    // icons and pins). Caller need not hold COM.
    //   exeDir  — the shared copy's folder.
    //   aumid   — its AppUserModelID for this user ("" = nothing to stamp).
    //   returns — the number of links stamped.
    //   throws  — nothing.
    inline int StampMachineCopyUserLinks(const std::wstring& exeDir, const std::wstring& aumid) noexcept
    {
        try
        {
            if (exeDir.empty() || aumid.empty() || !InstallHandoff::UserCopyFor(exeDir, kAppExeLeaf).empty())
            {
                return 0;
            }
            detail::ComScope com;
            const std::wstring exe = AppExeIn(exeDir);
            std::vector<std::wstring> links;
            for (const std::wstring& folder : { detail::ShellFolder(CSIDL_DESKTOPDIRECTORY), detail::ShellFolder(CSIDL_PROGRAMS) })
            {
                if (!folder.empty())
                {
                    links.push_back(folder + L"\\" + kShortcutLeaf);
                }
            }
            std::error_code ec;
            const std::wstring pins = detail::TaskbarPinsDir();
            if (!pins.empty())
            {
                for (auto it = std::filesystem::directory_iterator{ std::filesystem::path{ pins }, ec }; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
                {
                    if (::_wcsicmp(it->path().extension().c_str(), L".lnk") == 0)
                    {
                        links.push_back(it->path().wstring());
                    }
                }
            }
            int stamped = 0;
            for (const std::wstring& lnk : links)
            {
                std::error_code existsEc;
                if (!std::filesystem::exists(std::filesystem::path{ lnk }, existsEc))
                {
                    continue;
                }
                const std::wstring target = detail::ShortcutTarget(lnk);
                if (target.empty() || !IsSameOrBelow(std::filesystem::path{ target }.parent_path().wstring(), exeDir))
                {
                    continue; // another app's link, or one that cannot be read: left alone
                }
                if (detail::ShortcutAumid(lnk) == aumid)
                {
                    continue; // already right: never rewritten
                }
                if (detail::RepointShortcut(lnk, exe, exeDir, aumid))
                {
                    ++stamped;
                }
            }
            if (stamped > 0)
            {
                LogInstall(L"all-users copy " + exeDir + L": its id stamped on " + std::to_wstring(stamped) + L" of this user's icons and taskbar pins", false);
            }
            return stamped;
        }
        catch (...)
        {
            try
            {
                LogInstall(L"stamping the all-users copy's id on this user's icons CRASHED (exception) \x2014 left as they were", false);
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return 0;
        }
    }

    // Where a user's own install registers itself (RegisterInstall), as subkeys of `root`. The defaults are the real
    // places; a test passes scratch keys under a key of its own.
    struct UserRegistrationKeys
    {
        HKEY root{ HKEY_CURRENT_USER };
        std::wstring classesRoot{ L"Software\\Classes" }; // the "Open in Agentmaster" verbs live under <this>\<base>\shell
        std::wstring uninstallKey{ kUninstallKey }; // the Apps & Features entry
        std::wstring environmentKey{ kEnvironmentKey }; // the value Path
    };

    // PURE-ish: whether a registered install folder is GONE (deleted by hand) rather than just out of reach. A folder
    // on a drive that is not there right now (an unplugged USB stick, a disconnected share) is out of reach and its
    // registration must survive; a folder whose drive is there but which holds no app exe is gone.
    //   dir     — the folder an HKCU entry names.
    //   returns — true when its drive or share answers and the folder holds no app exe.
    //   throws  — std::bad_alloc only.
    inline bool InstallFolderIsGone(const std::wstring& dir)
    {
        if (dir.empty())
        {
            return false;
        }
        std::error_code ec;
        const std::filesystem::path root = std::filesystem::path{ dir }.root_path();
        if (root.empty() || !std::filesystem::exists(root, ec))
        {
            return false;
        }
        return !DirHoldsAppExe(dir);
    }

    // Remove what a user's own install left when its folder was deleted by hand instead of uninstalled — only the
    // entries that point INTO that folder: the "Open in Agentmaster" verbs (they keep replacing the shared copy's
    // for this user, and open nothing), the Apps & Features entry, the PATH entry, and the desktop and Start-menu
    // icons; its taskbar pins go back to the shared copy. Caller holds COM.
    //   staleDir    — the deleted install's folder.
    //   keys        — where the registrations live (UserRegistrationKeys{} = the real places).
    //   desktopDir  — the user's desktop folder ("" skips it).
    //   programsDir — the user's Start-menu Programs folder ("" skips it).
    //   pinsDir     — the user's taskbar pins folder ("" skips the pins).
    //   sharedExe   — the shared copy's app exe the pins move to.
    //   sharedAumid — its AppUserModelID for this user.
    //   returns     — the number of entries removed or re-pointed.
    //   throws      — std::bad_alloc only.
    inline int CleanStaleUserInstall(const std::wstring& staleDir, const UserRegistrationKeys& keys, const std::wstring& desktopDir, const std::wstring& programsDir,
                                     const std::wstring& pinsDir, const std::wstring& sharedExe, const std::wstring& sharedAumid)
    {
        int n = 0;
        for (const wchar_t* base : { L"Directory", L"Directory\\Background", L"Drive" })
        {
            const std::wstring verbKey = keys.classesRoot + L"\\" + base + L"\\shell\\" + kContextVerb;
            const std::wstring program = CommandProgram(detail::RegReadString(keys.root, (verbKey + L"\\command").c_str(), nullptr));
            if (!program.empty() && IsSameOrBelow(std::filesystem::path{ program }.parent_path().wstring(), staleDir) &&
                ::RegDeleteTreeW(keys.root, verbKey.c_str()) == ERROR_SUCCESS)
            {
                ++n;
            }
        }
        const std::wstring loc = detail::RegReadString(keys.root, keys.uninstallKey.c_str(), L"InstallLocation");
        if (!loc.empty() && Profiles::detail::SamePath(loc, staleDir) && ::RegDeleteTreeW(keys.root, keys.uninstallKey.c_str()) == ERROR_SUCCESS)
        {
            ++n;
        }
        const std::wstring path = detail::RegReadString(keys.root, keys.environmentKey.c_str(), L"Path");
        if (PathListContains(path, staleDir) && detail::RegWriteString(keys.root, keys.environmentKey, L"Path", RemoveDirFromPathList(path, staleDir), REG_EXPAND_SZ))
        {
            ++n;
            if (keys.root == HKEY_CURRENT_USER && keys.environmentKey == kEnvironmentKey)
            {
                detail::BroadcastEnvironmentChange(); // only for the real PATH: new shells pick the change up
            }
        }
        for (const std::wstring& folder : { desktopDir, programsDir })
        {
            if (folder.empty())
            {
                continue;
            }
            const std::wstring lnk = folder + L"\\" + kShortcutLeaf;
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::path{ lnk }, ec))
            {
                continue;
            }
            const std::wstring target = detail::ShortcutTarget(lnk);
            if (!target.empty() && IsSameOrBelow(std::filesystem::path{ target }.parent_path().wstring(), staleDir) && ::DeleteFileW(lnk.c_str()))
            {
                ++n;
            }
        }
        n += RepointPinsIn(pinsDir, staleDir, sharedExe, sharedAumid);
        return n;
    }

    // The all-users install's per-user part, for the user running this process: their OWN "Agentmaster" desktop and
    // Start-menu icons for the shared copy. Windows runs it through Active Setup at each user's next sign-in
    // (HandleRegisterUserFlagAtStartup); the shared copy's own start runs it too (MachineCopyUserUpkeep), which
    // covers the user who just installed it. ONCE per user: the HKCU Version marks it done exactly as Active Setup
    // marks it, so an icon the user deletes stays deleted (an update keeps the install's Version; only a new install
    // writes a newer one). A user with an install of their own gets no icons — their copy is the one they use
    // (InstallHandoff.h), and its icons carry the same names.
    //   exeDir — the shared copy's folder (the running exe's).
    //   aumid  — the AppUserModelID this copy runs under for this user ("" = none).
    //   force  — make missing icons even when this user got them before (after CleanStaleUserInstall removed the
    //            icons of their deleted own copy).
    //   returns — the number of icons made.
    //   throws  — nothing.
    inline int EnsureMachineCopyUserSetup(const std::wstring& exeDir, const std::wstring& aumid, bool force) noexcept
    {
        try
        {
            const std::wstring machineVersion = detail::RegReadString(HKEY_LOCAL_MACHINE, kActiveSetupKey, L"Version");
            if (machineVersion.empty() || !ActiveSetupNamesFolder(exeDir))
            {
                return 0; // no per-user model here: an all-users install registered before 2026-10-22, or another copy
            }
            const std::wstring userVersion = detail::RegReadString(HKEY_CURRENT_USER, kActiveSetupKey, L"Version");
            if (!force && ActiveSetupDone(userVersion, machineVersion))
            {
                return 0;
            }
            int made = 0;
            const bool ownCopy = !InstallHandoff::UserCopyFor(exeDir, kAppExeLeaf).empty();
            if (!ownCopy)
            {
                detail::ComScope com;
                made = RegisterMachineCopyUserIconsIn(detail::ShellFolder(CSIDL_DESKTOPDIRECTORY), detail::ShellFolder(CSIDL_PROGRAMS), AppExeIn(exeDir), aumid);
            }
            // Marked done whatever was made: an icon that already existed is the user's, and one they delete later
            // must not come back at their next sign-in or start.
            detail::RegWriteString(HKEY_CURRENT_USER, kActiveSetupKey, L"Version", machineVersion);
            LogInstall(ownCopy ? L"all-users copy " + exeDir + L": this user has an install of their own, no icons made for the shared copy" :
                                 L"all-users copy " + exeDir + L": " + std::to_wstring(made) + L" icon(s) made in this user's own desktop and Start menu",
                       false);
            return made;
        }
        catch (...)
        {
            try
            {
                LogInstall(L"the all-users copy's per-user step CRASHED (exception) \x2014 no icons made", false);
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return 0;
        }
    }

    // The shared copy's per-user upkeep at every start that runs it (EnsureInstallDecidedAtStartup): first the
    // leftovers of this user's own install when that was deleted by hand (its HKCU entry names a folder that is gone —
    // its right-click entry would keep replacing the shared copy's and open nothing), then the user's icons
    // (EnsureMachineCopyUserSetup; forced after such a clean-up, which removed their icons). Registry reads only, unless
    // something is due.
    //   exeDir — the shared copy's folder.
    //   aumid  — its AppUserModelID for this user.
    //   throws — nothing.
    inline void MachineCopyUserUpkeep(const std::wstring& exeDir, const std::wstring& aumid) noexcept
    {
        try
        {
            bool force = false;
            const std::wstring registered = InstallHandoff::ReadUserInstallDir();
            if (!registered.empty() && !Profiles::detail::SamePath(registered, exeDir) && InstallFolderIsGone(registered))
            {
                detail::ComScope com;
                const int n = CleanStaleUserInstall(registered, UserRegistrationKeys{}, detail::ShellFolder(CSIDL_DESKTOPDIRECTORY), detail::ShellFolder(CSIDL_PROGRAMS),
                                                    detail::TaskbarPinsDir(), AppExeIn(exeDir), aumid);
                LogInstall(L"this user's own install " + registered + L" is gone (deleted by hand): " + std::to_wstring(n) +
                           L" leftover entr" + (n == 1 ? L"y" : L"ies") + L" removed, or moved to the all-users copy");
                force = true;
            }
            EnsureMachineCopyUserSetup(exeDir, aumid, force);
        }
        catch (...)
        {
            try
            {
                LogInstall(L"the all-users copy's per-user upkeep CRASHED (exception) \x2014 left as it was");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
        }
    }

    // `Agentmaster.exe --register-user` — the all-users install's Active Setup command (kRegisterUserFlag). Handled
    // BEFORE the single-instance handoff: Windows runs it at sign-in, before any Agentmaster could be running, and a
    // running instance must never receive it as a command line either. Never shows UI, never starts the app. Two
    // steps: the user's icons when due (EnsureMachineCopyUserSetup), then — every run — this copy's id stamped on the
    // user's icons and pins of it that lack it (StampMachineCopyUserLinks; the uninstall of a user's own copy runs this
    // command for that after it handed its icons and pins back).
    //   aumidFor — the EXE's AppUserModelID formula for an image path: the icons carry the id the copy runs under.
    //   returns  — true when the flag was present (handled: the caller exits); false otherwise.
    //   throws   — nothing.
    inline bool HandleRegisterUserFlagAtStartup(const std::function<std::wstring(const std::wstring&)>& aumidFor) noexcept
    {
        try
        {
            if (!Profiles::detail::CommandLineHasFlag(kRegisterUserFlag))
            {
                return false;
            }
            const std::wstring exeDir = Profiles::detail::ExeDirPath();
            if (!exeDir.empty() && Profiles::IsMachineCopyIn(exeDir))
            {
                const std::wstring exe = AppExeIn(exeDir);
                const std::wstring aumid = aumidFor ? aumidFor(exe) : std::wstring{};
                EnsureMachineCopyUserSetup(exeDir, aumid, false);
                // Every run, not only a due one: a user's own copy's uninstall hands its icons and pins back made by a
                // script, which cannot compute this id, and then runs this command for the id (Uninstall-AmCopy).
                StampMachineCopyUserLinks(exeDir, aumid);
            }
            return true;
        }
        catch (...)
        {
            try
            {
                LogInstall(L"--register-user handling CRASHED (exception) \x2014 no icons made", false);
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return true; // never start the app on this flag
        }
    }

    // ============================ the embedded install core (other-rights steps) ============================

    // The outcome of one RunEmbeddedScript call.
    struct ScriptRun
    {
        bool started{ false }; // the PowerShell process was created
        bool cancelled{ false }; // the UAC prompt was declined (ERROR_CANCELLED) — not an error to show
        DWORD exitCode{ 0 }; // the script's exit code (only when started and waited for)
        DWORD error{ 0 }; // why ShellExecuteEx failed (GetLastError); 0 when it succeeded or was never reached
    };

    // PURE: why a script did NOT start (run.started false, not cancelled) — the reason in the user's
    // terms, never a guess; each caller adds its own advice. Seen in a test desktop on 2026-10-21: a
    // standard user whose elevation the PC's policy refuses ("Automatically deny elevation requests",
    // common on company PCs) gets ERROR_ACCESS_DISABLED_BY_POLICY (1260) from the "runas" verb — which the
    // first version reported as "Windows PowerShell or the embedded script is missing".
    //   run     — the outcome.
    //   elevate — whether administrator rights were asked for.
    //   returns — one sentence.
    //   throws  — std::bad_alloc only.
    inline std::wstring ScriptStartFailureText(const ScriptRun& run, bool elevate)
    {
        if (elevate && (run.error == ERROR_ACCESS_DISABLED_BY_POLICY || run.error == ERROR_ACCESS_DENIED))
        {
            return L"Windows does not let this account run programs as administrator (a setting of this PC).";
        }
        if (run.error == ERROR_FILE_NOT_FOUND || run.error == ERROR_PATH_NOT_FOUND)
        {
            return L"Windows PowerShell (powershell.exe) was not found on this PC.";
        }
        if (run.error == 0)
        {
            return L"The install script could not be prepared in the temporary folder.";
        }
        return L"The install script could not be started (Windows error " + std::to_wstring(run.error) + L").";
    }

    // PURE: one argument for a PowerShell `-File` command line: wrapped in double quotes, with any
    // trailing backslashes removed (`"C:\dir\"` would escape the closing quote under the C runtime's
    // argument rules) and any embedded double quote refused by dropping it (a Windows path cannot
    // hold one; nothing we pass legitimately does).
    //   value   — a path or token.
    //   returns — the quoted argument.
    //   throws  — std::bad_alloc only.
    inline std::wstring ScriptArg(std::wstring_view value)
    {
        std::wstring v;
        v.reserve(value.size() + 2);
        for (const wchar_t c : value)
        {
            if (c != L'"')
            {
                v.push_back(c);
            }
        }
        while (!v.empty() && v.back() == L'\\')
        {
            v.pop_back();
        }
        return L"\"" + v + L"\"";
    }

    // The commandline tail encoded for the script's -ExeArgs (base64 of its UTF-16LE bytes): an
    // arbitrary tail (quotes, spaces, carets) survives the PowerShell command line untouched, and the
    // script decodes it with [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String(...)).
    //   tail    — the verbatim commandline tail (may be empty).
    //   returns — the base64 text ("" for an empty tail).
    //   throws  — std::bad_alloc only.
    inline std::wstring EncodeExeArgs(const std::wstring& tail)
    {
        return tail.empty() ? std::wstring{} : detail::Base64Utf16(tail);
    }

    // PURE: what an elevated run of the embedded install script asks administrator rights FOR, in words, from the
    // script parameter it starts with — the text the UAC prompt's echo prints to the command's console (DialogEcho.h
    // ShellExecuteWithEcho), since the prompt itself (on the secure desktop) names only "Windows PowerShell".
    //   args    — the script's parameters as RunEmbeddedScript gets them ("-InstallMachine -SourceDir ...").
    //   returns — e.g. "install Agentmaster for all users of this PC".
    //   throws  — std::bad_alloc only.
    inline std::wstring ElevationPurpose(std::wstring_view args)
    {
        const auto first = args.substr(0, args.find(L' '));
        if (first == L"-InstallMachine")
        {
            return L"install Agentmaster for all users of this PC";
        }
        if (first == L"-UninstallMachine")
        {
            return L"uninstall the Agentmaster installed for all users of this PC";
        }
        return L"run the Agentmaster install script (" + std::wstring{ first } + L")";
    }

    // Materialize the embedded am-update.ps1 (the AM_UPDATE_PS1 resource — the shared PowerShell
    // install core) into %TEMP%\Agentmaster-update and run it with `args`. Used for the steps that need
    // DIFFERENT RIGHTS than this process has: the all-users install / uninstall (elevated: a UAC prompt)
    // and starting a copy as the signed-in user without administrator rights (-RunAsMainUser).
    //   args       — the script's parameters, each already quoted (ScriptArg).
    //   elevate    — ask for administrator rights (the "runas" verb; a no-op prompt-wise when this process
    //                is already elevated).
    //   wait       — block until the script exits and report its exit code.
    //   showWindow — show the console (progress for an install / uninstall) or keep it hidden.
    //   returns    — started / cancelled / the exit code; never started when the resource is missing or
    //                cannot be written (logged).
    //   throws     — nothing.
    inline ScriptRun RunEmbeddedScript(const std::wstring& args, bool elevate, bool wait, bool showWindow) noexcept
    {
        ScriptRun run;
        try
        {
            const std::wstring ps1 = Updater::InstallerPs1();
            if (ps1.empty())
            {
                LogInstall(L"embedded install script REFUSED (the AM_UPDATE_PS1 resource is missing)");
                return run;
            }
            std::wstring temp = Profiles::detail::GetEnvVar(L"TEMP");
            if (temp.empty())
            {
                temp = Profiles::detail::GetEnvVar(L"LOCALAPPDATA");
            }
            if (temp.empty())
            {
                return run;
            }
            const std::wstring dir = temp + L"\\Agentmaster-update";
            Profiles::detail::EnsureDirExists(dir);
            const std::wstring ps1Path = dir + L"\\am-update.ps1";
            if (!Updater::detail::WriteFileUtf8(ps1Path, ps1))
            {
                LogInstall(L"embedded install script materialize FAILED in " + dir);
                return run;
            }
            std::wstring sysDir(MAX_PATH, L'\0');
            const UINT n = ::GetSystemDirectoryW(sysDir.data(), MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
            {
                return run;
            }
            sysDir.resize(n);
            const std::wstring powershell = sysDir + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
            const std::wstring params = L"-NoProfile -ExecutionPolicy Bypass -File " + ScriptArg(ps1Path) + L" " + args;

            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            sei.lpVerb = elevate ? L"runas" : L"open";
            sei.lpFile = powershell.c_str();
            sei.lpParameters = params.c_str();
            sei.lpDirectory = dir.c_str();
            sei.nShow = showWindow ? SW_SHOWNORMAL : SW_HIDE;
            // An elevated run shows Windows' UAC prompt: a dialog our code causes, so it is echoed to the command's
            // console with its answer (DialogEcho.h); a plain "open" is the plain call. GetLastError survives it.
            if (!DialogEcho::ShellExecuteWithEcho(&sei, ElevationPurpose(args)))
            {
                const DWORD err = ::GetLastError();
                run.error = err;
                run.cancelled = err == ERROR_CANCELLED; // the user said No to the UAC prompt
                LogInstall(std::wstring{ L"embedded install script " } + (run.cancelled ? L"cancelled at the UAC prompt" : L"could not be started (error " + std::to_wstring(err) + L")"));
                return run;
            }
            run.started = true;
            if (sei.hProcess)
            {
                if (wait)
                {
                    ::WaitForSingleObject(sei.hProcess, INFINITE);
                    DWORD code = 0;
                    if (::GetExitCodeProcess(sei.hProcess, &code))
                    {
                        run.exitCode = code;
                    }
                }
                ::CloseHandle(sei.hProcess);
            }
            return run;
        }
        catch (...)
        {
            LogInstall(L"embedded install script launch CRASHED (exception \x2014 not started)");
            return run;
        }
    }

    // Start `exe` (with the commandline `tail`) as the user SIGNED IN to this session, WITHOUT
    // administrator rights — through the install core's -RunAsMainUser (a one-shot scheduled task for
    // that user, interactive, limited rights). The de-escalation every install path needs once it ran
    // elevated or under another account: the app must never keep rights it was not meant to run with.
    // The started copy may come to the front (see the AllowSetForegroundWindow note below).
    //   exe     — the app exe to start.
    //   tail    — its commandline tail (verbatim; may be empty).
    //   returns — true when the script reported that the start was handed to the user's session.
    //   throws  — nothing.
    inline bool StartAsSessionUser(const std::wstring& exe, const std::wstring& tail) noexcept
    {
        try
        {
            // The Task Scheduler starts the copy, so it never inherits a right to the foreground: the
            // restarted question / app came up inactive behind the previous window (a test desktop,
            // 2026-10-21). Its process id is unknown until the task runs, so this process — which just
            // took the user's click — lets ANY process take the foreground once; Windows withdraws the
            // grant at the user's next input. A process without that right changes nothing by calling.
            ::AllowSetForegroundWindow(ASFW_ANY);
            std::wstring args = L"-RunAsMainUser -Exe " + ScriptArg(exe);
            if (!tail.empty())
            {
                args += L" -ExeArgs " + ScriptArg(EncodeExeArgs(tail));
            }
            const ScriptRun run = RunEmbeddedScript(args, false, true, false);
            LogInstall(L"start as the signed-in user: " + exe + (run.started && run.exitCode == 0 ? L" \x2014 handed over" : L" \x2014 FAILED (exit " + std::to_wstring(run.exitCode) + L")"));
            return run.started && run.exitCode == 0;
        }
        catch (...)
        {
            LogInstall(L"start as the signed-in user CRASHED (exception)");
            return false;
        }
    }

    // ============================ the install ============================

    struct InstallOptions
    {
        std::wstring sourceDir; // the running unzip
        std::wstring destDir; // where to install (== sourceDir for an in-place install)
        std::wstring profileDir; // the profile the installed copy will use (the Default profile)
        std::wstring aumid; // the unpackaged AUMID for <destDir>\Agentmaster.exe ("" = none)
        bool takeOverPackage{ false }; // remove the installed MSIX package afterwards
        std::wstring packageFullName;
        std::wstring packagePath;
    };

    struct InstallOutcome
    {
        bool ok{ false };
        std::wstring error; // the user-facing reason when !ok
        std::wstring exe; // the installed app exe (ok)
        std::vector<std::wstring> notes; // non-fatal step failures (a missing shortcut, ...)
        bool packageRemoved{ false };
    };

    namespace detail
    {
        // Remove the "downloaded from the Internet" mark from one INSTALLED file: the Zone.Identifier
        // stream a browser attaches to a downloaded zip and Explorer's zip extraction copies onto every
        // extracted file, and which CopyFileW carries along. Measured in a Windows 11 25H2 test desktop on
        // 2026-10-21: 60 of the 63 installed files kept it (SmartScreen's "Run anyway" clears only the
        // one exe the user ran). An installed program's files carry no such mark, and a marked helper
        // started through the shell — elevate-shim.exe for an elevated tab — would meet a SmartScreen
        // prompt of its own. SmartScreen already judged the copy the user chose to run and install.
        //   file    — the installed file's full path.
        //   returns — nothing; a file without the mark (or one that refuses) is left as it is.
        //   throws  — std::bad_alloc only.
        inline void DropDownloadMark(const std::wstring& file)
        {
            ::DeleteFileW((file + L":Zone.Identifier").c_str());
        }

        // The in-place twin of CopyTree's mark removal: "Install here" registers the unzip itself, so the
        // folder's SHIPPED entries (its manifest's names, else the legacy list — IsLegacyShippedLeaf) lose
        // the download mark; a file of the user's own kept in that folder keeps whatever it carries.
        //   dir     — the install folder (the unzip).
        //   returns — nothing (best effort, per file).
        //   throws  — std::bad_alloc only (filesystem errors end that entry's walk).
        inline void DropDownloadMarksOfShipped(const std::filesystem::path& dir)
        {
            const std::vector<std::wstring> shipped = ReadManifestNamesIn(dir.wstring());
            const auto isShipped = [&](const std::wstring& leaf) {
                if (shipped.empty())
                {
                    return IsLegacyShippedLeaf(leaf);
                }
                const auto key = Profiles::detail::NormPathKey(leaf);
                return std::any_of(shipped.begin(), shipped.end(), [&](const std::wstring& n) { return Profiles::detail::NormPathKey(n) == key; });
            };
            std::error_code ec;
            for (const auto& top : std::filesystem::directory_iterator{ dir, ec })
            {
                if (!isShipped(top.path().filename().wstring()))
                {
                    continue;
                }
                if (!top.is_directory(ec))
                {
                    DropDownloadMark(top.path().wstring());
                    continue;
                }
                std::error_code walk;
                for (auto it = std::filesystem::recursive_directory_iterator{ top.path(), walk }; !walk && it != std::filesystem::recursive_directory_iterator{}; it.increment(walk))
                {
                    if (it->is_regular_file(walk))
                    {
                        DropDownloadMark(it->path().wstring());
                    }
                }
            }
        }

        // Copy `src` into `dst` (recursive; top-level entries filtered by IsCopyExcludedLeaf; files
        // overwritten), each copied file without the download mark (DropDownloadMark).
        //   src / dst — the layout and the install folder.
        //   status    — progress text ("Copying files… N MB") every 25 files.
        //   bytes     — incremented by the bytes copied (for the Apps & Features size).
        //   err       — receives the user-facing reason on a failure (may be null).
        //   returns   — true when every file was copied.
        //   throws    — std::bad_alloc only (filesystem errors land in *err).
        inline bool CopyTree(const std::filesystem::path& src, const std::filesystem::path& dst, const std::function<void(const std::wstring&)>& status, uint64_t& bytes, std::wstring* err)
        {
            std::error_code ec;
            std::filesystem::create_directories(dst, ec);
            if (ec)
            {
                if (err)
                {
                    *err = L"cannot create " + dst.wstring();
                }
                return false;
            }
            size_t files = 0;
            for (const auto& top : std::filesystem::directory_iterator{ src, ec })
            {
                const std::wstring leaf = top.path().filename().wstring();
                if (IsCopyExcludedLeaf(leaf))
                {
                    continue;
                }
                if (top.is_directory(ec))
                {
                    for (auto it = std::filesystem::recursive_directory_iterator{ top.path(), ec }; !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec))
                    {
                        const auto rel = std::filesystem::relative(it->path(), src, ec);
                        const auto target = dst / rel;
                        if (it->is_directory(ec))
                        {
                            std::filesystem::create_directories(target, ec);
                            continue;
                        }
                        std::filesystem::create_directories(target.parent_path(), ec);
                        if (!::CopyFileW(it->path().c_str(), target.c_str(), FALSE))
                        {
                            if (err)
                            {
                                *err = L"copy failed: " + it->path().wstring() + L" (error " + std::to_wstring(::GetLastError()) + L")";
                            }
                            return false;
                        }
                        DropDownloadMark(target.wstring());
                        bytes += it->file_size(ec);
                        if ((++files % 25) == 0)
                        {
                            status(L"Copying files\x2026 " + std::to_wstring(bytes / (1024 * 1024)) + L" MB");
                        }
                    }
                    continue;
                }
                const auto target = dst / top.path().filename();
                if (!::CopyFileW(top.path().c_str(), target.c_str(), FALSE))
                {
                    if (err)
                    {
                        *err = L"copy failed: " + top.path().wstring() + L" (error " + std::to_wstring(::GetLastError()) + L")";
                    }
                    return false;
                }
                DropDownloadMark(target.wstring());
                bytes += top.file_size(ec);
                if ((++files % 25) == 0)
                {
                    status(L"Copying files\x2026 " + std::to_wstring(bytes / (1024 * 1024)) + L" MB");
                }
            }
            return true;
        }

        // The one door every recursive delete of this installer goes through. It deletes `target` only when the target lies
        // strictly INSIDE `within` (the install folder the step works on — an installer never deletes outside the folder it
        // installs) and holds no Agentmaster state (Profiles::PathHoldsProfileNow: a profile folder, a folder above one, a
        // `.agentmaster*` folder, or a folder with one directly inside). A refusal is logged and leaves everything as it
        // was: the owner's rule of 2026-10-26 — "never delete any .agentmaster folder, they contain states the user MUST NOT
        // lose" — outranks a tidy install folder (a leftover set-aside folder is the worst a refusal costs).
        //   target  — the file or folder to delete.
        //   within  — the install folder it must lie inside.
        //   ec      — receives the filesystem error of the delete (cleared on a refusal: nothing was tried).
        //   returns — true when the target was deleted or did not exist; false when refused or the delete failed.
        //   throws  — std::bad_alloc only.
        inline bool RemoveTreeGuarded(const std::filesystem::path& target, const std::filesystem::path& within, std::error_code& ec)
        {
            ec.clear();
            const std::wstring t = target.wstring();
            const std::wstring w = within.wstring();
            // Strictly below the install folder: IsSameOrBelow alone would let the install folder itself through.
            if (!IsSameOrBelow(t, w) || Profiles::detail::SamePath(t, w))
            {
                LogInstall(L"REFUSED to delete " + t + L": it is not inside the install folder " + w);
                return false;
            }
            if (Profiles::PathHoldsProfileNow(t))
            {
                LogInstall(L"REFUSED to delete " + t + L": it holds Agentmaster profile data (a profile folder, or a .agentmaster folder)");
                return false;
            }
            std::filesystem::remove_all(target, ec);
            return !ec;
        }

        // PURE: the name a folder in the way of an install gets when it is set aside: `<leaf>.before-install-<yyyyMMdd-
        // HHmmss>`, plus `-<attempt>` from the second attempt on (a name already taken by an earlier set-aside that second).
        //   leaf    — the folder's own name ("bin").
        //   now     — the local time to stamp.
        //   attempt — 0 for the first try, then 1, 2, ….
        //   returns — the new leaf.
        //   throws  — std::bad_alloc only.
        inline std::wstring SetAsideFolderLeaf(std::wstring_view leaf, const SYSTEMTIME& now, unsigned attempt)
        {
            wchar_t stamp[32];
            ::swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
                       static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond));
            std::wstring out{ leaf };
            out += L".before-install-";
            out += stamp;
            if (attempt > 0)
            {
                out += L"-" + std::to_wstring(attempt);
            }
            return out;
        }

        // PURE: whether a top-level entry of an install destination that holds NO app exe is an Agentmaster leftover an
        // install may lay its files in beside: the decision and pointer files, the version / manifest / portable markers,
        // an interrupted upgrade's set-aside folder, and the preserved `settings` / `profile` folders (never touched by a
        // copy: IsCopyExcludedLeaf). What an uninstall leaves (profile.path, a profile) is the common case.
        //   leaf    — a top-level entry name.
        //   returns — true for such a leftover; false for anything else (a file or folder of the user's own).
        //   throws  — std::bad_alloc only.
        inline bool IsAgentmasterLeftoverLeaf(std::wstring_view leaf)
        {
            const auto k = Profiles::detail::NormPathKey(leaf);
            return IsPreservedLeaf(k) || IsSetAsideLeaf(k) || k == L".am-version" || k == L".am-manifest" || k == L".portable";
        }

        // The top-level entries of `dir` that are NOT Agentmaster leftovers (IsAgentmasterLeftoverLeaf): what an install
        // would otherwise mix its files into. Empty for a missing or empty folder.
        //   dir     — the install destination.
        //   returns — the foreign entries' names, in directory order.
        //   throws  — std::bad_alloc only (a folder that cannot be listed reads as empty).
        inline std::vector<std::wstring> ForeignEntriesIn(const std::wstring& dir)
        {
            std::vector<std::wstring> out;
            std::error_code ec;
            for (auto it = std::filesystem::directory_iterator{ std::filesystem::path{ dir }, ec }; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
            {
                std::wstring leaf = it->path().filename().wstring();
                if (!IsAgentmasterLeftoverLeaf(leaf))
                {
                    out.push_back(std::move(leaf));
                }
            }
            return out;
        }

        // Rename a destination folder that is in the way of an install (it holds files that are not an Agentmaster
        // install's) to SetAsideFolderLeaf in the same parent: one rename on the same volume, nothing copied, overwritten
        // or deleted — the folder is KEPT, under a name that says why. Refused, with nothing changed, for a folder that
        // holds Agentmaster state (Profiles::PathHoldsProfileNow): renaming a profile would point every install that uses it
        // at an empty folder, which would look exactly like the reset the owner reported.
        //   dir     — the destination folder (it exists).
        //   movedTo — receives the folder's new full path.
        //   err     — receives the user-facing reason on a failure.
        //   returns — true when the folder was renamed.
        //   throws  — std::bad_alloc only.
        inline bool SetAsideFolderInTheWay(const std::wstring& dir, std::wstring& movedTo, std::wstring& err)
        {
            if (Profiles::PathHoldsProfileNow(dir))
            {
                err = dir + L" holds Agentmaster data (a profile). It is left as it is: pick another folder to install to.";
                return false;
            }
            const std::filesystem::path p{ dir };
            const std::filesystem::path parent = p.parent_path();
            const std::wstring leaf = p.filename().wstring();
            SYSTEMTIME now{};
            ::GetLocalTime(&now);
            for (unsigned attempt = 0; attempt < 50; ++attempt)
            {
                const std::filesystem::path candidate = parent / SetAsideFolderLeaf(leaf, now, attempt);
                std::error_code ec;
                if (std::filesystem::exists(candidate, ec))
                {
                    continue;
                }
                if (::MoveFileExW(p.c_str(), candidate.c_str(), 0))
                {
                    movedTo = candidate.wstring();
                    return true;
                }
                const DWORD code = ::GetLastError();
                if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS)
                {
                    err = dir + L" holds files that are not Agentmaster's, and it could not be renamed to keep them (Windows error " + std::to_wstring(code) +
                          L"; something may be using it). Nothing was changed: close what uses it, or pick another folder.";
                    return false;
                }
            }
            err = dir + L" could not be renamed: every name for it is taken. Nothing was changed: pick another folder.";
            return false;
        }

        // The entries of an existing install at dst that an UPGRADE replaces — only what WE SHIP: a
        // top-level entry the NEW source layout also has, or one the OLD install's manifest lists (a file a
        // newer version no longer ships must not linger), and the old version stamp — never the preserved
        // user state, never a set-aside folder (RemoveStaleSetAsides deals with those). An old install
        // WITHOUT a manifest (a zip built before 2026-10-21) is read through the legacy list instead
        // (IsLegacyShippedLeaf), exactly as the PowerShell core's Get-AmShippedNames reads it: until then
        // this upgrade left a pre-manifest release's binaries behind (seen in a test desktop on 2026-10-21,
        // v0.7.10 -> a new layout). A folder the user shares with other files keeps them.
        //   src     — the new layout (the running unzip).
        //   dst     — the existing install.
        //   returns — the entries' full paths, in directory order.
        //   throws  — std::bad_alloc only (filesystem errors end the listing).
        inline std::vector<std::filesystem::path> ShippedEntriesToReplace(const std::filesystem::path& src, const std::filesystem::path& dst)
        {
            std::vector<std::filesystem::path> entries;
            const std::vector<std::wstring> oldShipped = ReadManifestNamesIn(dst.wstring());
            const auto listedInOld = [&](const std::wstring& leaf) {
                if (oldShipped.empty())
                {
                    return IsLegacyShippedLeaf(leaf);
                }
                const auto key = Profiles::detail::NormPathKey(leaf);
                return std::any_of(oldShipped.begin(), oldShipped.end(), [&](const std::wstring& n) { return Profiles::detail::NormPathKey(n) == key; });
            };
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator{ dst, ec })
            {
                const std::wstring leaf = entry.path().filename().wstring();
                if (IsPreservedLeaf(leaf) || IsSetAsideLeaf(leaf))
                {
                    continue;
                }
                std::error_code probe;
                if (Profiles::detail::NormPathKey(leaf) == L".am-version" || std::filesystem::exists(src / leaf, probe) || listedInOld(leaf))
                {
                    entries.push_back(entry.path());
                }
            }
            return entries;
        }

        // Delete the set-aside folders (IsSetAsideLeaf) an earlier, interrupted upgrade left in dst — by this
        // installer or by the PowerShell core, which uses the same prefix. They hold only Agentmaster's own files.
        //   dst    — the install folder.
        //   throws — std::bad_alloc only (a folder that cannot be deleted yet stays for the next run).
        inline void RemoveStaleSetAsides(const std::filesystem::path& dst)
        {
            std::vector<std::filesystem::path> stale;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator{ dst, ec })
            {
                if (IsSetAsideLeaf(entry.path().filename().wstring()))
                {
                    stale.push_back(entry.path());
                }
            }
            for (const auto& p : stale)
            {
                std::error_code rm;
                RemoveTreeGuarded(p, dst, rm); // a refusal or a file still in use keeps it for the next run
            }
        }

        // An UPGRADE of an existing install at dst, the plain way: delete what it replaces
        // (ShippedEntriesToReplace) and a stale set-aside folder. PerformInstall moves the previous version
        // aside instead (SetAsideShippedEntries), so a failed copy can put it back; this stays the rule's
        // reference and the harness's check of it.
        //   src — the new layout (the running unzip).
        //   dst — the existing install.
        //   throws — std::bad_alloc only (filesystem errors are absorbed per entry).
        inline void RemoveShippedEntries(const std::filesystem::path& src, const std::filesystem::path& dst)
        {
            RemoveStaleSetAsides(dst);
            for (const auto& p : ShippedEntriesToReplace(src, dst))
            {
                std::error_code ec;
                RemoveTreeGuarded(p, dst, ec);
            }
        }

        // The previous version of an install, moved aside by SetAsideShippedEntries while the new one is copied in.
        struct SetAside
        {
            std::filesystem::path dir; // <dst>\.am-previous-<n>; empty when nothing was moved, or once it was removed
            std::vector<std::wstring> moved; // the top-level names now inside dir
        };

        // Undo SetAsideShippedEntries: move every set-aside entry back into dst, then delete the set-aside
        // folder when all came back. Nothing in dst is removed first — for after a COPY, RestoreSetAside.
        //   dst     — the install folder.
        //   aside   — what was moved; `dir` is cleared once the folder is gone.
        //   returns — true when every entry is back.
        //   throws  — std::bad_alloc only.
        inline bool PutBackSetAside(const std::filesystem::path& dst, SetAside& aside)
        {
            bool all = true;
            for (const auto& leaf : aside.moved)
            {
                if (!::MoveFileExW((aside.dir / leaf).c_str(), (dst / leaf).c_str(), 0))
                {
                    all = false; // the entry stays in the set-aside folder, which is then kept
                }
            }
            if (all && !aside.dir.empty())
            {
                std::error_code rm;
                RemoveTreeGuarded(aside.dir, dst, rm); // empty by now; a refusal only leaves the empty folder
                aside.dir.clear();
                aside.moved.clear();
            }
            return all;
        }

        // Move what an upgrade replaces (ShippedEntriesToReplace) into a new set-aside folder inside dst, one
        // rename each: the same volume, and a running image can be renamed (unlike deleted). When one move
        // fails, the entries already moved go back (PutBackSetAside) and the upgrade stops before anything was
        // replaced. Until 2026-10-21 the previous version was deleted before the copy, and a copy that failed
        // half way left an install holding neither version.
        //   src / dst — the new layout and the existing install.
        //   aside     — receives the set-aside folder and the moved names.
        //   err       — receives the user-facing reason on a failure (may be null).
        //   returns   — true when everything was moved (or there was nothing to move).
        //   throws    — std::bad_alloc only.
        inline bool SetAsideShippedEntries(const std::filesystem::path& src, const std::filesystem::path& dst, SetAside& aside, std::wstring* err)
        {
            RemoveStaleSetAsides(dst);
            const std::vector<std::filesystem::path> entries = ShippedEntriesToReplace(src, dst);
            if (entries.empty())
            {
                return true;
            }
            const std::wstring stem = std::wstring{ kSetAsideLeafPrefix } + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
            std::error_code ec;
            for (unsigned attempt = 0; attempt < 50 && aside.dir.empty(); ++attempt)
            {
                const auto candidate = dst / (attempt == 0 ? stem : stem + L"-" + std::to_wstring(attempt));
                if (std::filesystem::create_directory(candidate, ec))
                {
                    aside.dir = candidate;
                }
            }
            if (aside.dir.empty())
            {
                if (err)
                {
                    *err = L"no folder could be created in " + dst.wstring();
                }
                return false;
            }
            for (const auto& p : entries)
            {
                const std::wstring leaf = p.filename().wstring();
                if (!::MoveFileExW(p.c_str(), (aside.dir / leaf).c_str(), 0))
                {
                    const DWORD code = ::GetLastError();
                    PutBackSetAside(dst, aside);
                    if (err)
                    {
                        *err = L"its " + leaf + L" could not be moved (Windows error " + std::to_wstring(code) + L"; something may be using it). Nothing was changed.";
                    }
                    return false;
                }
                aside.moved.push_back(leaf);
            }
            return true;
        }

        // Undo an upgrade whose COPY failed: remove what the copy may have laid down — every top-level entry of
        // the new layout a copy carries (CopyTree's rule); each one that existed before was moved aside first,
        // since ShippedEntriesToReplace takes every name the new layout has — then put the set-aside entries back.
        //   src     — the new layout.
        //   dst     — the install folder.
        //   aside   — what SetAsideShippedEntries moved.
        //   returns — true when the previous version is complete again (the set-aside folder is then gone);
        //             false keeps the set-aside folder with what could not be moved back.
        //   throws  — std::bad_alloc only.
        inline bool RestoreSetAside(const std::filesystem::path& src, const std::filesystem::path& dst, SetAside& aside)
        {
            std::error_code ec;
            for (const auto& top : std::filesystem::directory_iterator{ src, ec })
            {
                const std::wstring leaf = top.path().filename().wstring();
                if (IsCopyExcludedLeaf(leaf) || IsPreservedLeaf(leaf))
                {
                    continue; // never laid down by the copy, and never touched in dst
                }
                std::error_code rm;
                RemoveTreeGuarded(dst / leaf, dst, rm);
            }
            return PutBackSetAside(dst, aside);
        }

        // After a successful upgrade: delete the previous version's set-aside folder (best effort — one that
        // cannot be deleted yet goes at the next upgrade or uninstall). It holds only the previous version's SHIPPED
        // entries (SetAsideShippedEntries), and the delete still goes through RemoveTreeGuarded, inside the install folder.
        //   aside  — what SetAsideShippedEntries moved; cleared.
        //   throws — nothing.
        inline void DropSetAside(SetAside& aside) noexcept
        {
            if (!aside.dir.empty())
            {
                try
                {
                    std::error_code rm;
                    RemoveTreeGuarded(aside.dir, aside.dir.parent_path(), rm);
                }
                catch (...)
                {
                    // Only bad_alloc reaches here (RemoveTreeGuarded's paths and its log line): the set-aside folder then
                    // stays, and the next upgrade or uninstall deletes it (RemoveStaleSetAsides) — nothing is lost.
                    try
                    {
                        LogInstall(L"the previous version's set-aside folder was not deleted (out of memory): " + aside.dir.wstring());
                    }
                    catch (...)
                    {
                        // logger-failed: nothing left to report through (this function is noexcept)
                    }
                }
            }
            aside.dir.clear();
            aside.moved.clear();
        }

        // Retire the Start-menu entry the old install script made (kLegacyScriptShortcutLeaf) once the per-user
        // install it leads to is registered under kShortcutLeaf: only when it points into installDir, or into a
        // launcher stub whose install.path names installDir — a shortcut to any other copy stays (the PowerShell
        // core's Remove-AmLegacyScriptShortcut, the same rule).
        //   installDir — the registered per-user install.
        //   returns    — true when the shortcut was removed.
        //   throws     — std::bad_alloc only.
        inline bool RetireLegacyScriptShortcut(const std::wstring& installDir)
        {
            if (installDir.empty())
            {
                return false;
            }
            ComScope com;
            const std::wstring programs = ShellFolder(CSIDL_PROGRAMS);
            if (programs.empty())
            {
                return false;
            }
            const std::wstring lnk = programs + L"\\" + kLegacyScriptShortcutLeaf;
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::path{ lnk }, ec))
            {
                return false;
            }
            const std::wstring target = ShortcutTarget(lnk);
            if (target.empty())
            {
                return false; // unreadable: left alone
            }
            const std::wstring targetDir = std::filesystem::path{ target }.parent_path().wstring();
            bool retire = IsSameOrBelow(targetDir, installDir);
            if (!retire)
            {
                const InstallDecision stub = ReadInstallDecisionIn(targetDir);
                retire = stub.kind == Decision::InstalledTo && Profiles::detail::SamePath(stub.dir, installDir);
            }
            if (!retire || !::DeleteFileW(lnk.c_str()))
            {
                return false;
            }
            LogInstall(L"retired the old install script's Start-menu entry " + lnk);
            return true;
        }

        // The deepest folder of `path` that exists ("" when none does — an unknown drive): where a write
        // probe tells whether a NOT-YET-CREATED install folder could be created by this process.
        inline std::wstring NearestExistingDir(const std::wstring& path)
        {
            std::error_code ec;
            std::filesystem::path p{ path };
            while (!p.empty())
            {
                if (std::filesystem::is_directory(p, ec))
                {
                    return p.wstring();
                }
                const auto parent = p.parent_path();
                if (parent == p)
                {
                    break; // a root that does not exist
                }
                p = parent;
            }
            return {};
        }

        inline bool RemoveInstalledPackage(const std::wstring& fullName, const std::wstring& packagePath, std::wstring* err)
        {
            // Stop the GUI under the package path ONLY (never its OpenConsole.exe hosts — a host killed
            // outright orphans its tab's pwsh + claude with a dead console; the hosts end themselves
            // once the GUI's pipe handles close), wait for the hosts to drain, then remove per-user.
            std::wstring script =
                L"$ErrorActionPreference='Stop'; try { "
                L"$dir=" +
                PsQuote(packagePath) + L"; "
                                       L"if ($dir) { Get-CimInstance Win32_Process -Filter \"Name='Agentmaster.exe' OR Name='WindowsTerminal.exe'\" -ErrorAction SilentlyContinue | "
                                       L"Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase) } | "
                                       L"ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }; "
                                       L"$deadline=(Get-Date).AddSeconds(30); do { Start-Sleep -Milliseconds 500; "
                                       L"$hosts = Get-CimInstance Win32_Process -Filter \"Name='OpenConsole.exe'\" -ErrorAction SilentlyContinue | "
                                       L"Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase) } } "
                                       L"while ($hosts -and (Get-Date) -lt $deadline) }; "
                                       L"Remove-AppxPackage -Package " +
                PsQuote(fullName) + L" -ErrorAction Stop; exit 0 } catch { exit 7 }";
            const int rc = RunPowerShellHidden(script, kPackageRemoveTimeoutMs);
            if (rc != 0 && err)
            {
                *err = rc < 0 ? L"PowerShell could not be started / timed out" : L"Remove-AppxPackage failed (exit " + std::to_wstring(rc) + L")";
            }
            return rc == 0;
        }
    }

    // The install proper: lay the running copy into the destination (or register it in place), point it at the profile,
    // register it, and make the copy a launcher stub. Runs on a worker thread under the progress dialog.
    // What it never does: write into a destination folder that holds someone else's files — that folder is renamed aside
    // and kept, or the install is refused when it holds a profile (detail::SetAsideFolderInTheWay) — nor delete anything
    // outside the install folder or holding Agentmaster state (detail::RemoveTreeGuarded). An upgrade of an install of
    // ours replaces only what releases ship and puts the previous version back when the copy fails.
    //   o       — what to install where (InstallOptions).
    //   status  — receives the phase text for the progress dialog (called on this worker thread).
    //   returns — the outcome: ok with the installed exe and non-fatal notes (a folder renamed aside is one), or the
    //             user-facing error.
    //   throws  — nothing (an exception reads as a failed install).
    inline InstallOutcome PerformInstall(const InstallOptions& o, const std::function<void(const std::wstring&)>& status)
    {
        InstallOutcome out;
        try
        {
            const bool inPlace = Profiles::detail::SamePath(o.sourceDir, o.destDir);
            if (o.sourceDir.empty() || o.destDir.empty())
            {
                out.error = L"The install folder is empty.";
                return out;
            }
            if (!inPlace && (IsSameOrBelow(o.sourceDir, o.destDir) || IsSameOrBelow(o.destDir, o.sourceDir)))
            {
                out.error = L"The install folder must not contain (or sit inside) the folder Agentmaster is running from. Pick another folder.";
                return out;
            }
            const std::filesystem::path src{ o.sourceDir };
            const std::filesystem::path dst{ o.destDir };
            const Updater::Version ours = VersionStampIn(o.sourceDir);
            uint64_t bytes = 0;

            if (!inPlace)
            {
                // A per-user install never needs administrator rights (the user's rule: prefer a
                // non-admin install): a destination this process cannot create files in is refused up
                // front with a way out, instead of failing half way through the copy.
                if (!Profiles::detail::IsDirWritableByUs(detail::NearestExistingDir(o.destDir)))
                {
                    out.error = L"Agentmaster cannot write to " + o.destDir + L" without administrator rights. Pick a folder of your own (your user folder is the default), or choose \x201CInstall for all users\x201D.";
                    return out;
                }
                // A destination that holds files of someone else's — anything but an install of ours or the leftovers
                // of one (detail::ForeignEntriesIn) — is never mixed into, overwritten or emptied: the whole folder is
                // renamed aside and KEPT (`<folder>.before-install-<time>`), and the install lands in a fresh folder.
                // The owner's rule of 2026-10-26: an install never loses what was there. A folder that holds a profile
                // is refused instead (renaming it would point its install at an empty profile: it would look like the reported reset).
                if (!DirHoldsAppExe(o.destDir))
                {
                    const std::vector<std::wstring> foreign = detail::ForeignEntriesIn(o.destDir);
                    if (!foreign.empty())
                    {
                        status(L"Keeping what is already in " + o.destDir + L"\x2026");
                        std::wstring movedTo;
                        std::wstring moveErr;
                        if (!detail::SetAsideFolderInTheWay(o.destDir, movedTo, moveErr))
                        {
                            out.error = moveErr;
                            return out;
                        }
                        std::wstring names;
                        for (size_t i = 0; i < foreign.size() && i < 5; ++i)
                        {
                            names += (i ? L", " : L"") + foreign[i];
                        }
                        out.notes.push_back(o.destDir + L" already held " + std::to_wstring(foreign.size()) + L" item(s) that are not Agentmaster's (" + names +
                                            (foreign.size() > 5 ? L", \x2026" : L"") + L"): kept, the folder was renamed to " + movedTo);
                    }
                }
            }
            // A destination the user kept portable on purpose (the old install script's copy, for one) is
            // updated, never turned into an install: its files only — no install.path change, no profile
            // pointer, no registration (the PowerShell core's Install-AmUserCopy, the same rule). Read AFTER a folder in
            // the way was renamed aside: the fresh folder then decides nothing.
            const bool keptPortable = !inPlace && ReadInstallDecisionIn(o.destDir).kind == Decision::Portable;

            if (!inPlace)
            {
                const bool existing = DirHoldsAppExe(o.destDir);
                detail::SetAside aside;
                if (existing)
                {
                    const Updater::Version theirs = VersionStampIn(o.destDir);
                    if (Updater::CompareVersion(theirs, ours) > 0)
                    {
                        out.error = L"A newer Agentmaster (v" + FormatVersion(theirs) + L") already lives in " + o.destDir + L" \x2014 this copy is v" + FormatVersion(ours) + L". Use that install, or pick another folder.";
                        return out;
                    }
                    if (detail::AnyProcessUnder(o.destDir))
                    {
                        out.error = L"Agentmaster is running from " + o.destDir + L". Close it, then try again.";
                        return out;
                    }
                    // Moved aside, not deleted: a copy that fails half way puts the previous version back.
                    status(L"Setting the previous version aside\x2026");
                    std::wstring asideErr;
                    if (!detail::SetAsideShippedEntries(src, dst, aside, &asideErr))
                    {
                        out.error = L"The Agentmaster in " + o.destDir + L" could not be updated: " + asideErr;
                        return out;
                    }
                }
                status(L"Copying files\x2026");
                std::wstring err;
                bool copied = false;
                try
                {
                    copied = detail::CopyTree(src, dst, status, bytes, &err);
                }
                catch (...)
                {
                    copied = false; // only bad_alloc: handled like a failed copy, so the previous version goes back
                }
                if (!copied)
                {
                    out.error = err.empty() ? L"Copying the files failed." : err;
                    if (existing)
                    {
                        status(L"Putting the previous version back\x2026");
                        out.error += detail::RestoreSetAside(src, dst, aside) ?
                                         L"\n\nThe installed Agentmaster was put back as it was." :
                                         L"\n\nThe previous version could not be put back completely; what remains of it is in " + aside.dir.wstring() + L".";
                    }
                    return out;
                }
                detail::DropSetAside(aside);
            }
            else
            {
                // In place: size the folder for Apps & Features, and clear the download mark of what the
                // release shipped there (the copy branch clears it file by file in CopyTree).
                std::error_code ec;
                for (auto it = std::filesystem::recursive_directory_iterator{ src, ec }; !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec))
                {
                    if (it->is_regular_file(ec))
                    {
                        bytes += it->file_size(ec);
                    }
                }
                detail::DropDownloadMarksOfShipped(src);
            }

            const std::wstring exe = AppExeIn(o.destDir);
            if (!std::filesystem::exists(std::filesystem::path{ exe }))
            {
                out.error = L"The installed folder holds no " + std::wstring{ kAppExeLeaf } + L".";
                return out;
            }

            if (keptPortable)
            {
                out.notes.push_back(o.destDir + L" was kept portable by choice: only its files were updated");
            }
            else
            {
                // The installed copy's state: it is an INSTALL -> the Default profile, silently (its
                // pointer), and whatever a "later" run left in <source>\profile is migrated in.
                status(L"Setting up the profile\x2026");
                if (!o.profileDir.empty())
                {
                    Profiles::detail::EnsureDirExists(o.profileDir);
                    const std::wstring sourceProfile = Profiles::PortableDefaultProfileDirIn(o.sourceDir);
                    if (Profiles::ProfileDirHasData(sourceProfile))
                    {
                        Profiles::MigrateProfileData(sourceProfile, o.profileDir);
                    }
                    // A pre-profile portable kept Terminal's own settings in <source>\settings (upstream portable mode):
                    // the copy no longer carries that folder (IsCopyExcludedLeaf), so its files go where the installed copy
                    // reads them — <profile>\terminal\, only where the profile has none (it never overwrites).
                    if (!inPlace)
                    {
                        Profiles::SeedTerminalSettingsFromDir(o.sourceDir + L"\\settings", o.profileDir);
                    }
                    if (Profiles::ReadLocalProfilePointerIn(o.destDir).empty())
                    {
                        // A destination that never chose a profile (the old install script's folder, run with "Ask
                        // me later") kept its data in <dest>\profile: it comes along too (never over a file the
                        // profile already holds), and <dest>\profile itself stays where it is.
                        const std::wstring destProfile = Profiles::PortableDefaultProfileDirIn(o.destDir);
                        if (!inPlace && Profiles::ProfileDirHasData(destProfile))
                        {
                            Profiles::MigrateProfileData(destProfile, o.profileDir);
                        }
                        Profiles::SaveLocalProfilePointerIn(o.destDir, o.profileDir);
                    }
                }
                SaveInstallDecisionIn(o.destDir, { Decision::Installed, {} });

                status(L"Creating shortcuts and the right-click menu\x2026");
                RegistrationOptions r;
                r.installDir = o.destDir;
                r.exe = exe;
                r.aumid = o.aumid;
                r.versionText = FormatVersion(VersionStampIn(o.destDir));
                r.estimatedSizeKb = static_cast<DWORD>(std::min<uint64_t>(bytes / 1024, 0xFFFFFFFFull));
                RegisterInstall(r, &out.notes);
                // The old install script's "Agentmaster (Portable)" entry led here (directly, or through a
                // launcher stub): the standard entry just written replaces it.
                detail::RetireLegacyScriptShortcut(o.destDir);
            }

            if (o.takeOverPackage && !o.packageFullName.empty())
            {
                status(L"Removing the installed Agentmaster package\x2026");
                std::wstring err;
                out.packageRemoved = detail::RemoveInstalledPackage(o.packageFullName, o.packagePath, &err);
                if (!out.packageRemoved)
                {
                    out.notes.push_back(L"The installed package was left in place: " + err);
                }
            }

            if (!inPlace)
            {
                // The unzip becomes a launcher stub for the install (best-effort: read-only media
                // simply asks again — and the install is complete regardless).
                SaveInstallDecisionIn(o.sourceDir, { Decision::InstalledTo, o.destDir });
            }
            out.exe = exe;
            out.ok = true;
        }
        catch (...)
        {
            out.ok = false;
            if (out.error.empty())
            {
                out.error = L"The install failed unexpectedly (exception).";
            }
        }
        return out;
    }

    // ============================ the downloaded copy, once it handed over ============================
    //
    // A portable zip must be unpacked somewhere to run; once that copy installed Agentmaster elsewhere (or handed over to
    // an install that was already there) it only forwards launches (a launcher stub). The owner's request of 2026-10-26:
    // the user was never asked whether to delete it — "there is no use for that folder, so we can suggest the user to
    // delete it". So the copy asks, and only for a folder whose every file the download brought.

    // PURE: whether a top-level entry of a copy's folder is something only Agentmaster put there: a name its release
    // ships (the copy's own .am-manifest list, else the legacy list of names releases shipped before the manifest), a
    // file the copy itself wrote (install.path, profile.path), an interrupted upgrade's set-aside folder, or the markers.
    //   leaf          — the entry's name.
    //   manifestNames — ReadManifestNamesIn(copyDir); empty for a copy without a manifest.
    //   returns       — true when deleting the entry loses nothing of the user's (an EMPTY profile / settings folder is
    //                   judged by the caller: its emptiness is not in the name).
    //   throws        — std::bad_alloc only.
    inline bool IsDisposableCopyLeaf(std::wstring_view leaf, const std::vector<std::wstring>& manifestNames)
    {
        const auto k = Profiles::detail::NormPathKey(leaf);
        if (k == L"install.path" || k == L"profile.path" || k == L".am-version" || k == L".am-manifest" || k == L".portable" || IsSetAsideLeaf(k))
        {
            return true;
        }
        if (manifestNames.empty())
        {
            return IsLegacyShippedLeaf(k);
        }
        return std::any_of(manifestNames.begin(), manifestNames.end(), [&](const std::wstring& n) { return Profiles::detail::NormPathKey(n) == k; });
    }

    // What JudgeLeftoverCopy found.
    struct LeftoverCopyVerdict
    {
        bool offer{ false }; // the folder may be offered for deletion
        std::wstring reason; // why not (for the [install] log), "" when offered
    };

    // Whether the folder a copy runs from may be offered for deletion once it handed over to `installDir`. Offered only
    // when deleting it can lose nothing: it is not the install (nor inside or around it), not a well-known folder (the
    // user folder, Desktop, Documents, Downloads, Program Files, Windows, a drive root — a flat unzip into one of them
    // must never put that folder up for deletion), holds no Agentmaster state (Profiles::PathHoldsProfileNow), this
    // account can write it, and every top-level entry is disposable (IsDisposableCopyLeaf) — `profile` and `settings`
    // only when EMPTY (a "later" run's profile with data stays the user's, even after the install migrated a copy of it).
    //   copyDir    — the folder this process runs from.
    //   installDir — where Agentmaster runs from now.
    //   returns    — the verdict.
    //   throws     — std::bad_alloc only (a folder that cannot be read is never offered).
    inline LeftoverCopyVerdict JudgeLeftoverCopy(const std::wstring& copyDir, const std::wstring& installDir)
    {
        if (copyDir.empty() || installDir.empty() || IsSameOrBelow(copyDir, installDir) || IsSameOrBelow(installDir, copyDir))
        {
            return { false, L"the copy is the install, or lies inside or around it" };
        }
        std::filesystem::path p;
        try
        {
            p = std::filesystem::path{ copyDir }.lexically_normal();
        }
        catch (const std::bad_alloc&)
        {
            throw;
        }
        catch (...)
        {
            return { false, L"the copy's path cannot be read" };
        }
        if (!p.has_parent_path() || p.parent_path() == p || p == p.root_path())
        {
            return { false, L"the copy is a drive root" };
        }
        // Downloads has no CSIDL; its default place is what a flat unzip into it would be.
        const std::wstring home = Profiles::detail::GetEnvVar(L"USERPROFILE");
        const std::wstring downloads = home.empty() ? std::wstring{} : home + L"\\Downloads";
        for (const std::wstring& known : { detail::ShellFolder(CSIDL_PROFILE), detail::ShellFolder(CSIDL_DESKTOPDIRECTORY), detail::ShellFolder(CSIDL_PERSONAL),
                                           downloads, detail::ShellFolder(CSIDL_PROGRAM_FILES), detail::ShellFolder(CSIDL_PROGRAM_FILESX86), detail::ShellFolder(CSIDL_WINDOWS) })
        {
            if (!known.empty() && Profiles::detail::SamePath(known, copyDir))
            {
                return { false, L"the copy is a well-known folder (" + known + L")" };
            }
        }
        if (Profiles::PathHoldsProfileNow(copyDir))
        {
            return { false, L"the copy's folder holds Agentmaster profile data" };
        }
        if (!Profiles::detail::IsDirWritableByUs(copyDir))
        {
            return { false, L"this account cannot write the copy's folder" };
        }
        const std::vector<std::wstring> manifest = ReadManifestNamesIn(copyDir);
        std::error_code ec;
        bool holdsExe = false;
        for (auto it = std::filesystem::directory_iterator{ p, ec }; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            const std::wstring leaf = it->path().filename().wstring();
            const auto key = Profiles::detail::NormPathKey(leaf);
            if (key == L"profile" || key == L"settings")
            {
                std::error_code inner;
                if (std::filesystem::directory_iterator{ it->path(), inner } != std::filesystem::directory_iterator{} || inner)
                {
                    return { false, L"its " + leaf + L" folder holds data" };
                }
                continue;
            }
            if (!IsDisposableCopyLeaf(leaf, manifest))
            {
                return { false, L"it holds " + leaf + L", which the download did not bring" };
            }
            holdsExe = holdsExe || key == L"agentmaster.exe" || key == L"windowsterminal.exe";
        }
        if (ec)
        {
            return { false, L"the copy's folder cannot be listed" };
        }
        if (!holdsExe)
        {
            return { false, L"the folder holds no Agentmaster" };
        }
        return { true, {} };
    }

    // Ask whether the copy's folder should go (JudgeLeftoverCopy said it may be offered). A Win32 TaskDialog with two
    // command links, the deletion pre-selected; Cancel / X / Esc keep it. Echoed to the command's console (DialogEcho.h).
    //   copyDir    — the folder this process runs from.
    //   installDir — where Agentmaster runs from now.
    //   returns    — true when the user chose to delete it.
    //   throws     — std::bad_alloc only (the texts are built in std::wstring).
    inline bool AskToRemoveLeftoverCopy(const std::wstring& copyDir, const std::wstring& installDir)
    {
        const std::wstring content = L"Agentmaster now runs from:\n    " + installDir + L"\n\nThe folder you started this copy from is not needed any more:\n    " + copyDir +
                                     L"\n\nIt holds only the files of the download. If you keep it, starting Agentmaster.exe from it opens the installed Agentmaster.";
        constexpr int idDelete = 1201, idKeep = 1202;
        const TASKDIALOG_BUTTON buttons[] = {
            { idDelete, L"Delete the downloaded copy (recommended)\nMoves the folder to the Recycle Bin once this window closes; the installed Agentmaster is not touched" },
            { idKeep, L"Keep it\nIt stays as a shortcut to the installed Agentmaster" },
        };
        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
        cfg.pszWindowTitle = L"Agentmaster";
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszMainInstruction = L"Delete the downloaded copy?";
        cfg.pszContent = content.c_str();
        cfg.cButtons = static_cast<UINT>(ARRAYSIZE(buttons));
        cfg.pButtons = buttons;
        cfg.nDefaultButton = idDelete;
        cfg.pfCallback = detail::ForegroundOnCreate;
        int pressed = 0;
        if (FAILED(DialogEcho::ShowTaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)))
        {
            return false; // no dialog (comctl v6 unavailable): nothing is deleted without a yes
        }
        return pressed == idDelete;
    }

    // PURE: the hidden PowerShell that removes a copy's folder after this process ended. In order: wait for the process
    // (its exe and DLLs are in that folder) — two minutes at most; then for anything else that runs from the folder —
    // one minute at most; then prove nothing holds it (a rename there and back, which Windows refuses while a file in
    // it is open) and move it to the Recycle Bin (Microsoft.VisualBasic FileSystem.DeleteDirectory, SendToRecycleBin:
    // recoverable, never a plain delete); then remove the extraction folder around it ("Extract All" makes
    // Agentmaster_<version>_<arch>\agentmaster-<version>) when that is left EMPTY and is named Agentmaster* — a
    // non-recursive delete, which Windows refuses for a folder with anything in it. A folder still held is left as it is.
    //   copyDir — the folder to remove (JudgeLeftoverCopy offered it).
    //   waitPid — the process to wait for (this one).
    //   returns — the script text (for -EncodedCommand).
    //   throws  — std::bad_alloc only.
    inline std::wstring LeftoverCopyRemovalScript(const std::wstring& copyDir, DWORD waitPid)
    {
        return L"$ErrorActionPreference = 'Stop'\n"
               L"$dir = " +
               detail::PsQuote(copyDir) + L"\n"
                                          L"try { Wait-Process -Id " +
               std::to_wstring(waitPid) + L" -Timeout 120 -ErrorAction SilentlyContinue } catch { }\n"
                                          L"$deadline = (Get-Date).AddSeconds(60)\n"
                                          L"while ((Get-Date) -lt $deadline) {\n"
                                          L"  $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($dir + '\\', [StringComparison]::OrdinalIgnoreCase) })\n"
                                          L"  if ($busy.Count -eq 0) { break }\n"
                                          L"  Start-Sleep -Milliseconds 500\n"
                                          L"}\n"
                                          L"$probe = $dir + '.am-removing'\n"
                                          L"try { [IO.Directory]::Move($dir, $probe) } catch { exit 3 }\n"
                                          L"try { [IO.Directory]::Move($probe, $dir) } catch { $dir = $probe }\n"
                                          L"Add-Type -AssemblyName Microsoft.VisualBasic\n"
                                          L"[Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($dir, [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs, [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)\n"
                                          L"$parent = Split-Path -Parent $dir\n"
                                          L"if ($parent -and (Split-Path -Leaf $parent) -like 'Agentmaster*' -and -not (Get-ChildItem -LiteralPath $parent -Force | Select-Object -First 1)) {\n"
                                          L"  try { [IO.Directory]::Delete($parent) } catch { }\n"
                                          L"}\n"
                                          L"exit 0\n";
    }

    // Start the removal of a copy's folder (LeftoverCopyRemovalScript) in a hidden Windows PowerShell that outlives this
    // process: no wait, no window, out of this process's job when Windows allows it (a job that ends its processes with
    // it would end the removal too). The caller exits right after; the script waits for that.
    //   copyDir — the folder to remove.
    //   returns — true when the PowerShell process started.
    //   throws  — nothing.
    inline bool ScheduleLeftoverCopyRemoval(const std::wstring& copyDir) noexcept
    {
        try
        {
            std::wstring sysDir(MAX_PATH, L'\0');
            const UINT n = ::GetSystemDirectoryW(sysDir.data(), MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
            {
                return false;
            }
            sysDir.resize(n);
            std::wstring cmd = Quoted(sysDir + L"\\WindowsPowerShell\\v1.0\\powershell.exe") +
                               L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand " +
                               detail::Base64Utf16(LeftoverCopyRemovalScript(copyDir, ::GetCurrentProcessId()));
            // Its working folder must not be the copy's folder (a process's current directory keeps that folder busy).
            const std::wstring workDir = sysDir;
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            BOOL started = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, workDir.c_str(), &si, &pi);
            if (!started)
            {
                // A job that forbids breaking away refuses the flag (ERROR_ACCESS_DENIED): start it inside the job then.
                started = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi);
            }
            if (!started)
            {
                LogInstall(L"leftover copy: the removal of " + copyDir + L" could not be started (error " + std::to_wstring(::GetLastError()) + L")");
                return false;
            }
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            LogInstall(L"leftover copy: " + copyDir + L" goes to the Recycle Bin once this process ends");
            return true;
        }
        catch (...)
        {
            try
            {
                LogInstall(L"leftover copy: scheduling the removal CRASHED (exception) \x2014 the folder stays");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through (this function is noexcept)
            }
            return false;
        }
    }

    // The whole question for a copy that handed over: judge the folder (JudgeLeftoverCopy), and ask only when it may be
    // offered. The removal itself is started by the caller AFTER the installed copy started (ScheduleLeftoverCopyRemoval):
    // a copy whose hand-over failed goes back to the install question and must keep its folder.
    //   copyDir    — the folder this process runs from.
    //   installDir — where Agentmaster runs from now.
    //   returns    — true when the folder should be removed once the hand-over succeeded.
    //   throws     — nothing (a failure keeps the folder, logged).
    inline bool AskWhetherToRemoveLeftoverCopy(const std::wstring& copyDir, const std::wstring& installDir) noexcept
    {
        try
        {
            const LeftoverCopyVerdict verdict = JudgeLeftoverCopy(copyDir, installDir);
            if (!verdict.offer)
            {
                LogInstall(L"leftover copy: " + copyDir + L" is not offered for deletion: " + verdict.reason);
                return false;
            }
            const bool remove = AskToRemoveLeftoverCopy(copyDir, installDir);
            LogInstall(L"leftover copy: " + copyDir + (remove ? L" \x2014 the user chose to delete it" : L" \x2014 the user kept it"));
            return remove;
        }
        catch (...)
        {
            try
            {
                LogInstall(L"leftover copy: the question CRASHED (exception) \x2014 the folder stays");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through (this function is noexcept)
            }
            return false;
        }
    }

    // ============================ the progress dialog ============================

    namespace detail
    {
        struct ProgressState
        {
            std::mutex mtx;
            std::wstring text{ L"Preparing\x2026" };
            std::atomic<bool> done{ false };
        };

        inline HRESULT CALLBACK ProgressCallback(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR ref)
        {
            auto* st = reinterpret_cast<ProgressState*>(ref);
            switch (msg)
            {
            case TDN_CREATED:
                ::SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
                ForegroundOnCreate(hwnd, msg, 0, 0, 0);
                return S_OK;
            case TDN_TIMER:
                if (st)
                {
                    std::wstring text;
                    {
                        std::lock_guard<std::mutex> lock{ st->mtx };
                        text = st->text;
                    }
                    ::SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(text.c_str()));
                    if (st->done.load())
                    {
                        ::SendMessageW(hwnd, TDM_CLICK_BUTTON, IDOK, 0);
                    }
                }
                return S_OK;
            case TDN_BUTTON_CLICKED:
                // No cancelling mid-install: the click closes the dialog only once the work is done.
                return (st && st->done.load()) ? S_OK : S_FALSE;
            default:
                return S_OK;
            }
        }
    }

    // Run PerformInstall on a worker under a marquee TaskDialog ("Installing Agentmaster…"); the
    // dialog closes itself when the work completes.
    inline InstallOutcome RunInstallWithProgress(HWND owner, const InstallOptions& o)
    {
        detail::ProgressState st;
        InstallOutcome result;
        std::thread worker([&]() {
            result = PerformInstall(o, [&](const std::wstring& s) {
                std::lock_guard<std::mutex> lock{ st.mtx };
                st.text = s;
            });
            st.done.store(true);
        });

        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = owner;
        cfg.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
        cfg.pszWindowTitle = L"Agentmaster";
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszMainInstruction = L"Installing Agentmaster\x2026";
        cfg.pszContent = L"Preparing\x2026";
        const TASKDIALOG_BUTTON wait{ IDOK, L"Please wait\x2026" };
        cfg.cButtons = 1;
        cfg.pButtons = &wait;
        cfg.nDefaultButton = IDOK;
        cfg.pfCallback = detail::ProgressCallback;
        cfg.lpCallbackData = reinterpret_cast<LONG_PTR>(&st);
        // Echoed to the command's console (DialogEcho.h), like every dialog: "Installing Agentmaster..." and "closed".
        if (FAILED(DialogEcho::ShowTaskDialogIndirect(&cfg, nullptr, nullptr, nullptr)))
        {
            // No dialog (comctl v6 unavailable): the work still runs — just wait for it.
        }
        worker.join();
        return result;
    }

    // ============================ the prompt ============================

    enum class Choice
    {
        Later, // Cancel / X / Esc / "Ask me later"
        StayPortable,
        InstallHere,
        InstallDefault, // to DefaultUserInstallDir() — or an UPGRADE of the detected user install (dir set)
        InstallBrowse, // dir = the picked destination
        UseExisting, // the detected user install is already this version or newer: hand off (dir set)
        InstallMachine, // the ALL-USERS install (administrator rights): a new one, or an UPGRADE of the detected one (dir set)
        UseMachine, // the detected all-users install is already this version or newer: hand off (dir set)
        ForSessionUser, // another account runs this copy: start it again as the user signed in to Windows
    };

    struct PromptResult
    {
        Choice choice{ Choice::Later };
        std::wstring dir;
        bool takeOverPackage{ false };
    };

    // Which command link the install prompt pre-selects (DecidePromptDefault).
    enum class PromptDefault
    {
        ForSessionUser, // the account fix: another account runs this copy and can hand it to the signed-in user
        UseMachine, // an all-users install that is already current: no rights needed, no second copy
        PerUser, // the per-user install (just for you): the preferred, non-admin answer
    };

    // PURE: the pre-selected answer of the install prompt. The NON-ADMIN per-user install is the
    // preferred default (the user's rule of 2026-10-21) unless something better needs no rights either:
    // handing the copy to the signed-in user when another account runs it, or - when this user has NO
    // per-user install yet - using an all-users install that is already this version or newer (no
    // second copy). A per-user install of this user's own wins over the all-users one, current or not:
    // "use it" or "update it" also makes no second copy, and it is the kind the user prefers (checked
    // in a test desktop on 2026-10-21, where both existed and the all-users one was recommended).
    //   account        — the account plan (DecideAccountPlan).
    //   userInstalled  — this user has a per-user install of ours (the prompt's first option then uses
    //                    or updates it).
    //   machineCurrent — an all-users install exists, holds the exe, and is this version or newer.
    //   returns        — the default (PromptDefault).
    //   throws         — nothing.
    inline PromptDefault DecidePromptDefault(AccountPlan account, bool userInstalled, bool machineCurrent) noexcept
    {
        if (account == AccountPlan::OtherAccountRelaunch)
        {
            return PromptDefault::ForSessionUser;
        }
        if (userInstalled)
        {
            return PromptDefault::PerUser;
        }
        return machineCurrent ? PromptDefault::UseMachine : PromptDefault::PerUser;
    }

    // The first-launch question "Where should Agentmaster live?" (see the header comment). Pure Win32
    // (a TaskDialog with command links; a Browse… that is cancelled returns to the question).
    //   owner   — the dialog owner (none in the prelude: the app has no window yet).
    //   exeDir  — the running unzip.
    //   ex      — the installs already on this PC (DetectExistingInstalls).
    //   ours    — this copy's version (its .am-version stamp).
    //   who     — the identity (DetectInstallIdentity); account — its plan (DecideAccountPlan).
    //   returns — the choice; Later for Cancel / X / Esc or a TaskDialog failure.
    //   throws  — std::bad_alloc only (the labels are built in std::wstring).
    inline PromptResult ShowInstallPrompt(HWND owner, const std::wstring& exeDir, const ExistingInstalls& ex, const Updater::Version& ours, const InstallIdentity& who, AccountPlan account)
    {
        PromptResult result;
        const std::wstring defaultDir = DefaultUserInstallDir();
        const std::wstring oursText = FormatVersion(ours);
        const bool otherAccount = account == AccountPlan::OtherAccountRelaunch || account == AccountPlan::OtherAccountExplain;
        const bool machineCurrent = ex.machineHasExe && Updater::CompareVersion(ex.machineVersion, ours) >= 0;
        const PromptDefault def = DecidePromptDefault(account, !ex.userDir.empty(), machineCurrent);
        // The per-user options name the account they would set Agentmaster up for whenever that is NOT
        // the user signed in to Windows — the whole point of the account check is that this is visible.
        const std::wstring forWhom = otherAccount && !who.runningUser.empty() ? L"for " + who.runningUser : L"just for you";
        const std::wstring recommended = def == PromptDefault::PerUser ? L" (recommended)" : L"";

        // ---- the first (per-user) option adapts to a detected user install of ours ----
        std::wstring firstLabel;
        Choice firstChoice = Choice::InstallDefault;
        std::wstring firstDir = defaultDir;
        if (!ex.userDir.empty())
        {
            firstDir = ex.userDir;
            const std::wstring theirs = FormatVersion(ex.userVersion);
            // The old install script's copy has no registration yet: what an update does to it is spelled out.
            const std::wstring updateNote = !ex.userFromLegacyScript ? std::wstring{ L"keeps its settings and shortcuts" } :
                                            ex.userKeptPortable      ? std::wstring{ L"the copy the install script made, kept portable: only its files are updated" } :
                                                                       std::wstring{ L"the copy the install script made; keeps its data and gets the shortcuts and Apps & Features entry" };
            if (Updater::CompareVersion(ours, ex.userVersion) > 0)
            {
                firstLabel = L"Update the installed Agentmaster (v" + theirs + L" \x2192 v" + oursText + L")" + recommended + L"\n" + ex.userDir + L"  \x2014  " + updateNote;
            }
            else
            {
                firstChoice = Choice::UseExisting;
                firstLabel = L"Use the installed Agentmaster (v" + theirs + L")" + recommended + L"\n" + ex.userDir + L"  \x2014  " +
                             (ex.userFromLegacyScript ? L"the copy the install script made, " : L"") + L"already this version or newer; this copy just launches it";
            }
        }
        else
        {
            firstLabel = L"Install " + forWhom + recommended + L"\n" + defaultDir + L"  \x2014  no administrator rights needed";
            if (DirHoldsAppExe(defaultDir))
            {
                firstLabel += L"; existing files (v" + FormatVersion(VersionStampIn(defaultDir)) + L")";
            }
        }

        // ---- the all-users option adapts to a detected all-users install ----
        std::wstring machineLabel;
        Choice machineChoice = Choice::InstallMachine;
        std::wstring machineDir = DefaultMachineInstallDir();
        if (ex.machineHasExe && machineCurrent)
        {
            machineChoice = Choice::UseMachine;
            machineDir = ex.machineDir;
            machineLabel = L"Use the Agentmaster installed for all users (v" + FormatVersion(ex.machineVersion) + L")" + (def == PromptDefault::UseMachine ? L" (recommended)" : L"") +
                           L"\n" + ex.machineDir + L"  \x2014  already this version or newer; this copy just launches it, no rights needed";
        }
        else if (ex.machineHasExe)
        {
            machineDir = ex.machineDir;
            machineLabel = L"Update the Agentmaster installed for all users (v" + FormatVersion(ex.machineVersion) + L" \x2192 v" + oursText + L")\n" +
                           ex.machineDir + L"  \x2014  needs administrator rights";
        }
        else
        {
            machineLabel = L"Install for all users of this PC\n" + machineDir + L"  \x2014  needs administrator rights; every user gets their own icons and keeps their own data";
        }

        const std::wstring forUserLabel = L"Set it up for " + who.sessionUser + L" (recommended)\nStarts this copy again as " + who.sessionUser +
                                          L", the account signed in to Windows, without administrator rights";
        const std::wstring browseLabel = L"Install to a folder you pick\x2026\nBrowse; lands in <folder>\\Agentmaster unless the folder is empty";
        const std::wstring hereLabel = L"Install here\n" + exeDir + L"  \x2014  no copy; adds the shortcuts, right-click menu and Apps & Features entry";
        const std::wstring portableLabel = L"Keep it portable, run from here\n" + exeDir + L"  \x2014  no shortcuts or registry entries; never asks again";
        const std::wstring laterLabel = L"Ask me later\nRun from here for now; nothing is written, this question returns next launch";

        std::wstring content =
            L"This is a portable copy of Agentmaster v" + oursText + L", running from:\n    " + exeDir +
            L"\n\nInstalling it just for you needs no administrator rights: a desktop icon, a Start-menu entry (Start search finds it), "
            L"an \x201COpen in Agentmaster\x201D entry on the folder right-click menu, an Apps & Features entry with Uninstall, "
            L"and its folder on your PATH. An installed copy keeps its data in your profile folder (" +
            UserInstallProfileDir() + L") and updates itself in place.";
        if (otherAccount)
        {
            content += L"\n\n\x26A0 Agentmaster is running as " + (who.runningUser.empty() ? std::wstring{ L"another account" } : who.runningUser) + L", but " +
                       who.sessionUser + L" is signed in to Windows. An install sets Agentmaster up for the account that runs it: that account's desktop, Start menu and data.";
            if (account == AccountPlan::OtherAccountExplain)
            {
                content += L" To set it up for " + who.sessionUser + L", close this and start Agentmaster.exe from " + who.sessionUser +
                           L"'s own account, without \x201CRun as administrator\x201D or \x201CRun as different user\x201D.";
            }
        }
        else if (account == AccountPlan::AsIsElevated)
        {
            content += L"\n\nAgentmaster is running with administrator rights. Installing it just for you needs none, and the installed copy starts without them.";
        }
        if (ex.machineHasExe && !otherAccount)
        {
            // What a copy of one's own means next to the shared one (InstallHandoff.h, RepointSharedCopyTaskbarPins):
            // stated before the user picks, because the shared copy's entries stay visible (an administrator owns them).
            content += L"\n\nAgentmaster is also installed for all users of this PC (v" + FormatVersion(ex.machineVersion) +
                       L"). A copy of your own takes over for you: your Agentmaster icons, your taskbar pins, the right-click entry and "
                       L"the agentmaster command open it, and it updates without administrator rights. The copy for all users stays for "
                       L"the other users.";
        }
        if (ex.package)
        {
            content += L"\n\nAn installed Agentmaster package v" + FormatVersion(ex.packageVersion) +
                       L" already exists on this PC. It is left untouched; an installed copy of this portable shares its data. "
                       L"Tick the box below to remove the package afterwards and let this copy take over.";
            if (ex.packageRunning)
            {
                content += L" It is RUNNING right now \x2014 removing it closes its windows.";
            }
        }
        const std::wstring verification = L"Also remove the installed Agentmaster package v" + FormatVersion(ex.packageVersion) + L" (its data in " + UserInstallProfileDir() + L" is kept and reused)";
        const std::wstring footer = L"Uninstall later from Settings \x2192 Apps, or in Agentmaster: the Home button (the Agent Manager page) \x2192 \x2699 Settings \x2192 About.";

        constexpr int idForUser = 1100, idFirst = 1101, idBrowse = 1102, idHere = 1103, idPortable = 1104, idLater = 1105, idMachine = 1106;
        std::vector<TASKDIALOG_BUTTON> buttons;
        if (account == AccountPlan::OtherAccountRelaunch && !who.sessionUser.empty())
        {
            buttons.push_back({ idForUser, forUserLabel.c_str() });
        }
        buttons.push_back({ idFirst, firstLabel.c_str() });
        buttons.push_back({ idMachine, machineLabel.c_str() });
        buttons.push_back({ idBrowse, browseLabel.c_str() });
        buttons.push_back({ idHere, hereLabel.c_str() });
        buttons.push_back({ idPortable, portableLabel.c_str() });
        buttons.push_back({ idLater, laterLabel.c_str() });
        const int defaultId = def == PromptDefault::ForSessionUser && account == AccountPlan::OtherAccountRelaunch && !who.sessionUser.empty() ? idForUser :
                              def == PromptDefault::UseMachine                                                                          ? idMachine :
                                                                                                                                          idFirst;

        for (;;)
        {
            TASKDIALOGCONFIG cfg{};
            cfg.cbSize = sizeof(cfg);
            cfg.hwndParent = owner;
            cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
            cfg.pszWindowTitle = L"Agentmaster";
            cfg.pszMainIcon = TD_INFORMATION_ICON;
            cfg.pszMainInstruction = L"Where should Agentmaster live?";
            cfg.pszContent = content.c_str();
            cfg.cButtons = static_cast<UINT>(buttons.size());
            cfg.pButtons = buttons.data();
            cfg.nDefaultButton = defaultId;
            cfg.pszFooter = footer.c_str();
            cfg.pszFooterIcon = TD_INFORMATION_ICON;
            cfg.pfCallback = detail::ForegroundOnCreate;
            if (ex.package)
            {
                cfg.pszVerificationText = verification.c_str();
                if (!ex.packageRunning)
                {
                    cfg.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED; // "if we can, take over" — but never by default while it is live
                }
            }

            int pressed = 0;
            BOOL verified = FALSE;
            // Echoed to the command's console (DialogEcho.h): an unzip started from a shell asks here, every option listed.
            if (FAILED(DialogEcho::ShowTaskDialogIndirect(&cfg, &pressed, nullptr, &verified)))
            {
                return result; // comctl v6 unavailable / failure -> like "later"
            }
            result.takeOverPackage = ex.package && verified;
            switch (pressed)
            {
            case idForUser:
                result.choice = Choice::ForSessionUser;
                result.dir = exeDir;
                return result;
            case idFirst:
                result.choice = firstChoice;
                result.dir = firstDir;
                return result;
            case idMachine:
                result.choice = machineChoice;
                result.dir = machineDir;
                return result;
            case idBrowse:
            {
                std::wstring picked = Profiles::detail::BrowseForFolder(owner, L"Choose where to install Agentmaster");
                if (picked.empty())
                {
                    continue; // browse cancelled -> back to the question
                }
                // A non-empty folder that isn't already ours gets a subfolder, like any installer.
                if (!DirHoldsAppExe(picked) && Profiles::ProfileDirHasData(picked))
                {
                    picked += L"\\Agentmaster";
                }
                result.choice = Choice::InstallBrowse;
                result.dir = picked;
                return result;
            }
            case idHere:
                result.choice = Choice::InstallHere;
                result.dir = exeDir;
                return result;
            case idPortable:
                result.choice = Choice::StayPortable;
                result.dir = exeDir;
                return result;
            default:
                result.choice = Choice::Later; // idLater / Cancel / X / Esc
                return result;
            }
        }
    }

    // ============================ startup entry ============================

    enum class StartupAction
    {
        Continue, // proceed with the normal prelude (profile picker included)
        ContinueDeferProfile, // proceed, but the §2a profile picker is deferred too (nothing persisted)
        Exit, // handed off / installed and relaunched — the caller TerminateProcess-es
    };

    // Read every exe-side fact DecideStartup needs, for the folder `exeDir`.
    //   exeDir  — the folder holding the app exe.
    //   allowUi — whether a prompt may be shown at all.
    //   returns — the facts.
    //   throws  — std::bad_alloc only.
    inline StartupFacts GatherStartupFacts(const std::wstring& exeDir, bool allowUi)
    {
        StartupFacts f;
        f.allowUi = allowUi;
        f.portableMarker = Profiles::IsPortableInstallIn(exeDir);
        f.envProfileSet = !Profiles::detail::GetEnvVar(L"AGENTMASTER_PROFILE").empty();
        f.decision = ReadInstallDecisionIn(exeDir);
        f.profilePointerExists = Profiles::LocalProfilePointerExistsIn(exeDir);
        f.installedTargetExists = f.decision.kind == Decision::InstalledTo && DirHoldsAppExe(f.decision.dir);
        f.machineCopy = f.portableMarker && Profiles::IsMachineCopyIn(exeDir);
        // The hand-off is looked into only for a shared copy: every other launch skips the registry read.
        if (f.machineCopy)
        {
            InstallHandoff::HandoffFacts h;
            h.sharedCopy = true;
            h.allowed = allowUi; // a -Embedding activation and session 0 are served by this process itself
            h.optOut = InstallHandoff::OptedOut();
            f.userCopyDir = h.optOut ? std::wstring{} : InstallHandoff::UserCopyFor(exeDir, kAppExeLeaf);
            h.userCopyUsable = !f.userCopyDir.empty();
            f.handoffToUserCopy = InstallHandoff::ShouldHandOff(h);
        }
        return f;
    }

    // An error box of the install / uninstall flows (a Win32 TaskDialog: the prelude has no window yet).
    //   owner   — the dialog owner (nullptr in the prelude).
    //   text    — the message: what failed and what to do.
    //   heading — the main instruction; the install flows' default, the uninstall passes its own (a removal
    //             that "did not complete as an install" read wrong in a test desktop, 2026-10-21).
    //   throws  — nothing (a TaskDialog failure only means no box).
    inline void ShowError(HWND owner, const std::wstring& text, const wchar_t* heading = L"The install did not complete")
    {
        // Echoed to the command's console (DialogEcho.h): the error text reaches a caller that cannot see the screen.
        DialogEcho::ShowTaskDialog(owner, nullptr, L"Agentmaster", heading, text.c_str(), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
    }

    namespace detail
    {
        // The full path of the RUNNING exe ("" on failure) — what a "start this copy again as the
        // signed-in user" hop must start (the process image, not a guessed leaf: a pre-rename
        // WindowsTerminal.exe shim forwards to Agentmaster.exe, which is what runs this code).
        inline std::wstring SelfExePath()
        {
            wchar_t buf[MAX_PATH * 2];
            const DWORD n = ::GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
            if (n == 0 || n >= ARRAYSIZE(buf))
            {
                return {};
            }
            return std::wstring{ buf, n };
        }
    }

    // The prelude's first-launch install step (see the header comment).
    //   allowUi  — false for a `-Embedding` activation (and forced false in session 0): never prompt.
    //   aumidFor — the EXE computes the unpackaged AppUserModelID it would use for a given image path
    //              (the WindowEmperor formula: branding + exe-path hash + user-SID hash); "" if none.
    //   returns  — Continue / ContinueDeferProfile for the normal prelude, Exit when this process handed
    //              the launch to another one (a stub, an install, a start as the signed-in user): the
    //              caller then TerminateProcess-es.
    //   throws   — nothing (a crash inside is logged and reads as a plain portable run).
    inline StartupAction EnsureInstallDecidedAtStartup(bool allowUi, const std::function<std::wstring(const std::wstring&)>& aumidFor)
    {
        try
        {
            const std::wstring exeDir = Profiles::detail::ExeDirPath();
            if (exeDir.empty())
            {
                return StartupAction::Continue;
            }
            // Who runs this launch. Session 0 (a service) has no desktop a question could be shown on.
            const InstallIdentity who = DetectInstallIdentity();
            const AccountPlan account = DecideAccountPlan(who);
            if (account == AccountPlan::NoSessionUser)
            {
                allowUi = false;
            }
            const StartupFacts facts = GatherStartupFacts(exeDir, allowUi);
            // The shared (all-users) copy's per-user upkeep when it runs ITSELF: the user's own icons for it, and the
            // leftovers of a user's own install that was deleted by hand (MachineCopyUserUpkeep). Never for a launch
            // without UI (-Embedding, session 0): those are not the user starting Agentmaster.
            const auto machineCopyUpkeep = [&]() {
                if (facts.machineCopy && facts.allowUi)
                {
                    MachineCopyUserUpkeep(exeDir, aumidFor ? aumidFor(AppExeIn(exeDir)) : std::wstring{});
                }
            };
            switch (DecideStartup(facts))
            {
            case StartupPlan::Continue:
                machineCopyUpkeep();
                return StartupAction::Continue;
            case StartupPlan::ContinueDeferProfile:
                return StartupAction::ContinueDeferProfile;
            case StartupPlan::Handoff:
            {
                const std::wstring target = AppExeIn(facts.decision.dir);
                LogInstall(L"launcher stub " + exeDir + L" -> handing off to " + target);
                if (detail::LaunchApp(target, detail::CommandLineTail()))
                {
                    return StartupAction::Exit;
                }
                LogInstall(L"handoff FAILED to start " + target + L" \x2014 running from the unzip instead");
                return StartupAction::ContinueDeferProfile;
            }
            case StartupPlan::HandoffToUserCopy:
            {
                // The shared copy started by a user who has an install of their own (InstallHandoff.h): that copy runs
                // instead, with the same arguments — whatever started this one (the machine PATH in a terminal, a
                // Start pin, an old icon, a double-click in Program Files). The TOKEN is kept, as the launcher stub's
                // hand-off above keeps it: this redirects a start the user made, it installs nothing, so an elevated
                // start ("Run as administrator", an elevated terminal) gives the elevated start a launch of their own
                // copy would; dropping rights (startInstalled) would cost a one-shot scheduled task (~6 s) at EVERY
                // start on a PC with UAC off, where every administrator process is elevated and the hop changes nothing.
                const std::wstring target = FindAppExeIn(facts.userCopyDir);
                LogInstall(L"all-users copy " + exeDir + L" -> handing off to this user's own install " + target);
                if (!target.empty() && detail::LaunchApp(target, detail::CommandLineTail()))
                {
                    return StartupAction::Exit;
                }
                LogInstall(L"handoff FAILED to start " + target + L" \x2014 the all-users copy runs itself");
                machineCopyUpkeep();
                return StartupAction::Continue;
            }
            case StartupPlan::Prompt:
                break;
            }

            const ExistingInstalls ex = DetectExistingInstalls(exeDir);
            const Updater::Version ours = VersionStampIn(exeDir);
            const std::wstring tail = detail::CommandLineTail();
            // How an installed (or adopted) copy is started from here: WITHOUT administrator rights when
            // this process is the signed-in user running elevated — the app must not inherit rights it was
            // not meant to run with (every claude.exe it starts would run elevated) — directly otherwise.
            // A failed unelevated hop falls back to the direct start: a running app beats none.
            const auto startInstalled = [&](const std::wstring& exe, const std::wstring& args) -> bool {
                if (account == AccountPlan::AsIsElevated && StartAsSessionUser(exe, args))
                {
                    return true;
                }
                return detail::LaunchApp(exe, args);
            };
            for (;;)
            {
                const PromptResult pick = ShowInstallPrompt(nullptr, exeDir, ex, ours, who, account);
                switch (pick.choice)
                {
                case Choice::Later:
                    return StartupAction::ContinueDeferProfile;
                case Choice::StayPortable:
                    SaveInstallDecisionIn(exeDir, { Decision::Portable, {} });
                    return StartupAction::Continue;
                case Choice::ForSessionUser:
                {
                    // Another account runs this copy: start it again as the signed-in user, who then sees this
                    // same question in their own account. Nothing is installed or registered for the running
                    // account; only this install trace (LogInstall) lands in its own Default profile folder.
                    LogInstall(L"another account (" + who.runningUser + L") runs " + exeDir + L" \x2014 starting it again as " + who.sessionUser);
                    if (StartAsSessionUser(detail::SelfExePath(), tail))
                    {
                        return StartupAction::Exit;
                    }
                    ShowError(nullptr, L"Agentmaster could not be started as " + who.sessionUser + L". Its folder must be one that " + who.sessionUser +
                                           L" can open (for example their own Downloads folder):\n" + exeDir);
                    continue;
                }
                case Choice::UseExisting:
                case Choice::UseMachine:
                {
                    const std::wstring target = AppExeIn(pick.dir);
                    SaveInstallDecisionIn(exeDir, { Decision::InstalledTo, pick.dir });
                    LogInstall(L"unzip " + exeDir + L" (v" + FormatVersion(ours) + L") defers to the installed copy " + target +
                               (pick.choice == Choice::UseMachine ? L" (all users)" : L""));
                    // This copy is a launcher stub from here on: offer to delete it (the removal starts only once the
                    // installed copy runs — a failed start comes back to the question, which needs this folder).
                    const bool removeCopy = AskWhetherToRemoveLeftoverCopy(exeDir, pick.dir);
                    if (startInstalled(target, tail))
                    {
                        if (removeCopy)
                        {
                            ScheduleLeftoverCopyRemoval(exeDir);
                        }
                        return StartupAction::Exit;
                    }
                    ShowError(nullptr, L"The installed Agentmaster could not be started:\n" + target);
                    continue;
                }
                case Choice::InstallMachine:
                {
                    // The all-users install needs administrator rights: the shared install core runs it
                    // ELEVATED (a UAC prompt) and this unelevated process waits, then starts the installed
                    // copy itself — so the app ends up running without the rights the copy needed.
                    LogInstall(L"all-users install begin: " + exeDir + L" -> " + pick.dir + L" (as " + who.runningUser + L")");
                    const std::wstring args = L"-InstallMachine -SourceDir " + ScriptArg(exeDir) + L" -DestDir " + ScriptArg(pick.dir);
                    const ScriptRun run = RunEmbeddedScript(args, true, true, true);
                    if (!run.started)
                    {
                        if (!run.cancelled)
                        {
                            // Name what the user chose: updating the existing all-users install reads as an
                            // update (the question offered "Update the Agentmaster installed for all users"),
                            // a first install as an install (a test desktop's wording check, 2026-10-21).
                            const bool update = ex.machineHasExe && Profiles::detail::SamePath(pick.dir, ex.machineDir);
                            ShowError(nullptr,
                                      (update ? L"The update of Agentmaster for all users could not be started. " : L"The install for all users could not be started. ") +
                                          ScriptStartFailureText(run, true) +
                                          (update ? L"\n\nAsk an administrator to update it for all users, or install Agentmaster just for you: that needs no administrator rights." :
                                                    L"\n\nAsk an administrator to install it for all users, or install Agentmaster just for you: that needs no administrator rights."),
                                      update ? L"The update did not complete" : L"The install did not complete");
                        }
                        continue; // a declined UAC prompt simply returns to the question
                    }
                    if (run.exitCode != 0)
                    {
                        LogInstall(L"all-users install FAILED: exit " + std::to_wstring(run.exitCode));
                        ShowError(nullptr, L"The install for all users did not complete (exit code " + std::to_wstring(run.exitCode) + L"). Its window showed why.");
                        continue;
                    }
                    LogInstall(L"all-users install done: " + pick.dir);
                    if (pick.takeOverPackage && ex.package)
                    {
                        std::wstring err;
                        if (detail::RemoveInstalledPackage(ex.packageFullName, ex.packagePath, &err))
                        {
                            LogInstall(L"package removed after the all-users install: " + ex.packageFullName);
                        }
                        else
                        {
                            LogInstall(L"install note: the installed package was left in place: " + err);
                        }
                    }
                    SaveInstallDecisionIn(exeDir, { Decision::InstalledTo, pick.dir }); // this unzip -> a launcher stub
                    // The downloaded copy is of no use now: offer to delete it; the removal starts once the installed copy runs.
                    const bool removeCopy = AskWhetherToRemoveLeftoverCopy(exeDir, pick.dir);
                    if (!startInstalled(AppExeIn(pick.dir), {}))
                    {
                        ShowError(nullptr, L"Installed, but the new copy could not be started:\n" + AppExeIn(pick.dir) + L"\n\nStart it from the Start menu.");
                    }
                    else if (removeCopy)
                    {
                        ScheduleLeftoverCopyRemoval(exeDir);
                    }
                    return StartupAction::Exit;
                }
                case Choice::InstallHere:
                case Choice::InstallDefault:
                case Choice::InstallBrowse:
                {
                    InstallOptions o;
                    o.sourceDir = exeDir;
                    o.destDir = pick.dir;
                    o.profileDir = UserInstallProfileDir();
                    o.aumid = aumidFor ? aumidFor(AppExeIn(pick.dir)) : std::wstring{};
                    o.takeOverPackage = pick.takeOverPackage;
                    o.packageFullName = ex.packageFullName;
                    o.packagePath = ex.packagePath;
                    LogInstall(L"install begin: " + exeDir + L" -> " + pick.dir + (Profiles::detail::SamePath(exeDir, pick.dir) ? L" (in place)" : L"") +
                               (o.takeOverPackage ? L" + take over the package " + ex.packageFullName : L"") + L" (as " + who.runningUser +
                               (who.elevated ? L", elevated" : L"") + L")");
                    const InstallOutcome done = RunInstallWithProgress(nullptr, o);
                    for (const auto& n : done.notes)
                    {
                        LogInstall(L"install note: " + n);
                    }
                    if (!done.ok)
                    {
                        LogInstall(L"install FAILED: " + done.error);
                        ShowError(nullptr, done.error);
                        continue; // back to the question
                    }
                    LogInstall(L"install done: " + done.exe + (done.packageRemoved ? L" (package removed)" : L""));
                    if (Profiles::detail::SamePath(exeDir, pick.dir))
                    {
                        // In place: this very process carries on as the installed copy — unless it holds
                        // administrator rights the app must not keep: then an unelevated start takes over.
                        if (account == AccountPlan::AsIsElevated && StartAsSessionUser(detail::SelfExePath(), tail))
                        {
                            return StartupAction::Exit;
                        }
                        return StartupAction::Continue;
                    }
                    // Installed elsewhere: this unzip only forwards launches now — offer to delete it (the removal starts
                    // once the installed copy runs; the copy's own process must end first, its files are in use).
                    const bool removeCopy = AskWhetherToRemoveLeftoverCopy(exeDir, pick.dir);
                    if (!startInstalled(done.exe, {}))
                    {
                        ShowError(nullptr, L"Installed, but the new copy could not be started:\n" + done.exe + L"\n\nStart it from the Start menu.");
                    }
                    else if (removeCopy)
                    {
                        ScheduleLeftoverCopyRemoval(exeDir);
                    }
                    return StartupAction::Exit;
                }
                }
            }
        }
        catch (...)
        {
            try
            {
                LogInstall(L"startup install step CRASHED (exception) \x2014 continuing as a plain portable run");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return StartupAction::ContinueDeferProfile;
        }
    }

    // ============================ uninstall target ============================

    // What `--uninstall-portable` / the cog's Uninstall acts on for a folder (DecideUninstallTarget).
    enum class UninstallTarget
    {
        NotInstalled, // a package, a plain portable, a launcher stub, a build folder: nothing registered here
        UserInstall, // a per-user install (install.path = installed)
        MachineInstall, // the all-users install (install.path = machine), or an unwritable portable folder
    };

    // PURE: what an uninstall of `exeDir` means. Before 2026-10-21 the flag ran the portable
    // uninstaller on WHATEVER folder the exe sat in — on an MSIX package it stopped the running app
    // and then failed on the package folder's ACLs; on a kept-portable copy it deleted its binaries.
    //   portableMarker — `<exedir>\.portable` exists.
    //   decision       — the folder's install.path.
    //   machineCopy    — Profiles::IsMachineCopyIn(exeDir).
    //   returns        — the target (UninstallTarget).
    //   throws         — nothing.
    inline UninstallTarget DecideUninstallTarget(bool portableMarker, const InstallDecision& decision, bool machineCopy) noexcept
    {
        if (!portableMarker)
        {
            return UninstallTarget::NotInstalled; // a package (Settings -> Apps) or a plain build folder
        }
        if (decision.kind == Decision::Installed)
        {
            return UninstallTarget::UserInstall;
        }
        if (decision.kind == Decision::Machine || machineCopy)
        {
            return UninstallTarget::MachineInstall;
        }
        return UninstallTarget::NotInstalled; // pristine / kept portable / a launcher stub
    }

    // ============================ steady state: an installed copy ============================

    inline bool IsInstalledPortableIn(const std::wstring& exeDir)
    {
        return Profiles::IsPortableInstallIn(exeDir) && ReadInstallDecisionIn(exeDir).kind == Decision::Installed;
    }

    inline bool IsInstalledPortable()
    {
        return IsInstalledPortableIn(Profiles::detail::ExeDirPath());
    }

    // True when the RUNNING copy is an all-users install (or an unwritable portable folder) that an
    // uninstall can act on — the cog shows its Uninstall button for it (needs administrator rights).
    //   returns — DecideUninstallTarget(...) == MachineInstall for the running exe's folder.
    //   throws  — std::bad_alloc only.
    inline bool IsMachineInstall()
    {
        const std::wstring exeDir = Profiles::detail::ExeDirPath();
        return DecideUninstallTarget(Profiles::IsPortableInstallIn(exeDir), ReadInstallDecisionIn(exeDir), Profiles::IsMachineCopy()) == UninstallTarget::MachineInstall;
    }

    // After an in-place zip update the Apps & Features entry would still show the OLD version —
    // re-stamp the entry (and re-assert the right-click verbs' command paths) when they differ.
    // Cheap registry reads on every launch of an installed copy; writes only on a change. A per-user
    // install only: the all-users entry lives in HKLM, which the elevated update re-stamps itself.
    inline void RefreshInstalledRegistration()
    {
        try
        {
            const std::wstring exeDir = Profiles::detail::ExeDirPath();
            if (!IsInstalledPortableIn(exeDir))
            {
                return;
            }
            const std::wstring exe = AppExeIn(exeDir);
            const std::wstring version = FormatVersion(VersionStampIn(exeDir));
            const std::wstring loc = detail::RegReadString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
            const std::wstring shown = detail::RegReadString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion");
            if (!Profiles::detail::SamePath(loc, exeDir) || shown != version)
            {
                RegistrationOptions r;
                r.installDir = exeDir;
                r.exe = exe;
                r.versionText = version;
                r.desktopShortcut = false;
                r.startMenuShortcut = false;
                r.userPath = false;
                RegisterInstall(r, nullptr);
                LogInstall(L"registration refreshed: v" + version + L" at " + exeDir);
            }
        }
        catch (...)
        {
            // Best-effort steady-state upkeep (the registration stays as it was); the trace says so.
            LogInstall(L"registration refresh CRASHED (exception) \x2014 left as it was");
        }
    }

    // The user's OWN install, at every start: move the user's taskbar pins of the all-users copy over to THIS copy,
    // so the pin they already use opens their own copy and its window groups under that pin (the pin then carries
    // this copy's AppUserModelID). The EXE runs this right before it sets its own AppUserModelID (_setupAumid), which
    // then finds the pin already matching. Only the user's own REGISTERED install does it (the HKCU entry names this
    // folder). Start pins cannot be changed by an app: the shared copy hands those launches on instead
    // (InstallHandoff.h). Registry reads only, unless a pin targets the all-users copy.
    //   ourAumid — the AppUserModelID this copy runs under.
    //   returns  — the number of pins re-pointed.
    //   throws   — nothing.
    inline int RepointSharedCopyTaskbarPins(const std::wstring& ourAumid) noexcept
    {
        try
        {
            const std::wstring exeDir = Profiles::detail::ExeDirPath();
            if (exeDir.empty() || !IsInstalledPortableIn(exeDir) || !Profiles::detail::SamePath(InstallHandoff::ReadUserInstallDir(), exeDir))
            {
                return 0;
            }
            const std::wstring machineDir = MachineInstallDirFromRegistry();
            if (machineDir.empty() || Profiles::detail::SamePath(machineDir, exeDir))
            {
                return 0;
            }
            detail::ComScope com;
            const int n = RepointPinsIn(detail::TaskbarPinsDir(), machineDir, AppExeIn(exeDir), ourAumid);
            if (n > 0)
            {
                LogInstall(L"moved " + std::to_wstring(n) + L" taskbar pin(s) of the all-users copy (" + machineDir + L") to this user's own install " + exeDir);
            }
            return n;
        }
        catch (...)
        {
            try
            {
                LogInstall(L"moving the all-users copy's taskbar pins CRASHED (exception) \x2014 left as they were");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return 0;
        }
    }

    // ============================ uninstall ============================

    // Materialize the embedded am-update.ps1 into %TEMP% (NOT the install dir — it is about to be
    // deleted) + an am-uninstall-portable.cmd launcher, and run it DETACHED with -UninstallPortable.
    // Returns true if launched — the caller then exits so the binaries are free.
    inline bool LaunchPortableUninstaller(const std::wstring& installDir)
    {
        try
        {
            const std::wstring ps1 = Updater::InstallerPs1();
            if (ps1.empty() || installDir.empty())
            {
                LogInstall(L"uninstaller REFUSED (embedded script missing or no install dir)");
                return false;
            }
            std::wstring temp = Profiles::detail::GetEnvVar(L"TEMP");
            if (temp.empty())
            {
                temp = Profiles::detail::GetEnvVar(L"LOCALAPPDATA");
            }
            if (temp.empty())
            {
                return false;
            }
            const std::wstring dir = temp + L"\\Agentmaster-update";
            Profiles::detail::EnsureDirExists(dir);
            // The file is UTF-8 (WriteFileUtf8, no BOM), and cmd.exe decodes a batch file in the console's code
            // page: `chcp 65001` first, so a non-ASCII install path (a user name with Hebrew letters or an
            // umlaut) reaches PowerShell intact — cmd reads each later line again in the new code page.
            std::wstring cmd = Updater::kCmdUtf8Prologue;
            cmd += L"@echo off\r\ntitle Agentmaster Uninstall\r\n";
            cmd += L"powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0am-update.ps1\" -UninstallPortable";
            cmd += L" -PortableDir " + Updater::CmdArg(installDir);
            cmd += L" -WaitPid " + std::to_wstring(::GetCurrentProcessId());
            cmd += L"\r\n";
            const std::wstring ps1Path = dir + L"\\am-update.ps1";
            const std::wstring cmdPath = dir + L"\\am-uninstall-portable.cmd";
            if (!Updater::detail::WriteFileUtf8(ps1Path, ps1) || !Updater::detail::WriteFileUtf8(cmdPath, cmd))
            {
                LogInstall(L"uninstaller materialize FAILED in " + dir);
                return false;
            }
            const HINSTANCE h = ::ShellExecuteW(nullptr, L"open", cmdPath.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
            const bool launched = reinterpret_cast<INT_PTR>(h) > 32;
            LogInstall(launched ? L"uninstaller launched for " + installDir + L" \x2014 app exiting for removal" : L"uninstaller ShellExecute FAILED");
            return launched;
        }
        catch (...)
        {
            try
            {
                LogInstall(L"uninstaller launch CRASHED (exception \x2014 not launched)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return false;
        }
    }

    // The ALL-USERS uninstall: the embedded install core with -UninstallMachine, ELEVATED (a UAC
    // prompt), detached — it waits for this process to exit, then removes the machine-wide shortcuts,
    // verbs, Apps & Features entry, machine PATH entry and the shipped binaries; every user's own profile
    // data is kept.
    //   installDir — the all-users install folder.
    //   outcome    — optional: receives the launch's outcome (cancelled at the UAC prompt, or the error that
    //                kept it from starting), so a caller can stay quiet on a decline and explain a refusal.
    //   returns    — true when the elevated uninstaller started (the caller then exits so the binaries
    //                are free); false when the UAC prompt was declined or it could not start.
    //   throws     — nothing.
    inline bool LaunchMachineUninstaller(const std::wstring& installDir, ScriptRun* outcome = nullptr) noexcept
    {
        try
        {
            if (installDir.empty())
            {
                return false;
            }
            const std::wstring args = L"-UninstallMachine -PortableDir " + ScriptArg(installDir) + L" -WaitPid " + std::to_wstring(::GetCurrentProcessId());
            const ScriptRun run = RunEmbeddedScript(args, true, false, true);
            if (outcome)
            {
                *outcome = run;
            }
            LogInstall(run.started ? L"all-users uninstaller launched for " + installDir + L" \x2014 app exiting for removal" :
                                     (run.cancelled ? L"all-users uninstall cancelled at the UAC prompt" : L"all-users uninstaller could not be started"));
            return run.started;
        }
        catch (...)
        {
            LogInstall(L"all-users uninstaller launch CRASHED (exception \x2014 not launched)");
            return false;
        }
    }

    // `Agentmaster.exe --uninstall-portable` (the Apps & Features UninstallString): confirm, launch
    // the uninstaller, and report true so the caller exits WITHOUT starting the app. Runs BEFORE the
    // single-instance handoff (a running instance would otherwise receive the flag as a commandline).
    // Acts ONLY on an installed copy (DecideUninstallTarget); any other copy is told how it is removed.
    //   returns — true when the flag was present (handled, whatever the outcome — never start the app
    //             on it); false when the commandline does not carry the flag.
    //   throws  — nothing.
    inline bool HandleUninstallFlagAtStartup()
    {
        try
        {
            if (!Profiles::detail::CommandLineHasFlag(kUninstallFlag))
            {
                return false;
            }
            const std::wstring exeDir = Profiles::detail::ExeDirPath();
            const UninstallTarget target = DecideUninstallTarget(Profiles::IsPortableInstallIn(exeDir), ReadInstallDecisionIn(exeDir), Profiles::IsMachineCopyIn(exeDir));
            if (target == UninstallTarget::NotInstalled)
            {
                const std::wstring how = Profiles::IsPortableInstallIn(exeDir) ?
                                             L"This copy of Agentmaster is not installed: nothing is registered for it. To remove it, close Agentmaster and delete its folder:\n    " + exeDir :
                                             L"This Agentmaster is not a portable install. If it is the app package, remove it from Settings \x2192 Apps \x2192 Installed apps (or in Agentmaster: the Home button \x2192 \x2699 Settings \x2192 About \x2192 Uninstall).";
                // Echoed to the command's console (DialogEcho.h): `Agentmaster.exe --uninstall-portable` from a shell.
                DialogEcho::ShowTaskDialog(nullptr, nullptr, L"Agentmaster", L"Nothing to uninstall here", how.c_str(), TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
                LogInstall(L"--uninstall-portable on " + exeDir + L": not an installed copy \x2014 nothing done");
                return true;
            }
            const bool machine = target == UninstallTarget::MachineInstall;
            std::wstring profile = Profiles::ReadLocalProfilePointerIn(exeDir);
            if (profile.empty())
            {
                profile = Profiles::DefaultProfileDir();
            }
            // A user's own copy next to an all-users one hands its icons and taskbar pins back to the shared copy
            // instead of deleting them (the PowerShell core's Move-AmUserEntriesToMachineCopy): the confirm says so.
            const std::wstring sharedDir = machine ? std::wstring{} : MachineInstallDirFromRegistry();
            const std::wstring content =
                L"This removes Agentmaster" + std::wstring{ machine ? L" (installed for all users of this PC)" : L"" } + L" from\n    " + exeDir +
                (machine ? std::wstring{ L"\n\nGone: its files, the Agentmaster icons it gave each user, the \x201COpen in Agentmaster\x201D right-click entry, the Apps & Features entry and the PATH entry." } :
                 !sharedDir.empty() ?
                           L"\n\nGone: its files, the \x201COpen in Agentmaster\x201D right-click entry, the Apps & Features entry and the PATH entry. Your Agentmaster icons and taskbar pins open the copy installed for all users instead:\n    " + sharedDir :
                           std::wstring{ L"\n\nGone: its files, the desktop and Start-menu shortcuts, the \x201COpen in Agentmaster\x201D right-click entry, the Apps & Features entry and the PATH entry." }) +
                (machine ? std::wstring{ L"\n\nKept: every user's own data (sessions, settings, window layouts in their profile folder, such as\n    " } + profile + L")." +
                               L"\n\nThis needs administrator rights." :
                           std::wstring{ L"\n\nKept: your data \x2014 sessions, settings, window layouts \x2014 in the profile folder\n    " } + profile + L".") +
                L"\n\nAgentmaster closes to finish.";
            const TASKDIALOG_BUTTON buttons[] = { { IDOK, L"Uninstall" }, { IDCANCEL, L"Cancel" } };
            TASKDIALOGCONFIG cfg{};
            cfg.cbSize = sizeof(cfg);
            cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
            cfg.pszWindowTitle = L"Agentmaster";
            cfg.pszMainIcon = TD_WARNING_ICON;
            cfg.pszMainInstruction = L"Uninstall Agentmaster?";
            cfg.pszContent = content.c_str();
            cfg.cButtons = 2;
            cfg.pButtons = buttons;
            cfg.nDefaultButton = IDCANCEL;
            cfg.pfCallback = detail::ForegroundOnCreate;
            int pressed = 0;
            // Echoed to the command's console (DialogEcho.h), with the answer.
            if (SUCCEEDED(DialogEcho::ShowTaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)) && pressed == IDOK)
            {
                ScriptRun machineRun;
                const bool launched = machine ? LaunchMachineUninstaller(exeDir, &machineRun) : LaunchPortableUninstaller(exeDir);
                // A declined UAC prompt is the user's own "no": nothing to explain. Anything else says why.
                if (!launched && !(machine && machineRun.cancelled))
                {
                    ShowError(nullptr,
                              machine ? L"The uninstaller did not start. " + ScriptStartFailureText(machineRun, true) + L"\n\nAsk an administrator to remove:\n" + exeDir :
                                        L"The uninstaller could not be started. Delete the folder by hand:\n" + exeDir,
                              L"Agentmaster was not uninstalled");
                }
            }
            return true; // the flag was handled either way — never start the app on it
        }
        catch (...)
        {
            LogInstall(L"--uninstall-portable handling CRASHED (exception) \x2014 the app is not started");
            return true;
        }
    }
}
