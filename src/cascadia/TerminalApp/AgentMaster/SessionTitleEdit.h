// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — EDITING A SESSION'S TITLE IN A BOX (SIDE_PANEL.md §2o "Rename on the card"): the pure rules the side panel's
// floating rename card applies to what the user typed, before the one rename seam (TerminalPage::_RenameClaudeSession: the
// registry title, the tab title, every window's cards and the Telegram topic — Rule #11) is called.
//
// Three facts make the rules what they are:
//   1. Every rename path stores the typed text without its surrounding whitespace and line breaks, and keeps the line breaks
//      inside it: the Manager tree's editor (AgentManagerContent::_CommitRename), the tab header's renamer
//      (TabHeaderControl::RenameBoxLostFocusHandler) and the tab -> registry mirror (TerminalPage::_SyncClaudeTitleFromTab)
//      all trim " \t\r\n" at both ends. A blank result renames nothing: a session's title never goes empty.
//   2. A title can have TWO LINES, and its line break is a lone CR: XAML's TextBox writes a lone CR for each line break (its
//      rich-edit base), and on 2026-10-18 187 of production's 1,731 titles had one (none an LF). The same TextBox shows an LF
//      as nothing at all — the lines run together — so a title written elsewhere with LF breaks must reach the box with CR
//      breaks to show its lines.
//   3. The box hands back CR breaks whatever the title held. A card that opened on an LF title and was committed untouched
//      must not count as a rename: it would rewrite the title only to respell its breaks, and — since the card writes only
//      a CHANGE — overwrite a rename made elsewhere while the card was open with the very title the card started from.
//
// Plain C++, no WinRT: the m5 harness tests it (TestSessionTitleEdit, tests_sidepanel.cpp).

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Agentmaster
{
    /// <summary>
    /// The characters every rename path drops at both ends of a typed title: the space, the tab, CR and LF.
    /// </summary>
    /// <remarks>
    /// The set the Manager tree's editor, the tab header's renamer and the tab -> registry mirror already use (their inline
    /// trims predate this header), so a title renamed on a side panel card is stored exactly as the same text renamed on the
    /// tab or in the tree. Nothing else counts as blank here (no no-break space, no zero-width space): a title that starts
    /// with one keeps it, as it does on every other path.
    /// </remarks>
    inline constexpr std::wstring_view kSessionTitleTrimChars = L" \t\r\n";

    /// <summary>
    /// A typed title as every rename path stores it: without the spaces, tabs and line breaks at its two ends; the line
    /// breaks inside it are the user's (a two-line title) and stay as typed.
    /// </summary>
    /// <param name="typed">The box's text, as the user left it.</param>
    /// <returns>The trimmed title; empty when <paramref name="typed"/> holds nothing but those characters.</returns>
    /// <exception cref="std::bad_alloc">When the copy cannot be allocated.</exception>
    inline std::wstring TrimEditedSessionTitle(std::wstring_view typed)
    {
        const size_t first = typed.find_first_not_of(kSessionTitleTrimChars);
        if (first == std::wstring_view::npos)
        {
            return {};
        }
        const size_t last = typed.find_last_not_of(kSessionTitleTrimChars);
        return std::wstring{ typed.substr(first, last - first + 1) };
    }

    /// <summary>
    /// A title as XAML's TextBox must receive it to show its lines: every line break spelled as a lone CR (a CR LF pair
    /// becomes one CR, a lone LF becomes a CR). A title with no LF is returned as it is.
    /// </summary>
    /// <param name="title">The stored title (the registry's).</param>
    /// <returns>The same text with CR line breaks only.</returns>
    /// <exception cref="std::bad_alloc">When the copy cannot be allocated.</exception>
    /// <remarks>
    /// Only the box's COPY is respelled: the stored title is never rewritten for its breaks, and
    /// <see cref="SessionTitlesMatch"/> treats the two spellings as one title, so a card committed untouched writes nothing.
    /// </remarks>
    inline std::wstring SessionTitleBoxText(std::wstring_view title)
    {
        std::wstring out;
        out.reserve(title.size());
        for (size_t i = 0; i < title.size(); ++i)
        {
            const wchar_t c = title[i];
            if (c == L'\r' && i + 1 < title.size() && title[i + 1] == L'\n')
            {
                out.push_back(L'\r');
                ++i; // the pair's LF is part of the one break just written
            }
            else if (c == L'\n')
            {
                out.push_back(L'\r');
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    /// <summary>
    /// Do two titles read the same once their line breaks are spelled alike? A CR LF pair, a lone LF and a lone CR are one
    /// break each; every other character must match exactly (case included: a rename that only changes case is a rename).
    /// </summary>
    /// <param name="a">One title.</param>
    /// <param name="b">The other title.</param>
    /// <returns>True when they hold the same characters and the same breaks.</returns>
    /// <exception cref="std::bad_alloc">When a respelled copy cannot be allocated.</exception>
    inline bool SessionTitlesMatch(std::wstring_view a, std::wstring_view b)
    {
        return SessionTitleBoxText(a) == SessionTitleBoxText(b);
    }

    /// <summary>
    /// What a rename card's commit writes: the trimmed typed title, or nothing.
    /// </summary>
    /// <param name="openedTitle">The title the card opened with (the registry's at the open, untrimmed).</param>
    /// <param name="typed">The box's text at the commit.</param>
    /// <returns>
    /// Nothing (<c>std::nullopt</c>) when the typed text is blank — a session's title never goes empty, the other rename paths'
    /// rule — or when it matches the trimmed opened title (<see cref="SessionTitlesMatch"/>: the same characters, line breaks
    /// spelled either way). Otherwise the trimmed typed title, its inner line breaks as the box gave them.
    /// </returns>
    /// <exception cref="std::bad_alloc">When a copy cannot be allocated.</exception>
    /// <remarks>
    /// Comparing with the title the card OPENED with, not the one the registry holds at the commit, is deliberate: a card
    /// committed untouched (Enter at once, a click away) then writes nothing, so a rename made meanwhile on the tab, in another
    /// window or by Telegram stands; a card the user did edit writes the edit, which wins like any later rename.
    /// </remarks>
    inline std::optional<std::wstring> SessionTitleToCommit(std::wstring_view openedTitle, std::wstring_view typed)
    {
        std::wstring trimmed = TrimEditedSessionTitle(typed);
        if (trimmed.empty() || SessionTitlesMatch(trimmed, TrimEditedSessionTitle(openedTitle)))
        {
            return std::nullopt;
        }
        return trimmed;
    }

    /// <summary>
    /// The first line of a title: the text before its first CR or LF. The rename card's header has room for one line.
    /// </summary>
    /// <param name="title">The title (one line or more).</param>
    /// <returns>Its first line; the whole title when it has a single line.</returns>
    /// <exception cref="std::bad_alloc">When the copy cannot be allocated.</exception>
    inline std::wstring FirstSessionTitleLine(std::wstring_view title)
    {
        return std::wstring{ title.substr(0, title.find_first_of(L"\r\n")) };
    }
}
