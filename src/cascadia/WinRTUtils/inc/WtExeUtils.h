// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#pragma once

constexpr std::wstring_view WtExe{ L"wt.exe" };
constexpr std::wstring_view WtdExe{ L"wtd.exe" };
// Agentmaster: the GUI binary is Agentmaster.exe (WindowsTerminal.vcxproj's TargetName — the
// PROJECT keeps upstream's name, the OUTPUT is ours). The identifier keeps its upstream spelling
// so the call sites stay minimal diffs.
constexpr std::wstring_view WindowsTerminalExe{ L"Agentmaster.exe" };
constexpr std::wstring_view LocalAppDataAppsPath{ L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\" };
constexpr std::wstring_view ElevateShimExe{ L"elevate-shim.exe" };

// Forward declared from appmodel.h so that we don't need to pull in that header everywhere.
extern "C" {
WINBASEAPI LONG WINAPI GetCurrentPackageId(UINT32* bufferLength, BYTE* buffer);
}

#ifdef WINRT_Windows_ApplicationModel_H
_TIL_INLINEPREFIX bool IsPackaged()
{
    static const auto isPackaged = []() {
        UINT32 bufferLength = 0;
        const auto hr = GetCurrentPackageId(&bufferLength, nullptr);
        return hr != APPMODEL_ERROR_NO_PACKAGE;
    }();
    return isPackaged;
}

// Function Description:
// - This is a helper to determine if we're running as a part of the Dev Build
//   Package or the release package. We'll need to return different text, icons,
//   and use different commandlines depending on which one the user requested.
// - Uses a C++11 "magic static" to make sure this is only computed once.
// - If we can't determine if it's the dev build or not, we'll default to true
// Arguments:
// - <none>
// Return Value:
// - true if we believe this extension is being run in the dev build package.
_TIL_INLINEPREFIX bool IsDevBuild()
{
    // use C++11 magic statics to make sure we only do this once.
    static const auto isDevBuild = []() -> bool {
        if (IsPackaged())
        {
            try
            {
                const auto package = winrt::Windows::ApplicationModel::Package::Current();
                const auto id = package.Id();
                const auto name = id.FullName();
                return til::starts_with(name, L"WindowsTerminalDev");
            }
            CATCH_LOG();
        }

        return true;
    }();
    return isDevBuild;
}

// Function Description:
// - Helper function for getting the path to the appropriate executable to use
//   for this instance of the shell extension. If we're running the dev build,
//   it should be a `wtd.exe`, but if we're preview or release, we want to make
//   sure to get the correct `wt.exe` that corresponds to _us_.
// - If we're unpackaged, this needs to get us `Agentmaster.exe`, because
//   the execution alias won't have been installed for this install.
// Arguments:
// - <none>
// Return Value:
// - the full path to the exe: our per-identity alias, or the neighbor `Agentmaster.exe`.
_TIL_INLINEPREFIX const std::wstring& GetWtExePath()
{
    static const auto exePath = []() -> std::wstring {
        // First, check a packaged location for the exe. If we've got a package
        // family name, that means we're one of the packaged Dev build, packaged
        // Release build, or packaged Preview build.
        //
        // If we're the preview or release build, there's no way of knowing if the
        // `wt.exe` on the %PATH% is us or not. Fortunately, _our_ execution alias
        // is located in "%LOCALAPPDATA%\Microsoft\WindowsApps\<our package family
        // name>", _always_, so we can use that to look up the exe easier.
        if (IsPackaged())
        {
            try
            {
                const auto package = winrt::Windows::ApplicationModel::Package::Current();
                const auto id = package.Id();
                const auto pfn = id.FamilyName();
                if (!pfn.empty())
                {
                    const std::filesystem::path windowsAppsPath{ wil::ExpandEnvironmentStringsW<std::wstring>(LocalAppDataAppsPath.data()) };
                    // Agentmaster: our packages register their OWN execution aliases (see
                    // Package-Rel.appxmanifest / Package-Dev.appxmanifest), NOT wt.exe/wtd.exe — so the
                    // upstream assumption resolves to a non-existent <PFN>\wt.exe and every launcher
                    // (new-window, jump list, ...) silently fails. Pick OUR alias by package family
                    // name; the alias is per-IDENTITY (release = agentmaster.exe, the AgentmasterDev
                    // package = agentmasterdev.exe) so a side-by-side release+dev pair can never
                    // launch each other. ORDER MATTERS: "Agentmaster" is a prefix of "AgentmasterDev",
                    // so test Dev first. Verified: ShellExecuteEx on this full <PFN>\<alias> path
                    // activates our packaged app and hands off.
                    const std::wstring_view pfnView{ pfn };
                    const std::wstring_view alias = til::starts_with(pfnView, std::wstring_view{ L"AgentmasterDev" }) ?
                                                        std::wstring_view{ L"agentmasterdev.exe" } :
                                                    til::starts_with(pfnView, std::wstring_view{ L"Agentmaster" }) ?
                                                        std::wstring_view{ L"agentmaster.exe" } :
                                                        (IsDevBuild() ? WtdExe : WtExe);
                    const auto wtPath = windowsAppsPath / std::wstring_view{ pfn } / alias;
                    return wtPath;
                }
            }
            CATCH_LOG();
        }

        // If we're here, then we couldn't resolve our exe from the package. This
        // means we're running unpackaged. We should just use the
        // Agentmaster.exe that's sitting in the directory next to us.
        try
        {
            std::filesystem::path module = wil::GetModuleFileNameW<std::wstring>(nullptr);
            module.replace_filename(WindowsTerminalExe);
            return module;
        }
        CATCH_LOG();

        return std::wstring{ WtExe };
    }();
    return exePath;
}
#endif

// Method Description:
// - Quotes and escapes the given string so that it can be used as a command-line arg.
// - e.g. given `\";foo\` will return `"\\\"\;foo\\"` so that the caller can construct a command-line
//   using something such as `fmt::format(FMT_COMPILE(L"wt --title {}"), QuoteAndQuoteAndEscapeCommandlineArg(TabTitle()))`.
// Arguments:
// - arg - the command-line argument to quote and escape.
// Return Value:
// - the quoted and escaped command-line argument.
inline void QuoteAndEscapeCommandlineArg(const std::wstring_view& arg, std::wstring& out)
{
    out.push_back(L'"');

    size_t backslashes = 0;
    for (const auto ch : arg)
    {
        if (ch == L'\\')
        {
            backslashes++;
        }
        else
        {
            if (ch == L';' || ch == L'"')
            {
                out.append(backslashes + 1, L'\\');
            }
            backslashes = 0;
        }
        out.push_back(ch);
    }

    out.append(backslashes, L'\\');
    out.push_back(L'"');
}

_TIL_INLINEPREFIX std::wstring QuoteAndEscapeCommandlineArg(const std::wstring_view& arg)
{
    std::wstring out;
    out.reserve(arg.size() + 2);
    QuoteAndEscapeCommandlineArg(arg, out);
    return out;
}
