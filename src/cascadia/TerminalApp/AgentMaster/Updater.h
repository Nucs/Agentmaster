// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the in-app auto-updater (header-only, plain Win32; no WinRT, no engine-lib deps,
// exactly like ProfileBootstrap.h). It checks the GitHub Releases of Nucs/Agentmaster for a newer
// version, prompts the user (Update now / Postpone [next restart · tomorrow 08:00 · 3·7·30 days · skip])
// with a Win32 TaskDialog, and — on Update — materializes the installer script into the active
// profile dir (am-update.ps1 + a tiny am-update.cmd launcher) and runs it detached to download +
// cert-trust + Add-AppxPackage the new .msixbundle and relaunch. The script (am-update.ps1) is the
// REAL file at src/cascadia/TerminalApp/AgentMaster/am-update.ps1, compiled into WindowsTerminal.exe
// as the AM_UPDATE_PS1 RT_RCDATA resource and READ FROM THE BINARY here (FindResource/LoadResource)
// — never fetched from GitHub, and never read/copied/opened as a loose file on disk. The same script
// also performs an UNINSTALL (LaunchUninstaller -> am-update.ps1 -Uninstall). It mirrors
// tools\Install-Agentmaster.ps1's install core + recovery (VCLibs dependency; removing a conflicting
// "already installed"/unpackaged registration that blocks deployment, 0x80073CFB).
//
// Included by BOTH the WindowsTerminal EXE (the startup pre-restoration check — it asks BEFORE the
// "Reopen your N windows?" prompt) AND TerminalApp.dll's Settings cog ("Check for updates" button +
// the "vX.Y.Z available!" label). That dual use is WHY this is header-only + pure Win32: the EXE
// links TerminalApp.dll, not the TerminalAppLib static lib, so it cannot call the engine's JSON /
// settings helpers — but it CAN include this header (and Json.h, which is itself header-only) just
// as it includes ProfileBootstrap.h.
//
// Persisted update state lives as four keys INSIDE settings.json (the active profile) — and
// settings.json is the engine's ENVELOPE `{version: 1, settings: {...}}` (Persistence.cpp
// SerializeAppSettings), so the keys live NESTED under "settings", never at the top level (a
// top-level key is silently DROPPED by the next engine save, which rebuilds the envelope from the
// AppSettings struct — the original schema-mismatch bug that blinded the startup/hourly checks):
//   allowUpdatePrerelease (bool)        — the cog's "Allow updating to pre-release versions" toggle
//                                         (INSTANT-APPLY: the switch itself RMWs it on flip — no Save).
//   allowUpdateNightly (bool)           — the cog's NIGHTLY opt-in (warning-gated). A nightly is an
//                                         unstable development build whose tag CONTAINS "nightly"
//                                         (e.g. v0.6.10-prerelease-nightly, published as a GitHub
//                                         prerelease); it is ALWAYS skipped — even with the
//                                         pre-release toggle on — unless this is set (IsNightlyTag /
//                                         ReleaseAllowedOnChannel). Same INSTANT-APPLY RMW.
//   updateSkippedVersion (string tag)   — "Skip this version" -> never re-prompt for that exact tag.
//   updatePostponedUntilUnixMs (number) — "Postpone N days" -> no check / no prompt until this time.
// skip/postpone are written by THIS module via a freshest-disk JSON read-modify-write INTO the
// envelope (so the EXE can write them too without linking the engine); AppSettings carries all four
// fields, so the engine round-trips them and the cog's Save preserves them from disk (the
// summaryPanel idiom). "Remind me next restart" persists NOTHING durable — it latches a process-scoped
// declined-this-run marker (kDeclinedEnvVar) that silences the startup + hourly checks ENTIRELY
// (pre-network presence gate: no query, no prompt, even for a newer release published mid-run)
// until the NEXT LAUNCH, which always asks again (RunStartupUpdateCheck clears an inherited latch);
// the cog's explicit "Check for updates" stays fully live. Every check/prompt/decision logs an
// "[update]" line to the profile's hooks.log (LogUpdate — the EXE-safe twin of the engine's
// AppendStateLog), and NO catch swallows silently: every guard either logs, or is itself the
// logger / a logger-failed nested catch (each annotated as such at the site).

#pragma once

