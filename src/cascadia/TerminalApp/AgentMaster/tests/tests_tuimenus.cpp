// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.34: a LOCAL command's dialog answered from Telegram — /add-dir's "Add directory to
// workspace" (the user's request of 2026-10-19: "Can you support /add-dir from here and also picker for ... the menu that comes
// after ... The default is the 1st option and we should support in agentmaster auto-answer after 60s in a clean way"). This file
// pins the pure half (TuiMenus.h) and the delivery fixes beside it (Scheduler.h):
//   1. the queued text read as the command (ParseAddDirCommandText), and the bare command's refusal text;
//   2. the screen parser against the probe's OWN captures (tools/add-dir-probe, Claude Code 2.1.291): the open dialog found with
//      its path and focus, the bare command's path input and the screen after an answer not taken for it, and the shapes a real
//      terminal can make (a wrapped path, a wrapped note, a truncated label, another option focused, a frame left behind, a
//      different menu, two dialogs on one screen) — plus the input box's detector reading the dialog as a MENU, which the
//      lost-send hold depends on;
//   3. the paths (compare, absolute, belongs-to, the form shown);
//   4. the window's press: the action's argument and reply words, and DecideMenuAnswer on every case;
//   5. the transcript's word: terminal controls stripped, the echo and the result lines read, every message Claude Code writes
//      classified, and which outcome an option produces;
//   6. the card: its callback data, its text in every phase, its buttons, the reply, the usage and the unreadable note;
//   7. the delivery fixes: the command echo matched to a delivered prompt (CommandEchoMatchesPromptText, PickCommandEchoPrompt) and
//      the lost-send verdict held while a menu is open (DecideLostSend).
// The service's half (the watch, the screen reads, the card's posts and edits, the clock's press) needs Telegram and a window: the
// live check is the dev bot (MESSAGING.md §21.34, "Check after the build").

#include "m5_tests.h"

#include <string>
#include <vector>

