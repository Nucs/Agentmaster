// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — the WindowsTerminal.exe BACKWARDS-COMPATIBILITY shim.
//
// The GUI binary was renamed WindowsTerminal.exe -> Agentmaster.exe (WindowsTerminal.vcxproj's
// TargetName). This tiny GUI-subsystem exe keeps the OLD name alive next to the new one: it
// forwards its ENTIRE commandline to the neighbor Agentmaster.exe and exits, so anything that
// still launches WindowsTerminal.exe — an old Start-menu / taskbar shortcut, a script, muscle
// memory, an old portable-folder icon — lands in the ONE real app.
//
// WHY A SHIM AND NOT A COPY OF THE BINARY: WindowEmperor derives its single-instance identity
// from the process IMAGE path (unpackaged: a hash of the full exe path; see WindowEmperor.cpp).
// A literal second copy named WindowsTerminal.exe would hash to a DIFFERENT identity than
// Agentmaster.exe, so the two would NOT hand off to each other — launching the old name would
// spin up a SECOND independent instance on the same profile (the profile mutex would then warn).
// Forwarding into Agentmaster.exe keeps exactly one single-instance identity — the real one — so
// this shim behaviourally IS Agentmaster.exe. Its .rc embeds the SAME branded terminal icon, so
// it also LOOKS identical in Explorer and on any pinned shortcut.
//
// It is NOT referenced by the appxmanifest (the Application + all COM ExeServers name
// Agentmaster.exe directly), so defterm/COM handoff and Start-menu/AUMID activation never touch
// this shim — only a by-name launch of WindowsTerminal.exe does. It rides into the MSIX and the
// portable zip as an ordinary payload (CascadiaPackage.wapproj flattens each referenced project's
// exe into the package root).

#include <string>
#include <string_view>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wil/stl.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>

namespace
{
    // The commandline with argv[0] (the program path/name) stripped — i.e. the classic wWinMain
    // pCmdLine — preserving the ORIGINAL quoting/spacing of the remaining args verbatim, so the
    // forward below is byte-for-byte what a direct Agentmaster.exe launch would have received.
    // (The same tail-extraction the wt launcher shim uses.)
    std::wstring_view CommandlineTail()
    {
        std::wstring_view s{ GetCommandLineW() };
        if (!s.empty() && s.front() == L'"')
        {
            const auto e = s.find(L'"', 1);
            s = (e == std::wstring_view::npos) ? std::wstring_view{} : s.substr(e + 1);
        }
        else
        {
            const auto e = s.find(L' ');
            s = (e == std::wstring_view::npos) ? std::wstring_view{} : s.substr(e);
        }
        while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
        {
            s.remove_prefix(1);
        }
        return s;
    }
}

// GUI subsystem (SubSystem=Windows) — no console is ever allocated, so a launch from Explorer or a
// shortcut never flashes one, and this creates no window of its own.
#pragma warning(suppress : 26461) // we can't change the wWinMain signature
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    std::filesystem::path module{ wil::GetModuleFileNameW<std::wstring>(nullptr) };
    // The real GUI binary, sitting right next to us in the same folder.
    module.replace_filename(L"Agentmaster.exe");

    const std::wstring_view tail = CommandlineTail();

    // argv[0] = the real target path (QUOTED — an install path may contain spaces), then the
    // verbatim tail. With lpApplicationName set, the child's GetCommandLineW() is exactly this
    // string, so WindowEmperor parses it identically to a direct Agentmaster.exe launch (it
    // strips argv[0] and derives single-instance identity from the process image, not argv[0]).
    std::wstring cmdline;
    if (FAILED(wil::str_printf_nothrow(cmdline, L"\"%s\" %.*s", module.c_str(), static_cast<int>(tail.size()), tail.data())))
    {
        return 1;
    }

    // Forward our startup info (show state, std handles) like the wt shim does.
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    GetStartupInfoW(&si);

    // Fire-and-forget: the real app is single-instance (a duplicate hands off + exits at once), so
    // there is nothing to wait for — exactly like Agentmaster.exe launched directly.
    wil::unique_process_information pi;
    return !CreateProcessW(module.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
}
