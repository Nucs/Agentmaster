// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the in-app auto-updater (header-only, plain Win32; no WinRT, no engine-lib deps,
// exactly like ProfileBootstrap.h). It checks the GitHub Releases of Nucs/Agentmaster for a newer
// version, prompts the user (Update now / Postpone [3·7·30 days / skip this version] / Not now)
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
// Persisted update state lives as three keys INSIDE settings.json (the active profile) — and
// settings.json is the engine's ENVELOPE `{version: 1, settings: {...}}` (Persistence.cpp
// SerializeAppSettings), so the keys live NESTED under "settings", never at the top level (a
// top-level key is silently DROPPED by the next engine save, which rebuilds the envelope from the
// AppSettings struct — the original schema-mismatch bug that blinded the startup/hourly checks):
//   allowUpdatePrerelease (bool)        — the cog's "Allow updating to pre-release versions" toggle
//                                         (INSTANT-APPLY: the switch itself RMWs it on flip — no Save).
//   updateSkippedVersion (string tag)   — "Skip this version" -> never re-prompt for that exact tag.
//   updatePostponedUntilUnixMs (number) — "Postpone N days" -> no check / no prompt until this time.
// skip/postpone are written by THIS module via a freshest-disk JSON read-modify-write INTO the
// envelope (so the EXE can write them too without linking the engine); AppSettings carries all three
// fields, so the engine round-trips them and the cog's Save preserves them from disk (the
// summaryPanel idiom). "Not now" persists NOTHING durable — it latches a process-scoped
// declined-this-run marker (kDeclinedEnvVar) so the hourly autocheck doesn't nag, and the next
// LAUNCH asks again (the original semantic). Every check/prompt/decision logs an "[update]" line to
// the profile's hooks.log (LogUpdate — the EXE-safe twin of the engine's AppendStateLog).

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
        for (; i < s.size(); ++i)
        {
            const wchar_t c = s[i];
            if (c >= L'0' && c <= L'9')
            {
                acc = acc * 10 + static_cast<int>(c - L'0');
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
    inline Version CurrentPackageVersion()
    {
        Version v;
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
        return v;
    }

    inline bool IsPackaged()
    {
        return !Profiles::PackageFamilyName().empty();
    }

    // ============================ small helpers ============================

    namespace detail
    {
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
                    return false;
                }
                if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    ::DeleteFileW(tmp.c_str()); // the old file stays fully intact on failure
                    return false;
                }
                return true;
            }
            catch (...)
            {
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
                    return {};
                }
                const std::string bytes{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
                return Utf8ToWide(bytes);
            }
            catch (...)
            {
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
    inline HRESULT CALLBACK UpdatePromptCallback(HWND /*hwnd*/, UINT msg, WPARAM /*wParam*/, LPARAM lParam, LONG_PTR /*ref*/)
    {
        if (msg == TDN_HYPERLINK_CLICKED && lParam)
        {
            ::ShellExecuteW(nullptr, L"open", reinterpret_cast<PCWSTR>(lParam), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return S_OK;
    }

    // A single HTTPS GET; returns the UTF-8 body bytes ("" on any failure), with a short note in
    // errOut. One timeout value governs each WinHTTP phase (resolve/connect/send/receive), so the
    // worst-case wall time is bounded — the startup caller relies on this to never wedge launch.
    inline std::string HttpsGet(const std::wstring& host, const std::wstring& path, DWORD timeoutMs, std::wstring& errOut)
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
        // GitHub requires a User-Agent; the v3 Accept header is good manners.
        const std::wstring headers = L"User-Agent: Agentmaster-Updater\r\nAccept: application/vnd.github+json\r\n";
        BOOL ok = ::WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
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
        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        return body;
    }

    // Fill an UpdateInfo from one parsed release JSON object (tag/prerelease/body/url + the
    // .msixbundle + .cer assets), and decide `available` against the current version.
    inline void ParseReleaseObj(const json::Value& rel, const Version& cur, UpdateInfo& info)
    {
        info.latestTag = rel.StrAt(L"tag_name");
        info.isPrerelease = rel.BoolAt(L"prerelease", false);
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
                if (url.empty())
                {
                    continue;
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

    // Query GitHub for the newest release. allowPrerelease -> the list endpoint, first non-draft
    // (== newest published, pre or stable); otherwise /releases/latest (excludes drafts AND
    // prereleases). Synchronous, bounded by timeoutMs per phase. Never throws.
    inline UpdateInfo CheckForUpdate(const Version& cur, bool allowPrerelease, DWORD timeoutMs = 6000)
    {
        UpdateInfo info;
        info.currentVersionStr = VersionToString(cur);
        const std::wstring base = std::wstring{ L"/repos/" } + kRepo + L"/releases";
        std::wstring err;
        try
        {
            if (allowPrerelease)
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
                    ParseReleaseObj(rel, cur, info);
                    break; // first non-draft is the newest published release
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
        const auto parsed = json::Parse(detail::ReadFileWide(SettingsPath(stateDir)));
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return p;
        }
        const json::Value* nested = parsed->Find(L"settings");
        const bool haveNested = nested && nested->type == json::Value::Type::Obj;
        const json::Value& o = haveNested ? *nested : *parsed;
        p.allowPrerelease = o.BoolAt(L"allowUpdatePrerelease", false);
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
            return false;
        }
    }

    inline void WriteSkip(const std::wstring& stateDir, const std::wstring& tag)
    {
        WriteUpdateState(stateDir, &tag, nullptr);
    }

    inline void WritePostpone(const std::wstring& stateDir, long long untilUnixMs)
    {
        WriteUpdateState(stateDir, nullptr, &untilUnixMs);
    }

    // ============================ "Not now" (declined this run) ============================

    // "Not now" means "ask me again NEXT LAUNCH" — but the hourly autocheck re-runs the same flow
    // every hour, which decayed it into an hourly nag. The latch is PROCESS-scoped, keyed by the
    // declined tag, and deliberately an ENVIRONMENT VARIABLE: Updater.h is compiled into BOTH
    // WindowsTerminal.exe (the startup + hourly checks) and TerminalApp.dll (the cog's prompt) — an
    // inline/static would exist once PER MODULE, but the env block is one per PROCESS, so a "Not
    // now" clicked on the cog's prompt also silences the EXE's hourly re-prompt. It dies with the
    // process (the next launch asks again — the original semantic), and a NEWER release appearing
    // mid-run (a different tag) still prompts. Child processes inherit it; nothing else reads it.
    inline constexpr const wchar_t* kDeclinedEnvVar = L"AGENTMASTER_UPDATE_DECLINED";

    inline void MarkDeclinedThisRun(const std::wstring& tag)
    {
        ::SetEnvironmentVariableW(kDeclinedEnvVar, tag.empty() ? nullptr : tag.c_str());
    }

    inline bool WasDeclinedThisRun(const std::wstring& tag)
    {
        return !tag.empty() && detail::GetEnv(kDeclinedEnvVar) == tag;
    }

    // ============================ the prompt ============================

    enum class Decision
    {
        NotNow, // ask again next launch (also the Cancel / X outcome)
        UpdateNow,
        Postpone3,
        Postpone7,
        Postpone30,
        Skip
    };

    // The "same question" shown at startup AND from the cog: Update now / Postpone / Not now, with
    // the postpone DURATION chosen via a radio group (3 / 7 / 30 days / skip this version) — the
    // TaskDialog analog of the requested dropdown (a TaskDialog can't host a combobox; radios are
    // the idiomatic in-dialog choice). Cancel / X == Not now (the least-destructive default).
    inline Decision ShowUpdatePrompt(HWND owner, const UpdateInfo& info)
    {
        const std::wstring instruction = L"Agentmaster " + DisplayVersion(info) + L" is available";
        std::wstring content = L"You're on v" + info.currentVersionStr + L".\n\n";
        content += info.installable ?
                       L"Choose \x201CUpdate now\x201D and Agentmaster will close and reopen automatically once the new version is installed, or pick when to be reminded." :
                       L"Choose \x201CUpdate now\x201D to open the download page, or pick when to be reminded.";

        constexpr int idUpdate = 2001, idPostpone = 2002, idNotNow = 2003;
        constexpr int rid3 = 3001, rid7 = 3002, rid30 = 3003, ridSkip = 3004;

        const TASKDIALOG_BUTTON buttons[] = {
            { idUpdate, L"Update now" },
            { idPostpone, L"Postpone" },
            { idNotNow, L"Not now" },
        };
        const TASKDIALOG_BUTTON radios[] = {
            { rid3, L"Remind me in 3 days" },
            { rid7, L"Remind me in 7 days" },
            { rid30, L"Remind me in 30 days" },
            { ridSkip, L"Skip this version" },
        };

        // "What's new" OPENS this release's GitHub page (the changelog fixated on the specific
        // release), rather than dumping notes inline — a TaskDialog hyperlink (TDF_ENABLE_HYPERLINKS)
        // that UpdatePromptCallback ShellExecutes on click. The URL has no chars that need escaping
        // inside the <a href="…"> markup (a GitHub release URL).
        const std::wstring footer = L"<a href=\"" + ChangelogUrl(info) + L"\">What's new \x2192</a>";

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
        cfg.nDefaultRadioButton = rid7;
        cfg.pszFooter = footer.c_str();
        cfg.pszFooterIcon = TD_INFORMATION_ICON;
        cfg.pfCallback = UpdatePromptCallback;

        int pressed = 0, radio = rid7;
        if (FAILED(::TaskDialogIndirect(&cfg, &pressed, &radio, nullptr)))
        {
            return Decision::NotNow; // comctl v6 unavailable / unexpected failure -> least-destructive
        }
        switch (pressed)
        {
        case idUpdate:
            return Decision::UpdateNow;
        case idPostpone:
            return radio == rid3 ? Decision::Postpone3 :
                   radio == rid30 ? Decision::Postpone30 :
                   radio == ridSkip ? Decision::Skip :
                                      Decision::Postpone7;
        default:
            return Decision::NotNow; // Not now / Cancel / X
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
        if (!info.installable)
        {
            return false;
        }
        const std::wstring ps1 = InstallerPs1();
        if (ps1.empty())
        {
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
        }
        if (!detail::WriteFileUtf8(ps1Path, ps1) || !detail::WriteFileUtf8(cmdPath, cmd))
        {
            LogUpdate(stateDir, L"installer materialize FAILED (am-update.ps1 / am-update.cmd write)");
            return false;
        }
        const HINSTANCE h = ::ShellExecuteW(nullptr, L"open", cmdPath.c_str(), nullptr, stateDir.c_str(), SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(h) > 32;
    }

    // Materialize the SAME am-update.ps1 + an am-uninstall.cmd launcher that runs it with -Uninstall,
    // and launch it DETACHED. Removes THIS install's package (the current package family — release OR
    // dev), per-user, no admin; profile data (e.g. ~/.agentmaster) lives outside the package and is
    // kept. Returns true if launched (caller then quits so the package isn't in use); false if this
    // is an unpackaged build (nothing registered to remove) or the resource is missing.
    inline bool LaunchUninstaller(const std::wstring& stateDir)
    {
        const std::wstring family = Profiles::PackageFamilyName();
        if (family.empty())
        {
            return false; // unpackaged — there is no registered package to uninstall
        }
        const std::wstring ps1 = InstallerPs1();
        if (ps1.empty())
        {
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


    // Apply the user's choice. Returns true IFF the installer was launched (UpdateNow + installable)
    // — the caller then exits/quits. Postpone/Skip persist to settings.json (inside the envelope);
    // Not now latches the declined-this-run marker (no durable state — the next launch asks again);
    // UpdateNow with no installable asset opens the releases page instead. Every outcome logs an
    // [update] line — this is the ONE chokepoint every prompt (startup / hourly / cog) applies
    // through, so the decision trail is complete regardless of which surface asked.
    inline bool ApplyDecision(const std::wstring& stateDir, const UpdateInfo& info, Decision d, HWND owner)
    {
        constexpr long long kDayMs = 24LL * 60 * 60 * 1000;
        const long long now = NowUnixMs();
        switch (d)
        {
        case Decision::UpdateNow:
            if (info.installable)
            {
                const bool launched = LaunchInstaller(stateDir, info);
                LogUpdate(stateDir, launched ? L"prompt " + DisplayVersion(info) + L" -> Update now; installer launched \x2014 app exiting for upgrade" :
                                               L"prompt " + DisplayVersion(info) + L" -> Update now; installer launch FAILED (resource/write/exec)");
                return launched;
            }
            LogUpdate(stateDir, L"prompt " + DisplayVersion(info) + L" -> Update now (no installable assets \x2014 opening releases page)");
            ::ShellExecuteW(owner, L"open", info.htmlUrl.empty() ? kReleasesPage : info.htmlUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return false;
        case Decision::Postpone3:
            WritePostpone(stateDir, now + 3 * kDayMs);
            LogUpdate(stateDir, L"prompt " + DisplayVersion(info) + L" -> Postpone 3d");
            return false;
        case Decision::Postpone7:
            WritePostpone(stateDir, now + 7 * kDayMs);
            LogUpdate(stateDir, L"prompt " + DisplayVersion(info) + L" -> Postpone 7d");
            return false;
        case Decision::Postpone30:
            WritePostpone(stateDir, now + 30 * kDayMs);
            LogUpdate(stateDir, L"prompt " + DisplayVersion(info) + L" -> Postpone 30d");
            return false;
        case Decision::Skip:
            WriteSkip(stateDir, info.latestTag);
            LogUpdate(stateDir, L"prompt " + DisplayVersion(info) + L" -> Skip this version");
            return false;
        case Decision::NotNow:
        default:
            MarkDeclinedThisRun(info.latestTag); // silence the hourly re-prompt for THIS tag, this run
            LogUpdate(stateDir, L"prompt " + DisplayVersion(info) + L" -> Not now (asking again next launch; hourly re-prompt latched off)");
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
    inline bool IsUpdaterChannel()
    {
        if (!detail::GetEnv(L"AGENTMASTER_UPDATE_STARTUP").empty())
        {
            return true;
        }
        return IsPackaged() && !Profiles::IsDevPackage();
    }

    // ============================ check orchestration ============================

    // The shared check core, used by BOTH the startup check and the periodic (hourly) autocheck:
    // reads prefs, gates on identity + postpone + skip, checks GitHub (bounded so a slow-but-present
    // network can't wedge the caller), prompts (Update now / Postpone 3·7·30 / Skip / Not now), and
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
            const Version cur = CurrentPackageVersion();
            LogUpdate(stateDir, tag + L" begin: cur=" + VersionToString(cur) + L" prerelease=" + (prefs.allowPrerelease ? L"on" : L"off"));

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
            std::thread([shared, cur, pre = prefs.allowPrerelease]() {
                shared->info = CheckForUpdate(cur, pre, 4000);
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
            if (WasDeclinedThisRun(info.latestTag))
            {
                // "Not now" was clicked for THIS tag earlier in this run (startup prompt, an earlier
                // hourly tick, or the cog's prompt) — asking again is the next LAUNCH's job.
                LogUpdate(stateDir, tag + L" done: " + DisplayVersion(info) + L" available but declined this run \x2014 no re-prompt");
                return false;
            }
            LogUpdate(stateDir, tag + L" done: " + DisplayVersion(info) + L" available (" + (info.isPrerelease ? L"pre-release" : L"stable") + (info.installable ? L", installable) \x2014 prompting" : L", NO installable assets) \x2014 prompting"));
            const Decision d = ShowUpdatePrompt(owner, info);
            return ApplyDecision(stateDir, info, d, owner);
        }
        catch (...)
        {
            return false; // an update check must never throw into the caller (nor block startup)
        }
    }

    // The startup check (WindowEmperor, BEFORE the "Reopen your N windows?" prompt). Runs on the
    // main thread — the bounded worker-and-poll inside the core keeps it from wedging launch.
    inline bool RunStartupUpdateCheck(HWND owner)
    {
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
