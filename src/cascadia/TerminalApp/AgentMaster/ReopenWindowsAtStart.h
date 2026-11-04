// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — what a start does with the windows that were open at the last exit when there are SEVERAL of them
// (PERSISTENCE.md §18, SETTINGS.md "Behavior and session state"). Header-only and pure (the standard library only).
//
// WHY ONE HEADER: four places must agree on the same three choices and on how each is spelled in settings.json — the
// engine model (SessionModels.h: AppSettings::reopenWindowsAtStart), the engine's reader and writer (Persistence.cpp), the
// app exe's startup question (ReopenWindowsPrompt.h, called from WindowEmperor.cpp; the exe links TerminalApp.dll, not the
// engine library, so it reads and writes the key itself) and the m5 harness. A word list kept in two places drifts: a
// choice remembered by the question in one spelling would read back as "Ask" in the other, and the question would come
// back at every start although the user ticked "Remember my choice".

#pragma once

#include <cstddef>
#include <string_view>

namespace Agentmaster
{
    /// <summary>
    /// What a start does when MORE THAN ONE window was open at the last exit. GLOBAL (one value for the profile), stored as
    /// settings.json's <c>settings.reopenWindowsAtStart</c>; set from the cog (Settings → Behavior → STARTUP) or by
    /// ticking "Remember my choice" in the "Reopen your N previous Agentmaster windows?" question.
    /// </summary>
    /// <remarks>
    /// A LONE window always comes back without a question, whatever this says (DecideReopenAtStart): the question exists
    /// because reopening several windows at once is worth a heads-up, while one window is just "where I left off".
    /// The integer values are the cog's ComboBox item order (AgentManagerContent.Settings.cpp seeds and reads the box by
    /// index), so a new choice goes at the END; the file never stores the integer, only the token.
    /// </remarks>
    enum class ReopenWindowsAtStart
    {
        /// <summary>The default (and every settings.json written before the setting existed): ask the question.</summary>
        Ask = 0,
        /// <summary>Reopen every window that was open at the last exit, without asking (the question's remembered Yes).</summary>
        ReopenAll = 1,
        /// <summary>
        /// Start with a single window (the question's remembered No): the start dispatches none of the saved windows, so it
        /// opens Windows Terminal's one default window — which takes over the first saved window record no other window
        /// claimed (TerminalPage's ClaimWindowRecord front-pop, PERSISTENCE.md §13.5), exactly as answering No does — and
        /// the other previous windows stay one click away behind the Manager's "Reopen Windows (N)" button. Nothing is
        /// deleted.
        /// </summary>
        OneWindow = 2,
    };

    /// <summary>The settings.json member (inside the engine's <c>settings</c> object) that holds the choice.</summary>
    inline constexpr std::wstring_view kReopenWindowsAtStartKey = L"reopenWindowsAtStart";

    /// <summary>
    /// The word settings.json stores for a choice. Use it for every write, so the engine's save and the startup question's
    /// read-modify-write can never spell one choice two ways.
    /// </summary>
    /// <param name="choice">The choice.</param>
    /// <returns><c>"ask"</c>, <c>"reopen"</c> or <c>"oneWindow"</c>; an out-of-range value (a cast integer) answers
    /// <c>"ask"</c>, the default, so a corrupt value can only ever make the app ask. The view points at a string literal,
    /// valid for the whole run.</returns>
    /// <remarks>Never throws (<c>constexpr</c>, <c>noexcept</c>).</remarks>
    constexpr std::wstring_view ReopenWindowsAtStartToken(ReopenWindowsAtStart choice) noexcept
    {
        switch (choice)
        {
        case ReopenWindowsAtStart::ReopenAll:
            return L"reopen";
        case ReopenWindowsAtStart::OneWindow:
            return L"oneWindow";
        case ReopenWindowsAtStart::Ask:
        default:
            return L"ask";
        }
    }

