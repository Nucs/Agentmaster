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
// shape one level up (HKLM, the common Start menu + public desktop, the machine PATH), done by the
// shared PowerShell install core that tools\Install-Agentmaster.ps1 uses too.
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
    inline constexpr const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Agentmaster";
    inline constexpr const wchar_t* kContextVerb = L"Agentmaster";
    inline constexpr const wchar_t* kContextLabel = L"Open in Agentmaster";
    inline constexpr const wchar_t* kEnvironmentKey = L"Environment";

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

    // Entries a COPY-install never carries from the source: the source's own profile data (it is
    // MIGRATED into the destination's profile instead), its pointer/decision files (the
    // destination gets its own), and temp leftovers.
    inline bool IsCopyExcludedLeaf(std::wstring_view leaf)
    {
        const auto k = Profiles::detail::NormPathKey(leaf);
        if (k == L"profile" || k == L"profile.path" || k == L"install.path")
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
    };

    enum class StartupPlan
    {
        Continue, // nothing to do here — the normal prelude follows (incl. the §2a profile picker)
        ContinueDeferProfile, // pristine + no UI (or "later"): run self-contained, persist nothing, ask next time
        Handoff, // a launcher stub: start the installed copy, exit
        Prompt, // pristine + UI: ask
    };

    // PURE: the prelude's plan for this launch.
    //   f       — the gathered facts (GatherStartupFacts).
    //   returns — Continue for a non-portable / overridden / decided / machine copy; Handoff for a
    //             launcher stub whose install exists; Prompt for a pristine copy with UI;
    //             ContinueDeferProfile for a pristine copy without UI.
    //   throws  — nothing.
    inline StartupPlan DecideStartup(const StartupFacts& f)
    {
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
    inline void LogInstall(const std::wstring& msg)
    {
        try
        {
            const std::wstring dir = Profiles::DefaultProfileDir();
            if (dir.empty())
            {
                return;
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
        // A previous USER install of ours (HKCU Apps & Features entry with a live app exe).
        std::wstring userDir;
        Updater::Version userVersion;
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

    inline ExistingInstalls DetectExistingInstalls()
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
            // ---- a machine-wide entry (the all-users install; read-only for a normal user) ----
            const std::wstring mloc = detail::RegReadString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation");
            if (!mloc.empty())
            {
                ex.machineDir = mloc;
                ex.machineVersionText = detail::RegReadString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion");
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
            if (!::ShellExecuteExW(&sei))
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

        // An UPGRADE of an existing install at dst: remove only what WE SHIP — a top-level entry the
        // NEW source layout also has, or one the OLD install's manifest lists (a file a newer version
        // no longer ships must not linger) — and never the preserved user state. An old install WITHOUT
        // a manifest (a zip built before 2026-10-21) is read through the legacy list instead
        // (IsLegacyShippedLeaf), exactly as the PowerShell core's Get-AmShippedNames reads it: until
        // then this upgrade left a pre-manifest release's binaries behind (seen in a test desktop on
        // 2026-10-21, v0.7.10 -> a new layout). A folder the user shares with other files keeps them.
        //   src — the new layout (the running unzip).
        //   dst — the existing install.
        //   throws — std::bad_alloc only (filesystem errors are absorbed per entry).
        inline void RemoveShippedEntries(const std::filesystem::path& src, const std::filesystem::path& dst)
        {
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
                if (IsPreservedLeaf(leaf))
                {
                    continue;
                }
                if (Profiles::detail::NormPathKey(leaf) == L".am-version" || std::filesystem::exists(src / leaf, ec) || listedInOld(leaf))
                {
                    std::filesystem::remove_all(entry.path(), ec);
                }
            }
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

    // The install proper. Runs on a worker thread under the progress dialog; `status` receives
    // the phase text. No-throw.
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
                const bool existing = DirHoldsAppExe(o.destDir);
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
                    status(L"Removing the previous version\x2026");
                    detail::RemoveShippedEntries(src, dst);
                }
                status(L"Copying files\x2026");
                std::wstring err;
                if (!detail::CopyTree(src, dst, status, bytes, &err))
                {
                    out.error = err.empty() ? L"Copying the files failed." : err;
                    return out;
                }
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
                if (Profiles::ReadLocalProfilePointerIn(o.destDir).empty())
                {
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
        if (FAILED(::TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr)))
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
            if (Updater::CompareVersion(ours, ex.userVersion) > 0)
            {
                firstLabel = L"Update the installed Agentmaster (v" + theirs + L" \x2192 v" + oursText + L")" + recommended + L"\n" + ex.userDir + L"  \x2014  keeps its settings and shortcuts";
            }
            else
            {
                firstChoice = Choice::UseExisting;
                firstLabel = L"Use the installed Agentmaster (v" + theirs + L")" + recommended + L"\n" + ex.userDir + L"  \x2014  already this version or newer; this copy just launches it";
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
            machineLabel = L"Install for all users of this PC\n" + machineDir + L"  \x2014  needs administrator rights; every user keeps their own data";
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
            if (FAILED(::TaskDialogIndirect(&cfg, &pressed, nullptr, &verified)))
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
        ::TaskDialog(owner, nullptr, L"Agentmaster", heading, text.c_str(), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
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
            switch (DecideStartup(facts))
            {
            case StartupPlan::Continue:
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
            case StartupPlan::Prompt:
                break;
            }

            const ExistingInstalls ex = DetectExistingInstalls();
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
                    if (startInstalled(target, tail))
                    {
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
                    if (!startInstalled(AppExeIn(pick.dir), {}))
                    {
                        ShowError(nullptr, L"Installed, but the new copy could not be started:\n" + AppExeIn(pick.dir) + L"\n\nStart it from the Start menu.");
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
                    if (!startInstalled(done.exe, {}))
                    {
                        ShowError(nullptr, L"Installed, but the new copy could not be started:\n" + done.exe + L"\n\nStart it from the Start menu.");
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
            std::wstring cmd = L"@echo off\r\ntitle Agentmaster Uninstall\r\n";
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
                ::TaskDialog(nullptr, nullptr, L"Agentmaster", L"Nothing to uninstall here", how.c_str(), TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
                LogInstall(L"--uninstall-portable on " + exeDir + L": not an installed copy \x2014 nothing done");
                return true;
            }
            const bool machine = target == UninstallTarget::MachineInstall;
            std::wstring profile = Profiles::ReadLocalProfilePointerIn(exeDir);
            if (profile.empty())
            {
                profile = Profiles::DefaultProfileDir();
            }
            const std::wstring content =
                L"This removes Agentmaster" + std::wstring{ machine ? L" (installed for all users of this PC)" : L"" } + L" from\n    " + exeDir +
                L"\n\nGone: its files, the desktop and Start-menu shortcuts, the \x201COpen in Agentmaster\x201D right-click entry, the Apps & Features entry and the PATH entry." +
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
            if (SUCCEEDED(::TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)) && pressed == IDOK)
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
