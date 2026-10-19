// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Claude Code's TUI MENUS of LOCAL commands, answered from Telegram (MESSAGING.md §21.34). The first one is
// /add-dir's "Add directory to workspace" dialog. The user's request (2026-10-19): "Can you support /add-dir from here and also
// picker for is supporting the menu that comes after of in this session or this folder or whatever that asks you. You can see
// how we deal with agentmaster's AskUserQuestion ... The default is the 1st option and we should support in agentmaster
// auto-answer after 60s in a clean way".
//
// What Claude Code does (2.1.291 — measured live by tools/add-dir-probe, and read in its bundle):
//   * "/add-dir <path>" is a LOCAL command: Claude Code runs it itself. It fires no UserPromptSubmit (and no
//     UserPromptExpansion) hook and writes no prompt line. A path that resolves to a directory the session cannot read yet opens
//     this dialog in place of the input box (the RESOLVED absolute path on its second line):
//         Add directory to workspace
//         Q:\work\other
//         Claude Code will be able to read files in this directory and make edits when auto-accept edits is on.
//         ❯ 1. Yes, for this session
//           2. Yes, and remember this directory          (saved to <cwd>\.claude\settings.local.json)
//           3. No
//         Enter to confirm · Esc to cancel
//     A digit picks its option at once (no Enter needed); Esc is "No". NOTHING reaches the transcript while it is open; once it
//     is answered three lines land together: a <local-command-caveat> meta line, the ECHO (<command-name>/add-dir
//     </command-name> + <command-args>) and the RESULT (<local-command-stdout>); an add also fires a DirectoryAdded hook.
//   * A path that is already accessible, missing, not a directory, a network share or unreadable opens NO dialog: the echo and
//     its message land at once. A BARE "/add-dir" opens a path INPUT under the same title — nothing a phone can drive, so the
//     service refuses it with the usage line instead of sending it.
//
// What Agentmaster does with it:
//   * A Telegram /add-dir is queued like every message of a topic (D30). The service WATCHES it (Messaging.Menus.cpp): once it
//     is delivered and the box reads as a menu, the window reads the screen ("menu-screen") and ParseAddDirMenuScreen finds the
//     dialog; the topic gets ONE managed MENU CARD with the three options as buttons and a countdown — the user's rule: the
//     dialog's DEFAULT, option 1, is picked by itself kTuiMenuAutoAnswerMs after the dialog opened. A tap (or the clock) asks the
//     hosting window to press the option's digit ("menu-answer"), and the window presses ONLY into the very dialog the card
//     shows (DecideMenuAnswer: the dialog is still on the screen, its path is the card's, and no delivery holds the box). The
//     transcript's result then edits the card with the outcome and who answered: from Telegram, by itself after 60 s, or at
//     the PC (the PC's dialog stays fully usable; the first answer wins).
//   * A command with no dialog gets its result as a reply to the user's message; a dialog this build cannot read gets a note
//     that names the PC.
//   * The delivery's own bookkeeping needed two fixes, outside this header: the transcript's command ECHO is now the delivered
//     prompt's pull echo (Scheduler.h PickCommandEchoPrompt; before, every local command sent from a topic stayed Sent and
//     unechoed, so the lost-send verdict failed it with a 👎 and paused the runner 15 s later), and the lost-send verdict waits
//     while a menu is open (DecideLostSend: the dialog's command writes its echo only once it is answered).
//
// This header is the pure half (no service, no window, no Telegram call): the command and its dialog's texts, the screen
// parser, the window's decision and the action protocol, the transcript's result, and the card's text and buttons. The m5 suite
// pins every rule (tests_tuimenus.cpp), the screen parser against the probe's own capture of the dialog.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CommandWatch.h" // ParseCommandEcho: the ONE reader of a transcript's command echo (the scanner's and CommandWatch's)
#include "TelegramFormat.h" // TgEscapeHtml, TgClockText

namespace Agentmaster::Msg
{
    // ===== the command, its dialog and the user's rule ====================================================================

    /// <summary>
    /// The command word (lowercase, without the '/'): Claude Code's /add-dir. A message of a topic that queues it with a path is
    /// watched (MessagingService::_watchAddDir); a bare one is refused with AddDirUsageHtml.
    /// </summary>
    inline constexpr std::wstring_view kAddDirCommand = L"add-dir";

    /// <summary>
    /// The user's rule (2026-10-19): a dialog nobody answers is answered with its DEFAULT this long after Agentmaster saw it
    /// open. Counted from the moment the dialog was found on the screen, so a slow detection never shortens the user's minute;
    /// the card shows the deadline as a countdown the Telegram apps keep current.
    /// </summary>
    inline constexpr int64_t kTuiMenuAutoAnswerMs = 60 * 1000;

    /// <summary>
    /// The option the clock picks: 1, "Yes, for this session" — the dialog's own default focus (the one Enter takes), and the
    /// one that changes nothing on disk (2 writes the project's local settings, 3 adds nothing — what the user asked for is a
    /// folder in the workspace).
    /// </summary>
    inline constexpr int kAddDirDefaultOption = 1;

    /// <summary>One option of the dialog, as Claude Code 2.1.291 defines it (its options table, verbatim).</summary>
    struct AddDirOption
    {
        int digit; // the key that picks it in the dialog, and the number on its button
        std::wstring_view value; // Claude Code's own value for it (yes-session / yes-remember / no), for the logs
        std::wstring_view label; // the label the dialog shows, verbatim: the screen parser keys on it
    };

    /// <summary>The dialog's three options in their order on the screen (the digit is the position).</summary>
    inline constexpr AddDirOption kAddDirOptions[] = {
        { 1, L"yes-session", L"Yes, for this session" },
        { 2, L"yes-remember", L"Yes, and remember this directory" },
        { 3, L"no", L"No" },
    };

