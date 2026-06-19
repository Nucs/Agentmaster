// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Agentmaster — the in-app auto-updater (header-only, plain Win32; no WinRT, no engine-lib deps,
// exactly like ProfileBootstrap.h). It checks the GitHub Releases of Nucs/Agentmaster for a newer
// version, prompts the user (Update now / Postpone [3·7·30 days / skip this version] / Not now)
// with a Win32 TaskDialog, and — on Update — writes an EMBEDDED installer script (am-update.cmd +
// am-update.ps1, baked into THIS header) into the active profile dir and launches it detached to
// download + cert-trust + Add-AppxPackage the new .msixbundle and relaunch. The install script is
// BAKED IN here, never fetched from GitHub.
//
// Included by BOTH the WindowsTerminal EXE (the startup pre-restoration check — it asks BEFORE the
// "Reopen your N windows?" prompt) AND TerminalApp.dll's Settings cog ("Check for updates" button +
// the "vX.Y.Z available!" label). That dual use is WHY this is header-only + pure Win32: the EXE
// links TerminalApp.dll, not the TerminalAppLib static lib, so it cannot call the engine's JSON /
// settings helpers — but it CAN include this header (and Json.h, which is itself header-only) just
// as it includes ProfileBootstrap.h.
//
// Persisted update state lives as three keys INSIDE settings.json (the active profile):
//   allowUpdatePrerelease (bool)        — the cog's "Allow updating to pre-release versions" toggle.
//   updateSkippedVersion (string tag)   — "Skip this version" -> never re-prompt for that exact tag.
//   updatePostponedUntilUnixMs (number) — "Postpone N days" -> no check / no prompt until this time.
// The cog FORM owns allowUpdatePrerelease (it round-trips through AppSettings); skip/postpone are
// written by THIS module via a freshest-disk JSON read-modify-write (so the EXE can write them too
// without linking the engine), and the cog's Save preserves them from disk (the summaryPanel idiom).

#pragma once

