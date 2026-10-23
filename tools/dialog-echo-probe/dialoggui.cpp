// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// A GUI-subsystem stand-in for Agentmaster.exe that opens REAL dialogs through DialogEcho.h, exactly as the app's
// startup does: CallerSink::Capture() first (wWinMain), the dialog, then CallerSink::RequestRelease() (WindowEmperor
// before its message loop). Every dialog answers itself, so the probe needs no hands, and run by launcher.exe it opens
// on a hidden desktop, never on the user's screen. Exit code = the dialog's answer (IDYES 6, IDNO 7, a button id).
//   dialoggui msgbox     — the "profile in use" box (Yes / No, No by default), answered No
//   dialoggui taskdialog — the update prompt's shape (2 buttons, radio options, a link footer), answered Postpone + 3 days
//   dialoggui deferred   — the --help box path: a Hold taken, the release requested, the box shown on another thread
//   dialoggui none       — no dialog: a sink is captured and released, and nothing must be printed
//   dialoggui diag       — no dialog; first writes diag.txt beside the exe: the AGENTMASTER_CALLER_SINK value and the
//                          standard handles this process was started with, BEFORE Capture() consumes them. For a FAIL of
//                          run.sh's shim checks: it shows what the launcher shim handed over (no diag.txt at all = the
//                          stand-in never ran its first line; check its exit code)

#include <windows.h>

#include "DialogEcho.h"

#include <memory>
#include <string>
#include <thread>

// TaskDialogIndirect exists only in Common Controls v6, which an exe gets only by asking in its manifest; without this
// the stand-in binds System32's v5.82 and dies at load with STATUS_ORDINAL_NOT_FOUND (0xC0000138). The app asks in
// WindowsTerminal.manifest. build.cmd links with /MANIFEST:EMBED so the manifest travels INSIDE the exe: the copy that
// build.cmd places in shim\ under another name would find no separate dialoggui.exe.manifest beside it.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
    // Clicks `buttonId` in the first dialog window titled `caption` that appears on this process's desktop (a message
    // box answers WM_COMMAND). Polls for up to 5 s; a dialog that never appears leaves the probe waiting, which run.sh's
    // timeout reports.
    void AnswerMessageBox(const wchar_t* caption, int buttonId)
    {
        std::thread([caption, buttonId] {
            for (int i = 0; i < 200; ++i)
            {
                ::Sleep(25);
                if (const HWND dialog = ::FindWindowW(L"#32770", caption))
                {
                    ::Sleep(150); // let the box finish appearing
                    // Click the button itself (BM_CLICK on the dialog's control), as a user would. A box whose only
                    // button is OK has no control with id IDOK (it ignored WM_COMMAND IDOK in the first runs of this
                    // probe), so with no control of that id the first button of the box is clicked.
                    HWND button = ::GetDlgItem(dialog, buttonId);
                    if (!button)
                    {
                        button = ::FindWindowExW(dialog, nullptr, L"Button", nullptr);
                    }
                    if (button)
                    {
                        ::PostMessageW(button, BM_CLICK, 0, 0);
                    }
                    return;
                }
            }
        }).detach();
    }

    // The task dialog answers itself once it exists: the "Remind me in 3 days" option, then "Postpone".
    HRESULT CALLBACK AnswerTaskDialog(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR) noexcept
    {
        if (msg == TDN_CREATED)
        {
            ::PostMessageW(hwnd, TDM_CLICK_RADIO_BUTTON, 3002, 0);
            ::PostMessageW(hwnd, TDM_CLICK_BUTTON, 2002, 0);
        }
        return S_OK;
    }

    // The update prompt's shape, through the wrapper.
    int UpdatePrompt()
    {
        const TASKDIALOG_BUTTON buttons[] = { { 2001, L"Update now" }, { 2002, L"Postpone" } };
        const TASKDIALOG_BUTTON radios[] = { { 3000, L"Remind me next restart" }, { 3002, L"Remind me in 3 days" } };
        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS;
        cfg.pszWindowTitle = L"Probe update";
        cfg.pszMainInstruction = L"Agentmaster 9.9.9 is available";
        cfg.pszContent = L"You have 0.8.0.";
        cfg.pszFooter = L"<a href=\"https://example.invalid/notes\">What's new</a>";
        cfg.cButtons = 2;
        cfg.pButtons = buttons;
        cfg.nDefaultButton = 2001;
        cfg.cRadioButtons = 2;
        cfg.pRadioButtons = radios;
        cfg.nDefaultRadioButton = 3000;
        cfg.pfCallback = AnswerTaskDialog;
        int pressed = 0;
        int radio = 0;
        ::Agentmaster::DialogEcho::ShowTaskDialogIndirect(&cfg, &pressed, &radio, nullptr);
        return pressed;
    }
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmd, int)
{
    const std::wstring mode{ cmd };
    if (mode == L"diag")
    {
        // What a caller handed over, BEFORE Capture consumes it: AGENTMASTER_CALLER_SINK and the standard handles.
        wchar_t value[64]{};
        ::GetEnvironmentVariableW(::Agentmaster::CallerSink::kEnvVariable, value, ARRAYSIZE(value));
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        ::GetStartupInfoW(&si);
        wchar_t self[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring path{ self };
        path = path.substr(0, path.find_last_of(L'\\') + 1) + L"diag.txt";
        if (FILE* f = nullptr; _wfopen_s(&f, path.c_str(), L"w") == 0 && f)
        {
            fwprintf(f, L"env=[%ls] usestd=%d err=%p\n", value, (si.dwFlags & STARTF_USESTDHANDLES) != 0, si.hStdError);
            fclose(f);
        }
    }
    ::Agentmaster::CallerSink::Capture();
    int answer = 0;
    if (mode == L"msgbox")
    {
        AnswerMessageBox(L"Probe \x2014 profile in use", IDNO);
        answer = ::Agentmaster::DialogEcho::ShowMessageBox(nullptr, L"The profile\n\n    C:\\probe\\profile\n\nis already in use by another running instance.\n\nContinue anyway?",
                                                           L"Probe \x2014 profile in use", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    }
    else if (mode == L"taskdialog")
    {
        answer = UpdatePrompt();
    }
    else if (mode == L"deferred")
    {
        // WindowEmperor::_showMessageBox: the Hold is taken on the main thread, the main thread then reaches the
        // release point, and the box (with its echo) is shown on another thread.
        auto hold = std::make_unique<::Agentmaster::CallerSink::Hold>();
        ::Agentmaster::CallerSink::RequestRelease();
        std::thread worker([&answer, hold = std::move(hold)]() mutable {
            AnswerMessageBox(L"Probe help", IDOK);
            answer = ::Agentmaster::DialogEcho::ShowMessageBox(nullptr, L"Usage: probe [options]", L"Probe help", MB_OK | MB_ICONWARNING);
            hold.reset(); // the answer was echoed: the release may happen now
        });
        worker.join();
    }
    ::Agentmaster::CallerSink::RequestRelease();
    return answer;
}
