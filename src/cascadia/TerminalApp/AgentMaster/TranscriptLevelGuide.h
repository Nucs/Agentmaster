// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - the LEVEL GUIDE: what each Copy > Transcript detail level shows and what it leaves out, as
// ONE table that the transcript overlay draws in two places (doc/agentmaster/TRANSCRIPT_OVERLAY.md 3f):
//   * the panel behind the (i) button after the level tabs - every level side by side, a check mark where
//     a level shows a row - plus what every level shows and what no level shows;
//   * each level tab's tooltip - a cheat sheet of that one level (TranscriptLevelTip): what it adds over
//     the level below it, and what it still leaves out, with the level that first shows each.
//
// WHY one table tied to the options: a guide that is written apart from the levels drifts the first time a
// level changes. Each row names the TurnsOptions feature that decides it, and the m5 harness
// (TestTranscriptLevelGuide) requires TurnsOptionsForLevel to turn that feature on at EXACTLY the row's
// first level and every level above it - so the guide can never promise a row a level does not render, or
// hide one it does. The levels are cumulative (TranscriptTurns.h), which is what lets one "first level" per
// row describe all five.
//
// Pure data and string building, no WinRT and no PCH: the overlay (TerminalApp) and the standalone harness
// include it alike. Every literal is ASCII with \x escapes (the repository's convention for wide literals),
// and the guide's own words use the plain hyphen, never an em dash.

#pragma once

#include "TranscriptTurns.h" // TurnsLevel / TurnsOptions / kTurnsLevelCount - the levels this guide describes

#include <cstddef>
#include <string>

namespace Agentmaster
{
    // One row of the guide: something a transcript can show, and the least detailed level that shows it.
    struct TranscriptLevelGuideRow
    {
        // The row as the (i) panel prints it: a full sentence-like label, capitalized, no closing period.
        const wchar_t* text;
        // The row as a tab's tooltip prints it: a short lowercase form that fits a tooltip line.
        const wchar_t* brief;
        // The least detailed level that shows it. Every more detailed level shows it too (the levels are
        // cumulative), so this one value places the row in all five columns.
        TurnsLevel firstLevel;
        // The option that decides the row (a TurnsOptions member), or nullptr for what no option controls
        // (the messages and final replies every level shows). The harness checks the option against
        // TurnsOptionsForLevel for every level - see the file comment.
        bool TurnsOptions::*feature;
    };

    // The guide, least detailed first: the rows Minimal shows, then the ones Compact adds, then Default's,
    // Full's and Max's (the harness keeps this order, so the (i) panel reads as a staircase of check marks).
    // A row's `text` is what the panel shows; its `brief` is what a tab's tooltip shows.
    inline constexpr TranscriptLevelGuideRow kTranscriptLevelGuide[] = {
        { L"Your messages and each turn's final reply",
          L"your messages and each turn's final reply",
          TurnsLevel::Minimal,
          nullptr },
        { L"Slash commands you ran that did not start a turn (/model, /cost), with their reply",
          L"slash commands you ran outside a turn",
          TurnsLevel::Compact,
          &TurnsOptions::notes },
        { L"Where the context was compacted: auto or manual, tokens before and after, how long it took",
          L"compaction markers",
          TurnsLevel::Compact,
          &TurnsOptions::compactions },
        { L"Your answers to Claude's questions (or that you dismissed one), and your words when you rejected a call",
          L"your answers to Claude's questions",
          TurnsLevel::Compact,
          &TurnsOptions::answers },
        { L"One entry per subagent or teammate: its brief, the messages it got, its final report",
          L"each agent's brief and final report",
          TurnsLevel::Compact,
          &TurnsOptions::agents },
        { L"Every tool call on one line: its main arguments and how it ended (ok, \x2717 exit 1, 40 of 120 lines, created)",
          L"every tool call, with its arguments and outcome",
          TurnsLevel::Default,
          &TurnsOptions::ops },
        { L"Claude's remarks between calls (the first 200 characters of each)",
          L"Claude's remarks between calls",
          TurnsLevel::Default,
          &TurnsOptions::narration },
        // Background completions are on in every level (bgEvents), but they print under their call's line,
        // so they appear exactly where the calls do - which is what the harness checks (ops). An agent's own
        // calls follow the same switch inside its entry.
        { L"When background tasks finished, and each agent's own tool calls",
          L"background-task ends and agents' own calls",
          TurnsLevel::Default,
          &TurnsOptions::ops },
        { L"The exact Bash command, in a code block",
          L"the exact Bash commands",
          TurnsLevel::Full,
          &TurnsOptions::fullBash },
        { L"The first line each call returned",
          L"the first line each call returned",
          TurnsLevel::Full,
          &TurnsOptions::opOutput },
        { L"How long each call took",
          L"how long each call took",
          TurnsLevel::Full,
          &TurnsOptions::opDuration },
        { L"The full content of every file Claude wrote",
          L"the content of every file Claude wrote",
          TurnsLevel::Max,
          &TurnsOptions::fullWrite },
        { L"A stats line per turn: calls, time, API calls, model, context size, output and thinking tokens",
          L"a stats line per turn (tokens, model, context)",
          TurnsLevel::Max,
          &TurnsOptions::stats },
    };

    // What EVERY level shows beyond the guide's first row - the features no level switches off (times, the
    // fork chain, peer messages, the compacted history, the typed-while-it-worked inputs). Lowercase, no
    // closing period; the (i) panel lists them under "Also in every level".
    inline constexpr const wchar_t* kTranscriptLevelGuideEveryLevel[] = {
        L"each turn's header: its number, when it started and last replied, and whether you interrupted it",
        L"what you typed while Claude worked, and the skills you started (/commit)",
        L"messages from teammates and other sessions, under their sender's name",
        L"the sessions it was forked or restarted from, numbered as one conversation",
        L"the history before each compaction",
    };