    /// <summary>
    /// The choice a settings.json word stands for: the inverse of ReopenWindowsAtStartToken. Use it for every read.
    /// </summary>
    /// <param name="token">The stored word (case-sensitive, as written).</param>
    /// <returns>The choice; a missing, empty or unknown word (a hand edit, a newer build's choice) reads as
    /// ReopenWindowsAtStart::Ask.</returns>
    /// <remarks>
    /// FAILS TOWARD ASKING on purpose: an unreadable choice must never silently reopen ten windows, nor silently leave
    /// them closed — the question is the one outcome the user can always correct on the spot. A newer build's word is
    /// still KEPT in the file by this build's saves while the user does not change the setting (PERSISTENCE.md §17, the
    /// residue: "a value this build cannot represent"). Never throws (<c>constexpr</c>, <c>noexcept</c>).
    /// </remarks>
    constexpr ReopenWindowsAtStart ReopenWindowsAtStartFromToken(std::wstring_view token) noexcept
    {
        if (token == L"reopen")
        {
            return ReopenWindowsAtStart::ReopenAll;
        }
        if (token == L"oneWindow")
        {
            return ReopenWindowsAtStart::OneWindow;
        }
        return ReopenWindowsAtStart::Ask;
    }

    /// <summary>What the start does with the windows open at the last exit (DecideReopenAtStart's answer).</summary>
    enum class ReopenStartAction
    {
        /// <summary>
        /// Dispatch none of them: the start's own fallback (WindowEmperor's <c>_windows.empty()</c> guard) opens the single
        /// default window, which takes over the first unclaimed saved record, if any (ReopenWindowsAtStart::OneWindow).
        /// </summary>
        OpenSingleWindow,
        /// <summary>Reopen every window that was open at the last exit, without a question.</summary>
        ReopenAll,
        /// <summary>Ask "Reopen your N previous Agentmaster windows?" (with "Remember my choice").</summary>
        Ask,
    };

    /// <summary>
    /// The startup rule: what to do with <paramref name="windowCount"/> windows that were open at the last exit, given the
    /// user's setting. Pure, so the m5 harness pins the whole table.
    /// </summary>
    /// <param name="windowCount">How many windows the start would reopen (the open-at-exit records it found).</param>
    /// <param name="setting">AppSettings::reopenWindowsAtStart as read from settings.json.</param>
    /// <returns>
    /// 0 windows → OpenSingleWindow (nothing to reopen); 1 window → ReopenAll, WHATEVER the setting (a lone window comes
    /// back silently, as it always has); 2 or more → Ask for ReopenWindowsAtStart::Ask, ReopenAll for ReopenAll,
    /// OpenSingleWindow for OneWindow; an out-of-range setting (a cast integer) → Ask, like an unknown word.
    /// </returns>
    /// <remarks>
    /// The lone-window rule is why "Start with a single window" never strands a user's only window: with one window open
    /// at the exit, reopening it IS starting with a single window. Never throws (<c>constexpr</c>, <c>noexcept</c>).
    /// </remarks>
    constexpr ReopenStartAction DecideReopenAtStart(std::size_t windowCount, ReopenWindowsAtStart setting) noexcept
    {
        if (windowCount == 0)
        {
            return ReopenStartAction::OpenSingleWindow;
        }
        if (windowCount == 1)
        {
            return ReopenStartAction::ReopenAll;
        }
        switch (setting)
        {
        case ReopenWindowsAtStart::ReopenAll:
            return ReopenStartAction::ReopenAll;
        case ReopenWindowsAtStart::OneWindow:
            return ReopenStartAction::OpenSingleWindow;
        case ReopenWindowsAtStart::Ask:
        default:
            return ReopenStartAction::Ask;
        }
    }

    /// <summary>
    /// The setting a REMEMBERED answer to the question stores: Yes → ReopenAll, No → OneWindow. Never Ask: remembering
    /// "ask me" would change nothing, so the question offers no such answer.
    /// </summary>
    /// <param name="reopen">True when the user answered Yes (reopen the windows), false for No (a single window).</param>
    /// <returns>The choice to write into settings.json.</returns>
    /// <remarks>Never throws (<c>constexpr</c>, <c>noexcept</c>).</remarks>
    constexpr ReopenWindowsAtStart RememberedReopenChoice(bool reopen) noexcept
    {
        return reopen ? ReopenWindowsAtStart::ReopenAll : ReopenWindowsAtStart::OneWindow;
    }
}