#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>
#include <shellapi.h>
#include <appmodel.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Json.h"
#include "ProfileBootstrap.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace Agentmaster::Updater
{
    // The published RELEASE identity (stable: the PFN hash derives from Publisher CN=Agentmaster;
    // mirrors Install-Agentmaster.ps1's constants). ONLY the release family is ever published to
    // GitHub, so the updater always installs + relaunches the release package — a dev install that
    // runs the check therefore "graduates" to the release. The repo + API host are constants too.
    inline constexpr const wchar_t* kRepo = L"Nucs/Agentmaster";
    inline constexpr const wchar_t* kApiHost = L"api.github.com";
    inline constexpr const wchar_t* kReleaseAumid = L"Agentmaster_56k4f06dsfp9r!App";
    inline constexpr const wchar_t* kReleaseFamily = L"Agentmaster_56k4f06dsfp9r";
    inline constexpr const wchar_t* kReleasesPage = L"https://github.com/Nucs/Agentmaster/releases";

    // Defined in the observability section below; declared first so EVERY function in this header —
    // incl. the version gates and the detail:: file helpers — can trace its own failures (POLICY:
    // no catch swallows without a log — every guard either logs or is itself the logger / a
    // logger-failed nested catch, annotated as such).
    inline void LogUpdate(const std::wstring& stateDir, const std::wstring& msg);

    // ============================ version ============================

    struct Version
    {
        int major{ 0 }, minor{ 0 }, patch{ 0 }, build{ 0 };
    };

    // Parse "vX.Y.Z" / "X.Y.Z" / "X.Y.Z.W" / "X.Y.Z-beta" — a leading 'v'/'V' and a trailing
    // non-numeric suffix (e.g. "-beta") are tolerated; missing parts default to 0.
    inline Version ParseVersion(std::wstring_view s)
    {
        Version v;
        size_t i = 0;
        while (i < s.size() && !(s[i] >= L'0' && s[i] <= L'9'))
        {
            ++i; // skip a leading 'v'/'V' / spaces
        }
        int idx = 0, acc = 0;
        bool any = false;
        auto store = [&](int value) {
            switch (idx)
            {
            case 0: v.major = value; break;
            case 1: v.minor = value; break;
            case 2: v.patch = value; break;
            case 3: v.build = value; break;
            default: break;
            }
        };
        // Component cap: a degenerate/hostile tag ("v99999999999.0.0") must not overflow the int
        // accumulator (signed overflow is UB — a garbage/negative component would corrupt every
        // later CompareVersion). Real versions are tiny; clamping preserves ordering semantics.
        constexpr int kMaxComponent = 100000000; // 1e8: acc < 1e8 ⇒ acc*10+9 ≤ 1e9+9 < INT_MAX (no overflow)
        for (; i < s.size(); ++i)
        {
            const wchar_t c = s[i];
            if (c >= L'0' && c <= L'9')
            {
                acc = acc < kMaxComponent ? acc * 10 + static_cast<int>(c - L'0') : kMaxComponent;
                any = true;
            }
            else if (c == L'.')
            {
                store(any ? acc : 0);
                ++idx;
                acc = 0;
                any = false;
                if (idx > 3)
                {
                    break;
                }
            }
            else
            {
                break; // stop at the first non-numeric, non-dot char (e.g. "-beta")
            }
        }
        if (idx <= 3)
        {
            store(any ? acc : 0);
        }
        return v;
    }

    // -1 / 0 / 1 over major.minor.patch (the tag is X.Y.Z; the manifest stamps X.Y.Z.0 — the 4th
    // part is build metadata and never participates in "is a newer release available?").
    inline int CompareVersion(const Version& a, const Version& b)
    {
        if (a.major != b.major)
        {
            return a.major < b.major ? -1 : 1;
        }
        if (a.minor != b.minor)
        {
            return a.minor < b.minor ? -1 : 1;
        }
        if (a.patch != b.patch)
        {
            return a.patch < b.patch ? -1 : 1;
        }
        return 0;
    }

    inline std::wstring VersionToString(const Version& v)
    {
        return std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.patch);
    }

    // The running package's manifest version, or {0,0,0,0} when unpackaged (so an unpackaged run
    // treats every release as newer — harmless, since the startup auto-check is gated off it).
    // No-throw: called from unguarded UI paths (the cog) — zeros are the safe default.
    inline Version CurrentPackageVersion()
    {
        Version v;
        try
        {
            UINT32 bufLen = 0;
            const LONG rc = ::GetCurrentPackageId(&bufLen, nullptr);
            if (rc != ERROR_INSUFFICIENT_BUFFER || bufLen == 0)
            {
                return v;
            }
            std::vector<BYTE> buf(bufLen);
            if (::GetCurrentPackageId(&bufLen, buf.data()) != ERROR_SUCCESS)
            {
                return v;
            }
            const auto* id = reinterpret_cast<const PACKAGE_ID*>(buf.data());
            v.major = id->version.Major;
            v.minor = id->version.Minor;
            v.patch = id->version.Build; // PACKAGE_VERSION: Major.Minor.Build.Revision == X.Y.Z.W
            v.build = id->version.Revision;
        }
        catch (...)
        {
            v = Version{};
            try
            {
                LogUpdate(Profiles::ResolveProfileDir(), L"package-version read CRASHED (0.0.0 assumed)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
        }
        return v;
    }

    // No-throw: gate helpers must never take down a caller (Profiles resolution allocates).
    inline bool IsPackaged()
    {
        try
        {
            return !Profiles::PackageFamilyName().empty();
        }
        catch (...)
        {
            try
            {
                LogUpdate(Profiles::ResolveProfileDir(), L"packaged-state read CRASHED (assuming unpackaged)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return false;
        }
    }

    // ============================ small helpers ============================

    namespace detail
    {
        // The directory holding `path` — where that file's hooks.log lives, for the file helpers'
        // failure traces ("" when path has no separator; LogUpdate no-ops on "").
        inline std::wstring DirOfPath(const std::wstring& path)
        {
            const size_t at = path.find_last_of(L"\\/");
            return at == std::wstring::npos ? std::wstring{} : path.substr(0, at);
        }
        inline std::wstring GetEnv(const wchar_t* name)
        {
            const DWORD need = ::GetEnvironmentVariableW(name, nullptr, 0);
            if (need == 0)
            {
                return {};
            }
            std::wstring v(need, L'\0');
            const DWORD got = ::GetEnvironmentVariableW(name, v.data(), need);
            v.resize(got);
            return v;
        }

        inline std::wstring Utf8ToWide(std::string_view s)
        {
            if (s.empty())
            {
                return {};
            }
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            if (n <= 0)
            {
                return {};
            }
            std::wstring out(static_cast<size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
            return out;
        }

        inline std::string WideToUtf8(std::wstring_view s)
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

        inline bool EndsWithNoCase(std::wstring_view s, std::wstring_view suffix)
        {
            if (s.size() < suffix.size())
            {
                return false;
            }
            const size_t off = s.size() - suffix.size();
            for (size_t i = 0; i < suffix.size(); ++i)
            {
                wchar_t a = s[off + i];
                wchar_t b = suffix[i];
                if (a >= L'A' && a <= L'Z')
                {
                    a = static_cast<wchar_t>(a - L'A' + L'a');
                }
                if (b >= L'A' && b <= L'Z')
                {
                    b = static_cast<wchar_t>(b - L'A' + L'a');
                }
                if (a != b)
                {
                    return false;
                }
            }
            return true;
        }

        inline void ReplaceAll(std::wstring& s, std::wstring_view tok, std::wstring_view val)
        {
            if (tok.empty())
            {
                return;
            }
            for (size_t at = s.find(tok); at != std::wstring::npos; at = s.find(tok, at + val.size()))
            {
                s.replace(at, tok.size(), val);
            }
        }

        // ATOMIC write — the engine's Persistence::WriteAllUtf8 recipe (temp sibling + FlushFileBuffers
        // + MoveFileExW REPLACE_EXISTING|WRITE_THROUGH), which this EXE-safe module cannot call. The
        // settings.json RMW below rides this, so a crash/power loss mid-write can never leave the user's
        // whole settings file torn/truncated (the old truncate-in-place ofstream could); a reader sees
        // either the whole old file or the whole new one. The installer scripts ride it too (harmless).
        inline bool WriteFileUtf8(const std::wstring& path, std::wstring_view content)
        {
            try
            {
                const auto bytes = WideToUtf8(content);
                // Per-thread temp name so two concurrent writers of one target never collide; same
                // directory as the target so the rename is a same-volume (atomic) metadata move.
                const std::wstring tmp = path + L".tmp." + std::to_wstring(::GetCurrentThreadId());
                const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                {
                    LogUpdate(DirOfPath(path), L"file-write FAILED (temp open): " + path);
                    return false;
                }
                DWORD wrote = 0;
                const BOOL ok = bytes.empty() ? TRUE : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
                if (ok)
                {
                    ::FlushFileBuffers(h); // best-effort: data on the platter BEFORE the rename commits
                }
                ::CloseHandle(h);
                if (!ok || wrote != bytes.size())
                {
                    ::DeleteFileW(tmp.c_str());
                    LogUpdate(DirOfPath(path), L"file-write FAILED (incomplete): " + path);
                    return false;
                }
                if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    ::DeleteFileW(tmp.c_str()); // the old file stays fully intact on failure
                    LogUpdate(DirOfPath(path), L"file-write FAILED (atomic replace): " + path);
                    return false;
                }
                return true;
            }
            catch (...)
            {
                try
                {
                    LogUpdate(DirOfPath(path), L"file-write CRASHED (exception): " + path);
                }
                catch (...)
                {
                    // logger-failed: nothing left to report through
                }
                return false;
            }
        }

        inline std::wstring ReadFileWide(const std::wstring& path)
        {
            try
            {
                std::ifstream in{ std::filesystem::path{ path }, std::ios::binary };
                if (!in)
                {
                    return {}; // a MISSING file is the normal first-run case — deliberately unlogged
                }
                const std::string bytes{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
                return Utf8ToWide(bytes);
            }
            catch (...)
            {
                // A genuine exception (not file-absence, which early-returns above) — trace it.
                try
                {
                    LogUpdate(DirOfPath(path), L"file-read CRASHED (exception): " + path);
                }
                catch (...)
                {
                    // logger-failed: nothing left to report through
                }
                return {};
            }
        }

        // Read an embedded RT_RCDATA resource (UTF-8 bytes) into a wide string. The installer payload
        // (am-update.ps1) is baked into WindowsTerminal.exe as the AM_UPDATE_PS1 resource — the REAL
        // .ps1 file, compiled in, so it is read from the binary at runtime and never shipped/opened as
        // a loose file. Both callers (the EXE startup check AND the DLL-hosted cog) run inside the
        // WindowsTerminal.exe process, so the process module (GetModuleHandleW(nullptr)) carries it;
        // we also fall back to the module this inline code is linked into, for robustness.
        inline std::wstring LoadResourceTextUtf8(const wchar_t* resName)
        {
            try
            {
                const auto readFrom = [resName](HMODULE mod) -> std::wstring {
                    if (!mod)
                    {
                        return {};
                    }
                    const HRSRC res = ::FindResourceW(mod, resName, RT_RCDATA);
                    if (!res)
                    {
                        return {};
                    }
                    const HGLOBAL loaded = ::LoadResource(mod, res);
                    const DWORD sz = ::SizeofResource(mod, res);
                    const void* ptr = loaded ? ::LockResource(loaded) : nullptr;
                    if (!ptr || sz == 0)
                    {
                        return {};
                    }
                    return Utf8ToWide(std::string_view{ reinterpret_cast<const char*>(ptr), sz });
                };
                if (std::wstring s = readFrom(::GetModuleHandleW(nullptr)); !s.empty())
                {
                    return s;
                }
                HMODULE self{};
                ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(&LoadResourceTextUtf8),
                                     &self);
                return readFrom(self);
            }
            catch (...)
            {
                // Missing/unreadable resource reads as absent — callers refuse-and-log the semantic
                // consequence ("REFUSED, resource missing"); this traces the raw exception itself.
                try
                {
                    LogUpdate(Profiles::ResolveProfileDir(), std::wstring{ L"resource read CRASHED (exception): " } + (resName ? resName : L"(null)"));
                }
                catch (...)
                {
                    // logger-failed: nothing left to report through
                }
                return {};
            }
        }
    }

    // Epoch milliseconds (FILETIME is 100ns since 1601; Unix epoch is +11644473600s).
    inline long long NowUnixMs()
    {
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        constexpr unsigned long long kEpochDiff100ns = 116444736000000000ULL;
        const unsigned long long t = u.QuadPart - kEpochDiff100ns;
        return static_cast<long long>(t / 10000ULL);
    }

    // The hour "Remind me tomorrow" lands on: the start of the next working day, local time.
    constexpr int kPostponeMorningHour = 8;

    // A LOCAL wall-clock SYSTEMTIME -> epoch ms (the inverse of the GetLocalTime path). Returns
    // false when the timezone conversion is unavailable, so callers can degrade honestly rather
    // than persist a garbage instant.
    inline bool LocalSystemTimeToUnixMs(const SYSTEMTIME& local, long long& outMs)
    {
        SYSTEMTIME utc{};
        FILETIME ft{};
        if (!::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !::SystemTimeToFileTime(&utc, &ft))
        {
            return false;
        }
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        constexpr unsigned long long kEpochDiff100ns = 116444736000000000ULL;
        outMs = static_cast<long long>((u.QuadPart - kEpochDiff100ns) / 10000ULL);
        return true;
    }

    // "Tomorrow" == the NEXT local `hour`:00, NOT now + 24h. A rolling 24h window re-asks at
    // whatever hour you happened to click — press it at 23:40 and it interrupts you at 23:40
    // tomorrow, the worst moment of the day — whereas "the next 08:00" lands where the decision
    // actually belongs: the start of the next working day. The next-OCCURRENCE rule also does the
    // right thing in the small hours (click it at 02:00 and the coming 08:00 IS the morning you
    // meant), so the span is always within (0, 24h].
    //
    // DST-safe by construction: the +1 day step is taken on the LOCAL wall clock (08:00 stays
    // 08:00 across a shift) and local -> UTC is applied AFTER, at that date's real offset. If the
    // timezone conversion is unavailable at either step it falls back to now + 24h, and a final
    // belt guarantees a strictly-future instant — a postpone must never resolve to "now" (which
    // would re-prompt on the next hourly tick, reading as if the choice was ignored).
    // outLocalMorning (optional) receives the resolved LOCAL wall-clock time; it stays zeroed on
    // the fallback path.
    inline long long NextLocalMorningUnixMs(int hour = kPostponeMorningHour, SYSTEMTIME* outLocalMorning = nullptr)
    {
        constexpr long long kDayMs = 24LL * 60 * 60 * 1000;
        const long long now = NowUnixMs();
        if (outLocalMorning)
        {
            *outLocalMorning = SYSTEMTIME{};
        }

        SYSTEMTIME target{};
        ::GetLocalTime(&target);
        target.wHour = static_cast<WORD>(hour);
        target.wMinute = 0;
        target.wSecond = 0;
        target.wMilliseconds = 0;

        long long ms = 0;
        if (!LocalSystemTimeToUnixMs(target, ms))
        {
            return now + kDayMs;
        }
        if (ms <= now)
        {
            // Already past today's hour: step the LOCAL date forward one day. SYSTEMTIME has no
            // arithmetic, so round-trip through FILETIME ticks — legal here precisely because both
            // ends are the same local wall clock (no UTC conversion is involved in the step).
            FILETIME localTicks{};
            if (!::SystemTimeToFileTime(&target, &localTicks))
            {
                return now + kDayMs;
            }
            ULARGE_INTEGER u{};
            u.LowPart = localTicks.dwLowDateTime;
            u.HighPart = localTicks.dwHighDateTime;
            u.QuadPart += 24ULL * 60 * 60 * 10000000ULL; // one day in 100ns ticks
            localTicks.dwLowDateTime = u.LowPart;
            localTicks.dwHighDateTime = u.HighPart;
            if (!::FileTimeToSystemTime(&localTicks, &target) || !LocalSystemTimeToUnixMs(target, ms))
            {
                return now + kDayMs;
            }
        }
        if (ms <= now)
        {
            return now + kDayMs; // belt: never postpone to the past/present
        }
        if (outLocalMorning)
        {
            *outLocalMorning = target;
        }
        return ms;
    }

    // ============================ observability ============================

    // Append one "[update] …" line to the profile's hooks.log — the SAME file, local-time stamp
    // format ([HH:MM:SS.mmm]) and whole-line-per-write discipline as the engine's AppendStateLog,
    // which this module cannot call (the EXE links TerminalApp.dll, not the engine lib). One
    // FILE_APPEND_DATA WriteFile per line keeps records intact against the engine's concurrent
    // appends (append-mode writes serialize per-write). Best-effort, never throws — an update trace
    // must never break an update. This is what makes the hourly autocheck VERIFIABLE from the log
    // (one line per tick), previously a fully silent path.
    inline void LogUpdate(const std::wstring& stateDir, const std::wstring& msg)
    {
        try
        {
            if (stateDir.empty())
            {
                return;
            }
            const std::wstring path = stateDir + L"\\hooks.log";
            const HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            SYSTEMTIME st{};
            ::GetLocalTime(&st);
            wchar_t stamp[24];
            ::swprintf(stamp, 24, L"[%02u:%02u:%02u.%03u] ", static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds));
            const std::string bytes = detail::WideToUtf8(std::wstring{ stamp } + L"[update] " + msg + L"\n");
            DWORD wrote = 0;
            ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
            ::CloseHandle(h);
        }
        catch (...)
        {
            // this IS the logger — a trace that can't be written has nowhere left to go
        }
    }

    // A short human span for the postpone-gate log line: "3d2h" / "5h10m" / "42m".
    inline std::wstring FormatSpanShort(long long ms)
    {
        if (ms < 0)
        {
            ms = 0;
        }
        const long long mins = ms / 60000;
        const long long days = mins / (24 * 60);
        const long long hours = (mins / 60) % 24;
        const long long rem = mins % 60;
        if (days > 0)
        {
            return std::to_wstring(days) + L"d" + (hours > 0 ? std::to_wstring(hours) + L"h" : L"");
        }
        if (hours > 0)
        {
            return std::to_wstring(hours) + L"h" + (rem > 0 ? std::to_wstring(rem) + L"m" : L"");
        }
        return std::to_wstring(rem) + L"m";
    }

    // ============================ GitHub check ============================

    // A NIGHTLY is an unstable development build — a tier BELOW pre-release: published as a GitHub
    // prerelease, but its TAG carries "nightly" (the naming contract, e.g. "v0.6.10-prerelease-nightly";
    // matched case-insensitively by CONTAINS). The TAG is the authoritative signal — the prerelease
    // flag only says "not stable", the tag says "nightly" — so a nightly is recognized even if a
    // publish forgot the prerelease checkbox. Nightlies are ALWAYS skipped by every check (startup /
    // hourly / cog) unless the user opted in via the cog's warning-gated nightly switch
    // (allowUpdateNightly), which is ORTHOGONAL to the pre-release opt-in: nightly ON alone offers
    // nightlies + stable, pre-release ON alone offers betas + stable, both ON offers everything.
    // NOTE for publishers: CompareVersion is numeric major.minor.patch — a nightly must BUMP the
    // patch past the installed version to ever be offered (v0.6.10-…-nightly reads as 0.6.10).
    inline bool IsNightlyTag(std::wstring_view tag)
    {
        constexpr std::wstring_view needle = L"nightly";
        if (tag.size() < needle.size())
        {
            return false;
        }
        for (size_t i = 0; i + needle.size() <= tag.size(); ++i)
        {
            size_t j = 0;
            for (; j < needle.size(); ++j)
            {
                wchar_t c = tag[i + j];
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
                if (c != needle[j])
                {
                    break;
                }
            }
            if (j == needle.size())
            {
                return true;
            }
        }
        return false;
    }

    // Channel eligibility for ONE release: stable is always offered; a NIGHTLY (by tag — see
    // IsNightlyTag) only under the nightly opt-in, REGARDLESS of the prerelease flag/opt-in; any
    // other prerelease only under the pre-release opt-in. Drafts are excluded by the callers (the
    // list scan skips them; /releases/latest never returns one). Pure — unit-tested in the harness.
    inline bool ReleaseAllowedOnChannel(std::wstring_view tag, bool isPrerelease, bool allowPrerelease, bool allowNightly)
    {
        if (IsNightlyTag(tag))
        {
            return allowNightly;
        }
        return !isPrerelease || allowPrerelease;
    }

    struct UpdateInfo
    {
        bool checked{ false }; // the network round-trip completed + parsed (regardless of result)
        bool available{ false }; // a strictly-newer release exists (latest > current, by major.minor.patch)
        bool installable{ false }; // the release carries BOTH a .msixbundle and a .cer asset
        std::wstring error; // a short human note when the check failed / found nothing
        std::wstring currentVersionStr; // "0.4.1"
        std::wstring latestTag; // "v0.4.3" (the GitHub tag verbatim)
        std::wstring latestVersionStr; // "0.4.3" (tag without a leading v)
        Version latest;
        bool isPrerelease{ false };
        bool isNightly{ false }; // tag contains "nightly" (IsNightlyTag) — an unstable development build; offered only under the nightly opt-in
        std::wstring bundleUrl; // .msixbundle browser_download_url
        std::wstring bundleSha256; // the bundle asset's "sha256:<hex>" digest (when GitHub provides it) — verified by am-update.ps1
        std::wstring cerUrl; // .cer browser_download_url
        std::wstring notes; // release body (markdown; parsed + available — the prompt now LINKS to the release page rather than showing it inline)
        std::wstring htmlUrl; // release page (changelog) — .../releases/tag/<tag>; "What's new" / "Update's changelog" open this
    };

    // "v0.4.3" — always with a leading v, for display.
    inline std::wstring DisplayVersion(const UpdateInfo& info)
    {
        if (!info.latestTag.empty() && (info.latestTag[0] == L'v' || info.latestTag[0] == L'V'))
        {
            return info.latestTag;
        }
        return L"v" + (info.latestVersionStr.empty() ? VersionToString(info.latest) : info.latestVersionStr);
    }

    // The GitHub release page for a specific tag (the changelog FIXATED on that release, not the
    // generic /releases list): https://github.com/<repo>/releases/tag/v<X.Y.Z>. A leading 'v' is
    // ensured (our tags are vX.Y.Z). Used for the cog's "Current version changelog" link and as the
    // fallback for an update's page when the API didn't carry an html_url.
    inline std::wstring ReleasePageForTag(const std::wstring& tag)
    {
        std::wstring t = tag;
        if (!t.empty() && t[0] != L'v' && t[0] != L'V')
        {
            t = L"v" + t;
        }
        return std::wstring{ L"https://github.com/" } + kRepo + L"/releases/tag/" + t;
    }

    // The release page (changelog) for an update — the API's html_url (already a .../releases/tag/<tag>
    // page), else built from the tag.
    inline std::wstring ChangelogUrl(const UpdateInfo& info)
    {
        return !info.htmlUrl.empty() ? info.htmlUrl : ReleasePageForTag(info.latestTag);
    }

    // TaskDialog hyperlink handler: a clicked <a href="URL"> hands the URL in lParam — open it in the
    // default browser. Used by ShowUpdatePrompt's "What's new" link (TDF_ENABLE_HYPERLINKS).
    // No-throw: an exception must never unwind through comctl32's callback boundary (UB).
    inline HRESULT CALLBACK UpdatePromptCallback(HWND /*hwnd*/, UINT msg, WPARAM /*wParam*/, LPARAM lParam, LONG_PTR /*ref*/)
    {
        try
        {
            if (msg == TDN_HYPERLINK_CLICKED && lParam)
            {
                ::ShellExecuteW(nullptr, L"open", reinterpret_cast<PCWSTR>(lParam), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        catch (...)
        {
            try
            {
                LogUpdate(Profiles::ResolveProfileDir(), L"changelog link open CRASHED (exception)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
        }
        return S_OK;
    }

    // A single HTTPS GET; returns the UTF-8 body bytes ("" on any failure), with a short note in
    // errOut. One timeout value governs each WinHTTP phase (resolve/connect/send/receive), so the
    // worst-case wall time is bounded — the startup caller relies on this to never wedge launch.
    //
    // `extraHeaders` defaults to the GitHub-API set this was written for (a User-Agent GitHub
    // REQUIRES, plus the v3 Accept), so every existing caller is byte-identical. It is a parameter
    // because the launch-model picker's "Specify a model..." prompt fetches the published model
    // catalogs through this SAME function (ModelCatalog.h) — Anthropic's needs `x-api-key` +
    // `anthropic-version` — and a second hand-rolled WinHTTP GET elsewhere in the tree would be one
    // more place to get the handle-leak and bounded-timeout discipline below subtly wrong. Must be
    // CRLF-terminated per header, WinHTTP's format.
    inline std::string HttpsGet(const std::wstring& host,
                                const std::wstring& path,
                                DWORD timeoutMs,
                                std::wstring& errOut,
                                const std::wstring& extraHeaders = L"User-Agent: Agentmaster-Updater\r\nAccept: application/vnd.github+json\r\n")
    {
        std::string body;
        HINTERNET hSession = ::WinHttpOpen(L"Agentmaster-Updater/1.0",
                                           WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                           WINHTTP_NO_PROXY_NAME,
                                           WINHTTP_NO_PROXY_BYPASS,
                                           0);
        if (!hSession)
        {
            errOut = L"WinHttpOpen failed";
            return body;
        }
        ::WinHttpSetTimeouts(hSession, static_cast<int>(timeoutMs), static_cast<int>(timeoutMs), static_cast<int>(timeoutMs), static_cast<int>(timeoutMs));
        HINTERNET hConnect = ::WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect)
        {
            errOut = L"WinHttpConnect failed";
            ::WinHttpCloseHandle(hSession);
            return body;
        }
        HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest)
        {
            errOut = L"WinHttpOpenRequest failed";
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            return body;
        }
        // The allocating section (headers string, chunk buffers, body growth) is guarded so a
        // bad_alloc mid-read can't UNWIND PAST the handle closes below — the three WinHTTP handles
        // would leak once per tick, forever. On an exception the partial body is discarded (a
        // truncated JSON must never parse as a real answer) and the closes still run.
        try
        {
            // GitHub requires a User-Agent; the v3 Accept header is good manners. (Both live in the
            // extraHeaders default above, so a caller that passes nothing behaves exactly as before.)
            const std::wstring headers = extraHeaders;
            BOOL ok = ::WinHttpSendRequest(hRequest,
                                           headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                           headers.empty() ? 0 : static_cast<DWORD>(-1),
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (ok)
            {
                ok = ::WinHttpReceiveResponse(hRequest, nullptr);
            }
            if (ok)
            {
                DWORD status = 0, slen = sizeof(status);
                ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
                if (status >= 200 && status < 300)
                {
                    for (;;)
                    {
                        DWORD avail = 0;
                        if (!::WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0)
                        {
                            break;
                        }
                        std::string chunk(avail, '\0');
                        DWORD read = 0;
                        if (!::WinHttpReadData(hRequest, chunk.data(), avail, &read) || read == 0)
                        {
                            break;
                        }
                        body.append(chunk.data(), read);
                    }
                }
                else
                {
                    errOut = L"HTTP status " + std::to_wstring(status);
                }
            }
            else
            {
                errOut = L"request failed (" + std::to_wstring(::GetLastError()) + L")";
            }
        }
        catch (...)
        {
            body.clear();
            try
            {
                errOut = L"exception during read"; // surfaced by the caller's "check failed: …" log line
            }
            catch (...)
            {
                // even the error string failed to build (alloc exhaustion) — terminal swallow
            }
        }
        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        return body;
    }

    // Defense-in-depth on the asset URLs: the installer DOWNLOADS AND EXECUTES what these point at
    // (am-update.ps1 fetches the bundle, trusts the cer, Add-AppxPackage), so only OUR repo's own
    // release-download URLs are ever accepted — https://github.com/<kRepo>/releases/download/…,
    // exactly what the GitHub API emits for a release asset. The TLS channel to api.github.com
    // already authenticates the JSON; this is the belt should that channel ever be intercepted or
    // the parse confused — a foreign URL simply reads as "no installable assets" (the prompt then
    // links to the releases page instead of silently installing from elsewhere).
    inline bool IsTrustedAssetUrl(const std::wstring& url)
    {
        const std::wstring prefix = std::wstring{ L"https://github.com/" } + kRepo + L"/releases/download/";
        return url.size() > prefix.size() && url.compare(0, prefix.size(), prefix) == 0;
    }

    // Fill an UpdateInfo from one parsed release JSON object (tag/prerelease/body/url + the
    // .msixbundle + .cer assets), and decide `available` against the current version.
    inline void ParseReleaseObj(const json::Value& rel, const Version& cur, UpdateInfo& info)
    {
        info.latestTag = rel.StrAt(L"tag_name");
        info.isPrerelease = rel.BoolAt(L"prerelease", false);
        info.isNightly = IsNightlyTag(info.latestTag); // by TAG, not the prerelease flag (see IsNightlyTag)
        info.notes = rel.StrAt(L"body");
        info.htmlUrl = rel.StrAt(L"html_url");
        info.latest = ParseVersion(info.latestTag);
        {
            std::wstring t = info.latestTag;
            size_t i = 0;
            while (i < t.size() && (t[i] == L'v' || t[i] == L'V'))
            {
                ++i;
            }
            info.latestVersionStr = t.substr(i);
        }
        if (const auto* assets = rel.Find(L"assets"); assets && assets->type == json::Value::Type::Arr)
        {
            for (const auto& a : assets->arr)
            {
                const std::wstring name = a.StrAt(L"name");
                const std::wstring url = a.StrAt(L"browser_download_url");
                if (url.empty() || !IsTrustedAssetUrl(url))
                {
                    continue; // a non-own-repo download URL is never an installable asset (see IsTrustedAssetUrl)
                }
                if (detail::EndsWithNoCase(name, L".msixbundle"))
                {
                    info.bundleUrl = url;
                    info.bundleSha256 = a.StrAt(L"digest"); // "sha256:<hex>" when present (newer GitHub API), else "" -> no verify
                }
                else if (detail::EndsWithNoCase(name, L".cer"))
                {
                    info.cerUrl = url;
                }
            }
        }
        info.installable = !info.bundleUrl.empty() && !info.cerUrl.empty();
        info.available = CompareVersion(info.latest, cur) > 0;
    }

    // Query GitHub for the newest release ON THE USER'S CHANNEL. Either opt-in -> the list endpoint,
    // first non-draft release that ReleaseAllowedOnChannel admits (nightlies gated on allowNightly,
    // other prereleases on allowPrerelease — so a nightly at the top of the list is SKIPPED for a
    // prerelease-only user and the next eligible release is offered instead); neither opt-in ->
    // /releases/latest (excludes drafts AND prereleases). Synchronous, bounded by timeoutMs per
    // phase. Never throws. allowNightly deliberately has NO default: a defaulted bool before the
    // defaulted timeout would let a legacy 3-arg call's numeric timeout silently convert into the
    // flag — every caller must say what channel it wants.
    inline UpdateInfo CheckForUpdate(const Version& cur, bool allowPrerelease, bool allowNightly, DWORD timeoutMs = 6000)
    {
        UpdateInfo info;
        info.currentVersionStr = VersionToString(cur);
        const std::wstring base = std::wstring{ L"/repos/" } + kRepo + L"/releases";
        std::wstring err;
        try
        {
            if (allowPrerelease || allowNightly)
            {
                const std::string raw = HttpsGet(kApiHost, base + L"?per_page=30", timeoutMs, err);
                if (raw.empty())
                {
                    info.error = err.empty() ? std::wstring{ L"no response" } : err;
                    return info;
                }
                const auto parsed = json::Parse(detail::Utf8ToWide(raw));
                if (!parsed || parsed->type != json::Value::Type::Arr)
                {
                    info.error = L"unexpected response";
                    return info;
                }
                info.checked = true;
                for (const auto& rel : parsed->arr)
                {
                    if (rel.BoolAt(L"draft", false))
                    {
                        continue;
                    }
                    if (!ReleaseAllowedOnChannel(rel.StrAt(L"tag_name"), rel.BoolAt(L"prerelease", false), allowPrerelease, allowNightly))
                    {
                        continue; // off-channel (e.g. a nightly without the nightly opt-in) — keep scanning
                    }
                    ParseReleaseObj(rel, cur, info);
                    break; // first eligible non-draft is the newest release on this channel
                }
                if (info.latestTag.empty())
                {
                    info.error = L"no releases";
                }
            }
            else
            {
                const std::string raw = HttpsGet(kApiHost, base + L"/latest", timeoutMs, err);
                if (raw.empty())
                {
                    info.error = err.empty() ? std::wstring{ L"no response" } : err;
                    return info;
                }
                const auto parsed = json::Parse(detail::Utf8ToWide(raw));
                if (!parsed || parsed->type != json::Value::Type::Obj)
                {
                    info.error = L"unexpected response";
                    return info;
                }
                info.checked = true;
                if (parsed->Find(L"tag_name"))
                {
                    ParseReleaseObj(*parsed, cur, info);
                    if (info.isNightly)
                    {
                        // Belt: a nightly should NEVER surface from /releases/latest (it's published
                        // as a prerelease, which /latest excludes) — but a mis-published one must not
                        // reach the stable channel. Reads as up-to-date; the trail shows the tag.
                        info.available = false;
                        info.installable = false;
                    }
                }
                else
                {
                    info.error = L"no releases"; // {"message":"Not Found"} when the repo has no published release
                }
            }
        }
        catch (...)
        {
            info.error = L"check failed";
        }
        return info;
    }

    // ============================ persisted prefs (settings.json) ============================

    struct UpdatePrefs
    {
        bool allowPrerelease{ false };
        bool allowNightly{ false }; // NIGHTLY opt-in (warning-gated in the cog) — without it a nightly-tagged release is always skipped
        std::wstring skippedVersion; // a tag the user chose to skip (e.g. "v0.4.3")
        long long postponedUntilUnixMs{ 0 };
    };

    inline std::wstring SettingsPath(const std::wstring& stateDir)
    {
        return stateDir + L"\\settings.json";
    }

    // settings.json is the engine's ENVELOPE — `{version: 1, settings: {...}}` (Persistence.cpp
    // SerializeAppSettings) — so the update keys live NESTED under "settings". Reading/writing the
    // ROOT was the original bug: ReadPrefs never saw the cog-saved allowUpdatePrerelease (the
    // startup/hourly checks ran stable-only forever), and a Skip/Postpone written at the top level
    // was silently WIPED by the next engine save (which rebuilds the whole envelope from the
    // AppSettings struct). Nested-first with a top-level fallback: a file last touched by the
    // pre-fix RMW (strays beside "settings") or a hand-made flat file still honors the old choice.
    inline UpdatePrefs ReadPrefs(const std::wstring& stateDir)
    {
        UpdatePrefs p;
        try
        {
            const auto parsed = json::Parse(detail::ReadFileWide(SettingsPath(stateDir)));
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                return p;
            }
            const json::Value* nested = parsed->Find(L"settings");
            const bool haveNested = nested && nested->type == json::Value::Type::Obj;
            const json::Value& o = haveNested ? *nested : *parsed;
            p.allowPrerelease = o.BoolAt(L"allowUpdatePrerelease", false);
            // Nightly postdates the envelope fix, so it has NO pre-fix top-level stray era — the
            // nested-or-flat read through `o` is the whole story (no legacy fallback below).
            p.allowNightly = o.BoolAt(L"allowUpdateNightly", false);
            p.skippedVersion = o.StrAt(L"updateSkippedVersion");
            p.postponedUntilUnixMs = o.I64At(L"updatePostponedUntilUnixMs", 0);
            if (haveNested)
            {
                // Legacy strays from the pre-fix top-level RMW: honor them only where the nested key is
                // unset, so an old Skip/Postpone isn't forgotten by the fix itself (the next
                // WriteUpdateState heals them into the envelope and drops the strays).
                if (!p.allowPrerelease)
                {
                    p.allowPrerelease = parsed->BoolAt(L"allowUpdatePrerelease", false);
                }
                if (p.skippedVersion.empty())
                {
                    p.skippedVersion = parsed->StrAt(L"updateSkippedVersion");
                }
                if (p.postponedUntilUnixMs == 0)
                {
                    p.postponedUntilUnixMs = parsed->I64At(L"updatePostponedUntilUnixMs", 0);
                }
            }
        }
        catch (...)
        {
            p = UpdatePrefs{}; // unreadable prefs read as pristine defaults — check stable, prompt normally
            try
            {
                LogUpdate(stateDir, L"prefs read CRASHED (exception \x2014 defaults assumed: stable channel, nothing skipped/postponed)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
        }
        return p;
    }

    // Set-or-replace a member on an object (Json.h's Value::Set always APPENDS — for an RMW we must
    // replace an existing key, not duplicate it).
    inline void SetMember(json::Value& obj, const std::wstring& key, json::Value v)
    {
        for (auto& m : obj.members)
        {
            if (m.first == key)
            {
                m.second = std::move(v);
                return;
            }
        }
        obj.members.emplace_back(key, std::move(v));
    }

    // Mutable member lookup + removal (Json.h's Find is const-only and Set appends).
    inline json::Value* FindMember(json::Value& obj, std::wstring_view key)
    {
        for (auto& m : obj.members)
        {
            if (m.first == key)
            {
                return &m.second;
            }
        }
        return nullptr;
    }

    inline void RemoveMember(json::Value& obj, std::wstring_view key)
    {
        for (auto it = obj.members.begin(); it != obj.members.end();)
        {
            it = (it->first == key) ? obj.members.erase(it) : std::next(it);
        }
    }

    // Freshest-disk read-modify-write of settings.json: parse the whole file, set just the given
    // update key(s) INSIDE the engine's `{version, settings:{...}}` envelope (where AppSettings
    // round-trips them, so an engine save preserves instead of wiping them), re-serialize with
    // every other key preserved verbatim, and write ATOMICALLY. The EXE uses this to persist a
    // Skip/Postpone choice without linking the engine; the cog's Save preserves these same keys
    // from disk so a form Save never regresses them. A non-empty file that doesn't parse is NOT
    // ours to rebuild — bail (the choice stays unpersisted) rather than clobber the user's whole
    // settings with an update-keys-only skeleton; the engine's writes are atomic, so that is a
    // foreign/corrupt file, never a torn mid-write read.
    inline bool WriteUpdateState(const std::wstring& stateDir, const std::wstring* skipTag, const long long* postponeMs)
    {
        try
        {
            const std::wstring raw = detail::ReadFileWide(SettingsPath(stateDir));
            const auto parsed = json::Parse(raw);
            const bool haveObj = parsed && parsed->type == json::Value::Type::Obj;
            if (!raw.empty() && !haveObj)
            {
                LogUpdate(stateDir, L"settings-write REFUSED (settings.json unparseable \x2014 choice not persisted)");
                return false;
            }
            json::Value root = haveObj ? *parsed : json::Value::MkObj();
            // Capture the pre-fix top-level strays for MIGRATION (not deletion): a legacy Skip/
            // Postpone/prerelease written beside "settings" by the old RMW is folded INTO the
            // envelope below when this write isn't itself setting that key and the nested key is
            // still unset — healing must never forget a choice the user already made. Root-level
            // structure is mutated FIRST (every root append/erase can reallocate `members`), the
            // nested `settings` pointer is taken LAST, and root is never touched after.
            const std::wstring straySkip = root.StrAt(L"updateSkippedVersion");
            const long long strayPostpone = root.I64At(L"updatePostponedUntilUnixMs", 0);
            const bool strayPrerelease = root.BoolAt(L"allowUpdatePrerelease", false);
            RemoveMember(root, L"updateSkippedVersion");
            RemoveMember(root, L"updatePostponedUntilUnixMs");
            RemoveMember(root, L"allowUpdatePrerelease");
            if (!FindMember(root, L"version"))
            {
                SetMember(root, L"version", json::Value::MkNum(1));
            }
            json::Value* settings = FindMember(root, L"settings");
            if (settings && settings->type != json::Value::Type::Obj)
            {
                *settings = json::Value::MkObj(); // a malformed "settings" — replace in place
            }
            else if (!settings)
            {
                SetMember(root, L"settings", json::Value::MkObj());
                settings = FindMember(root, L"settings");
            }
            if (skipTag)
            {
                SetMember(*settings, L"updateSkippedVersion", json::Value::MkStr(*skipTag));
            }
            else if (!straySkip.empty() && settings->StrAt(L"updateSkippedVersion").empty())
            {
                SetMember(*settings, L"updateSkippedVersion", json::Value::MkStr(straySkip));
            }
            if (postponeMs)
            {
                SetMember(*settings, L"updatePostponedUntilUnixMs", json::Value::MkNum(static_cast<double>(*postponeMs)));
            }
            else if (strayPostpone != 0 && settings->I64At(L"updatePostponedUntilUnixMs", 0) == 0)
            {
                SetMember(*settings, L"updatePostponedUntilUnixMs", json::Value::MkNum(static_cast<double>(strayPostpone)));
            }
            if (strayPrerelease && !settings->BoolAt(L"allowUpdatePrerelease", false))
            {
                SetMember(*settings, L"allowUpdatePrerelease", json::Value::MkBool(true));
            }
            std::filesystem::create_directories(std::filesystem::path{ stateDir });
            const bool ok = detail::WriteFileUtf8(SettingsPath(stateDir), json::Dump(root));
            if (!ok)
            {
                LogUpdate(stateDir, L"settings-write FAILED (skip/postpone not persisted)");
            }
            return ok;
        }
        catch (...)
        {
            // Best-effort trace, then swallow: a failed persist means the user gets re-asked, never
            // a crash. (LogUpdate itself never throws, but its ARGUMENT construction can — nest.)
            try
            {
                LogUpdate(stateDir, L"settings-write CRASHED (exception \x2014 choice not persisted)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return false;
        }
    }

    // Both return whether the choice actually PERSISTED (the RMW can refuse/fail) so ApplyDecision
    // can log an honest outcome — a swallowed failure here used to read as a successful postpone.
    inline bool WriteSkip(const std::wstring& stateDir, const std::wstring& tag)
    {
        return WriteUpdateState(stateDir, &tag, nullptr);
    }

    inline bool WritePostpone(const std::wstring& stateDir, long long untilUnixMs)
    {
        return WriteUpdateState(stateDir, nullptr, &untilUnixMs);
    }

    // ==================== "Remind me next restart" (declined this run) ====================

    // "Remind me next restart" means "leave me alone until the NEXT LAUNCH" — a decline silences the
    // startup + hourly checks ENTIRELY (gated BEFORE the network round-trip in RunUpdateCheckAndPrompt: no
    // query, no prompt — even a NEWER release published mid-run waits for the next launch; the
    // cog's explicit "Check for updates" stays fully live, it's user-initiated). The latch is
    // PROCESS-scoped and deliberately an ENVIRONMENT VARIABLE: Updater.h is compiled into BOTH
    // WindowsTerminal.exe (the startup + hourly checks) and TerminalApp.dll (the cog's prompt) — an
    // inline/static would exist once PER MODULE, but the env block is one per PROCESS, so a decline
    // taken on the cog's prompt also silences the EXE's hourly timer. It dies with the
    // process; child processes inherit it (harmless — RunStartupUpdateCheck CLEARS it at every
    // fresh launch, so an installer-relaunched / child-spawned instance still asks). The tag is
    // stored for the log trail; the gate is PRESENCE-based.
    inline constexpr const wchar_t* kDeclinedEnvVar = L"AGENTMASTER_UPDATE_DECLINED";

    inline void MarkDeclinedThisRun(const std::wstring& tag) noexcept
    {
        ::SetEnvironmentVariableW(kDeclinedEnvVar, tag.empty() ? nullptr : tag.c_str());
    }

    // The declined tag ("" when nothing was declined this run). No-throw (GetEnv allocates): an
    // unreadable latch reads as "not declined" — fails toward ONE extra prompt, never toward a
    // silently dead updater.
    inline std::wstring DeclinedThisRunTag()
    {
        try
        {
            return detail::GetEnv(kDeclinedEnvVar);
        }
        catch (...)
        {
            try
            {
                LogUpdate(Profiles::ResolveProfileDir(), L"declined-latch read CRASHED (exception \x2014 assuming not declined)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return {};
        }
    }

    // Exact-tag query (kept for tests + callers that reason about a specific version).
    inline bool WasDeclinedThisRun(const std::wstring& tag)
    {
        return !tag.empty() && DeclinedThisRunTag() == tag;
    }

    // ============================ the prompt ============================

    enum class Decision
    {
        // "Remind me next restart" — the default radio, and the Cancel / X / any-failure outcome.
        // Persists NOTHING: it latches the process-scoped declined-this-run marker, so the startup
        // + hourly checks are off until the next LAUNCH (which clears it). Named for what actually
        // happens, unlike the "Not now" BUTTON it replaces, whose wording promised nothing.
        RemindNextRestart,
        UpdateNow,
        PostponeTomorrow, // "Remind me tomorrow" — the next local 08:00 (NOT a rolling 24h)
        Postpone3,
        Postpone7,
        Postpone30,
        Skip
    };

    // The "same question" shown at startup AND from the cog: TWO buttons (Update now / Postpone), with
    // the postpone DURATION chosen via a radio group (tomorrow 08:00 / 3 / 7 / 30 days / skip this version) — the
    // TaskDialog analog of the requested dropdown (a TaskDialog can't host a combobox; radios are
    // the idiomatic in-dialog choice). Cancel / X == the default radio, "Remind me next restart" — and
    // ALSO the answer on any exception: a broken prompt must never crash the caller (the cog path
    // calls this straight off a UI lambda) nor fabricate a consequential choice.
    inline Decision ShowUpdatePrompt(HWND owner, const UpdateInfo& info)
    {
        // The allocating part (string building) under guard; the remainder is plain structs + one
        // Win32 call, which do not throw.
        std::wstring instruction, content, footer, tomorrowLabel{ L"Remind me tomorrow" };
        try
        {
            // The tomorrow radio names the instant it actually resolves to ("Remind me tomorrow
            // (Sat, 8:00 AM)"), because that instant is NOT "now + a day" — and in the small hours
            // it isn't even the next calendar day. The weekday + locale-formatted time remove both
            // ambiguities; a formatting failure silently keeps the plain label.
            SYSTEMTIME morning{};
            NextLocalMorningUnixMs(kPostponeMorningHour, &morning);
            if (morning.wYear != 0) // zeroed == the timezone-conversion fallback -> plain label
            {
                wchar_t day[64]{}, clock[64]{};
                if (::GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &morning, L"ddd", day, ARRAYSIZE(day), nullptr) > 0 &&
                    ::GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &morning, nullptr, clock, ARRAYSIZE(clock)) > 0)
                {
                    tomorrowLabel += L" (" + std::wstring{ day } + L", " + clock + L")";
                }
            }

            instruction = L"Agentmaster " + DisplayVersion(info) + L" is available";
            content = L"You're on v" + info.currentVersionStr + L".\n\n";
            if (info.isNightly)
            {
                // The nightly opt-in already warned once (the cog's confirm), but the prompt is where
                // the install decision happens — restate what a nightly IS right where it's chosen.
                content += L"\x26A0 This is a NIGHTLY build \x2014 an unstable development version. It may have memory leaks, CPU issues, and crashes.\n\n";
            }
            content += info.installable ?
                           L"Choose \x201CUpdate now\x201D and Agentmaster will close and reopen automatically once the new version is installed, or pick when to be reminded." :
                           L"Choose \x201CUpdate now\x201D to open the download page, or pick when to be reminded.";
            // "What's new" OPENS this release's GitHub page (the changelog fixated on the specific
            // release), rather than dumping notes inline — a TaskDialog hyperlink (TDF_ENABLE_HYPERLINKS)
            // that UpdatePromptCallback ShellExecutes on click. The URL has no chars that need escaping
            // inside the <a href="…"> markup (a GitHub release URL).
            footer = L"<a href=\"" + ChangelogUrl(info) + L"\">What's new \x2192</a>";
        }
        catch (...)
        {
            try
            {
                LogUpdate(Profiles::ResolveProfileDir(), L"prompt build CRASHED (exception \x2014 treated as remind-next-restart, no dialog shown)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return Decision::RemindNextRestart;
        }

        constexpr int idUpdate = 2001, idPostpone = 2002;
        // Declared in DISPLAY order (the array below is what the user reads top-to-bottom); the ids
        // are dialog-local and never persisted, so they exist only to be matched back below.
        constexpr int ridRestart = 3000, ridTomorrow = 3001, rid3 = 3002, rid7 = 3003, rid30 = 3004, ridSkip = 3005;

        const TASKDIALOG_BUTTON buttons[] = {
            { idUpdate, L"Update now" },
            { idPostpone, L"Postpone" },
        };
        // "Remind me next restart" is FIRST + default: every other radio commits something durable
        // (a postpone instant, or a skipped tag), so the pre-selected one must be the choice that
        // writes nothing — the same outcome Cancel / X gives. It replaced the old "Not now" BUTTON:
        // as a button it was a third way to dismiss with no stated consequence, and it sat beside
        // "Postpone" implying it was NOT one, when it is exactly that — the shortest one.
        const TASKDIALOG_BUTTON radios[] = {
            { ridRestart, L"Remind me next restart" },
            { ridTomorrow, tomorrowLabel.c_str() },
            { rid3, L"Remind me in 3 days" },
            { rid7, L"Remind me in 7 days" },
            { rid30, L"Remind me in 30 days" },
            { ridSkip, L"Skip this version" },
        };

        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = owner;
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT | TDF_ENABLE_HYPERLINKS;
        cfg.pszWindowTitle = L"Agentmaster";
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszMainInstruction = instruction.c_str();
        cfg.pszContent = content.c_str();
        cfg.cButtons = ARRAYSIZE(buttons);
        cfg.pButtons = buttons;
        cfg.nDefaultButton = idUpdate;
        cfg.cRadioButtons = ARRAYSIZE(radios);
        cfg.pRadioButtons = radios;
        cfg.nDefaultRadioButton = ridRestart;
        cfg.pszFooter = footer.c_str();
        cfg.pszFooterIcon = TD_INFORMATION_ICON;
        cfg.pfCallback = UpdatePromptCallback;

        int pressed = 0, radio = ridRestart;
        if (FAILED(::TaskDialogIndirect(&cfg, &pressed, &radio, nullptr)))
        {
            return Decision::RemindNextRestart; // comctl v6 unavailable / unexpected failure -> least-destructive
        }
        switch (pressed)
        {
        case idUpdate:
            return Decision::UpdateNow;
        case idPostpone:
            // Every arm is matched EXPLICITLY and the fall-through is the write-nothing choice: an
            // unknown/absent radio must land on the default's outcome, never on a durable one.
            return radio == ridTomorrow ? Decision::PostponeTomorrow :
                   radio == rid3 ? Decision::Postpone3 :
                   radio == rid7 ? Decision::Postpone7 :
                   radio == rid30 ? Decision::Postpone30 :
                   radio == ridSkip ? Decision::Skip :
                                      Decision::RemindNextRestart;
        default:
            return Decision::RemindNextRestart; // Cancel / X / Esc
        }
    }

    // ============================ the embedded installer ============================

    // The installer PowerShell — the REAL am-update.ps1 file (src/cascadia/TerminalApp/AgentMaster/),
    // compiled into WindowsTerminal.exe as the AM_UPDATE_PS1 RT_RCDATA resource and read from the
    // binary here. It is NEVER fetched from GitHub, and never read/copied/opened as a loose file on
    // disk. It mirrors tools\Install-Agentmaster.ps1's install core + recovery (VCLibs dependency,
    // conflicting-install removal) and adds the in-app specifics (wait-for-app, relaunch, -Uninstall).
    // The dynamic values (bundle/cer URL, version, sha256, family, wait-pid) are passed as real
    // PARAMETERS by the generated .cmd — no string substitution into the script body.
    inline std::wstring InstallerPs1()
    {
        return detail::LoadResourceTextUtf8(L"AM_UPDATE_PS1");
    }

    // Quote a value for a generated .cmd line: wrap in double-quotes and double any % (cmd's escape
    // metacharacter). Our values (URLs / a version / a 64-hex digest / a PFN) never contain a quote.
    inline std::wstring CmdArg(std::wstring_view v)
    {
        std::wstring out;
        out.reserve(v.size() + 2);
        out.push_back(L'"');
        for (const wchar_t c : v)
        {
            if (c == L'%')
            {
                out += L"%%";
            }
            else
            {
                out.push_back(c);
            }
        }
        out.push_back(L'"');
        return out;
    }

    // Materialize am-update.ps1 (from the embedded resource) + a tiny am-update.cmd launcher into
    // stateDir and launch the .cmd DETACHED (it survives this process exiting). The .cmd invokes the
    // .ps1 with the resolved values as parameters. Returns true if launched — the caller MUST then
    // exit / quit the app so the package isn't in use while it upgrades + relaunches.
    inline bool LaunchInstaller(const std::wstring& stateDir, const UpdateInfo& info)
    {
        try
        {
            if (!info.installable)
            {
                return false;
            }
            // Belt on top of ParseReleaseObj's gate: never hand a non-own-repo URL to a script that
            // downloads + installs it, no matter how the UpdateInfo was assembled.
            if (!IsTrustedAssetUrl(info.bundleUrl) || !IsTrustedAssetUrl(info.cerUrl))
            {
                LogUpdate(stateDir, L"installer REFUSED (asset URL outside our release downloads)");
                return false;
            }
            const std::wstring ps1 = InstallerPs1();
            if (ps1.empty())
            {
                LogUpdate(stateDir, L"installer REFUSED (AM_UPDATE_PS1 resource missing \x2014 broken build)");
                return false; // the AM_UPDATE_PS1 resource is missing (only possible in a broken build)
            }

            std::wstring cmd = L"@echo off\r\ntitle Agentmaster Update\r\n";
            cmd += L"powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0am-update.ps1\"";
            cmd += L" -BundleUrl " + CmdArg(info.bundleUrl);
            cmd += L" -CerUrl " + CmdArg(info.cerUrl);
            cmd += L" -Version " + CmdArg(DisplayVersion(info));
            if (!info.bundleSha256.empty())
            {
                cmd += L" -BundleSha256 " + CmdArg(info.bundleSha256);
            }
            cmd += L" -Family " + CmdArg(kReleaseFamily);
            cmd += L" -WaitPid " + std::to_wstring(::GetCurrentProcessId());
            cmd += L"\r\n";

            const std::wstring ps1Path = stateDir + L"\\am-update.ps1";
            const std::wstring cmdPath = stateDir + L"\\am-update.cmd";
            try
            {
                std::filesystem::create_directories(std::filesystem::path{ stateDir });
            }
            catch (...)
            {
                // deliberately quiet: if the dir is truly unusable the very next WriteFileUtf8
                // fails AND logs ("file-write FAILED (temp open)"), so the failure is never silent
            }
            if (!detail::WriteFileUtf8(ps1Path, ps1) || !detail::WriteFileUtf8(cmdPath, cmd))
            {
                LogUpdate(stateDir, L"installer materialize FAILED (am-update.ps1 / am-update.cmd write)");
                return false;
            }
            const HINSTANCE h = ::ShellExecuteW(nullptr, L"open", cmdPath.c_str(), nullptr, stateDir.c_str(), SW_SHOWNORMAL);
            return reinterpret_cast<INT_PTR>(h) > 32;
        }
        catch (...)
        {
            // false == "not launched": the caller keeps the app open and the user can retry — the
            // one thing that must never happen is exiting the app with NO installer running.
            try
            {
                LogUpdate(stateDir, L"installer launch CRASHED (exception \x2014 not launched)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return false;
        }
    }

    // Materialize the SAME am-update.ps1 + an am-uninstall.cmd launcher that runs it with -Uninstall,
    // and launch it DETACHED. Removes THIS install's package (the current package family — release OR
    // dev), per-user, no admin; profile data (e.g. ~/.agentmaster) lives outside the package and is
    // kept. Returns true if launched (caller then quits so the package isn't in use); false if this
    // is an unpackaged build (nothing registered to remove) or the resource is missing.
    inline bool LaunchUninstaller(const std::wstring& stateDir)
    {
        try
        {
            const std::wstring family = Profiles::PackageFamilyName();
            if (family.empty())
            {
                return false; // unpackaged — there is no registered package to uninstall
            }
            const std::wstring ps1 = InstallerPs1();
            if (ps1.empty())
            {
                LogUpdate(stateDir, L"uninstaller REFUSED (AM_UPDATE_PS1 resource missing \x2014 broken build)");
                return false;
            }

            std::wstring cmd = L"@echo off\r\ntitle Agentmaster Uninstall\r\n";
            cmd += L"powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0am-update.ps1\" -Uninstall";
            cmd += L" -Family " + CmdArg(family);
            cmd += L" -WaitPid " + std::to_wstring(::GetCurrentProcessId());
            cmd += L"\r\n";

            const std::wstring ps1Path = stateDir + L"\\am-update.ps1";
            const std::wstring cmdPath = stateDir + L"\\am-uninstall.cmd";
            try
            {
                std::filesystem::create_directories(std::filesystem::path{ stateDir });
            }
            catch (...)
            {
                // deliberately quiet: if the dir is truly unusable the very next WriteFileUtf8
                // fails AND logs ("file-write FAILED (temp open)"), so the failure is never silent
            }
            if (!detail::WriteFileUtf8(ps1Path, ps1) || !detail::WriteFileUtf8(cmdPath, cmd))
            {
                LogUpdate(stateDir, L"uninstaller materialize FAILED (am-update.ps1 / am-uninstall.cmd write)");
                return false;
            }
            const HINSTANCE h = ::ShellExecuteW(nullptr, L"open", cmdPath.c_str(), nullptr, stateDir.c_str(), SW_SHOWNORMAL);
            const bool launched = reinterpret_cast<INT_PTR>(h) > 32;
            LogUpdate(stateDir, launched ? L"uninstaller launched (" + family + L") \x2014 app exiting for removal" : L"uninstaller ShellExecute FAILED");
            return launched;
        }
        catch (...)
        {
            // false == "not launched": the caller must NOT quit the app (nothing is uninstalling).
            try
            {
                LogUpdate(stateDir, L"uninstaller launch CRASHED (exception \x2014 not launched)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return false;
        }
    }


    // Apply the user's choice. Returns true IFF the installer was launched (UpdateNow + installable)
    // — the caller then exits/quits. Postpone/Skip persist to settings.json (inside the envelope);
    // Remind-next-restart latches the declined-this-run marker (no durable state — the next launch asks again);
    // UpdateNow with no installable asset opens the releases page instead. Every outcome logs an
    // [update] line — this is the ONE chokepoint every prompt (startup / hourly / cog) applies
    // through, so the decision trail is complete regardless of which surface asked.
    inline bool ApplyDecision(const std::wstring& stateDir, const UpdateInfo& info, Decision d, HWND owner)
    {
        try
        {
            constexpr long long kDayMs = 24LL * 60 * 60 * 1000;
            const long long now = NowUnixMs();
            // One honest outcome line per decision: a Postpone/Skip whose PERSIST failed must not
            // read like it stuck (the user WILL be asked again — say so).
            const auto logChoice = [&](const wchar_t* what, bool persisted) {
                LogUpdate(stateDir, L"prompt " + DisplayVersion(info) + L" -> " + what + (persisted ? L"" : L" \x2014 persist FAILED, will ask again"));
            };
            switch (d)
            {
            case Decision::UpdateNow:
                if (info.installable)
                {
                    const bool launched = LaunchInstaller(stateDir, info);
                    LogUpdate(stateDir, launched ? L"prompt " + DisplayVersion(info) + L" -> Update now; installer launched \x2014 app exiting for upgrade" :
                                                   L"prompt " + DisplayVersion(info) + L" -> Update now; installer launch FAILED (resource/write/exec) \x2014 app stays open");
                    return launched;
                }
                LogUpdate(stateDir, L"prompt " + DisplayVersion(info) + L" -> Update now (no installable assets \x2014 opening releases page)");
                ::ShellExecuteW(owner, L"open", info.htmlUrl.empty() ? kReleasesPage : info.htmlUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return false;
            case Decision::PostponeTomorrow:
            {
                // The next local 08:00, not now + 24h (NextLocalMorningUnixMs). Logged with the
                // resolved span so the log alone answers "when will it ask again?".
                const long long until = NextLocalMorningUnixMs();
                const std::wstring what = L"Postpone until tomorrow " + std::to_wstring(kPostponeMorningHour) + L":00 local (in " + FormatSpanShort(until - now) + L")";
                logChoice(what.c_str(), WritePostpone(stateDir, until));
                return false;
            }
            case Decision::Postpone3:
                logChoice(L"Postpone 3d", WritePostpone(stateDir, now + 3 * kDayMs));
                return false;
            case Decision::Postpone7:
                logChoice(L"Postpone 7d", WritePostpone(stateDir, now + 7 * kDayMs));
                return false;
            case Decision::Postpone30:
                logChoice(L"Postpone 30d", WritePostpone(stateDir, now + 30 * kDayMs));
                return false;
            case Decision::Skip:
                logChoice(L"Skip this version", WriteSkip(stateDir, info.latestTag));
                return false;
            case Decision::RemindNextRestart:
            default:
                MarkDeclinedThisRun(info.latestTag); // startup + hourly checks fully off until the next launch
                LogUpdate(stateDir, L"prompt " + DisplayVersion(info) + L" -> Remind me next restart (startup + hourly checks off until the next launch; the cog's manual check stays live)");
                return false;
            }
        }
        catch (...)
        {
            // Never let a decision crash the caller (the cog invokes this straight off a UI
            // lambda). Latch the tag like a decline so a persistently-failing path can't decay into
            // an hourly nag loop; the next LAUNCH asks again. false == installer not launched, so
            // the caller keeps the app open.
            MarkDeclinedThisRun(info.latestTag);
            try
            {
                LogUpdate(stateDir, L"decision apply CRASHED (exception \x2014 treated as remind-next-restart)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return false;
        }
    }

    // ============================ channel gate ============================

    // True when THIS install is the channel the GitHub releases actually target: the published
    // RELEASE package (Agentmaster). The updater is gated to it EVERYWHERE — the startup auto-prompt
    // AND the cog's "Check for updates" + the on-open label — because dev/unpackaged builds:
    //   * never publish to GitHub (there are no AgentmasterDev releases), and
    //   * carry the unstamped 0.0.1.0 manifest placeholder, so EVERY release looks "newer", and
    //   * would install the SEPARATE release family side-by-side (a different package) on "Update",
    //     not update themselves — a dev build updates by rebuilding.
    // So a dev cog must NOT present a release as a self-update (the confusing "dev checked an update").
    // AGENTMASTER_UPDATE_STARTUP forces it on so the full flow can still be exercised from a dev build.
    // NOTE: a LOCALLY-built RELEASE install is also 0.0.1.0 (only CI stamps the version), so it shows
    // "available" until it picks up a CI-published build — that's correct: same family, real in-place
    // update to the published release.
    // No-throw: this gate runs UNGUARDED on the launch path (_setupUpdateAutocheck) and in the cog;
    // an undeterminable channel reads as "not the updater channel" — the updater goes quiet, the
    // app never breaks.
    inline bool IsUpdaterChannel()
    {
        try
        {
            if (!detail::GetEnv(L"AGENTMASTER_UPDATE_STARTUP").empty())
            {
                return true;
            }
            return IsPackaged() && !Profiles::IsDevPackage();
        }
        catch (...)
        {
            try
            {
                LogUpdate(Profiles::ResolveProfileDir(), L"channel gate CRASHED (exception \x2014 updater treated as off for this call)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return false;
        }
    }

    // ============================ check orchestration ============================

    // The shared check core, used by BOTH the startup check and the periodic (hourly) autocheck:
    // reads prefs, gates on identity + postpone + skip, checks GitHub (bounded so a slow-but-present
    // network can't wedge the caller), prompts (Update now / Postpone next-restart·tomorrow·3·7·30 / Skip), and
    // applies the choice. Returns true IFF the installer was launched — the caller must then exit the
    // process (TerminateProcess, like the single-instance handoff) so the package isn't in use while
    // it upgrades + relaunches.
    //
    // Safe to call on the MAIN thread (startup, before the message loop) OR a BACKGROUND thread (the
    // hourly autocheck): the network round-trip runs on its own worker bounded to kDeadlineMs, and the
    // prompt is a modal Win32 TaskDialog that pumps its own nested message loop (independent of XAML,
    // so a background-thread call never touches the UI thread). Never throws. `origin` tags the
    // [update] trail ("startup" / "periodic") so each hourly tick is verifiable in hooks.log.
    inline bool RunUpdateCheckAndPrompt(HWND owner, const wchar_t* origin = L"startup")
    {
        try
        {
            if (!IsUpdaterChannel())
            {
                return false; // dev/unpackaged: the updater targets the RELEASE install (see IsUpdaterChannel)
            }
            const std::wstring stateDir = Profiles::ResolveProfileDir();
            const std::wstring tag = std::wstring{ L"check (" } + origin + L")";
            const UpdatePrefs prefs = ReadPrefs(stateDir);
            const long long nowMs = NowUnixMs();
            if (prefs.postponedUntilUnixMs > nowMs)
            {
                // Still postponed: no network check, no prompt. Logged so the hourly tick stays
                // visible (the liveness proof) even while it deliberately does nothing.
                LogUpdate(stateDir, tag + L" skipped: postponed (" + FormatSpanShort(prefs.postponedUntilUnixMs - nowMs) + L" left)");
                return false;
            }
            if (const std::wstring declined = DeclinedThisRunTag(); !declined.empty())
            {
                // A decline was taken THIS RUN (startup prompt, an earlier hourly tick, or the
                // cog's prompt) — the WHOLE check is off until the next launch: no network query,
                // no prompt, even for a newer release published mid-run. Gated BEFORE the network
                // so a decline truly quiets the hourly tick, not just its prompt. The cog's
                // explicit check never passes through here, so it stays fully live.
                LogUpdate(stateDir, tag + L" skipped: declined this run (" + declined + L" \x2014 asking again next launch)");
                return false;
            }
            const Version cur = CurrentPackageVersion();
            LogUpdate(stateDir, tag + L" begin: cur=" + VersionToString(cur) + L" prerelease=" + (prefs.allowPrerelease ? L"on" : L"off") + L" nightly=" + (prefs.allowNightly ? L"on" : L"off"));

            // Bound the TOTAL network wait so a slow-but-present network can't wedge launch: WinHTTP's
            // per-phase timeouts (resolve/connect/send/receive) could otherwise sum to ~4x, and this
            // runs synchronously before the window-restoration prompt. Run the check on a worker and
            // wait at most kDeadlineMs; if it doesn't finish, skip the prompt THIS launch (we re-check
            // next launch). The result lives in a shared_ptr so the detached worker can finish + write
            // into it harmlessly after we've moved on (no dangling reference, no leak).
            struct CheckResult
            {
                UpdateInfo info;
                std::atomic<bool> done{ false };
            };
            auto shared = std::make_shared<CheckResult>();
            std::thread([shared, cur, pre = prefs.allowPrerelease, night = prefs.allowNightly]() {
                shared->info = CheckForUpdate(cur, pre, night, 4000);
                shared->done.store(true, std::memory_order_release);
            }).detach();

            constexpr int kDeadlineMs = 6000;
            constexpr int kPollMs = 50;
            for (int waited = 0; waited < kDeadlineMs && !shared->done.load(std::memory_order_acquire); waited += kPollMs)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            }
            if (!shared->done.load(std::memory_order_acquire))
            {
                LogUpdate(stateDir, tag + L" abandoned: network slower than the 6s deadline (retry next tick/launch)");
                return false; // network too slow this launch — proceed; the detached worker self-cleans
            }
            const UpdateInfo info = shared->info; // done==true => the worker finished writing
            if (!info.checked)
            {
                LogUpdate(stateDir, tag + L" failed: " + (info.error.empty() ? std::wstring{ L"unknown" } : info.error));
                return false;
            }
            if (!info.available)
            {
                LogUpdate(stateDir, tag + L" done: up to date (latest=" + (info.latestTag.empty() ? std::wstring{ L"none" } : info.latestTag) + L")");
                return false;
            }
            if (!prefs.skippedVersion.empty() && prefs.skippedVersion == info.latestTag)
            {
                LogUpdate(stateDir, tag + L" done: " + DisplayVersion(info) + L" available but SKIPPED by the user \x2014 no prompt");
                return false; // the user skipped exactly this version
            }
            // (A this-run decline never reaches here — the presence gate above skips pre-network.)
            LogUpdate(stateDir, tag + L" done: " + DisplayVersion(info) + L" available (" + (info.isNightly ? L"NIGHTLY" : info.isPrerelease ? L"pre-release" : L"stable") + (info.installable ? L", installable) \x2014 prompting" : L", NO installable assets) \x2014 prompting"));
            const Decision d = ShowUpdatePrompt(owner, info);
            return ApplyDecision(stateDir, info, d, owner);
        }
        catch (...)
        {
            // An update check must never throw into the caller (nor block startup). Leave a trace —
            // a silent swallow here would be the one crash the [update] trail couldn't explain.
            try
            {
                LogUpdate(Profiles::ResolveProfileDir(), std::wstring{ L"check (" } + origin + L") CRASHED (exception swallowed \x2014 flow aborted this pass)");
            }
            catch (...)
            {
                // logger-failed: nothing left to report through
            }
            return false;
        }
    }

    // The startup check (WindowEmperor, BEFORE the "Reopen your N windows?" prompt). Runs on the
    // main thread — the bounded worker-and-poll inside the core keeps it from wedging launch.
    inline bool RunStartupUpdateCheck(HWND owner)
    {
        // A FRESH LAUNCH always asks again — clear a declined latch INHERITED through the env block
        // (the installer's relaunch and any child-spawned instance carry the parent's env; without
        // this, a decline could outlive its process and silently kill the new run's checks too).
        MarkDeclinedThisRun(L"");
        return RunUpdateCheckAndPrompt(owner, L"startup");
    }

    // The periodic autocheck (WindowEmperor's hourly WM_TIMER -> a detached background thread, so
    // neither the network round-trip nor the modal prompt touch the UI thread). Identical flow +
    // gates to the startup check; a distinct name only so it reads right at the call site. Returns
    // true IFF the installer was launched — the caller then exits the process so it isn't in use.
    inline bool RunPeriodicUpdateCheck(HWND owner)
    {
        return RunUpdateCheckAndPrompt(owner, L"periodic");
    }
}