#include "../TuiMenus.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    // The probe's captures, verbatim (generated from the files by the scratch script gen_screens.py: every non-ASCII character is
    // a \x escape, each row trimmed on the right as the window's read trims it).
    // screen-menu-1-menu.txt (tools/add-dir-probe, 2026-10-19, Claude Code 2.1.291, 160 x 50, fullscreen renderer)
    const std::wstring kProbeMenuScreen =
        L"" L"\n"
        L" \x2590\x259B\x2588\x2588\x2588\x259B\x2588   Claude Code v2.1.291" L"\n"
        L"\x259D\x259C\x2588\x2588\x2588\x2588\x2588\x2588\x2580  Opus 5.5 (1M context) \x00B7 Claude Max" L"\n"
        L" \x259D\x259D   \x259D\x259D   Q:\\tmp\\adddir-probe-1\\ws" L"\n"
        L"" L"\n"
        L"\x258E Debug mode enabled \x00B7 logging to C:\\Users\\ELI\\.claude\\debug\\5ac39b55-9073-42a8-a5c4-e428f1363047.txt" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594 \x25C8 max \x00B7 /effort \x2594" L"\n"
        L"   Add directory to workspace" L"\n"
        L"" L"\n"
        L"   Q:\\tmp\\adddir-probe-1\\targets\\one" L"\n"
        L"" L"\n"
        L"   Claude Code will be able to read files in this directory and make edits when auto-accept edits is on." L"\n"
        L"" L"\n"
        L"   \x276F 1. Yes, for this session" L"\n"
        L"     2. Yes, and remember this directory" L"\n"
        L"     3. No" L"\n"
        L"" L"\n"
        L"   Enter to confirm \x00B7 Esc to cancel";
    // screen-bare-shown.txt (tools/add-dir-probe, 2026-10-19, Claude Code 2.1.291, 160 x 50, fullscreen renderer)
    const std::wstring kProbeBareScreen =
        L"" L"\n"
        L" \x2590\x259B\x2588\x2588\x2588\x259B\x2588   Claude Code v2.1.291" L"\n"
        L"\x259D\x259C\x2588\x2588\x2588\x2588\x2588\x2588\x2580  Opus 5.5 (1M context) \x00B7 Claude Max" L"\n"
        L" \x259D\x259D   \x259D\x259D   Q:\\tmp\\adddir-probe-1\\ws" L"\n"
        L"" L"\n"
        L"\x258E Debug mode enabled \x00B7 logging to C:\\Users\\ELI\\.claude\\debug\\5ac39b55-9073-42a8-a5c4-e428f1363047.txt" L"\n"
        L"" L"\n"
        L"\x276F /add-dir Q:\\tmp\\adddir-probe-1\\targets\\one" L"\n"
        L"  \x23BF  Added Q:\\tmp\\adddir-probe-1\\targets\\one as a working directory for this session \x00B7 /permissions to manage" L"\n"
        L"" L"\n"
        L"\x276F /add-dir Q:\\tmp\\adddir-probe-1\\targets\\two" L"\n"
        L"  \x23BF  Added Q:\\tmp\\adddir-probe-1\\targets\\two as a working directory and saved to local settings \x00B7 /permissions to manage" L"\n"
        L"" L"\n"
        L"\x276F /add-dir Q:\\tmp\\adddir-probe-1\\targets\\three" L"\n"
        L"  \x23BF  Did not add Q:\\tmp\\adddir-probe-1\\targets\\three as a working directory." L"\n"
        L"" L"\n"
        L"\x276F /add-dir Q:\\tmp\\adddir-probe-1\\targets\\four" L"\n"
        L"  \x23BF  Did not add Q:\\tmp\\adddir-probe-1\\targets\\four as a working directory." L"\n"
        L"" L"\n"
        L"\x276F /add-dir Q:\\tmp\\adddir-probe-1\\targets\\one" L"\n"
        L"  \x23BF  Q:\\tmp\\adddir-probe-1\\targets\\one is already added as a working directory." L"\n"
        L"" L"\n"
        L"\x276F /add-dir Q:\\tmp\\adddir-probe-1\\targets\\nope" L"\n"
        L"  \x23BF  Path Q:\\tmp\\adddir-probe-1\\targets\\nope was not found." L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594\x2594 \x25C8 max \x00B7 /effort \x2594" L"\n"
        L"   Add directory to workspace" L"\n"
        L"" L"\n"
        L"   Claude Code will be able to read files in this directory and make edits when auto-accept edits is on." L"\n"
        L"" L"\n"
        L"   Enter the path to the directory:" L"\n"
        L"" L"\n"
        L"   \x256D\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x256E" L"\n"
        L"   \x2502 Directory path\x2026                                                                                                                                        \x2502" L"\n"
        L"   \x2570\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x256F" L"\n"
        L"" L"\n"
        L"   Tab to complete \x00B7 Enter to add \x00B7 Esc to cancel";
    // screen-menu-1-after.txt (tools/add-dir-probe, 2026-10-19, Claude Code 2.1.291, 160 x 50, fullscreen renderer)
    const std::wstring kProbeAfterScreen =
        L"" L"\n"
        L" \x2590\x259B\x2588\x2588\x2588\x259B\x2588   Claude Code v2.1.291" L"\n"
        L"\x259D\x259C\x2588\x2588\x2588\x2588\x2588\x2588\x2580  Opus 5.5 (1M context) \x00B7 Claude Max" L"\n"
        L" \x259D\x259D   \x259D\x259D   Q:\\tmp\\adddir-probe-1\\ws" L"\n"
        L"" L"\n"
        L"\x258E Debug mode enabled \x00B7 logging to C:\\Users\\ELI\\.claude\\debug\\5ac39b55-9073-42a8-a5c4-e428f1363047.txt" L"\n"
        L"" L"\n"
        L"\x276F /add-dir Q:\\tmp\\adddir-probe-1\\targets\\one" L"\n"
        L"  \x23BF  Added Q:\\tmp\\adddir-probe-1\\targets\\one as a working directory for this session \x00B7 /permissions to manage" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"" L"\n"
        L"                                                                                                                                               \x25C8 max \x00B7 /effort" L"\n"
        L"\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500" L"\n"
        L"\x276F" L"\n"
        L"\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500\x2500" L"\n"
        L"  \x23F5\x23F5 bypass permissions on \x00B7 (shift+tab to cycle) [c]";

    /// <summary>A screen built from rows (LF between them), for the shapes the probe did not capture.</summary>
    /// <param name="rows">The rows, top to bottom.</param>
    /// <returns>The screen text.</returns>
    std::wstring Screen(const std::vector<std::wstring>& rows)
    {
        std::wstring out;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            out += (i == 0 ? L"" : L"\n") + rows[i];
        }
        return out;
    }

    /// <summary>The dialog's rows for a path and a focused option, in the probe's layout (three spaces of indent, blank rows between).</summary>
    /// <param name="pathRows">The path's rows (one, or the pieces of a wrapped path).</param>
    /// <param name="focused">The option the cursor is on (1-3).</param>
    /// <returns>The rows.</returns>
    std::vector<std::wstring> DialogRows(const std::vector<std::wstring>& pathRows, int focused = 1)
    {
        std::vector<std::wstring> rows{ L"   Add directory to workspace", L"" };
        for (const auto& r : pathRows)
        {
            rows.push_back(L"   " + r);
        }
        rows.push_back(L"");
        rows.push_back(L"   Claude Code will be able to read files in this directory and make edits when auto-accept edits is on.");
        rows.push_back(L"");
        const wchar_t* labels[] = { L"1. Yes, for this session", L"2. Yes, and remember this directory", L"3. No" };
        for (int i = 1; i <= 3; ++i)
        {
            rows.push_back(std::wstring{ i == focused ? L"   \x276F " : L"     " } + labels[i - 1]);
        }
        rows.push_back(L"");
        rows.push_back(L"   Enter to confirm \x00B7 Esc to cancel");
        return rows;
    }

    /// <summary>A delivered prompt (Sent, unechoed, stamped, injected) as the queue holds it.</summary>
    /// <param name="id">Its id.</param>
    /// <param name="text">Its text.</param>
    /// <param name="at">Its send and injection stamp.</param>
    /// <returns>The entry.</returns>
    QueuedPrompt Delivered(const wchar_t* id, const wchar_t* text, int64_t at)
    {
        QueuedPrompt p;
        p.id = id;
        p.text = text;
        p.label = text;
        p.status = PromptStatus::Sent;
        p.origin = PromptOrigin::Autorun;
        p.echoed = false;
        p.sentAtUnixMs = at;
        p.injectedAtUnixMs = at;
        return p;
    }

    /// <summary>The screen as rows (the input box's detector reads rows).</summary>
    /// <param name="screen">The screen text.</param>
    /// <returns>Its rows.</returns>
    std::vector<std::wstring> RowsOf(const std::wstring& screen)
    {
        std::vector<std::wstring> rows;
        size_t start = 0;
        while (start <= screen.size())
        {
            const size_t nl = screen.find(L'\n', start);
            rows.push_back(screen.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start));
            if (nl == std::wstring::npos)
            {
                break;
            }
            start = nl + 1;
        }
        return rows;
    }
}