    // What NO level shows - the transcript holds it, but the turns view never renders it. Lowercase, no
    // closing period; the (i) panel lists them under "In no level".
    inline constexpr const wchar_t* kTranscriptLevelGuideNoLevel[] = {
        L"Claude's thinking (Max only counts its tokens)",
        L"tool output past its first line; Read, Write and Edit results show none at all",
        L"the text an Edit changed (only Telegram's All Screen draws diffs, with its File Diffs box)",
        L"images",
        L"system reminders, injected skill text, hook output, and the compaction summary itself",
        L"prompts you rewound, and Claude Code's filler lines",
        L"long arguments of other tools (MCP and the rest show one over 60 characters as its size)",
    };

    // How a Codex session differs (it has no turns view, so the guide's rows describe Claude sessions only):
    // the (i) panel's closing note. Mirrors AgentCopyActions.h kTranscriptCopyMenuTip's Codex sentence.
    inline constexpr const wchar_t* kTranscriptLevelGuideCodexNote =
        L"A Codex session has no turns view: Minimal and Compact show each of your messages with its final reply, "
        L"Default, Full and Max the whole plain conversation.";

    // Whether `level` shows `row`.
    // Parameters:
    //   row   - a guide row.
    //   level - the level asked about; a value outside 0..4 compares by its number, like any other.
    // Returns true when `level` is `row.firstLevel` or a more detailed level. Never throws.
    constexpr bool TranscriptLevelShows(const TranscriptLevelGuideRow& row, TurnsLevel level) noexcept
    {
        return static_cast<int>(level) >= static_cast<int>(row.firstLevel);
    }

    // A level's display label - the word its tab and its Copy > Transcript item show. The guide keeps its own
    // copy because the menu table (AgentCopyActions.h kTranscriptCopyMenu) lives in the WinRT layer, which
    // this pure header must not include (the harness could not compile it). ⚠ Rename a level in BOTH places:
    // the tabs take their words from the menu table, the tooltips and the (i) panel's column heads from here.
    // Parameters: level - the level; a value outside 0..4 reads as "Default", like TurnsLevelName.
    // Returns a static string, never null. Never throws.
    constexpr const wchar_t* TranscriptLevelLabel(TurnsLevel level) noexcept
    {
        switch (level)
        {
        case TurnsLevel::Minimal:
            return L"Minimal";
        case TurnsLevel::Compact:
            return L"Compact";
        case TurnsLevel::Full:
            return L"Full";
        case TurnsLevel::Max:
            return L"Max";
        case TurnsLevel::Default:
        default:
            return L"Default";
        }
    }

    // A level tab's tooltip: the cheat sheet of that level, built from kTranscriptLevelGuide so it can never
    // disagree with the (i) panel. Its shape (one item per line, for the shared tooltip host's plain text):
    //     Compact (key 4)
    //     Everything in Minimal, plus:
    //       <check> slash commands you ran outside a turn
    //       ...
    //     Leaves out:
    //       <dot> every tool call, with its arguments and outcome (from Default)
    //       ...
    //     The (i) after the tabs compares every level.
    // Minimal says "Shows:" instead of "Everything in ..., plus:"; Max, which leaves nothing out, says so in
    // place of the "Leaves out" list.
    // Parameters:
    //   level    - the tab's level; a value outside 0..4 is treated as Default (TurnsOptionsForLevel's rule).
    //   shortcut - the digit key that picks the tab (its 1-based position), or 0 to leave the key out.
    // Returns the tooltip text. Throws std::bad_alloc only.
    inline std::wstring TranscriptLevelTip(TurnsLevel level, size_t shortcut)
    {
        // An out-of-range level is Default everywhere else (TurnsOptionsForLevel, TurnsLevelName); the tip
        // follows, so a stray value never prints a cheat sheet for a level that does not exist.
        const int raw = static_cast<int>(level);
        const int rank = (raw < 0 || raw >= kTurnsLevelCount) ? static_cast<int>(TurnsLevel::Default) : raw;
        const auto at = [](int value) { return static_cast<TurnsLevel>(value); };

        std::wstring tip = TranscriptLevelLabel(at(rank));
        if (shortcut > 0)
        {
            tip += L" (key " + std::to_wstring(shortcut) + L")";
        }
        // What it shows: the rows this level is the FIRST to show, introduced by what it builds on - every
        // earlier row is in the level below, which the reader can hover for its own list.
        tip += rank == static_cast<int>(TurnsLevel::Minimal) ? std::wstring{ L"\nShows:" } :
                                                                std::wstring{ L"\nEverything in " } + TranscriptLevelLabel(at(rank - 1)) + L", plus:";
        for (const auto& row : kTranscriptLevelGuide)
        {
            if (static_cast<int>(row.firstLevel) == rank)
            {
                tip += L"\n  \x2713 ";
                tip += row.brief;
            }
        }
        // What it leaves out, each with the level that first shows it - the cheat sheet's "where to look".
        bool leavesOut = false;
        for (const auto& row : kTranscriptLevelGuide)
        {
            if (static_cast<int>(row.firstLevel) <= rank)
            {
                continue;
            }
            if (!leavesOut)
            {
                tip += L"\nLeaves out:";
                leavesOut = true;
            }
            tip += L"\n  \x00B7 ";
            tip += row.brief;
            tip += L" (from ";
            tip += TranscriptLevelLabel(row.firstLevel);
            tip += L")";
        }
        if (!leavesOut)
        {
            tip += L"\nLeaves out nothing another level shows.";
        }
        tip += L"\nThe \x24D8 after the tabs compares every level.";
        return tip;
    }
}
