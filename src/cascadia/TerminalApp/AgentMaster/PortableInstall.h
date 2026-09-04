// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the PORTABLE copy's first-launch INSTALL choice (header-only, pure Win32; no WinRT,
// no engine deps — the ProfileBootstrap.h / Updater.h idiom, included by the WindowsTerminal EXE
// prelude, the Settings cog's Uninstall button, and the standalone test harness).
//
// WHAT: the very first launch of a portable copy — BEFORE anything is written anywhere — asks
// "Where should Agentmaster live?":
//   * Install to your user folder    %USERPROFILE%\.agentmaster\bin   (the default, Enter)
//   * Install to a folder you pick   (Browse…; lands in <picked>\Agentmaster unless empty/ours)
//   * Install here                   the folder holding the RUNNING binary (never the cwd)
//   * Keep it portable               run from here, no shortcuts / registry — never asks again
//   * Ask me later                   run from here now, nothing written, ask again next launch
// "Install" is an MSI-like per-user install WITHOUT an MSI: copy the binaries (or register them in
// place), a DESKTOP shortcut, a START-MENU shortcut (Start search finds it; it carries the
// unpackaged AUMID so toasts route), an "Open in Agentmaster" RIGHT-CLICK menu on folders / folder
// backgrounds / drives (the classic registry verb — what the MSIX shell extension provides for a
// packaged install, migrated to HKCU\Software\Classes), an APPS & FEATURES entry (with a working
// Uninstall), and the install dir on the USER PATH (agentmaster-cli.exe reachable from any shell).
// No administrator rights, no package registration, no certificate.
//
// EXISTING INSTALLS are detected, never overridden, and taken over where we can:
//   * an installed Agentmaster PACKAGE (the MSIX release family, possibly admin-trusted) — its
//     files can't and won't be touched; the install adopts its DATA (the same Default profile
//     ~/.agentmaster), and a checkbox offers removing the package afterwards (per-user removal,
//     no admin; unchecked by default while it is running, since it would be closed).
//   * a MACHINE-wide install (an HKLM uninstall entry, administrator-managed) — left alone; this
//     copy installs for the current user only, and the prompt says so.
//   * a previous USER install of ours (the HKCU uninstall entry) — the first option becomes
//     "Update the installed copy (vX → vY)" (an in-place upgrade preserving its state) or, when
//     it is already the same/newer version, "Use the installed copy" (this unzip then just hands
//     off to it and becomes a launcher stub).
//
// THE DECISION FILE `<exedir>\install.path` (plain UTF-8, `#` comments, first value line wins —
// the profile.path idiom):
//   installed          this folder IS the install (registered) — in-place install, or the copy
//                      an install produced (the destination is written with this).
//   portable           the user chose to stay portable — never ask again.
//   <absolute dir>     this unzip was INSTALLED TO <dir>: it is now a launcher stub that hands
//                      every launch off to <dir>\Agentmaster.exe (a vanished target re-asks).
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
// cog's Uninstall button) confirms, then materializes the embedded am-update.ps1 into %TEMP% and
// runs it detached with -UninstallPortable: shortcuts, the context menu, the Apps & Features entry,
// the PATH entry and the binaries go; profile data (the profile folder) is kept.
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

namespace Agentmaster::PortableInstall
{
    // ============================ constants ============================

    inline constexpr std::wstring_view kDecisionLeaf{ L"install.path" };
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

    // PURE: the file's value line for a decision. `installed` / `portable` / the absolute dir.
    inline std::wstring EncodeInstallDecision(const InstallDecision& d)
    {
        switch (d.kind)
        {
        case Decision::Installed:
            return L"installed";
        case Decision::Portable:
            return L"portable";
        case Decision::InstalledTo:
            return d.dir;
        default:
            return {};
        }
    }