void TestTuiMenus()
{
    wprintf(L"== TestTuiMenus (MESSAGING.md 21.34: /add-dir's dialog answered from Telegram - the command, the screen, the press, the result, the card, the delivery fixes) ==\n");

    // ---- 1. the queued text ----
    {
        std::wstring args;
        CHECK(ParseAddDirCommandText(L"/add-dir Q:\\work\\other", args) && args == L"Q:\\work\\other", "/add-dir <path>: the command, the path its argument");
        CHECK(ParseAddDirCommandText(L"  /ADD-Dir   D:\\a b\\c  ", args) && args == L"D:\\a b\\c", "any case, whitespace around, the path's own space kept");
        CHECK(ParseAddDirCommandText(L"/add-dir", args) && args.empty(), "a bare /add-dir is the command with no path");
        CHECK(ParseAddDirCommandText(L"/add-dir\n../other", args) && args == L"../other", "the path may follow on the next line");
        CHECK(!ParseAddDirCommandText(L"/add-dirs x", args) && args.empty(), "a longer word is not the command");
        CHECK(!ParseAddDirCommandText(L"add-dir x", args), "without the slash it is text");
        CHECK(!ParseAddDirCommandText(L"/model opus", args), "another command is not it");
        CHECK(!ParseAddDirCommandText(L"", args), "an empty text is not it");
        const std::wstring usage = AddDirUsageHtml();
        CHECK(usage.find(L"<code>/add-dir &lt;path&gt;</code>") != std::wstring::npos && usage.find(L"60 s") != std::wstring::npos &&
                  usage.find(L"Yes, for this session") != std::wstring::npos,
              "the usage line shows the form that works, the default and its 60 s");
    }

    // ---- 2. the screen ----
    {
        const AddDirMenuScreen menu = ParseAddDirMenuScreen(kProbeMenuScreen);
        CHECK(menu.found, "the probe's capture of the open dialog is found");
        CHECK(menu.path == L"Q:\\tmp\\adddir-probe-1\\targets\\one", "its path is the dialog's second line, verbatim");
        CHECK(menu.focused == 1, "the cursor is on option 1, the default");
        CHECK(!ParseAddDirMenuScreen(kProbeBareScreen).found, "the bare command's path INPUT shares the title but is not the dialog (no options)");
        CHECK(!ParseAddDirMenuScreen(kProbeAfterScreen).found, "the screen after the answer holds no dialog (the transcript lines show /add-dir, not the title)");
        CHECK(!ParseAddDirMenuScreen(L"").found, "an empty screen (a dormant tab) holds no dialog");

        // The input box's detector reads the dialog as a MENU: the lost-send hold and the advance's hold depend on it.
        const auto d = DetectPendingInput(RowsOf(kProbeMenuScreen));
        CHECK(!d.boxFound && d.state == InputBoxState::MenuOpen, "the input box's detector reads the probe's dialog as MenuOpen");
        const auto after = DetectPendingInput(RowsOf(kProbeAfterScreen));
        CHECK(after.boxFound && after.state == InputBoxState::Empty, "and the screen after the answer as the empty input box again");

        // A path Ink wrapped onto two rows: its pieces joined; it still matches the user's form, which keeps the space.
        AddDirMenuScreen wrapped = ParseAddDirMenuScreen(Screen(DialogRows({ L"D:\\projects\\a very", L"long\\folder" })));
        CHECK(wrapped.found && wrapped.path == L"D:\\projects\\a verylong\\folder", "a wrapped path's pieces are joined without a separator");
        CHECK(AddDirPathsMatch(wrapped.path, L"D:\\projects\\a very long\\folder"), "and it still matches the typed path (whitespace never counts)");
        CHECK(AddDirShownPath(wrapped.path, L"D:\\projects\\a very long\\folder") == L"D:\\projects\\a very long\\folder", "the card shows the user's own form when it matches");

        // The note wrapped onto two rows in a narrow terminal: the path ends at its head; the rest is skipped.
        std::vector<std::wstring> narrow = DialogRows({ L"Q:\\x" });
        narrow[4] = L"   Claude Code will be able to read files in this directory and make";
        narrow.insert(narrow.begin() + 5, L"   edits when auto-accept edits is on.");
        const AddDirMenuScreen narrowMenu = ParseAddDirMenuScreen(Screen(narrow));
        CHECK(narrowMenu.found && narrowMenu.path == L"Q:\\x", "a wrapped note does not join the path");

        // Option 2 focused; a label a narrow terminal truncated.
        CHECK(ParseAddDirMenuScreen(Screen(DialogRows({ L"Q:\\x" }, 2))).focused == 2, "the focused option is the one with the cursor");
        std::vector<std::wstring> cut = DialogRows({ L"Q:\\x" });
        for (auto& r : cut)
        {
            if (r.find(L"2. Yes") != std::wstring::npos)
            {
                r = L"     2. Yes, and remember this dire\x2026";
            }
        }
        CHECK(ParseAddDirMenuScreen(Screen(cut)).found, "a label cut short with an ellipsis still names its option");

        // A frame left behind: the dialog followed by more than a footer (the input box under it) is not the open dialog.
        std::vector<std::wstring> stale = DialogRows({ L"Q:\\old" });
        for (const wchar_t* r : { L"\x2500\x2500\x2500\x2500", L"\x276F", L"\x2500\x2500\x2500\x2500", L"  status line", L"  another line" })
        {
            stale.push_back(r);
        }
        CHECK(!ParseAddDirMenuScreen(Screen(stale)).found, "a dialog with an input box below it is a frame the scrollback kept, not the open dialog");

        // Another menu under the same title, or the options out of order.
        std::vector<std::wstring> other = DialogRows({ L"Q:\\x" });
        for (auto& r : other)
        {
            if (r.find(L"2. Yes") != std::wstring::npos)
            {
                r = L"     2. Something else";
            }
        }
        CHECK(!ParseAddDirMenuScreen(Screen(other)).found, "a different option label: not this dialog");
        std::vector<std::wstring> swapped = DialogRows({ L"Q:\\x" });
        std::swap(swapped[7], swapped[8]);
        CHECK(!ParseAddDirMenuScreen(Screen(swapped)).found, "the options out of order: not this dialog");

        // Two dialogs on one screen: the BOTTOM one is the open one.
        std::vector<std::wstring> two = DialogRows({ L"Q:\\first" });
        two.push_back(L"  some transcript line");
        const auto second = DialogRows({ L"Q:\\second" });
        two.insert(two.end(), second.begin(), second.end());
        const AddDirMenuScreen bottom = ParseAddDirMenuScreen(Screen(two));
        CHECK(bottom.found && bottom.path == L"Q:\\second", "with two dialogs on the screen, the bottom one is read");

        // CRLF rows (ReadEntireBuffer ends rows with CRLF).
        std::wstring crlf = Screen(DialogRows({ L"Q:\\x" }));
        for (size_t at = crlf.find(L'\n'); at != std::wstring::npos; at = crlf.find(L'\n', at + 2))
        {
            crlf.insert(at, 1, L'\r');
        }
        const AddDirMenuScreen crlfMenu = ParseAddDirMenuScreen(crlf);
        CHECK(crlfMenu.found && crlfMenu.path == L"Q:\\x", "CRLF rows read the same");
    }

    // ---- 3. the paths ----
    {
        CHECK(AddDirPathsMatch(L"Q:\\Work\\Other", L"q:/work/other/"), "case, slashes and a trailing separator do not count");
        CHECK(!AddDirPathsMatch(L"Q:\\work\\other", L"Q:\\work\\others"), "a different folder does not match");
        CHECK(!AddDirPathsMatch(L"", L""), "two empty paths do not match");
        CHECK(AddDirArgsAbsolute(L"D:\\x") && AddDirArgsAbsolute(L"d:/x") && AddDirArgsAbsolute(L"\\\\server\\share") && AddDirArgsAbsolute(L"//server/share"),
              "drive-rooted and UNC paths are absolute");
        CHECK(!AddDirArgsAbsolute(L"..\\other") && !AddDirArgsAbsolute(L"~/x") && !AddDirArgsAbsolute(L"sub") && !AddDirArgsAbsolute(L"D:"),
              "relative, home-relative, a bare name and a bare drive are not");
        CHECK(AddDirMenuBelongsTo(L"Q:\\work\\other", L"Q:\\work\\other") && !AddDirMenuBelongsTo(L"Q:\\work\\other", L"Q:\\work\\else"),
              "a full path: the dialog belongs to the command only when it shows that path");
        CHECK(AddDirMenuBelongsTo(L"Q:\\work\\other", L"..\\other"), "a relative path: Claude Code resolved it, so the dialog is taken as the command's");
        CHECK(AddDirShownPath(L"Q:\\work\\other", L"..\\other") == L"Q:\\work\\other", "the card shows the resolved path when the typed form differs");
    }

    // ---- 4. the window's press ----
    {
        MenuAnswerRequest r;
        r.command = std::wstring{ kAddDirCommand };
        r.digit = 2;
        r.path = L"Q:\\tmp\\adddir-probe-1\\targets\\one";
        const std::wstring arg = BuildMenuAnswerArg(r);
        const auto back = ParseMenuAnswerArg(arg);
        CHECK(back && back->command == L"add-dir" && back->digit == 2 && back->path == r.path, "the action's argument round-trips");
        CHECK(!ParseMenuAnswerArg(L"") && !ParseMenuAnswerArg(L"add-dir") && !ParseMenuAnswerArg(L"add-dir\t0\tx") && !ParseMenuAnswerArg(L"add-dir\t12\tx") &&
                  !ParseMenuAnswerArg(L"\t1\tx"),
              "a malformed argument is refused (no command, a digit outside 1-9, two digits)");
        for (const auto res : { MenuAnswerResult::Pressed, MenuAnswerResult::Gone, MenuAnswerResult::Other, MenuAnswerResult::Busy, MenuAnswerResult::Failed })
        {
            CHECK(ParseMenuAnswerResult(MenuAnswerResultWord(res)) == res, "every reply word reads back as its result");
        }
        CHECK(ParseMenuAnswerResult(L"") == MenuAnswerResult::Failed && ParseMenuAnswerResult(L"yes") == MenuAnswerResult::Failed,
              "an empty or unknown reply reads as Failed");

        CHECK(DecideMenuAnswer(r, kProbeMenuScreen, false) == MenuAnswerResult::Pressed, "the card's dialog on the screen, the box free: press");
        CHECK(DecideMenuAnswer(r, kProbeMenuScreen, true) == MenuAnswerResult::Busy, "a delivery holding the box: nothing typed (busy)");
        CHECK(DecideMenuAnswer(r, kProbeAfterScreen, false) == MenuAnswerResult::Gone, "no dialog any more (answered at the PC): gone");
        CHECK(DecideMenuAnswer(r, L"", false) == MenuAnswerResult::Gone, "a dormant tab's empty screen: gone");
        MenuAnswerRequest elsewhere = r;
        elsewhere.path = L"Q:\\somewhere\\else";
        CHECK(DecideMenuAnswer(elsewhere, kProbeMenuScreen, false) == MenuAnswerResult::Other, "a dialog for another folder: other, nothing typed");
        MenuAnswerRequest any = r;
        any.path.clear();
        CHECK(DecideMenuAnswer(any, kProbeMenuScreen, false) == MenuAnswerResult::Pressed, "no path in the request: any /add-dir dialog");
        MenuAnswerRequest bad = r;
        bad.digit = 4;
        CHECK(DecideMenuAnswer(bad, kProbeMenuScreen, false) == MenuAnswerResult::Failed, "an option the dialog does not have: failed");
        bad = r;
        bad.command = L"model";
        CHECK(DecideMenuAnswer(bad, kProbeMenuScreen, false) == MenuAnswerResult::Failed, "another command's menu: failed (only /add-dir is known)");
    }

    // ---- 5. the transcript's word ----
    {
        // The probe's result line, verbatim (bold path, dim hint).
        const std::wstring added = L"Added \x1B[1mQ:\\tmp\\adddir-probe-1\\targets\\one\x1B[22m as a working directory for this session \x1B[2m\x00B7 /permissions to manage\x1B[22m";
        CHECK(StripTerminalControls(added) == L"Added Q:\\tmp\\adddir-probe-1\\targets\\one as a working directory for this session \x00B7 /permissions to manage",
              "the bold and dim sequences are stripped");
        CHECK(StripTerminalControls(L"a\x1B]8;;https://x\x07link\x1B]8;;\x1B\\b") == L"alinkb", "an OSC hyperlink (BEL and ST terminated) is stripped");
        CHECK(StripTerminalControls(L"x\x1B") == L"x" && StripTerminalControls(L"x\x1B" L"7y") == L"xy", "a lone ESC at the end and a two-character escape go too");

        const CommandTranscriptLine echo = ClassifyCommandTranscriptText(
            L"<command-name>/add-dir</command-name>\n            <command-message>add-dir</command-message>\n            <command-args>Q:\\tmp\\adddir-probe-1\\targets\\one</command-args>");
        CHECK(echo.kind == CommandTranscriptLine::Kind::Echo && echo.name == L"add-dir" && echo.args == L"Q:\\tmp\\adddir-probe-1\\targets\\one",
              "the probe's echo line reads as the command and its path");
        const CommandTranscriptLine out = ClassifyCommandTranscriptText(L"<local-command-stdout>" + added + L"</local-command-stdout>");
        CHECK(out.kind == CommandTranscriptLine::Kind::Output && out.text.find(L"Added Q:\\tmp") == 0 && out.text.find(L'\x1B') == std::wstring::npos,
              "the result line reads as the output, controls stripped");
        CHECK(ClassifyCommandTranscriptText(L"<local-command-stderr>boom</local-command-stderr>").text == L"boom", "the stderr form is an output too");
        CHECK(ClassifyCommandTranscriptText(L"<local-command-caveat>The command below was run directly in Claude Code</local-command-caveat>").kind ==
                  CommandTranscriptLine::Kind::None,
              "the caveat line is neither");
        CHECK(ClassifyCommandTranscriptText(L"please add the folder").kind == CommandTranscriptLine::Kind::None, "a prompt is neither");

        // Every message Claude Code 2.1.291 writes for /add-dir (its bundle's texts).
        CHECK(ClassifyAddDirOutput(StripTerminalControls(added)) == AddDirOutcome::AddedSession, "option 1: added for this session");
        CHECK(ClassifyAddDirOutput(L"Added Q:\\x as a working directory and saved to local settings \x00B7 /permissions to manage") == AddDirOutcome::AddedRemembered,
              "option 2: added and remembered");
        CHECK(ClassifyAddDirOutput(L"Added Q:\\x as a working directory. Failed to save to local settings: EACCES \x00B7 /permissions to manage") ==
                  AddDirOutcome::AddedNotSaved,
              "option 2 with a refused save: added, not remembered");
        CHECK(ClassifyAddDirOutput(L"Did not add Q:\\x as a working directory.") == AddDirOutcome::NotAdded, "option 3 or Esc: not added");
        CHECK(ClassifyAddDirOutput(L"Did not add a working directory.") == AddDirOutcome::NotAdded, "the bare command's Esc: not added");
        CHECK(ClassifyAddDirOutput(L"Q:\\x is already added as a working directory.") == AddDirOutcome::Already, "already added: already readable");
        CHECK(ClassifyAddDirOutput(L"Q:\\x is already the current working directory.") == AddDirOutcome::Already, "the session's own folder: already readable");
        CHECK(ClassifyAddDirOutput(L"Q:\\x\\sub is already accessible within the current working directory Q:\\x.") == AddDirOutcome::Already,
              "inside a working directory: already readable");
        CHECK(ClassifyAddDirOutput(L"Path Q:\\nope was not found.") == AddDirOutcome::Refused, "a missing folder: refused");
        CHECK(ClassifyAddDirOutput(L"Q:\\f.txt is not a directory. Did you mean to add the parent directory Q:\\?") == AddDirOutcome::Refused, "a file: refused");
        CHECK(ClassifyAddDirOutput(L"  ") == AddDirOutcome::Unknown, "no message: unknown");
        CHECK(AddDirOutcomeFitsOption(AddDirOutcome::AddedSession, 1) && AddDirOutcomeFitsOption(AddDirOutcome::AddedRemembered, 2) &&
                  AddDirOutcomeFitsOption(AddDirOutcome::AddedNotSaved, 2) && AddDirOutcomeFitsOption(AddDirOutcome::NotAdded, 3),
              "each option's outcome fits it");
        CHECK(!AddDirOutcomeFitsOption(AddDirOutcome::AddedRemembered, 1) && !AddDirOutcomeFitsOption(AddDirOutcome::AddedSession, 3) &&
                  !AddDirOutcomeFitsOption(AddDirOutcome::NotAdded, 0),
              "another option's outcome does not (the PC answered first)");
    }

    // ---- 6. the card ----
    {
        const std::wstring data = AddDirCallbackData(L"k3y0abcdef", 1);
        const auto tap = ParseAddDirCallbackData(data);
        CHECK(tap && tap->key == L"k3y0abcdef" && tap->digit == 1, "a button's data round-trips");
        CHECK(data.size() <= 64, "a button's data fits Telegram's 64 bytes");
        CHECK(!ParseAddDirCallbackData(L"q:k3y:1") && !ParseAddDirCallbackData(L"tm:k3y:4") && !ParseAddDirCallbackData(L"tm:K3Y:1") &&
                  !ParseAddDirCallbackData(L"tm::1") && !ParseAddDirCallbackData(L"tm:k3y:12") && !ParseAddDirCallbackData(L"tm:k3y"),
              "another feature's data, a digit outside 1-3, an uppercase key, no key and two digits are not a menu tap");

        const int64_t now = 1'760'000'000'000; // a fixed wall clock (the clock texts are local times; only their presence is checked)
        AddDirCardModel m;
        m.phase = AddDirCardPhase::Open;
        m.path = L"Q:\\a&b<c>";
        m.deadlineMs = now + kTuiMenuAutoAnswerMs;
        const std::wstring open = BuildAddDirCardHtml(m, now);
        CHECK(open.find(L"Add directory to workspace") != std::wstring::npos && open.find(L"<code>Q:\\a&amp;b&lt;c&gt;</code>") != std::wstring::npos,
              "Open: the dialog's title and its path, escaped");
        CHECK(open.find(L"<tg-time unix=\"" + std::to_wstring((now + kTuiMenuAutoAnswerMs) / 1000) + L"\" format=\"r\">") != std::wstring::npos,
              "Open: the countdown is a relative tg-time at the deadline");
        CHECK(open.find(L"Picks <b>Yes, for this session</b> by itself") != std::wstring::npos, "Open: it says the default is picked by itself");
        CHECK(open.find(L"auto-accept edits") != std::wstring::npos, "Open: what answering grants");
        const auto rows = BuildAddDirCardButtons(m, L"k3y");
        CHECK(rows.size() == 3 && rows[0].size() == 1 && rows[0][0].first.find(L"1 \x00B7 Yes, for this session") != std::wstring::npos &&
                  rows[0][0].first.find(L"\x23F1") == 0 && rows[2][0].first == L"3 \x00B7 No",
              "Open: one row per option, the default carries the clock's mark");
        for (const auto& row : rows)
        {
            const auto back = ParseAddDirCallbackData(row[0].second);
            CHECK(back && back->key == L"k3y", "Open: every button's data names the card");
        }

        m.phase = AddDirCardPhase::Answering;
        m.pressed = 2;
        m.pressedBy = AddDirAnswerer::Telegram;
        CHECK(BuildAddDirCardHtml(m, now).find(L"Answering</b> \x00B7 <b>Yes, and remember this directory</b>") != std::wstring::npos,
              "Answering: the option being pressed");
        CHECK(BuildAddDirCardButtons(m, L"k3y").empty(), "Answering: no buttons");
        m.pressedBy = AddDirAnswerer::Auto;
        m.pressed = 1;
        CHECK(BuildAddDirCardHtml(m, now).find(L"picked by itself after 60 s") != std::wstring::npos, "Answering by the clock: it says so");

        m.phase = AddDirCardPhase::Answered;
        m.outcome = AddDirOutcome::AddedSession;
        m.outcomeText = L"Added Q:\\x as a working directory for this session \x00B7 /permissions to manage";
        m.outcomeAtMs = now;
        m.answeredBy = AddDirAnswerer::Auto;
        const std::wstring answered = BuildAddDirCardHtml(m, now);
        CHECK(answered.find(L"Added for this session</b> \x00B7 \x23F1\xFE0F picked by itself after 60 s") != std::wstring::npos &&
                  answered.find(L"<i>Claude Code:</i> Added Q:\\x") != std::wstring::npos,
              "Answered by the clock: the outcome, who, and Claude Code's own message");
        m.answeredBy = AddDirAnswerer::Telegram;
        CHECK(BuildAddDirCardHtml(m, now).find(L"answered from Telegram") != std::wstring::npos, "Answered from Telegram: it says so");
        m.answeredBy = AddDirAnswerer::Pc;
        m.outcome = AddDirOutcome::NotAdded;
        const std::wstring pc = BuildAddDirCardHtml(m, now);
        CHECK(pc.find(L"Not added</b> \x00B7 answered at the PC") != std::wstring::npos, "Answered at the PC: the outcome and who");
        CHECK(BuildAddDirCardButtons(m, L"k3y").empty(), "Answered: no buttons (the edit removes them)");

        m.phase = AddDirCardPhase::Closed;
        m.closedWhy = L"its session ended";
        CHECK(BuildAddDirCardHtml(m, now).find(L"No longer open here</b>") != std::wstring::npos && BuildAddDirCardHtml(m, now).find(L"its session ended") != std::wstring::npos,
              "Closed: it says it is no longer open here, and why");

        const std::wstring reply = BuildAddDirReplyHtml(AddDirOutcome::Already, L"Q:\\x is already added as a working directory.");
        CHECK(reply.find(L"Already in the workspace") != std::wstring::npos && reply.find(L"<i>Q:\\x is already added") != std::wstring::npos,
              "the reply for a command with no dialog: the outcome and Claude Code's message");
        CHECK(AddDirUnreadableHtml().find(L"answer it at the PC") != std::wstring::npos, "the unreadable note names the PC");
    }

    // ---- 7. the delivery fixes ----
    {
        // The echo matched to the queued text.
        CHECK(CommandEchoMatchesPromptText(L"add-dir", L"Q:\\x", L"/add-dir Q:\\x"), "the echo of /add-dir <path> is the prompt that typed it");
        CHECK(CommandEchoMatchesPromptText(L"add-dir", L"Q:\\x", L"  /ADD-DIR   Q:\\x  \n"), "the command word in any case, whitespace around");
        CHECK(CommandEchoMatchesPromptText(L"model", L"opus", L"/model opus") && CommandEchoMatchesPromptText(L"compact", L"", L"/compact"),
              "any local command, with or without arguments");
        CHECK(CommandEchoMatchesPromptText(L"/add-dir", L"Q:\\a  b", L"/add-dir Q:\\a b"), "a '/' on the echo's name and a run of spaces do not count");
        CHECK(CommandEchoMatchesPromptText(L"add-dir", L"Q:\\x", L"/add-dir Q:\\\x200Bx"), "an invisible character claude's submit drops does not count");
        CHECK(!CommandEchoMatchesPromptText(L"add-dir", L"Q:\\y", L"/add-dir Q:\\x"), "other arguments: another run of the command");
        CHECK(!CommandEchoMatchesPromptText(L"add-dir", L"Q:\\x", L"/add-dirs Q:\\x") && !CommandEchoMatchesPromptText(L"add-dir", L"Q:\\x", L"add-dir Q:\\x"),
              "a longer word, or text without the slash, is not the command");
        CHECK(!CommandEchoMatchesPromptText(L"", L"", L"/"), "an empty name matches nothing");

        const int64_t T = 1'760'000'000'000;
        SessionInfo s = MakeSession(L"menu", SessionState::WaitingForInput);
        s.live = true;
        s.queue.push_back(Delivered(L"p1", L"/add-dir Q:\\x", T));
        CHECK(PickCommandEchoPrompt(s, L"add-dir", L"Q:\\x", T + 5000) == L"p1", "the echo (written once the dialog was answered) vouches for the delivered command");
        CHECK(PickCommandEchoPrompt(s, L"add-dir", L"Q:\\x", 0) == L"p1", "an echo without a timestamp vouches too (the prompt line's rule)");
        CHECK(PickCommandEchoPrompt(s, L"add-dir", L"Q:\\x", T - kCommandEchoSkewMs) == L"p1", "an echo stamped within the skew before the delivery counts");
        CHECK(PickCommandEchoPrompt(s, L"add-dir", L"Q:\\x", T - kCommandEchoSkewMs - 1).empty(), "an echo older than that is an earlier run: it vouches for nothing");
        CHECK(PickCommandEchoPrompt(s, L"add-dir", L"Q:\\y", T + 5000).empty(), "an echo of other arguments vouches for nothing");
        s.queue[0].echoed = true;
        CHECK(PickCommandEchoPrompt(s, L"add-dir", L"Q:\\x", T + 5000).empty(), "an echoed prompt is not taken twice");
        s.queue[0].echoed = false;
        s.queue[0].status = PromptStatus::Pending;
        CHECK(PickCommandEchoPrompt(s, L"add-dir", L"Q:\\x", T + 5000).empty(), "a queued (not sent) prompt has no echo yet");
        s.queue[0].status = PromptStatus::Sent;
        s.queue[0].origin = PromptOrigin::Typed;
        CHECK(PickCommandEchoPrompt(s, L"add-dir", L"Q:\\x", T + 5000).empty(), "a Typed record is not a delivery");
        s.queue[0].origin = PromptOrigin::Autorun;
        s.queue[0].injectedAtUnixMs = 0;
        CHECK(PickCommandEchoPrompt(s, L"add-dir", L"Q:\\x", T + 5000) == L"p1", "a delivery not stamped as injected yet is judged by its send stamp");
        s.queue[0].injectedAtUnixMs = T;
        s.queue.push_back(Delivered(L"p2", L"/add-dir Q:\\x", T + 1000));
        CHECK(PickCommandEchoPrompt(s, L"add-dir", L"Q:\\x", T + 5000) == L"p1", "two deliveries of the same command: the older one takes the first echo");

        // The lost-send verdict waits while a menu is open, and judges once it closed.
        SessionInfo lost = MakeSession(L"menu", SessionState::WaitingForInput);
        lost.live = true;
        lost.queue.push_back(Delivered(L"l1", L"/add-dir Q:\\x", T));
        CHECK(DecideLostSend(lost, T + kLostSendSettleMs).size() == 1, "an unechoed delivered command at rest is lost once settled (the baseline)");
        lost.pendingBoxState = InputBoxState::MenuOpen;
        CHECK(DecideLostSend(lost, T + kLostSendSettleMs).empty(), "while a menu is open (the dialog waits for its answer), nothing is judged lost");
        CHECK(DecideLostSend(lost, T + 20 * kLostSendSettleMs).empty(), "however long the menu stays open");
        lost.pendingBoxState = InputBoxState::Empty;
        CHECK(DecideLostSend(lost, T + 20 * kLostSendSettleMs).size() == 1, "once the menu closed, a send it never answered is judged as before");
        lost.pendingBoxState = InputBoxState::NoBox;
        CHECK(DecideLostSend(lost, T + kLostSendSettleMs).size() == 1, "a bare NoBox (no menu) holds nothing new");
    }
}
