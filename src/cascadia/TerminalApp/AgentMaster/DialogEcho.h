// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — EVERY DIALOG WE OPEN IS ALSO WRITTEN TO THE CONSOLE OF THE COMMAND THAT STARTED THE PROCESS
// (doc/agentmaster/CLI.md §2b). Header-only, plain Win32 (comctl32 v6 task dialogs, the shell's file dialogs).
//
// WHY: a command can make a dialog appear without its console knowing. On 2026-10-23 a Claude session's
// `agentmaster show a77419cf` started the app instead of the CLI, and the app asked "profile in use - Continue anyway?"
// on the user's screen while the session's output stayed empty. Now each dialog prints, BEFORE it shows:
//
//     [agentmaster dialog] opened: Agentmaster - profile in use (Agentmaster.exe, pid 1234, waits for an answer on the desktop)
//     [agentmaster dialog] | The profile
//     [agentmaster dialog] | ...
//     [agentmaster dialog] | choices: Yes | No (default)
//
// and, once answered:  [agentmaster dialog] answered: Agentmaster - profile in use: No
//
// The lines go to the CALLER SINK (CallerSink.h: the caller's stderr pipe or file, or its console), only while the
// app's startup lasts; with no sink every wrapper is the plain Win32 call plus one check. A UAC prompt our code causes,
// and a dialog the RUNNING app opens for a command line handed to it, are echoed the same way (ShellExecuteWithEcho,
// EchoForeignDialog).
//
// THE RULE: no Agentmaster code calls MessageBoxW, TaskDialog, TaskDialogIndirect or IFileDialog::Show directly; it
// calls the wrappers below (ShowMessageBox, ShowTaskDialog, ShowTaskDialogIndirect, ShowFileDialog). The m5 test
// TestDialogEchoCoverage scans src\cascadia and fails on a direct call, and on a call of any other Win32 dialog family
// (the common file / folder / color / font / print dialogs, the shell's message boxes, resource dialogs, the
// credential prompts), which needs a wrapper here before its first use. So a new dialog cannot skip the echo.

#pragma once