    /// <summary>The dialog's title line, verbatim (the bare command's path input shares it, which is why the options are required too).</summary>
    inline constexpr std::wstring_view kAddDirMenuTitle = L"Add directory to workspace";

    /// <summary>The head of the dim line under the path ("… and make edits when auto-accept edits is on."): where the path ends.</summary>
    inline constexpr std::wstring_view kAddDirMenuNoteHead = L"Claude Code will be able to read files in this directory";

    /// <summary>The dim line under the path, verbatim, for the card (it says what the answer grants).</summary>
    inline constexpr std::wstring_view kAddDirMenuNote = L"Claude Code will be able to read files in this directory and make edits when auto-accept edits is on.";

    /// <summary>
    /// The most non-blank lines the dialog's bottom may carry after its last option (the "Enter to confirm · Esc to cancel"
    /// footer, and room for a notification line). A title higher up whose options are followed by more is a dialog the screen
    /// left behind (a frame of the classic renderer pushed into the scrollback), never the open one.
    /// </summary>
    inline constexpr size_t kAddDirMenuTailMaxLines = 4;

    /// <summary>The option of a digit.</summary>
    /// <param name="digit">1, 2 or 3.</param>
    /// <returns>The option, or nullptr for any other number.</returns>
    inline const AddDirOption* AddDirOptionFor(int digit) noexcept
    {
        for (const auto& o : kAddDirOptions)
        {
            if (o.digit == digit)
            {
                return &o;
            }
        }
        return nullptr;
    }

    namespace tuimenu_detail
    {
        /// <summary>The selection cursor of Claude Code's menus (❯), and the older one (›) the input box's detector accepts too.</summary>
        inline constexpr wchar_t kCaretA = L'\x276F';
        inline constexpr wchar_t kCaretB = L'\x203A';

        /// <summary>Whitespace as the screen and a queued text carry it: ASCII spaces, line breaks, and the no-break space Ink pads with.</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for whitespace.</returns>
        inline constexpr bool IsWs(wchar_t c) noexcept
        {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f' || c == L'\v' || c == 0x00A0;
        }

        /// <summary>A text without its leading and trailing whitespace (IsWs).</summary>
        /// <param name="s">The text.</param>
        /// <returns>The view inside `s`.</returns>
        inline std::wstring_view Trim(std::wstring_view s) noexcept
        {
            size_t a = 0;
            while (a < s.size() && IsWs(s[a]))
            {
                ++a;
            }
            size_t b = s.size();
            while (b > a && IsWs(s[b - 1]))
            {
                --b;
            }
            return s.substr(a, b - a);
        }

        /// <summary>ASCII lowercase of one character (a command word is ASCII; anything else is left as it is).</summary>
        /// <param name="c">The character.</param>
        /// <returns>Its lowercase.</returns>
        inline constexpr wchar_t AsciiLower(wchar_t c) noexcept
        {
            return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        }

        /// <summary>The screen's lines, split on LF with a trailing CR dropped from each (ReadEntireBuffer ends rows with CRLF).</summary>
        /// <param name="screen">The screen text.</param>
        /// <returns>The lines, top to bottom (views into `screen`).</returns>
        /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
        inline std::vector<std::wstring_view> SplitLines(std::wstring_view screen)
        {
            std::vector<std::wstring_view> lines;
            size_t start = 0;
            while (start <= screen.size())
            {
                const size_t nl = screen.find(L'\n', start);
                std::wstring_view line = screen.substr(start, nl == std::wstring_view::npos ? std::wstring_view::npos : nl - start);
                if (!line.empty() && line.back() == L'\r')
                {
                    line.remove_suffix(1);
                }
                lines.push_back(line);
                if (nl == std::wstring_view::npos)
                {
                    break;
                }
                start = nl + 1;
            }
            return lines;
        }

        /// <summary>
        /// Reads one screen line as an OPTION of a numbered menu: optional indentation, an optional selection cursor (❯ or ›) and
        /// its space, then "N." and the label.
        /// </summary>
        /// <param name="line">The screen line.</param>
        /// <param name="digit">Out: N (0 when the line is no option).</param>
        /// <param name="label">Out: the label, trimmed.</param>
        /// <param name="caret">Out: whether the line carries the selection cursor (the focused option).</param>
        /// <returns>True for an option line.</returns>
        inline bool ReadOptionLine(std::wstring_view line, int& digit, std::wstring_view& label, bool& caret) noexcept
        {
            digit = 0;
            label = {};
            caret = false;
            std::wstring_view t = Trim(line);
            if (!t.empty() && (t.front() == kCaretA || t.front() == kCaretB))
            {
                caret = true;
                t = Trim(t.substr(1));
            }
            if (t.size() < 3 || t[0] < L'1' || t[0] > L'9' || t[1] != L'.' || !IsWs(t[2]))
            {
                return false;
            }
            digit = t[0] - L'0';
            label = Trim(t.substr(2));
            return !label.empty();
        }

        /// <summary>
        /// Whether a shown option label is the expected one: equal, or the expected label cut short with an ellipsis (a narrow
        /// terminal truncates a long label as "Yes, and remember this dire…").
        /// </summary>
        /// <param name="shown">The label on the screen, trimmed.</param>
        /// <param name="expected">The option's label (kAddDirOptions).</param>
        /// <returns>True when they name the same option.</returns>
        inline bool LabelMatches(std::wstring_view shown, std::wstring_view expected) noexcept
        {
            if (shown == expected)
            {
                return true;
            }
            if (shown.size() >= 2 && shown.back() == L'\x2026')
            {
                const std::wstring_view head = Trim(shown.substr(0, shown.size() - 1));
                return !head.empty() && expected.substr(0, head.size()) == head;
            }
            return false;
        }