    // PURE: file text -> decision. First non-empty non-`#` line wins; `installed` / `portable`
    // (case-insensitive) are the keywords, an ABSOLUTE path is an InstalledTo target, anything
    // else (a relative path, garbage) reads as None so a corrupt file simply re-asks.
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
        bool allowUi{ false };
    };

    enum class StartupPlan
    {
        Continue, // nothing to do here — the normal prelude follows (incl. the §2a profile picker)
        ContinueDeferProfile, // pristine + no UI (or "later"): run self-contained, persist nothing, ask next time
        Handoff, // a launcher stub: start the installed copy, exit
        Prompt, // pristine + UI: ask
    };

    inline StartupPlan DecideStartup(const StartupFacts& f)
    {
        if (!f.portableMarker || f.envProfileSet)
        {
            return StartupPlan::Continue;
        }
        switch (f.decision.kind)
        {
        case Decision::Installed:
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
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            return true;
        }

        // TaskDialog callback that FOREGROUNDS the dialog on creation: the prelude has no window
        // yet, so nothing of ours can be in front, but Explorer keeps focus after a double-click and
        // an un-foregrounded first-launch question was measured to sit unnoticed for minutes (the
        // MessageBox reopen prompt had the same fix: MB_SETFOREGROUND|MB_TOPMOST).
        inline HRESULT CALLBACK ForegroundOnCreate(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR)
        {
            if (msg == TDN_CREATED)
            {
                ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                ::SetForegroundWindow(hwnd);
            }
            return S_OK;
        }
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
        // A MACHINE-wide entry (HKLM — administrator-managed; read-only for us).
        std::wstring machineDir;
        std::wstring machineVersionText;
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
            // ---- a machine-wide entry (read-only for us) ----
            const std::wstring mloc = detail::RegReadString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation");
            if (!mloc.empty())
            {
                ex.machineDir = mloc;
                ex.machineVersionText = detail::RegReadString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion");
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
        // Copy `src` into `dst` (recursive; top-level entries filtered by IsCopyExcludedLeaf; files
        // overwritten). Returns bytes copied; throws std::runtime_error-free — errors land in *err.
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
                bytes += top.file_size(ec);
                if ((++files % 25) == 0)
                {
                    status(L"Copying files\x2026 " + std::to_wstring(bytes / (1024 * 1024)) + L" MB");
                }
            }
            return true;
        }

        // An UPGRADE of an existing install at dst: remove only what WE SHIP (a top-level entry that
        // also exists in the source layout) and never the preserved user state — a folder the user
        // shares with other files keeps them (the scripts' wipe-everything rule is not safe here).
        inline void RemoveShippedEntries(const std::filesystem::path& src, const std::filesystem::path& dst)
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator{ dst, ec })
            {
                const std::wstring leaf = entry.path().filename().wstring();
                if (IsPreservedLeaf(leaf))
                {
                    continue;
                }
                if (Profiles::detail::NormPathKey(leaf) == L".am-version" || std::filesystem::exists(src / leaf, ec))
                {
                    std::filesystem::remove_all(entry.path(), ec);
                }
            }
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
                // In place: size the folder for Apps & Features.
                std::error_code ec;
                for (auto it = std::filesystem::recursive_directory_iterator{ src, ec }; !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec))
                {
                    if (it->is_regular_file(ec))
                    {
                        bytes += it->file_size(ec);
                    }
                }
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
    };

    struct PromptResult
    {
        Choice choice{ Choice::Later };
        std::wstring dir;
        bool takeOverPackage{ false };
    };

    inline PromptResult ShowInstallPrompt(HWND owner, const std::wstring& exeDir, const ExistingInstalls& ex, const Updater::Version& ours)
    {
        PromptResult result;
        const std::wstring defaultDir = DefaultUserInstallDir();
        const std::wstring oursText = FormatVersion(ours);

        // ---- the first option adapts to a detected user install of ours ----
        std::wstring firstLabel;
        Choice firstChoice = Choice::InstallDefault;
        std::wstring firstDir = defaultDir;
        if (!ex.userDir.empty())
        {
            firstDir = ex.userDir;
            const std::wstring theirs = FormatVersion(ex.userVersion);
            if (Updater::CompareVersion(ours, ex.userVersion) > 0)
            {
                firstLabel = L"Update the installed Agentmaster (v" + theirs + L" \x2192 v" + oursText + L")\n" + ex.userDir + L"  \x2014  keeps its settings and shortcuts";
            }
            else
            {
                firstChoice = Choice::UseExisting;
                firstLabel = L"Use the installed Agentmaster (v" + theirs + L")\n" + ex.userDir + L"  \x2014  already this version or newer; this copy just launches it";
            }
        }
        else
        {
            firstLabel = L"Install to your user folder\n" + defaultDir;
            if (DirHoldsAppExe(defaultDir))
            {
                firstLabel += L"  \x2014  existing files (v" + FormatVersion(VersionStampIn(defaultDir)) + L")";
            }
        }
        const std::wstring browseLabel = L"Install to a folder you pick\x2026\nBrowse; lands in <folder>\\Agentmaster unless the folder is empty";
        const std::wstring hereLabel = L"Install here\n" + exeDir + L"  \x2014  no copy; adds the shortcuts, right-click menu and Apps & Features entry";
        const std::wstring portableLabel = L"Keep it portable, run from here\n" + exeDir + L"  \x2014  no shortcuts or registry entries; never asks again";
        const std::wstring laterLabel = L"Ask me later\nRun from here for now; nothing is written, this question returns next launch";

        std::wstring content =
            L"This is a portable copy of Agentmaster v" + oursText + L", running from:\n    " + exeDir +
            L"\n\nInstalling gives it a home like a regular app \x2014 a desktop icon, a Start-menu entry (Start search finds it), "
            L"an \x201COpen in Agentmaster\x201D entry on the folder right-click menu, an Apps & Features entry with Uninstall, "
            L"and its folder on your PATH. No administrator rights, no package. "
            L"An installed copy keeps its data in the Default profile folder (" +
            Profiles::DefaultProfileDir() + L") and updates itself in place.";
        if (ex.package)
        {
            content += L"\n\nAn installed Agentmaster package v" + FormatVersion(ex.packageVersion) +
                       L" already exists on this PC. It is left untouched; an installed copy of this portable shares its data (the same Default profile). "
                       L"Tick the box below to remove the package afterwards and let this copy take over.";
            if (ex.packageRunning)
            {
                content += L" It is RUNNING right now \x2014 removing it closes its windows.";
            }
        }
        if (!ex.machineDir.empty())
        {
            content += L"\n\nA machine-wide Agentmaster install exists at " + ex.machineDir +
                       (ex.machineVersionText.empty() ? std::wstring{} : L" (v" + ex.machineVersionText + L")") +
                       L" \x2014 administrator-managed, left untouched. This copy installs for your user account only.";
        }
        const std::wstring verification = L"Also remove the installed Agentmaster package v" + FormatVersion(ex.packageVersion) + L" (its data in " + Profiles::DefaultProfileDir() + L" is kept and reused)";
        const std::wstring footer = L"Uninstall later from Settings \x2192 Apps, or the Manager tab \x2192 \x2699 Settings \x2192 About.";

        constexpr int idFirst = 1101, idBrowse = 1102, idHere = 1103, idPortable = 1104, idLater = 1105;
        const TASKDIALOG_BUTTON buttons[] = {
            { idFirst, firstLabel.c_str() },
            { idBrowse, browseLabel.c_str() },
            { idHere, hereLabel.c_str() },
            { idPortable, portableLabel.c_str() },
            { idLater, laterLabel.c_str() },
        };

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
            cfg.cButtons = static_cast<UINT>(std::size(buttons));
            cfg.pButtons = buttons;
            cfg.nDefaultButton = idFirst;
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
            case idFirst:
                result.choice = firstChoice;
                result.dir = firstDir;
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

    inline StartupFacts GatherStartupFacts(const std::wstring& exeDir, bool allowUi)
    {
        StartupFacts f;
        f.allowUi = allowUi;
        f.portableMarker = Profiles::IsPortableInstallIn(exeDir);
        f.envProfileSet = !Profiles::detail::GetEnvVar(L"AGENTMASTER_PROFILE").empty();
        f.decision = ReadInstallDecisionIn(exeDir);
        f.profilePointerExists = Profiles::LocalProfilePointerExistsIn(exeDir);
        f.installedTargetExists = f.decision.kind == Decision::InstalledTo && DirHoldsAppExe(f.decision.dir);
        return f;
    }

    inline void ShowError(HWND owner, const std::wstring& text)
    {
        ::TaskDialog(owner, nullptr, L"Agentmaster", L"The install did not complete", text.c_str(), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
    }

    // `aumidFor(exePath)` — the EXE computes the unpackaged AppUserModelID it would use for a given
    // image path (the WindowEmperor formula: branding + exe-path hash + user-SID hash); "" if none.
    inline StartupAction EnsureInstallDecidedAtStartup(bool allowUi, const std::function<std::wstring(const std::wstring&)>& aumidFor)
    {
        try
        {
            const std::wstring exeDir = Profiles::detail::ExeDirPath();
            if (exeDir.empty())
            {
                return StartupAction::Continue;
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
            for (;;)
            {
                const PromptResult pick = ShowInstallPrompt(nullptr, exeDir, ex, ours);
                switch (pick.choice)
                {
                case Choice::Later:
                    return StartupAction::ContinueDeferProfile;
                case Choice::StayPortable:
                    SaveInstallDecisionIn(exeDir, { Decision::Portable, {} });
                    return StartupAction::Continue;
                case Choice::UseExisting:
                {
                    const std::wstring target = AppExeIn(pick.dir);
                    SaveInstallDecisionIn(exeDir, { Decision::InstalledTo, pick.dir });
                    LogInstall(L"unzip " + exeDir + L" (v" + FormatVersion(ours) + L") defers to the installed copy " + target);
                    if (detail::LaunchApp(target, detail::CommandLineTail()))
                    {
                        return StartupAction::Exit;
                    }
                    ShowError(nullptr, L"The installed Agentmaster could not be started:\n" + target);
                    continue;
                }
                case Choice::InstallHere:
                case Choice::InstallDefault:
                case Choice::InstallBrowse:
                {
                    InstallOptions o;
                    o.sourceDir = exeDir;
                    o.destDir = pick.dir;
                    o.profileDir = Profiles::DefaultProfileDir();
                    o.aumid = aumidFor ? aumidFor(AppExeIn(pick.dir)) : std::wstring{};
                    o.takeOverPackage = pick.takeOverPackage;
                    o.packageFullName = ex.packageFullName;
                    o.packagePath = ex.packagePath;
                    LogInstall(L"install begin: " + exeDir + L" -> " + pick.dir + (Profiles::detail::SamePath(exeDir, pick.dir) ? L" (in place)" : L"") + (o.takeOverPackage ? L" + take over the package " + ex.packageFullName : L""));
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
                        return StartupAction::Continue; // in place: this very process carries on as the installed copy
                    }
                    if (!detail::LaunchApp(done.exe, {}))
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

    // ============================ steady state: an installed copy ============================

    inline bool IsInstalledPortableIn(const std::wstring& exeDir)
    {
        return Profiles::IsPortableInstallIn(exeDir) && ReadInstallDecisionIn(exeDir).kind == Decision::Installed;
    }

    inline bool IsInstalledPortable()
    {
        return IsInstalledPortableIn(Profiles::detail::ExeDirPath());
    }

    // After an in-place zip update the Apps & Features entry would still show the OLD version —
    // re-stamp the entry (and re-assert the right-click verbs' command paths) when they differ.
    // Cheap registry reads on every launch of an installed copy; writes only on a change.
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
            // best-effort steady-state upkeep
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

    // `Agentmaster.exe --uninstall-portable` (the Apps & Features UninstallString): confirm, launch
    // the uninstaller, and report true so the caller exits WITHOUT starting the app. Runs BEFORE the
    // single-instance handoff (a running instance would otherwise receive the flag as a commandline).
    inline bool HandleUninstallFlagAtStartup()
    {
        try
        {
            if (!Profiles::detail::CommandLineHasFlag(kUninstallFlag))
            {
                return false;
            }
            const std::wstring exeDir = Profiles::detail::ExeDirPath();
            std::wstring profile = Profiles::ReadLocalProfilePointerIn(exeDir);
            if (profile.empty())
            {
                profile = Profiles::IsPortableInstallIn(exeDir) ? Profiles::PortableDefaultProfileDirIn(exeDir) : Profiles::DefaultProfileDir();
            }
            const std::wstring content =
                L"This removes Agentmaster from\n    " + exeDir +
                L"\n\nGone: its files, the desktop and Start-menu shortcuts, the \x201COpen in Agentmaster\x201D right-click entry, the Apps & Features entry and the PATH entry."
                L"\n\nKept: your data \x2014 sessions, settings, window layouts \x2014 in the profile folder\n    " +
                profile + L"\n\nAgentmaster closes to finish.";
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
                if (!LaunchPortableUninstaller(exeDir))
                {
                    ShowError(nullptr, L"The uninstaller could not be started. Delete the folder by hand:\n" + exeDir);
                }
            }
            return true; // the flag was handled either way — never start the app on it
        }
        catch (...)
        {
            return true;
        }
    }
}