#include "CallerSink.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace Agentmaster::DialogEcho
{
    // Every echoed line starts with this, so a reader (an agent, grep) finds the dialog lines among the command's output.
    inline constexpr std::wstring_view kPrefix = L"[agentmaster dialog] ";
    // The most characters one dialog's echo takes: a longer text is cut with a marker. Bounded because a write into a
    // pipe nobody drains blocks once the pipe is full, and that would stall the app's startup.
    inline constexpr size_t kMaxEchoChars = 8000;

    // What an "opened" echo prints: the title and the lines under it (the text, then the choices).
    struct Description
    {
        std::wstring title;
        std::vector<std::wstring> lines;
    };

    // PURE: splits a dialog text into lines at "\r\n", "\n" and "\r" (a lone "\r" breaks a line in a dialog too).
    //   text    — the text.
    //   returns — the lines, empty ones kept; none for an empty text.
    //   throws  — std::bad_alloc only.
    inline std::vector<std::wstring> SplitLines(std::wstring_view text)
    {
        std::vector<std::wstring> lines;
        if (text.empty())
        {
            return lines;
        }
        std::wstring line;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const wchar_t c = text[i];
            if (c == L'\r' || c == L'\n')
            {
                lines.push_back(line);
                line.clear();
                if (c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n')
                {
                    ++i;
                }
                continue;
            }
            line.push_back(c);
        }
        lines.push_back(line);
        return lines;
    }

    // PURE: a task dialog's hyperlink markup (TDF_ENABLE_HYPERLINKS: <a href="url">text</a>) as plain text, so the
    // console shows the words AND where they lead: `text (url)`. Unbalanced markup is left as it is.
    //   text    — the dialog text.
    //   returns — the text without markup.
    //   throws  — std::bad_alloc only.
    inline std::wstring StripLinkMarkup(std::wstring_view text)
    {
        std::wstring out;
        size_t i = 0;
        while (i < text.size())
        {
            const auto open = text.find(L"<a href=\"", i);
            if (open == std::wstring_view::npos)
            {
                out.append(text.substr(i));
                break;
            }
            const auto urlEnd = text.find(L"\">", open + 9);
            const auto close = urlEnd == std::wstring_view::npos ? std::wstring_view::npos : text.find(L"</a>", urlEnd + 2);
            if (close == std::wstring_view::npos)
            {
                out.append(text.substr(i));
                break;
            }
            out.append(text.substr(i, open - i));
            out.append(text.substr(urlEnd + 2, close - urlEnd - 2));
            out.append(L" (");
            out.append(text.substr(open + 9, urlEnd - open - 9));
            out.append(L")");
            i = close + 4;
        }
        return out;
    }

    // PURE: a text field of a dialog that may be a string resource id instead of a string (MAKEINTRESOURCE). None of
    // our dialogs uses resource ids; one that did would be named, not read.
    //   p       — the field.
    //   returns — the string, "" for null, "(a string resource)" for an id.
    //   throws  — std::bad_alloc only.
    inline std::wstring FieldText(PCWSTR p)
    {
        if (!p)
        {
            return {};
        }
        if (IS_INTRESOURCE(p))
        {
            return L"(a string resource)";
        }
        return p;
    }

    // PURE: the name of a standard button id (MessageBox / TaskDialog results and common buttons).
    //   id      — IDOK, IDCANCEL, IDYES, ...
    //   returns — "OK", "Cancel", "Yes", ...; "button <id>" for an unknown id.
    //   throws  — std::bad_alloc only.
    inline std::wstring CommonButtonName(int id)
    {
        switch (id)
        {
        case IDOK:
            return L"OK";
        case IDCANCEL:
            return L"Cancel";
        case IDABORT:
            return L"Abort";
        case IDRETRY:
            return L"Retry";
        case IDIGNORE:
            return L"Ignore";
        case IDYES:
            return L"Yes";
        case IDNO:
            return L"No";
        case IDCLOSE:
            return L"Close";
        case IDHELP:
            return L"Help";
        case IDTRYAGAIN:
            return L"Try Again";
        case IDCONTINUE:
            return L"Continue";
        default:
            return L"button " + std::to_wstring(id);
        }
    }

    // PURE: a MessageBox's buttons from its MB_ flags, in the order Windows shows them, the default one marked.
    //   type    — the MB_ flags.
    //   returns — e.g. "Yes | No (default)".
    //   throws  — std::bad_alloc only.
    inline std::wstring MessageBoxChoices(UINT type)
    {
        std::vector<int> ids;
        switch (type & MB_TYPEMASK)
        {
        case MB_OKCANCEL:
            ids = { IDOK, IDCANCEL };
            break;
        case MB_ABORTRETRYIGNORE:
            ids = { IDABORT, IDRETRY, IDIGNORE };
            break;
        case MB_YESNOCANCEL:
            ids = { IDYES, IDNO, IDCANCEL };
            break;
        case MB_YESNO:
            ids = { IDYES, IDNO };
            break;
        case MB_RETRYCANCEL:
            ids = { IDRETRY, IDCANCEL };
            break;
        case MB_CANCELTRYCONTINUE:
            ids = { IDCANCEL, IDTRYAGAIN, IDCONTINUE };
            break;
        default:
            ids = { IDOK };
            break;
        }
        const size_t defaultIndex = (type & MB_DEFMASK) == MB_DEFBUTTON2 ? 1 :
                                    (type & MB_DEFMASK) == MB_DEFBUTTON3 ? 2 :
                                    (type & MB_DEFMASK) == MB_DEFBUTTON4 ? 3 :
                                                                           0;
        std::wstring out;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i > 0)
            {
                out += L" | ";
            }
            out += CommonButtonName(ids[i]);
            if (i == defaultIndex)
            {
                out += L" (default)";
            }
        }
        return out;
    }

    // PURE: what a MessageBox returned, in words.
    //   result  — MessageBoxW's return value (0 = it failed).
    //   returns — "Yes", "No", ...; "not shown" for 0.
    //   throws  — std::bad_alloc only.
    inline std::wstring MessageBoxAnswer(int result)
    {
        return result == 0 ? std::wstring{ L"not shown (MessageBoxW failed)" } : CommonButtonName(result);
    }

    // PURE: a MessageBox, described for the echo.
    //   text    — its text.
    //   caption — its title (MessageBoxW shows "Error" for null).
    //   type    — its MB_ flags.
    //   returns — the description.
    //   throws  — std::bad_alloc only.
    inline Description DescribeMessageBox(PCWSTR text, PCWSTR caption, UINT type)
    {
        Description d;
        d.title = caption ? FieldText(caption) : std::wstring{ L"Error" };
        d.lines = SplitLines(FieldText(text));
        d.lines.push_back(L"choices: " + MessageBoxChoices(type));
        return d;
    }

    // PURE: the common buttons of a task dialog (TDCBF_ flags) in Windows' order, the default marked.
    //   flags     — the TDCBF_ flags.
    //   defaultId — the dialog's nDefaultButton (0 = Windows picks the first).
    //   returns   — e.g. "OK | Cancel (default)"; "" for no flags.
    //   throws    — std::bad_alloc only.
    inline std::wstring CommonButtonsChoices(TASKDIALOG_COMMON_BUTTON_FLAGS flags, int defaultId)
    {
        static constexpr struct
        {
            TASKDIALOG_COMMON_BUTTON_FLAGS flag;
            int id;
        } kOrder[] = { { TDCBF_OK_BUTTON, IDOK },     { TDCBF_YES_BUTTON, IDYES },     { TDCBF_NO_BUTTON, IDNO },
                       { TDCBF_RETRY_BUTTON, IDRETRY }, { TDCBF_CANCEL_BUTTON, IDCANCEL }, { TDCBF_CLOSE_BUTTON, IDCLOSE } };
        std::wstring out;
        bool first = true;
        for (const auto& b : kOrder)
        {
            if ((flags & b.flag) == 0)
            {
                continue;
            }
            if (!out.empty())
            {
                out += L" | ";
            }
            out += CommonButtonName(b.id);
            if (b.id == defaultId || (defaultId == 0 && first))
            {
                out += L" (default)";
            }
            first = false;
        }
        return out;
    }

    // PURE: a task dialog, described for the echo: the main instruction, the content, the details and the footer (link
    // markup as plain text), then every choice: the custom buttons (a command link's note after a dash), the common
    // buttons, the radio buttons and the check box, each with its default state.
    //   cfg     — the dialog as it is about to be shown.
    //   returns — the description.
    //   throws  — std::bad_alloc only.
    inline Description DescribeTaskDialog(const TASKDIALOGCONFIG& cfg)
    {
        Description d;
        d.title = FieldText(cfg.pszWindowTitle);
        if (d.title.empty())
        {
            d.title = L"(untitled)";
        }
        const auto add = [&](const std::wstring& text) {
            for (auto& line : SplitLines(StripLinkMarkup(text)))
            {
                d.lines.push_back(std::move(line));
            }
        };
        add(FieldText(cfg.pszMainInstruction));
        if (cfg.pszContent)
        {
            if (!d.lines.empty())
            {
                d.lines.emplace_back();
            }
            add(FieldText(cfg.pszContent));
        }
        if (cfg.pszExpandedInformation)
        {
            d.lines.emplace_back(L"details (collapsed in the dialog):");
            add(FieldText(cfg.pszExpandedInformation));
        }
        if (cfg.pszFooter)
        {
            add(FieldText(cfg.pszFooter));
        }
        for (UINT i = 0; i < cfg.cButtons && cfg.pButtons; ++i)
        {
            const auto lines = SplitLines(FieldText(cfg.pButtons[i].pszButtonText));
            std::wstring label = lines.empty() ? std::wstring{} : lines.front();
            for (size_t k = 1; k < lines.size(); ++k)
            {
                label += (k == 1 ? L" - " : L" ") + lines[k];
            }
            if (cfg.pButtons[i].nButtonID == cfg.nDefaultButton || (cfg.nDefaultButton == 0 && i == 0))
            {
                label += L" (default)";
            }
            d.lines.push_back(L"button: " + label);
        }
        TASKDIALOG_COMMON_BUTTON_FLAGS common = cfg.dwCommonButtons;
        if (common == 0 && cfg.cButtons == 0)
        {
            common = TDCBF_OK_BUTTON; // a task dialog with no button at all shows OK
        }
        if (common != 0)
        {
            // With custom buttons and no explicit default, Windows' default is the first CUSTOM button (marked above),
            // so no common button is marked (-1); an explicit default id is marked wherever it is.
            const int commonDefault = cfg.nDefaultButton != 0 ? cfg.nDefaultButton : (cfg.cButtons == 0 ? 0 : -1);
            d.lines.push_back(L"buttons: " + CommonButtonsChoices(common, commonDefault));
        }
        for (UINT i = 0; i < cfg.cRadioButtons && cfg.pRadioButtons; ++i)
        {
            const bool selected = cfg.pRadioButtons[i].nButtonID == cfg.nDefaultRadioButton ||
                                  (cfg.nDefaultRadioButton == 0 && i == 0 && (cfg.dwFlags & TDF_NO_DEFAULT_RADIO_BUTTON) == 0);
            d.lines.push_back(std::wstring{ selected ? L"(o) " : L"( ) " } + FieldText(cfg.pRadioButtons[i].pszButtonText) + (selected ? L" (default)" : L""));
        }
        if (cfg.pszVerificationText)
        {
            const bool checked = (cfg.dwFlags & TDF_VERIFICATION_FLAG_CHECKED) != 0;
            d.lines.push_back(std::wstring{ checked ? L"[x] " : L"[ ] " } + FieldText(cfg.pszVerificationText));
        }
        if ((cfg.dwFlags & (TDF_SHOW_PROGRESS_BAR | TDF_SHOW_MARQUEE_PROGRESS_BAR)) != 0)
        {
            d.lines.emplace_back(L"(a progress dialog: it closes by itself when the work is done)");
        }
        return d;
    }

    // PURE: what a task dialog returned, in words: the button (a custom one by its label), then the radio button and the
    // check box when the dialog had them.
    //   cfg      — the dialog as it was shown.
    //   hr       — TaskDialogIndirect's result.
    //   button   — the pressed button's id.
    //   radio    — the selected radio button's id.
    //   verified — the check box's state.
    //   returns  — e.g. "Postpone; option: Remind me in 3 days".
    //   throws   — std::bad_alloc only.
    inline std::wstring TaskDialogAnswer(const TASKDIALOGCONFIG& cfg, HRESULT hr, int button, int radio, BOOL verified)
    {
        if (FAILED(hr))
        {
            wchar_t code[16]{};
            swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(hr));
            return std::wstring{ L"not shown (error " } + code + L")";
        }
        if ((cfg.dwFlags & (TDF_SHOW_PROGRESS_BAR | TDF_SHOW_MARQUEE_PROGRESS_BAR)) != 0)
        {
            return L"closed";
        }
        std::wstring answer;
        for (UINT i = 0; i < cfg.cButtons && cfg.pButtons; ++i)
        {
            if (cfg.pButtons[i].nButtonID == button)
            {
                const auto lines = SplitLines(FieldText(cfg.pButtons[i].pszButtonText));
                answer = lines.empty() ? CommonButtonName(button) : lines.front();
                break;
            }
        }
        const bool cancelled = answer.empty() && button == IDCANCEL && (cfg.dwCommonButtons & TDCBF_CANCEL_BUTTON) == 0;
        if (answer.empty())
        {
            answer = cancelled ? std::wstring{ L"closed without a choice (Cancel, Esc or X)" } : CommonButtonName(button);
        }
        if (!cancelled)
        {
            for (UINT i = 0; i < cfg.cRadioButtons && cfg.pRadioButtons; ++i)
            {
                if (cfg.pRadioButtons[i].nButtonID == radio)
                {
                    answer += L"; option: " + FieldText(cfg.pRadioButtons[i].pszButtonText);
                    break;
                }
            }
            if (cfg.pszVerificationText)
            {
                answer += verified ? L"; check box: ticked" : L"; check box: not ticked";
            }
        }
        return answer;
    }

    // PURE: the "opened" echo: a header line, then every line of the description behind "| ", cut to kMaxEchoChars.
    //   d       — the dialog.
    //   who     — who shows it and where, e.g. "Agentmaster.exe, pid 1234, waits for an answer on the desktop".
    //   returns — the lines, joined with "\n".
    //   throws  — std::bad_alloc only.
    inline std::wstring FormatOpened(const Description& d, std::wstring_view who)
    {
        std::wstring out{ kPrefix };
        out += L"opened: ";
        out += d.title;
        out += L" (";
        out += who;
        out += L")";
        for (const auto& line : d.lines)
        {
            if (out.size() + kPrefix.size() + 2 + line.size() > kMaxEchoChars)
            {
                out += L"\n";
                out += kPrefix;
                out += L"| [... cut: the dialog shows the rest]";
                break;
            }
            out += L"\n";
            out += kPrefix;
            out += L"| ";
            out += line;
        }
        return out;
    }

    // PURE: the "answered" echo.
    //   title   — the dialog's title.
    //   answer  — the answer in words (MessageBoxAnswer, TaskDialogAnswer, ...).
    //   returns — one line.
    //   throws  — std::bad_alloc only.
    inline std::wstring FormatAnswered(std::wstring_view title, std::wstring_view answer)
    {
        std::wstring out{ kPrefix };
        out += L"answered: ";
        out += title;
        out += L": ";
        out += answer;
        return out;
    }

    namespace detail
    {
        // Builds an echo and writes it to the caller sink; nothing when there is no sink (the formatting is skipped).
        // An exception from the building (std::bad_alloc) loses that echo only: the dialog is shown regardless.
        //   build   — returns the text to write.
        //   throws  — nothing.
        template<typename Build>
        inline void Echo(Build&& build) noexcept
        {
            if (!CallerSink::Active())
            {
                return;
            }
            try
            {
                CallerSink::Write(build());
            }
            catch (...)
            {
                // std::bad_alloc only. There is no log to report to: the app has none before its profile resolves.
            }
        }

        // "<exe>, pid <n>": names this process in an echo (the leaf of its exe path).
        //   returns — e.g. "Agentmaster.exe, pid 1234".
        //   throws  — std::bad_alloc only.
        inline std::wstring Self()
        {
            wchar_t path[MAX_PATH * 2]{};
            const DWORD n = ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
            std::wstring_view leaf{ path, n };
            if (const auto slash = leaf.find_last_of(L"\\/"); slash != std::wstring_view::npos)
            {
                leaf.remove_prefix(slash + 1);
            }
            return std::wstring{ leaf } + L", pid " + std::to_wstring(::GetCurrentProcessId());
        }

        // Whether this process runs with administrator rights already (then a "runas" start shows no UAC prompt).
        //   returns — true when the token is elevated.
        //   throws  — nothing.
        inline bool IsElevated() noexcept
        {
            HANDLE token = nullptr;
            if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            {
                return false;
            }
            TOKEN_ELEVATION elevation{};
            DWORD size = 0;
            const bool elevated = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated != 0;
            ::CloseHandle(token);
            return elevated;
        }
    }

    // MessageBoxW, echoed (the module's echo of choice for a plain question or notice).
    //   owner   — the owner window (nullptr in the app's prelude).
    //   text    — the message.
    //   caption — the title.
    //   type    — the MB_ flags.
    //   returns — MessageBoxW's result (IDYES, IDNO, ...; 0 when it failed).
    //   throws  — nothing.
    inline int ShowMessageBox(HWND owner, PCWSTR text, PCWSTR caption, UINT type) noexcept
    {
        const CallerSink::Hold hold; // the startup's release waits for this answer
        detail::Echo([&] { return FormatOpened(DescribeMessageBox(text, caption, type), detail::Self() + L", waits for an answer on the desktop"); });
        const int result = ::MessageBoxW(owner, text, caption, type);
        detail::Echo([&] { return FormatAnswered(caption ? FieldText(caption) : std::wstring{ L"Error" }, MessageBoxAnswer(result)); });
        return result;
    }

    // TaskDialogIndirect, echoed. The out-parameters may be null, as for the real call; the echo reads the answer
    // through its own copies.
    //   cfg      — the dialog.
    //   button   — receives the pressed button's id (may be null).
    //   radio    — receives the selected radio button's id (may be null).
    //   verified — receives the check box's state (may be null).
    //   returns  — TaskDialogIndirect's HRESULT (E_POINTER for a null cfg).
    //   throws   — nothing.
    inline HRESULT ShowTaskDialogIndirect(const TASKDIALOGCONFIG* cfg, int* button, int* radio, BOOL* verified) noexcept
    {
        if (!cfg)
        {
            return E_POINTER;
        }
        const CallerSink::Hold hold;
        detail::Echo([&] { return FormatOpened(DescribeTaskDialog(*cfg), detail::Self() + L", waits for an answer on the desktop"); });
        int pressed = 0;
        int chosen = 0;
        BOOL ticked = FALSE;
        const HRESULT hr = ::TaskDialogIndirect(cfg, &pressed, &chosen, &ticked);
        detail::Echo([&] { return FormatAnswered(DescribeTaskDialog(*cfg).title, TaskDialogAnswer(*cfg, hr, pressed, chosen, ticked)); });
        if (button)
        {
            *button = pressed;
        }
        if (radio)
        {
            *radio = chosen;
        }
        if (verified)
        {
            *verified = ticked;
        }
        return hr;
    }

    // TaskDialog (the simple form), echoed: the same fields as a TASKDIALOGCONFIG for the echo, the real TaskDialog for
    // the screen.
    //   owner       — the owner window.
    //   instance    — the module for resource ids (nullptr for strings).
    //   title       — the window title.
    //   instruction — the main instruction.
    //   content     — the text.
    //   buttons     — the TDCBF_ common buttons.
    //   icon        — the TD_ icon.
    //   pressed     — receives the pressed button's id (may be null).
    //   returns     — TaskDialog's HRESULT.
    //   throws      — nothing.
    inline HRESULT ShowTaskDialog(HWND owner, HINSTANCE instance, PCWSTR title, PCWSTR instruction, PCWSTR content, TASKDIALOG_COMMON_BUTTON_FLAGS buttons, PCWSTR icon, int* pressed) noexcept
    {
        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = owner;
        cfg.hInstance = instance;
        cfg.pszWindowTitle = title;
        cfg.pszMainInstruction = instruction;
        cfg.pszContent = content;
        cfg.dwCommonButtons = buttons;
        cfg.pszMainIcon = icon;
        const CallerSink::Hold hold;
        detail::Echo([&] { return FormatOpened(DescribeTaskDialog(cfg), detail::Self() + L", waits for an answer on the desktop"); });
        int result = 0;
        const HRESULT hr = ::TaskDialog(owner, instance, title, instruction, content, buttons, icon, &result);
        detail::Echo([&] { return FormatAnswered(DescribeTaskDialog(cfg).title, TaskDialogAnswer(cfg, hr, result, 0, FALSE)); });
        if (pressed)
        {
            *pressed = result;
        }
        return hr;
    }

    // IFileDialog::Show, echoed: a file or folder picker. The answer names the chosen path, or "cancelled".
    //   dialog  — the configured dialog (its title set by the caller).
    //   owner   — the owner window.
    //   title   — the title the caller gave the dialog (the echo cannot read it back).
    //   what    — "folder" or "file", for the echo's words.
    //   returns — Show's HRESULT (HRESULT_FROM_WIN32(ERROR_CANCELLED) when the user cancelled).
    //   throws  — nothing.
    inline HRESULT ShowFileDialog(IFileDialog* dialog, HWND owner, std::wstring_view title, std::wstring_view what) noexcept
    {
        if (!dialog)
        {
            return E_POINTER;
        }
        const CallerSink::Hold hold;
        detail::Echo([&] {
            return FormatOpened(Description{ std::wstring{ title }, { L"choices: pick a " + std::wstring{ what } + L" | Cancel" } },
                                detail::Self() + L", a " + std::wstring{ what } + L" picker, waits for a choice on the desktop");
        });
        const HRESULT hr = dialog->Show(owner);
        detail::Echo([&] {
            std::wstring answer;
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            {
                answer = L"cancelled";
            }
            else if (FAILED(hr))
            {
                wchar_t code[16]{};
                swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(hr));
                answer = std::wstring{ L"not shown (error " } + code + L")";
            }
            else
            {
                answer = L"chosen";
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)) && item)
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                    {
                        answer += L": ";
                        answer += path;
                        ::CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            return FormatAnswered(title, answer);
        });
        return hr;
    }

    // ShellExecuteExW, echoed when it asks Windows for administrator rights: a "runas" start from a process that is not
    // elevated shows the UAC prompt (on the secure desktop), which is a dialog our code causes. Any other start is the
    // plain call.
    //   sei     — the start, as for ShellExecuteExW.
    //   purpose — what the rights are for, in words ("install Agentmaster for all users of this PC").
    //   returns — ShellExecuteExW's result; GetLastError() is preserved for the caller (ERROR_CANCELLED = declined).
    //   throws  — nothing.
    inline BOOL ShellExecuteWithEcho(SHELLEXECUTEINFOW* sei, std::wstring_view purpose) noexcept
    {
        const bool prompt = sei && sei->lpVerb && std::wstring_view{ sei->lpVerb } == L"runas" && !detail::IsElevated();
        if (!prompt)
        {
            return ::ShellExecuteExW(sei);
        }
        const CallerSink::Hold hold;
        detail::Echo([&] {
            return FormatOpened(Description{ L"User Account Control", { L"Windows asks for administrator approval to " + std::wstring{ purpose } + L".", L"choices: Yes | No" } },
                                L"Windows shows it on the secure desktop for " + detail::Self());
        });
        const BOOL ok = ::ShellExecuteExW(sei);
        const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
        detail::Echo([&] {
            const std::wstring answer = ok                                      ? std::wstring{ L"approved" } :
                                        error == ERROR_CANCELLED                ? std::wstring{ L"declined" } :
                                        error == ERROR_ACCESS_DISABLED_BY_POLICY ? std::wstring{ L"refused: a policy forbids this account to elevate" } :
                                                                                  L"not shown (error " + std::to_wstring(error) + L")";
            return FormatAnswered(L"User Account Control", answer);
        });
        ::SetLastError(error);
        return ok;
    }

    // Echoes a dialog that ANOTHER process opened for this process's command line: the running app's reply to a
    // single-instance hand-off (CallerSink.h HandoffReply). Its answer is given in that app and is not reported here.
    //   m       — the reply (title, text, the running app's pid).
    //   throws  — nothing.
    inline void EchoForeignDialog(const HandoffReply::Message& m) noexcept
    {
        detail::Echo([&] {
            Description d{ m.title, SplitLines(m.text) };
            d.lines.push_back(L"(this command line was handed to the running Agentmaster, which shows the dialog; its answer is not reported here)");
            return FormatOpened(d, L"the running Agentmaster, pid " + std::to_wstring(m.pid) + L", waits for an answer on the desktop");
        });
    }
}