        /// <summary>
        /// A path in the form two paths are compared in: every whitespace character dropped (a path Ink wrapped onto two lines comes
        /// back without the space it wrapped at, and the user's form keeps it), '/' as '\', no trailing separators, lowercase
        /// (Windows paths are case-insensitive, and Node keeps the drive letter's case as typed).
        /// </summary>
        /// <param name="path">The path.</param>
        /// <returns>The compare key ("" for a path of only whitespace).</returns>
        /// <exception cref="std::bad_alloc">If the key cannot be allocated.</exception>
        inline std::wstring PathKey(std::wstring_view path)
        {
            std::wstring key;
            key.reserve(path.size());
            for (const wchar_t c : path)
            {
                if (IsWs(c))
                {
                    continue;
                }
                key.push_back(c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c))));
            }
            while (!key.empty() && key.back() == L'\\')
            {
                key.pop_back();
            }
            return key;
        }
    }

    // ===== reading a queued text =====================================================================================

    /// <summary>
    /// Reads a queued prompt as "/add-dir [path]": the command word in any case after optional whitespace, then whitespace or
    /// the end. What follows is the path as typed (Claude Code resolves it against the session's folder, expanding "~").
    /// </summary>
    /// <param name="text">The prompt text as queued (a topic's message, or "/cc /add-dir …"'s forced command).</param>
    /// <param name="args">Out: the rest, trimmed ("" for a bare /add-dir); left empty when the text is not the command.</param>
    /// <returns>True when the text is the /add-dir command, bare or with a path.</returns>
    /// <exception cref="std::bad_alloc">If `args` cannot be assigned.</exception>
    inline bool ParseAddDirCommandText(std::wstring_view text, std::wstring& args)
    {
        using namespace tuimenu_detail;
        args.clear();
        const std::wstring_view t = Trim(text);
        if (t.size() < kAddDirCommand.size() + 1 || t.front() != L'/')
        {
            return false;
        }
        size_t end = 1;
        while (end < t.size() && !IsWs(t[end]))
        {
            ++end;
        }
        const std::wstring_view word = t.substr(1, end - 1);
        if (word.size() != kAddDirCommand.size())
        {
            return false;
        }
        for (size_t i = 0; i < word.size(); ++i)
        {
            if (AsciiLower(word[i]) != kAddDirCommand[i])
            {
                return false;
            }
        }
        args.assign(Trim(t.substr(end)));
        return true;
    }

    // ===== reading the screen ========================================================================================

    /// <summary>What ParseAddDirMenuScreen found on a screen.</summary>
    struct AddDirMenuScreen
    {
        bool found{ false }; // the OPEN /add-dir dialog with its three options is on the screen
        std::wstring path; // the dialog's path (its lines joined without separators: see AddDirShownPath for the form to show)
        int focused{ 0 }; // the option the cursor is on (1-3; 0 when no cursor was read)
    };

    /// <summary>
    /// Finds Claude Code's OPEN "Add directory to workspace" dialog on a screen (the hosting window's read of the terminal's last
    /// rows): the BOTTOM-MOST title line, the path lines under it (up to the dim note, or the first option), then the three options
    /// in order with their exact labels, and at most kAddDirMenuTailMaxLines non-blank lines after the last one. The bare command's
    /// path input shares the title but has no options, and an earlier dialog's frame the scrollback kept is followed by more than
    /// a footer, so neither reads as the dialog. Pure.
    /// </summary>
    /// <param name="screen">The screen's text, top to bottom, rows separated by LF (a CR before it is dropped).</param>
    /// <returns>The dialog, or found = false.</returns>
    /// <exception cref="std::bad_alloc">If the lines or the path cannot be allocated.</exception>
    inline AddDirMenuScreen ParseAddDirMenuScreen(std::wstring_view screen)
    {
        using namespace tuimenu_detail;
        AddDirMenuScreen out;
        const std::vector<std::wstring_view> lines = SplitLines(screen);
        size_t title = lines.size();
        for (size_t i = lines.size(); i-- > 0;)
        {
            if (Trim(lines[i]) == kAddDirMenuTitle)
            {
                title = i;
                break;
            }
        }
        if (title == lines.size())
        {
            return out;
        }
        std::wstring path;
        bool pathDone = false; // the note (or the first option) ended the path
        int next = 1; // the option expected next
        int focused = 0;
        size_t tail = 0; // non-blank lines after the last option
        for (size_t i = title + 1; i < lines.size(); ++i)
        {
            const std::wstring_view t = Trim(lines[i]);
            if (next > 3)
            {
                if (!t.empty() && ++tail > kAddDirMenuTailMaxLines)
                {
                    return out; // more than a footer under the options: a frame left behind, not the open dialog
                }
                continue;
            }
            if (t.empty())
            {
                continue;
            }
            int digit = 0;
            std::wstring_view label;
            bool caret = false;
            if (ReadOptionLine(t, digit, label, caret))
            {
                if (digit != next || !LabelMatches(label, kAddDirOptions[next - 1].label))
                {
                    return out; // another menu, or the options in another order: not this dialog
                }
                if (caret)
                {
                    focused = digit;
                }
                pathDone = true;
                ++next;
                continue;
            }
            if (next > 1)
            {
                return out; // text between the options: not this dialog
            }
            if (t.substr(0, kAddDirMenuNoteHead.size()) == kAddDirMenuNoteHead)
            {
                pathDone = true; // the note (it may wrap onto further lines: they are skipped until the first option)
                continue;
            }
            if (!pathDone)
            {
                path.append(t); // a path Ink wrapped comes in pieces; they are joined without a separator
            }
        }
        if (next <= 3 || path.empty())
        {
            return out; // the options are not all there (the path input, a dialog still drawing), or no path was read
        }
        out.found = true;
        out.path = std::move(path);
        out.focused = focused;
        return out;
    }

    /// <summary>
    /// Whether two paths name the same folder for the card's purposes: equal after PathKey (whitespace dropped, '/' as '\', no
    /// trailing separator, any case). Used to tie the dialog on the screen to the command the user sent, and the window's press
    /// to the dialog the card shows.
    /// </summary>
    /// <param name="a">One path.</param>
    /// <param name="b">The other.</param>
    /// <returns>True when both are non-empty and match.</returns>
    /// <exception cref="std::bad_alloc">If a compare key cannot be allocated.</exception>
    inline bool AddDirPathsMatch(std::wstring_view a, std::wstring_view b)
    {
        const std::wstring ka = tuimenu_detail::PathKey(a);
        return !ka.empty() && ka == tuimenu_detail::PathKey(b);
    }

    /// <summary>
    /// Whether the user's /add-dir argument is a full path (a drive root "X:\" or "X:/", or a UNC path) — the only form the
    /// dialog's path can be compared with: Claude Code resolves anything else against the session's folder (or "~"), which the
    /// service does not know the way Claude Code does.
    /// </summary>
    /// <param name="args">The argument as typed.</param>
    /// <returns>True for an absolute path.</returns>
    inline bool AddDirArgsAbsolute(std::wstring_view args) noexcept
    {
        const std::wstring_view t = tuimenu_detail::Trim(args);
        if (t.size() >= 3 && ((t[0] >= L'A' && t[0] <= L'Z') || (t[0] >= L'a' && t[0] <= L'z')) && t[1] == L':' && (t[2] == L'\\' || t[2] == L'/'))
        {
            return true;
        }
        return t.size() >= 2 && ((t[0] == L'\\' && t[1] == L'\\') || (t[0] == L'/' && t[1] == L'/'));
    }

    /// <summary>
    /// Whether a dialog found on the screen belongs to the command the user sent: always for a relative argument (Claude Code
    /// resolved it; the delivered command is the newest, and the dialog appeared after it), and for a full path only when the
    /// dialog shows that path.
    /// </summary>
    /// <param name="screenPath">The dialog's path (AddDirMenuScreen::path).</param>
    /// <param name="typedArgs">The command's argument as the user typed it.</param>
    /// <returns>True when the card may show this dialog.</returns>
    /// <exception cref="std::bad_alloc">If a compare key cannot be allocated.</exception>
    inline bool AddDirMenuBelongsTo(std::wstring_view screenPath, std::wstring_view typedArgs)
    {
        return !AddDirArgsAbsolute(typedArgs) || AddDirPathsMatch(screenPath, typedArgs);
    }

    /// <summary>
    /// The path the card shows: the user's own form when it names the dialog's folder (it keeps a space the wrap dropped from the
    /// screen's lines), else the dialog's resolved path as read.
    /// </summary>
    /// <param name="screenPath">The dialog's path as read.</param>
    /// <param name="typedArgs">The command's argument as typed.</param>
    /// <returns>The path to show.</returns>
    /// <exception cref="std::bad_alloc">If the path cannot be copied.</exception>
    inline std::wstring AddDirShownPath(std::wstring_view screenPath, std::wstring_view typedArgs)
    {
        if (AddDirPathsMatch(screenPath, typedArgs))
        {
            return std::wstring{ tuimenu_detail::Trim(typedArgs) };
        }
        return std::wstring{ screenPath };
    }

    // ===== the window's side: the two actions and the press ===============================================================

    /// <summary>
    /// The session action that reads the screen for the menu watch (ServiceHooks::sessionAction): the hosting window answers with
    /// the terminal's last rows as text (TerminalPage::_ReadScreenTextForSession), "" for a dormant tab.
    /// </summary>
    inline constexpr std::wstring_view kMenuScreenAction = L"menu-screen";

    /// <summary>The session action that presses a menu's option (its argument: BuildMenuAnswerArg; its answer: MenuAnswerResultWord).</summary>
    inline constexpr std::wstring_view kMenuAnswerAction = L"menu-answer";

    /// <summary>How many rows of the screen the window reads for either action: the dialog is about a dozen rows at the bottom.</summary>
    inline constexpr size_t kMenuScreenRows = 40;

    /// <summary>What the service asks the window to press.</summary>
    struct MenuAnswerRequest
    {
        std::wstring command; // the menu's command (kAddDirCommand, the only one so far)
        int digit{ 0 }; // the option to press (1-3)
        std::wstring path; // the path the card shows: the window presses only into a dialog that shows it ("" = any path)
    };

    /// <summary>The action's argument: "&lt;command&gt;\t&lt;digit&gt;\t&lt;path&gt;" (a Windows path holds no tab).</summary>
    /// <param name="r">The request.</param>
    /// <returns>The argument.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildMenuAnswerArg(const MenuAnswerRequest& r)
    {
        return r.command + L"\t" + std::to_wstring(r.digit) + L"\t" + r.path;
    }

    /// <summary>Reads the action's argument (BuildMenuAnswerArg's form).</summary>
    /// <param name="arg">The argument.</param>
    /// <returns>The request, or nullopt for anything malformed (no command, a digit outside 1-9).</returns>
    /// <exception cref="std::bad_alloc">If the request cannot be allocated.</exception>
    inline std::optional<MenuAnswerRequest> ParseMenuAnswerArg(std::wstring_view arg)
    {
        const size_t t1 = arg.find(L'\t');
        if (t1 == std::wstring_view::npos || t1 == 0)
        {
            return std::nullopt;
        }
        const size_t t2 = arg.find(L'\t', t1 + 1);
        if (t2 == std::wstring_view::npos || t2 != t1 + 2 || arg[t1 + 1] < L'1' || arg[t1 + 1] > L'9')
        {
            return std::nullopt;
        }
        MenuAnswerRequest r;
        r.command.assign(arg.substr(0, t1));
        r.digit = arg[t1 + 1] - L'0';
        r.path.assign(arg.substr(t2 + 1));
        return r;
    }

    /// <summary>The window's answer to a press request.</summary>
    enum class MenuAnswerResult
    {
        Pressed, // the dialog the card shows was on the screen: its option's digit was typed
        Gone, // no such dialog on the screen any more (answered or closed at the PC, the tab dormant or gone)
        Other, // a /add-dir dialog for ANOTHER path is open now (this one was answered at the PC, and another opened)
        Busy, // a delivery or a draft operation holds the input box right now: nothing was typed (ask again in a moment)
        Failed, // the request was malformed, or the digit could not be written (no injector): nothing was typed
    };

    /// <summary>The word of a result, as the window replies it (and the log prints it).</summary>
    /// <param name="r">The result.</param>
    /// <returns>pressed / gone / other / busy / failed.</returns>
    inline std::wstring_view MenuAnswerResultWord(MenuAnswerResult r) noexcept
    {
        switch (r)
        {
        case MenuAnswerResult::Pressed:
            return L"pressed";
        case MenuAnswerResult::Gone:
            return L"gone";
        case MenuAnswerResult::Other:
            return L"other";
        case MenuAnswerResult::Busy:
            return L"busy";
        case MenuAnswerResult::Failed:
        default:
            return L"failed";
        }
    }

    /// <summary>Reads a window's reply (MenuAnswerResultWord's words).</summary>
    /// <param name="word">The reply.</param>
    /// <returns>The result; anything unknown (an empty reply included) reads as Failed.</returns>
    inline MenuAnswerResult ParseMenuAnswerResult(std::wstring_view word) noexcept
    {
        for (const MenuAnswerResult r : { MenuAnswerResult::Pressed, MenuAnswerResult::Gone, MenuAnswerResult::Other, MenuAnswerResult::Busy })
        {
            if (word == MenuAnswerResultWord(r))
            {
                return r;
            }
        }
        return MenuAnswerResult::Failed;
    }

    /// <summary>
    /// The hosting window's decision before it types a digit into a terminal (TerminalPage::_RemoteSessionAction, "menu-answer"):
    /// press ONLY into the dialog the card shows. A digit typed anywhere else lands in the input box as text, so the screen is
    /// read at the press itself: the dialog must still be open (Gone otherwise), it must show the card's path (Other otherwise),
    /// and no delivery or draft operation may hold the box (Busy — the press would interleave with its keystrokes). Pure.
    /// </summary>
    /// <param name="request">What the service asked for (ParseMenuAnswerArg).</param>
    /// <param name="screen">The window's read of the terminal's last rows, made right now.</param>
    /// <param name="boxHeld">True while the session's delivery gate is held or a draft swap owns the box.</param>
    /// <returns>Pressed when the digit may be typed now; else why not.</returns>
    /// <exception cref="std::bad_alloc">If the screen's lines cannot be allocated.</exception>
    inline MenuAnswerResult DecideMenuAnswer(const MenuAnswerRequest& request, std::wstring_view screen, bool boxHeld)
    {
        if (request.command != kAddDirCommand || AddDirOptionFor(request.digit) == nullptr)
        {
            return MenuAnswerResult::Failed;
        }
        const AddDirMenuScreen menu = ParseAddDirMenuScreen(screen);
        if (!menu.found)
        {
            return MenuAnswerResult::Gone;
        }
        if (!request.path.empty() && !AddDirPathsMatch(menu.path, request.path))
        {
            return MenuAnswerResult::Other;
        }
        return boxHeld ? MenuAnswerResult::Busy : MenuAnswerResult::Pressed;
    }

    // ===== the transcript's word on it ================================================================================

    /// <summary>
    /// A text without its terminal control sequences: CSI sequences (ESC [ … final byte: the bold and dim Claude Code wraps the
    /// path and the "/permissions to manage" hint in), OSC sequences (ESC ] … BEL or ESC \: a hyperlink), and any other ESC pair.
    /// </summary>
    /// <param name="text">The text (a command's stdout as the transcript keeps it).</param>
    /// <returns>The text as it reads on the screen.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring StripTerminalControls(std::wstring_view text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            const wchar_t c = text[i];
            if (c != L'\x1B')
            {
                out.push_back(c);
                continue;
            }
            if (i + 1 >= text.size())
            {
                break;
            }
            const wchar_t kind = text[i + 1];
            if (kind == L'[')
            {
                size_t j = i + 2;
                while (j < text.size() && !(text[j] >= 0x40 && text[j] <= 0x7E))
                {
                    ++j;
                }
                i = j; // the final byte (or the end)
            }
            else if (kind == L']')
            {
                size_t j = i + 2;
                while (j < text.size() && text[j] != L'\x07' && !(text[j] == L'\x1B' && j + 1 < text.size() && text[j + 1] == L'\\'))
                {
                    ++j;
                }
                i = (j < text.size() && text[j] == L'\x1B') ? j + 1 : j; // past BEL, or past ESC \ (the string terminator)
            }
            else
            {
                i += 1; // a two-character escape
            }
        }
        return out;
    }

    /// <summary>What one user line of a transcript says about a local command (ClassifyCommandTranscriptText).</summary>
    struct CommandTranscriptLine
    {
        enum class Kind
        {
            None, // neither: any other line
            Echo, // the command's echo: <command-name>…</command-name> (+ <command-args>)
            Output, // its result: <local-command-stdout>…</local-command-stdout> (or the stderr form)
        };
        Kind kind{ Kind::None };
        std::wstring name; // Echo: the command word, lowercase, without the '/' (ParseCommandEcho)
        std::wstring args; // Echo: its arguments, trimmed
        std::wstring text; // Output: the message, terminal controls stripped, trimmed
    };

    /// <summary>
    /// Reads a transcript user line's text content as a local command's ECHO or its OUTPUT — the two lines Claude Code writes
    /// together once the command ran (after its dialog, for /add-dir). The echo is read by ParseCommandEcho, the one reader the
    /// scanner and CommandWatch share. Pure.
    /// </summary>
    /// <param name="content">The line's message content as text (a string content, or its text blocks joined).</param>
    /// <returns>What the line is.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline CommandTranscriptLine ClassifyCommandTranscriptText(std::wstring_view content)
    {
        CommandTranscriptLine out;
        static constexpr std::wstring_view kOutputTags[][2] = { { L"<local-command-stdout>", L"</local-command-stdout>" },
                                                                { L"<local-command-stderr>", L"</local-command-stderr>" } };
        for (const auto& tags : kOutputTags)
        {
            const size_t open = content.find(tags[0]);
            if (open == std::wstring_view::npos)
            {
                continue;
            }
            const size_t from = open + tags[0].size();
            const size_t close = content.find(tags[1], from);
            out.kind = CommandTranscriptLine::Kind::Output;
            out.text.assign(tuimenu_detail::Trim(StripTerminalControls(content.substr(from, close == std::wstring_view::npos ? std::wstring_view::npos : close - from))));
            return out;
        }
        SlashCommand echo;
        if (ParseCommandEcho(content, echo))
        {
            out.kind = CommandTranscriptLine::Kind::Echo;
            out.name = std::move(echo.name);
            out.args = std::move(echo.args);
        }
        return out;
    }

    /// <summary>What /add-dir did, by its message (ClassifyAddDirOutput).</summary>
    enum class AddDirOutcome
    {
        Unknown, // no message (or an empty one)
        AddedSession, // "Added X as a working directory for this session" (option 1, or Claude Code's non-interactive add)
        AddedRemembered, // "… and saved to local settings" (option 2)
        AddedNotSaved, // "Added X as a working directory. Failed to save to local settings: …" (option 2, the save refused)
        NotAdded, // "Did not add X as a working directory." (option 3, or Esc)
        Already, // the folder is already readable: "… is already added", "… is already the current working directory", "… is already accessible within …"
        Refused, // any other message: not found, not a directory, a network share, a path that cannot be resolved
    };

    /// <summary>
    /// Classifies /add-dir's message (terminal controls already stripped) by the texts Claude Code 2.1.291 writes. Pure.
    /// </summary>
    /// <param name="text">The command's output (CommandTranscriptLine::text).</param>
    /// <returns>The outcome.</returns>
    inline AddDirOutcome ClassifyAddDirOutput(std::wstring_view text) noexcept
    {
        const std::wstring_view t = tuimenu_detail::Trim(text);
        if (t.empty())
        {
            return AddDirOutcome::Unknown;
        }
        const auto has = [t](std::wstring_view part) noexcept { return t.find(part) != std::wstring_view::npos; };
        if (t.substr(0, 6) == L"Added ")
        {
            if (has(L" as a working directory and saved to local settings"))
            {
                return AddDirOutcome::AddedRemembered;
            }
            if (has(L"Failed to save to local settings"))
            {
                return AddDirOutcome::AddedNotSaved;
            }
            return AddDirOutcome::AddedSession;
        }
        if (t.substr(0, 11) == L"Did not add")
        {
            return AddDirOutcome::NotAdded;
        }
        if (has(L" is already added as a working directory") || has(L" is already the current working directory") || has(L" is already accessible within "))
        {
            return AddDirOutcome::Already;
        }
        return AddDirOutcome::Refused;
    }

    /// <summary>
    /// Whether an outcome is what pressing `digit` produces — how the card tells its own press from an answer the PC gave first
    /// (both can be on their way; the transcript's result names the one that ran).
    /// </summary>
    /// <param name="outcome">The transcript's outcome.</param>
    /// <param name="digit">The option Agentmaster pressed.</param>
    /// <returns>True when the outcome is that option's.</returns>
    inline bool AddDirOutcomeFitsOption(AddDirOutcome outcome, int digit) noexcept
    {
        switch (digit)
        {
        case 1:
            return outcome == AddDirOutcome::AddedSession;
        case 2:
            return outcome == AddDirOutcome::AddedRemembered || outcome == AddDirOutcome::AddedNotSaved;
        case 3:
            return outcome == AddDirOutcome::NotAdded;
        default:
            return false;
        }
    }

    // ===== the card ==================================================================================================

    /// <summary>Where a menu card stands.</summary>
    enum class AddDirCardPhase
    {
        Open, // the dialog is open and nobody answered: the options are buttons, and the clock runs
        Answering, // Agentmaster pressed an option (a tap, or the clock) and waits for the transcript's result
        Answered, // the transcript's result came: the outcome, and who answered
        Closed, // no longer answerable here (its session ended, or the dialog went away with no result to read)
    };

    /// <summary>Who chose the answer.</summary>
    enum class AddDirAnswerer
    {
        None,
        Telegram, // a tap on the card
        Auto, // the clock: kTuiMenuAutoAnswerMs passed with no answer
        Pc, // the PC (a key in the terminal), or anything the card did not press
    };

    /// <summary>What a menu card shows (the service keeps one per watched /add-dir that opened its dialog).</summary>
    struct AddDirCardModel
    {
        AddDirCardPhase phase{ AddDirCardPhase::Open };
        std::wstring path; // the dialog's folder as the card shows it (AddDirShownPath)
        int64_t deadlineMs{ 0 }; // Open: when the default is picked by itself (the dialog seen + kTuiMenuAutoAnswerMs)
        int pressed{ 0 }; // the option Agentmaster pressed (0 = none)
        AddDirAnswerer pressedBy{ AddDirAnswerer::None }; // who chose what was pressed (Telegram or Auto)
        AddDirAnswerer answeredBy{ AddDirAnswerer::None }; // Answered: who the outcome says answered
        AddDirOutcome outcome{ AddDirOutcome::Unknown }; // Answered: the outcome
        std::wstring outcomeText; // Answered: Claude Code's own message (ANSI-free)
        int64_t outcomeAtMs{ 0 }; // Answered / Closed: when (the result line's time, or when the card closed)
        std::wstring closedWhy; // Closed: why
    };

    /// <summary>A card button's callback data: "tm:&lt;key&gt;:&lt;digit&gt;" (tm = TUI menu; the key is the card's random key).</summary>
    /// <param name="key">The card's key ([0-9a-z], 1-16 characters).</param>
    /// <param name="digit">The option (1-3).</param>
    /// <returns>The data (at most 21 bytes; Telegram allows 64).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring AddDirCallbackData(std::wstring_view key, int digit)
    {
        return L"tm:" + std::wstring{ key } + L":" + std::to_wstring(digit);
    }

    /// <summary>A parsed card tap.</summary>
    struct AddDirTap
    {
        std::wstring key;
        int digit{ 0 };
    };

    /// <summary>Reads a card button's data (AddDirCallbackData's form).</summary>
    /// <param name="data">The callback data.</param>
    /// <returns>The tap, or nullopt for anything else (another feature's button).</returns>
    /// <exception cref="std::bad_alloc">If the tap cannot be allocated.</exception>
    inline std::optional<AddDirTap> ParseAddDirCallbackData(std::wstring_view data)
    {
        if (data.substr(0, 3) != L"tm:")
        {
            return std::nullopt;
        }
        data.remove_prefix(3);
        const size_t colon = data.find(L':');
        if (colon == std::wstring_view::npos || colon == 0 || colon > 16 || data.size() != colon + 2)
        {
            return std::nullopt;
        }
        for (size_t i = 0; i < colon; ++i)
        {
            const wchar_t c = data[i];
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z')))
            {
                return std::nullopt;
            }
        }
        const wchar_t d = data[colon + 1];
        if (d < L'1' || d > L'3')
        {
            return std::nullopt;
        }
        AddDirTap tap;
        tap.key.assign(data.substr(0, colon));
        tap.digit = d - L'0';
        return tap;
    }

    namespace tuimenu_detail
    {
        /// <summary>The outcome's headline: an icon and a bold phrase.</summary>
        /// <param name="o">The outcome.</param>
        /// <returns>Telegram HTML.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring OutcomeHeadline(AddDirOutcome o)
        {
            switch (o)
            {
            case AddDirOutcome::AddedSession:
                return L"\x2705 <b>Added for this session</b>";
            case AddDirOutcome::AddedRemembered:
                return L"\x2705 <b>Added and remembered</b>";
            case AddDirOutcome::AddedNotSaved:
                return L"\x26A0\xFE0F <b>Added for this session, not remembered</b>";
            case AddDirOutcome::NotAdded:
                return L"\x2716\xFE0F <b>Not added</b>";
            case AddDirOutcome::Already:
                return L"\x2139\xFE0F <b>Already in the workspace</b>";
            case AddDirOutcome::Refused:
            case AddDirOutcome::Unknown:
            default:
                return L"\x2139\xFE0F <b>Claude Code answered</b>";
            }
        }

        /// <summary>Who answered, as the card's headline says it.</summary>
        /// <param name="who">The answerer.</param>
        /// <returns>The phrase ("" for None).</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring AnswererPhrase(AddDirAnswerer who)
        {
            switch (who)
            {
            case AddDirAnswerer::Telegram:
                return L"answered from Telegram";
            case AddDirAnswerer::Auto:
                return L"\x23F1\xFE0F picked by itself after " + std::to_wstring(kTuiMenuAutoAnswerMs / 1000) + L" s";
            case AddDirAnswerer::Pc:
                return L"answered at the PC";
            case AddDirAnswerer::None:
            default:
                return {};
            }
        }

        /// <summary>The path line of a card ("" for none): the folder in a code span.</summary>
        /// <param name="path">The path.</param>
        /// <returns>Telegram HTML, with its leading newline.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring PathLine(std::wstring_view path)
        {
            return path.empty() ? std::wstring{} : L"\n<code>" + TgEscapeHtml(path) + L"</code>";
        }
    }

    /// <summary>
    /// The card's text, by phase (Telegram HTML, well under a message's length: the path is the only long part). Open: what the
    /// dialog asks and what answering grants, and the countdown — a &lt;tg-time&gt; the apps render as "in 1 minute" and keep
    /// current (its text, the deadline's clock time, is the fallback). Answering: the option being pressed. Answered: the outcome,
    /// who answered and when, and Claude Code's own message. Closed: why it can no longer be answered here.
    /// </summary>
    /// <param name="m">The card.</param>
    /// <param name="nowMs">Now (wall clock): the clock times are written as "14:02", or with the date when not today.</param>
    /// <returns>The card's HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildAddDirCardHtml(const AddDirCardModel& m, int64_t nowMs)
    {
        using namespace tuimenu_detail;
        const std::wstring head = L"\U0001F4C1 <b>Claude Code asks</b> \x00B7 <i>" + std::wstring{ kAddDirMenuTitle } + L"</i>";
        const std::wstring when = m.outcomeAtMs > 0 ? L" \x00B7 " + TgClockText(m.outcomeAtMs, nowMs) : std::wstring{};
        switch (m.phase)
        {
        case AddDirCardPhase::Answering:
        {
            const AddDirOption* o = AddDirOptionFor(m.pressed);
            return L"\x23F3 <b>Answering</b> \x00B7 <b>" + TgEscapeHtml(o ? o->label : std::wstring_view{ L"?" }) + L"</b>" +
                   (m.pressedBy == AddDirAnswerer::Auto ? L" <i>(" + AnswererPhrase(AddDirAnswerer::Auto) + L")</i>" : std::wstring{}) + L"\n<i>" +
                   std::wstring{ kAddDirMenuTitle } + L"</i>" + PathLine(m.path);
        }
        case AddDirCardPhase::Answered:
        {
            const std::wstring who = AnswererPhrase(m.answeredBy);
            return OutcomeHeadline(m.outcome) + (who.empty() ? std::wstring{} : L" \x00B7 " + who) + when + L"\n<i>" + std::wstring{ kAddDirMenuTitle } +
                   L"</i>" + PathLine(m.path) +
                   (m.outcomeText.empty() ? std::wstring{} : L"\n<i>Claude Code:</i> " + TgEscapeHtml(m.outcomeText.size() > 600 ? m.outcomeText.substr(0, 600) + L"\x2026" : m.outcomeText));
        }
        case AddDirCardPhase::Closed:
            return L"\x23F9\xFE0F <b>No longer open here</b>" + when + (m.closedWhy.empty() ? std::wstring{} : L" \x2014 " + TgEscapeHtml(m.closedWhy)) + L"\n<i>" +
                   std::wstring{ kAddDirMenuTitle } + L"</i>" + PathLine(m.path);
        case AddDirCardPhase::Open:
        default:
        {
            const AddDirOption* def = AddDirOptionFor(kAddDirDefaultOption);
            const std::wstring countdown = L"<tg-time unix=\"" + std::to_wstring(m.deadlineMs / 1000) + L"\" format=\"r\">at " +
                                           TgEscapeHtml(TgClockText(m.deadlineMs, nowMs)) + L"</tg-time>";
            return head + PathLine(m.path) + L"\n<i>" + std::wstring{ kAddDirMenuNote } + L"</i>\n\n\x23F1\xFE0F Picks <b>" +
                   TgEscapeHtml(def ? def->label : std::wstring_view{ L"?" }) + L"</b> by itself " + countdown + L" \x2014 or tap your answer.";
        }
        }
    }

    /// <summary>
    /// The card's buttons: one row per option while it is Open ("⏱ 1 · Yes, for this session" — the clock's mark on the default
    /// — "2 · Yes, and remember this directory", "3 · No"), none in any other phase (an edit without a keyboard removes it).
    /// </summary>
    /// <param name="m">The card.</param>
    /// <param name="key">The card's key (its buttons' data, AddDirCallbackData).</param>
    /// <returns>The rows of (label, callback data).</returns>
    /// <exception cref="std::bad_alloc">If the rows cannot be allocated.</exception>
    inline std::vector<std::vector<std::pair<std::wstring, std::wstring>>> BuildAddDirCardButtons(const AddDirCardModel& m, std::wstring_view key)
    {
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
        if (m.phase != AddDirCardPhase::Open)
        {
            return rows;
        }
        for (const auto& o : kAddDirOptions)
        {
            std::wstring label = (o.digit == kAddDirDefaultOption ? L"\x23F1\xFE0F " : L"") + std::to_wstring(o.digit) + L" \x00B7 " + std::wstring{ o.label };
            rows.push_back({ { std::move(label), AddDirCallbackData(key, o.digit) } });
        }
        return rows;
    }

    /// <summary>
    /// The reply to a /add-dir that opened no dialog (or whose result came with no card shown): the outcome's headline and Claude
    /// Code's own message — "ℹ️ Already in the workspace · /add-dir" over "Q:\x is already added as a working directory.".
    /// </summary>
    /// <param name="outcome">The outcome (ClassifyAddDirOutput).</param>
    /// <param name="text">Claude Code's message (ANSI-free).</param>
    /// <returns>Telegram HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildAddDirReplyHtml(AddDirOutcome outcome, std::wstring_view text)
    {
        const std::wstring body = text.size() > 600 ? std::wstring{ text.substr(0, 600) } + L"\x2026" : std::wstring{ text };
        return tuimenu_detail::OutcomeHeadline(outcome) + L" \x00B7 <code>/add-dir</code>" + (body.empty() ? std::wstring{} : L"\n<i>" + TgEscapeHtml(body) + L"</i>");
    }

    /// <summary>
    /// The answer to a BARE /add-dir: Claude Code would open a path INPUT, which a phone cannot drive, so nothing is sent and the
    /// user is shown the form that works — and what follows it here.
    /// </summary>
    /// <returns>Telegram HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring AddDirUsageHtml()
    {
        const AddDirOption* def = AddDirOptionFor(kAddDirDefaultOption);
        return L"Usage: <code>/add-dir &lt;path&gt;</code> \x2014 adds a folder to the session's workspace, e.g. <code>/add-dir D:\\work\\shared</code>. "
               L"Claude Code then asks how (for this session, or remembered in the project's local settings); the question comes here as "
               L"buttons, and <b>" +
               TgEscapeHtml(def ? def->label : std::wstring_view{ L"?" }) + L"</b> is picked by itself after " + std::to_wstring(kTuiMenuAutoAnswerMs / 1000) +
               L" s. (A bare /add-dir opens a path input at the PC, which cannot be filled from here.)";
    }

    /// <summary>
    /// The note for a delivered /add-dir whose terminal shows a menu this build cannot read (a changed dialog after a Claude Code
    /// update, or another menu in the way): it names the PC, and the screen command that shows the menu.
    /// </summary>
    /// <returns>Telegram HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring AddDirUnreadableHtml()
    {
        return L"\x26A0\xFE0F <code>/add-dir</code> opened a dialog this Agentmaster cannot read \x2014 answer it at the PC "
               L"(\U0001F5A5 /screen shows the terminal).";
    }
}