#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>
#include <shellapi.h>
#include <appmodel.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
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

        inline bool WriteFileUtf8(const std::wstring& path, std::wstring_view content)
        {
            try
            {
                std::ofstream f{ std::filesystem::path{ path }, std::ios::binary | std::ios::trunc };
                if (!f)
                {
                    return false;
                }
                const auto bytes = WideToUtf8(content);
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                return f.good();
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
        std::wstring cerUrl; // .cer browser_download_url
        std::wstring notes; // release body (for the "What's new" expander)
        std::wstring htmlUrl; // release page (the fallback when there's no installable asset)
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

    inline UpdatePrefs ReadPrefs(const std::wstring& stateDir)
    {
        UpdatePrefs p;
        const auto parsed = json::Parse(detail::ReadFileWide(SettingsPath(stateDir)));
        if (parsed && parsed->type == json::Value::Type::Obj)
        {
            p.allowPrerelease = parsed->BoolAt(L"allowUpdatePrerelease", false);
            p.skippedVersion = parsed->StrAt(L"updateSkippedVersion");
            p.postponedUntilUnixMs = parsed->I64At(L"updatePostponedUntilUnixMs", 0);
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

    // Freshest-disk read-modify-write of settings.json: parse the whole file, set just the given
    // update key(s), re-serialize (every other key is preserved verbatim). The EXE uses this to
    // persist a Skip/Postpone choice without linking the engine; the cog's Save preserves these
    // same keys from disk so a form Save never regresses them.
    inline bool WriteUpdateState(const std::wstring& stateDir, const std::wstring* skipTag, const long long* postponeMs)
    {
        try
        {
            json::Value obj;
            const auto parsed = json::Parse(detail::ReadFileWide(SettingsPath(stateDir)));
            obj = (parsed && parsed->type == json::Value::Type::Obj) ? *parsed : json::Value::MkObj();
            if (skipTag)
            {
                SetMember(obj, L"updateSkippedVersion", json::Value::MkStr(*skipTag));
            }
            if (postponeMs)
            {
                SetMember(obj, L"updatePostponedUntilUnixMs", json::Value::MkNum(static_cast<double>(*postponeMs)));
            }
            std::filesystem::create_directories(std::filesystem::path{ stateDir });
            return detail::WriteFileUtf8(SettingsPath(stateDir), json::Dump(obj));
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

        std::wstring notes = info.notes;
        if (notes.size() > 1200)
        {
            notes = notes.substr(0, 1200) + L"\n\x2026";
        }

        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = owner;
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT | TDF_EXPAND_FOOTER_AREA;
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
        if (!notes.empty())
        {
            cfg.pszExpandedInformation = notes.c_str();
            cfg.pszExpandedControlText = L"What's new";
            cfg.pszCollapsedControlText = L"What's new";
        }

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

    // The installer PowerShell — BAKED IN here (never fetched from GitHub). Given direct asset URLs
    // (the app already resolved them via the API), it downloads the .msixbundle + .cer, trusts the
    // self-signed cert (elevating only if it isn't already trusted), Add-AppxPackages the bundle
    // (with the VCLibs-dependency fallback), and relaunches. {{TOKENS}} are substituted below.
    inline std::wstring InstallerPs1()
    {
        return LR"PSUPD(# Agentmaster in-app updater (generated; baked into the app, not fetched from GitHub).
$ErrorActionPreference = 'Stop'
$BundleUrl = '{{BUNDLE_URL}}'
$CerUrl    = '{{CER_URL}}'
$Version   = '{{VERSION}}'
$Aumid     = '{{AUMID}}'
$WaitPid   = '{{WAIT_PID}}'
$Dir       = Join-Path $env:TEMP 'Agentmaster-update'

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "    $m" -ForegroundColor Green }
function Warn($m) { Write-Host "    $m" -ForegroundColor Yellow }

try {
  Write-Host ''
  Write-Host "Agentmaster updater - installing $Version" -ForegroundColor White
  Write-Host ''

  try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
  if ($PSVersionTable.PSVersion.Major -ge 7) { Import-Module Appx -UseWindowsPowerShell -WarningAction SilentlyContinue -ErrorAction SilentlyContinue }

  New-Item -ItemType Directory -Force -Path $Dir | Out-Null
  $bundle = Join-Path $Dir ([IO.Path]::GetFileName(([Uri]$BundleUrl).AbsolutePath))
  $cer    = Join-Path $Dir 'Agentmaster.cer'

  Step 'Downloading the update'
  $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
  try {
    Invoke-WebRequest -Uri $CerUrl    -OutFile $cer    -UseBasicParsing -Headers @{ 'User-Agent' = 'Agentmaster-Updater' }
    Invoke-WebRequest -Uri $BundleUrl -OutFile $bundle -UseBasicParsing -Headers @{ 'User-Agent' = 'Agentmaster-Updater' }
  } finally { $ProgressPreference = $old }
  Ok 'downloaded'

  Step 'Trusting the signing certificate (one-time; may prompt for administrator)'
  $cert  = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 ((Resolve-Path -LiteralPath $cer).Path)
  $thumb = $cert.Thumbprint
  if (Test-Path "Cert:\LocalMachine\TrustedPeople\$thumb") {
    Ok 'already trusted'
  } else {
    $isAdmin = (New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) {
      Import-Certificate -FilePath ((Resolve-Path -LiteralPath $cer).Path) -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null
    } else {
      $p = ((Resolve-Path -LiteralPath $cer).Path).Replace("'","''")
      $inner = "try { Import-Certificate -FilePath '$p' -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' -ErrorAction Stop | Out-Null; exit 0 } catch { exit 7 }"
      $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($inner))
      $proc = Start-Process -FilePath 'powershell.exe' -Verb RunAs -PassThru -Wait -WindowStyle Hidden -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand',$enc
      if ($proc.ExitCode -ne 0) { throw 'Administrator elevation was cancelled or failed; the signing certificate must be trusted to install the update.' }
    }
    Ok 'trusted'
  }

  if ($WaitPid -match '^\d+$' -and [int]$WaitPid -gt 0) {
    Step 'Waiting for Agentmaster to close'
    try { Wait-Process -Id ([int]$WaitPid) -Timeout 20 -ErrorAction SilentlyContinue } catch {}
  }

  Step "Installing $Version"
  try {
    Add-AppxPackage -Path $bundle -ForceUpdateFromAnyVersion -ForceApplicationShutdown -ErrorAction Stop
  } catch {
    $msg = $_.Exception.Message
    if ($msg -match '0x80073CF3' -or $msg -match 'dependency') {
      $arch = switch ($env:PROCESSOR_ARCHITECTURE) { 'ARM64' { 'arm64' } 'AMD64' { 'x64' } default { 'x64' } }
      $vc = Join-Path $Dir "Microsoft.VCLibs.$arch.14.00.Desktop.appx"
      Invoke-WebRequest -Uri "https://aka.ms/Microsoft.VCLibs.$arch.14.00.Desktop.appx" -OutFile $vc -UseBasicParsing
      Add-AppxPackage -Path $bundle -DependencyPath $vc -ForceUpdateFromAnyVersion -ForceApplicationShutdown -ErrorAction Stop
    } else { throw }
  }
  Ok 'installed'

  Step 'Relaunching Agentmaster'
  Start-Process "shell:appsFolder\$Aumid"
  Write-Host ''
  Write-Host "Updated to $Version." -ForegroundColor White
  Start-Sleep -Seconds 2
}
catch {
  Write-Host ''
  Write-Host "Update failed: $($_.Exception.Message)" -ForegroundColor Red
  Write-Host 'You can download the latest release manually from:' -ForegroundColor DarkGray
  Write-Host '    https://github.com/Nucs/Agentmaster/releases' -ForegroundColor DarkGray
  Write-Host ''
  Write-Host 'Press Enter to close...' -ForegroundColor DarkGray
  try { Read-Host | Out-Null } catch {}
}
)PSUPD";
    }

    // The launched entry point — a tiny .bat/.cmd that runs the .ps1 sitting beside it (%~dp0 is
    // the script's own folder == the profile dir we write both into). This is "the installer bat
    // script embedded resource" the design calls for; the heavy lifting (Add-AppxPackage, cert
    // trust) is the PowerShell payload it invokes.
    inline std::wstring InstallerCmd()
    {
        return LR"CMDUPD(@echo off
title Agentmaster Update
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0am-update.ps1"
)CMDUPD";
    }

    // Materialize am-update.cmd + am-update.ps1 into stateDir and launch the .cmd DETACHED (it
    // survives this process exiting). Returns true if launched — the caller MUST then exit / quit
    // the app so the package isn't in use while it upgrades + relaunches.
    inline bool LaunchInstaller(const std::wstring& stateDir, const UpdateInfo& info)
    {
        if (!info.installable)
        {
            return false;
        }
        std::wstring ps1 = InstallerPs1();
        detail::ReplaceAll(ps1, L"{{BUNDLE_URL}}", info.bundleUrl);
        detail::ReplaceAll(ps1, L"{{CER_URL}}", info.cerUrl);
        detail::ReplaceAll(ps1, L"{{VERSION}}", DisplayVersion(info));
        detail::ReplaceAll(ps1, L"{{AUMID}}", kReleaseAumid);
        detail::ReplaceAll(ps1, L"{{WAIT_PID}}", std::to_wstring(::GetCurrentProcessId()));

        const std::wstring ps1Path = stateDir + L"\\am-update.ps1";
        const std::wstring cmdPath = stateDir + L"\\am-update.cmd";
        try
        {
            std::filesystem::create_directories(std::filesystem::path{ stateDir });
        }
        catch (...)
        {
        }
        if (!detail::WriteFileUtf8(ps1Path, ps1) || !detail::WriteFileUtf8(cmdPath, InstallerCmd()))
        {
            return false;
        }
        const HINSTANCE h = ::ShellExecuteW(nullptr, L"open", cmdPath.c_str(), nullptr, stateDir.c_str(), SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(h) > 32;
    }

    // Apply the user's choice. Returns true IFF the installer was launched (UpdateNow + installable)
    // — the caller then exits/quits. Postpone/Skip persist to settings.json; Not now does nothing;
    // UpdateNow with no installable asset opens the releases page instead.
    inline bool ApplyDecision(const std::wstring& stateDir, const UpdateInfo& info, Decision d, HWND owner)
    {
        constexpr long long kDayMs = 24LL * 60 * 60 * 1000;
        const long long now = NowUnixMs();
        switch (d)
        {
        case Decision::UpdateNow:
            if (info.installable)
            {
                return LaunchInstaller(stateDir, info);
            }
            ::ShellExecuteW(owner, L"open", info.htmlUrl.empty() ? kReleasesPage : info.htmlUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return false;
        case Decision::Postpone3:
            WritePostpone(stateDir, now + 3 * kDayMs);
            return false;
        case Decision::Postpone7:
            WritePostpone(stateDir, now + 7 * kDayMs);
            return false;
        case Decision::Postpone30:
            WritePostpone(stateDir, now + 30 * kDayMs);
            return false;
        case Decision::Skip:
            WriteSkip(stateDir, info.latestTag);
            return false;
        case Decision::NotNow:
        default:
            return false;
        }
    }

    // ============================ startup orchestration ============================

    // The startup check (WindowEmperor, BEFORE the "Reopen your N windows?" prompt). Reads prefs,
    // gates on identity + postpone + skip, checks GitHub (bounded), prompts, and applies the
    // choice. Returns true IFF the installer was launched — the caller must then exit the process
    // (TerminateProcess, like the single-instance handoff) so the package isn't in use.
    //
    // Gated to the published RELEASE install: dev/unpackaged builds never publish to GitHub, so an
    // auto-nag there would always fire — skip them unless AGENTMASTER_UPDATE_STARTUP is set (so the
    // full startup path can still be exercised from a dev build). The cog's "Check for updates"
    // button is NOT gated (it's an explicit manual action, available on any build).
    inline bool RunStartupUpdateCheck(HWND owner)
    {
        try
        {
            const bool force = !detail::GetEnv(L"AGENTMASTER_UPDATE_STARTUP").empty();
            if (!force)
            {
                if (!IsPackaged() || Profiles::IsDevPackage())
                {
                    return false;
                }
            }
            const std::wstring stateDir = Profiles::ResolveProfileDir();
            const UpdatePrefs prefs = ReadPrefs(stateDir);
            if (prefs.postponedUntilUnixMs > NowUnixMs())
            {
                return false; // still postponed: no network check, no prompt
            }
            const Version cur = CurrentPackageVersion();
            const UpdateInfo info = CheckForUpdate(cur, prefs.allowPrerelease, 4000);
            if (!info.available)
            {
                return false;
            }
            if (!prefs.skippedVersion.empty() && prefs.skippedVersion == info.latestTag)
            {
                return false; // the user skipped exactly this version
            }
            const Decision d = ShowUpdatePrompt(owner, info);
            return ApplyDecision(stateDir, info, d, owner);
        }
        catch (...)
        {
            return false; // an update check must never block startup
        }
    }
}
