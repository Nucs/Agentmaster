// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - the transcript overlay's TABS beyond the five detail levels: the three BOX TABS Custom, User Only and
// Assistant Only (doc/agentmaster/TRANSCRIPT_OVERLAY.md 3j). The tab row reads
//     Default · Max · Full · Compact · Minimal | Custom · User Only · Assistant Only  (i)
// and each box tab is Minimal's view of ONE or BOTH sides of every turn, plus whatever the user ticks in a row of
// twelve BOXES above the transcript - one per feature the higher levels add (Compact's slash commands, compaction
// markers, answers, agents and call reasons; Default's full call lines and remarks; Full's Bash commands, first output
// lines and durations; Max's written files and stats line):
//   * Custom        - both sides: the user's messages and each turn's final reply. Every box ticked is exactly Max,
//                     and a level's own boxes ticked are exactly that level (the m5 harness pins both on every golden).
//   * User Only     - what the USER said: their messages (not a teammate's or another session's), no final reply; a turn
//                     with nothing of theirs is skipped, its number kept (TurnsOptions::peerInputs / finalReplies off).
//   * Assistant Only - Claude's side: no message that reached a turn, the user's or anyone else's
//                     (TurnsOptions::userInputs / peerInputs off).
// The SAME twelve boxes sit above all three tabs; a box from the other side brings that piece in anyway (User Only with
// "Tool calls with reasons" shows Claude's calls under the user's messages, still without Claude's reply). Each tab keeps
// its own boxes, remembered in settings.json for every window (AppSettings::transcriptOverlay<Tab>Boxes), and starts with
// every box clear.
//
// WHY a header of its own: the overlay (TerminalApp, WinRT) and the standalone m5 harness both need the same tables and
// rules - which features exist as boxes, what a tab's options are, the names settings.json stores - and the harness can
// only check what it can compile. Pure data and string building on top of TranscriptTurns.h and the level guide, no WinRT
// and no PCH. Every literal is ASCII with \x escapes, and the words use the plain hyphen (the guide's convention).

#pragma once

#include "TranscriptLevelGuide.h" // kTranscriptLevelGuide (a box's tooltip is its guide row), TranscriptLevelLabel / TranscriptLevelTip
#include "TranscriptTurns.h" // TurnsLevel / TurnsOptions / TurnsOptionsForLevel / TurnsLevelName / kTurnsLevelCount

#include <cstddef>
#include <cstdint>
#include <cwctype> // towlower - names compare without case (a hand-edited settings.json)
#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster
{
    // Every tab of the transcript overlay, by the index the overlay keeps its views under. The five detail levels keep their
    // TurnsLevel value, so a level's view index IS its TurnsLevel; the three box tabs follow. The tab ROW shows them in its
    // own order (the Copy menu's for the levels, then these three); this order only numbers them.
    enum class TranscriptView : int
    {
        Minimal = 0, // TurnsLevel::Minimal
        Compact = 1, // TurnsLevel::Compact
        Default = 2, // TurnsLevel::Default
        Full = 3, // TurnsLevel::Full
        Max = 4, // TurnsLevel::Max
        Custom = 5, // both sides of each turn, from Minimal, plus the ticked boxes
        UserOnly = 6, // the user's own messages, from Minimal, plus the ticked boxes (no final reply, no teammate's message)
        AssistantOnly = 7, // Claude's side, from Minimal, plus the ticked boxes (no message that reached a turn)
    };

    // How many views exist (the overlay's view array); the first box tab's index; how many box tabs there are.
    inline constexpr int kTranscriptViewCount = 8;
    inline constexpr int kTranscriptFirstBoxView = static_cast<int>(TranscriptView::Custom);
    inline constexpr int kTranscriptBoxViewCount = kTranscriptViewCount - kTranscriptFirstBoxView;
    static_assert(kTranscriptFirstBoxView == kTurnsLevelCount, "the box tabs follow the five levels: a level's view index is its TurnsLevel value");

    // Whether `view` is one of the three box tabs (Custom, User Only, Assistant Only).
    // Parameters: view - a view index; any int (out-of-range values are simply not box tabs).
    // Returns true for 5..7. Never throws.
    constexpr bool IsTranscriptBoxView(int view) noexcept
    {
        return view >= kTranscriptFirstBoxView && view < kTranscriptViewCount;
    }

    // When a box has any effect on the render - the ladder features are not all independent: some print only under a
    // call's FULL line (the `ops` feature, the box "Tool calls with outcome"), and the one-line call reason prints only
    // while those full lines are off (each full line already carries the same reason). A box that would do nothing is
    // drawn greyed (its tick kept), so ticking it never looks broken.
    enum class TranscriptBoxGate : uint8_t
    {
        Always, // takes effect on its own
        NeedsCalls, // prints under a call's full line: no effect while "ops" is off
        WithoutCalls, // a call's one-line reason: no effect while "ops" is on
    };

    // One BOX of the box tabs: a ladder feature of a higher level the user can add to the tab's view.
    struct TranscriptViewBox
    {
        // The name settings.json stores in a tab's ticked list: the TurnsOptions member's own name. ⚠ Never rename one -
        // a stored tick would silently drop (an unknown name is kept but means nothing).
        const wchar_t* name;
        // The check box's words, short enough for a row of twelve.
        const wchar_t* label;
        // The option it turns on.
        bool TurnsOptions::*feature;
        // The least detailed level that shows it - the group the box sits in (Compact, Default, Full, Max). The m5 harness
        // checks it against TurnsOptionsForLevel, so a group can never disagree with the levels.
        TurnsLevel level;
        // When the box has an effect (TranscriptBoxGate).
        TranscriptBoxGate gate;
    };

    // The twelve boxes, in the order the row shows them: grouped by level, least detailed first, and inside a group in the
    // level guide's row order - so the row reads like the (i) panel's staircase. One box per ladder feature (the harness
    // checks that none is missing and none is doubled).
    inline constexpr TranscriptViewBox kTranscriptViewBoxes[] = {
        { L"notes", L"Slash commands", &TurnsOptions::notes, TurnsLevel::Compact, TranscriptBoxGate::Always },
        { L"compactions", L"Compaction markers", &TurnsOptions::compactions, TurnsLevel::Compact, TranscriptBoxGate::Always },
        { L"answers", L"Your answers", &TurnsOptions::answers, TurnsLevel::Compact, TranscriptBoxGate::Always },
        { L"agents", L"Agents", &TurnsOptions::agents, TurnsLevel::Compact, TranscriptBoxGate::Always },
        { L"reasons", L"Tool calls with reasons", &TurnsOptions::reasons, TurnsLevel::Compact, TranscriptBoxGate::WithoutCalls },
        { L"ops", L"Tool calls with outcome", &TurnsOptions::ops, TurnsLevel::Default, TranscriptBoxGate::Always },
        { L"narration", L"Remarks between calls", &TurnsOptions::narration, TurnsLevel::Default, TranscriptBoxGate::Always },
        { L"fullBash", L"Exact Bash commands", &TurnsOptions::fullBash, TurnsLevel::Full, TranscriptBoxGate::NeedsCalls },
        { L"opOutput", L"First output line", &TurnsOptions::opOutput, TurnsLevel::Full, TranscriptBoxGate::NeedsCalls },
        { L"opDuration", L"Call durations", &TurnsOptions::opDuration, TurnsLevel::Full, TranscriptBoxGate::NeedsCalls },
        { L"fullWrite", L"Written files", &TurnsOptions::fullWrite, TurnsLevel::Max, TranscriptBoxGate::NeedsCalls },
        { L"stats", L"Stats per turn", &TurnsOptions::stats, TurnsLevel::Max, TranscriptBoxGate::Always },
    };

    // The name of the box every NeedsCalls box depends on and every WithoutCalls box gives way to (the full call lines).
    inline constexpr const wchar_t* kTranscriptCallsBoxName = L"ops";

    // Two names are the same box name: compared without case, so "OPS" hand-typed into settings.json still ticks "ops".
    // Parameters: a, b - the names.
    // Returns true when they match ignoring ASCII and Unicode case (towlower). Never throws.
    inline bool TranscriptViewNameEquals(std::wstring_view a, std::wstring_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (towlower(a[i]) != towlower(b[i]))
            {
                return false;
            }
        }
        return true;
    }

    // A view's settings.json name - what AppSettings::transcriptOverlayLevel stores for the tab the next open starts at: the
    // level names ("minimal" ... "max", TurnsLevelName) and "custom" / "user-only" / "assistant-only".
    // Parameters: view - a view index; an out-of-range value reads "default", like TurnsLevelName.
    // Returns a static string, never null. Never throws.
    inline const wchar_t* TranscriptViewName(int view) noexcept
    {
        switch (view)
        {
        case static_cast<int>(TranscriptView::Custom):
            return L"custom";
        case static_cast<int>(TranscriptView::UserOnly):
            return L"user-only";
        case static_cast<int>(TranscriptView::AssistantOnly):
            return L"assistant-only";
        default:
            return TurnsLevelName(static_cast<TurnsLevel>(view));
        }
    }

    // The view whose TranscriptViewName is `name`, compared without case (a hand-edited settings.json may say "Compact").
    // Parameters: name - the stored name; empty or unknown (a newer build's tab) finds nothing.
    // Returns the view index 0..7, or -1 when no view has that name - the caller opens at Default then. Never throws.
    inline int TranscriptViewFromName(std::wstring_view name) noexcept
    {
        for (int view = 0; view < kTranscriptViewCount; ++view)
        {
            if (TranscriptViewNameEquals(name, TranscriptViewName(view)))
            {
                return view;
            }
        }
        return -1;
    }

    // A view's display label - the words on its tab: the level labels (TranscriptLevelLabel) and "Custom" / "User Only" /
    // "Assistant Only". The overlay's box tabs take their words from here; ⚠ the LEVEL tabs take theirs from the Copy menu's
    // table (AgentCopyActions.h), and this header's tooltips from the guide - rename a level in both places (the guide's rule).
    // Parameters: view - a view index; an out-of-range value reads "Default".
    // Returns a static string, never null. Never throws.
    inline const wchar_t* TranscriptViewLabel(int view) noexcept
    {
        switch (view)
        {
        case static_cast<int>(TranscriptView::Custom):
            return L"Custom";
        case static_cast<int>(TranscriptView::UserOnly):
            return L"User Only";
        case static_cast<int>(TranscriptView::AssistantOnly):
            return L"Assistant Only";
        default:
            return TranscriptLevelLabel(static_cast<TurnsLevel>(view));
        }
    }

    // Whether a tab's ticked list ticks the box called `name`.
    // Parameters: ticked - the tab's stored names (any order, unknown names allowed); name - a box's name.
    // Returns true when the list holds the name (TranscriptViewNameEquals). Never throws.
    inline bool TranscriptViewTicks(const std::vector<std::wstring>& ticked, std::wstring_view name) noexcept
    {
        for (const auto& entry : ticked)
        {
            if (TranscriptViewNameEquals(entry, name))
            {
                return true;
            }
        }
        return false;
    }

    // A tab's ticked list after a click on one box: ticking adds the box's own name once (at the end, unless the list already
    // ticks it), clearing removes EVERY copy of it in any case. Every other entry - a name this build does not know
    // included, which a newer build wrote - is kept as it was and in its order, so a click never loses another version's
    // tick (PERSISTENCE.md section 17).
    // Parameters: ticked - the list to change (taken by value); name - the clicked box's name; on - its new state.
    // Returns the new list. Throws std::bad_alloc only.
    inline std::vector<std::wstring> TranscriptViewWithBox(std::vector<std::wstring> ticked, std::wstring_view name, bool on)
    {
        if (on)
        {
            if (!TranscriptViewTicks(ticked, name))
            {
                ticked.emplace_back(name);
            }
            return ticked;
        }
        std::vector<std::wstring> kept;
        kept.reserve(ticked.size());
        for (auto& entry : ticked)
        {
            if (!TranscriptViewNameEquals(entry, name))
            {
                kept.push_back(std::move(entry));
            }
        }
        return kept;
    }

    // Whether a box changes the render, given the tab's ticks (TranscriptBoxGate): a NeedsCalls box only while the full
    // call lines are ticked, a WithoutCalls box only while they are not, every other box always.
    // Parameters: box - the box; ticked - the tab's ticked list.
    // Returns true when ticking or clearing the box would change what the tab shows. Never throws.
    inline bool TranscriptViewBoxTakesEffect(const TranscriptViewBox& box, const std::vector<std::wstring>& ticked) noexcept
    {
        const bool calls = TranscriptViewTicks(ticked, kTranscriptCallsBoxName);
        switch (box.gate)
        {
        case TranscriptBoxGate::NeedsCalls:
            return calls;
        case TranscriptBoxGate::WithoutCalls:
            return !calls;
        case TranscriptBoxGate::Always:
        default:
            return true;
        }
    }

    // The render options of a view - what the overlay hands TranscriptTurns. A level is TurnsOptionsForLevel(level), its
    // ticks ignored. A box tab is Minimal with each ladder feature its box decides (ticked = on, everything else off), then
    // its sides: Custom keeps both, User Only drops the final replies and the messages teammates and other sessions sent,
    // Assistant Only drops every message that reached a turn.
    // Parameters: view - a view index (out of range: Default's options); ticked - the box tab's ticked list.
    // Returns the options. Never throws (nothing in it allocates).
    inline TurnsOptions TranscriptViewOptions(int view, const std::vector<std::wstring>& ticked) noexcept
    {
        if (!IsTranscriptBoxView(view))
        {
            return TurnsOptionsForLevel(static_cast<TurnsLevel>(view));
        }
        TurnsOptions opt = TurnsOptionsForLevel(TurnsLevel::Minimal);
        for (const auto& box : kTranscriptViewBoxes)
        {
            opt.*box.feature = TranscriptViewTicks(ticked, box.name);
        }
        switch (static_cast<TranscriptView>(view))
        {
        case TranscriptView::UserOnly:
            opt.finalReplies = false;
            opt.peerInputs = false;
            break;
        case TranscriptView::AssistantOnly:
            opt.userInputs = false;
            opt.peerInputs = false;
            break;
        case TranscriptView::Custom:
        default:
            break;
        }
        return opt;
    }

    // A box's tooltip: what it shows - the level guide's own sentence for its feature (every row of it, so "Tool calls with
    // outcome" also says that background completions come with it) - the level it comes from, and when it has no effect.
    // Shape (one item per line, for the shared tooltip host's plain text):
    //     Every tool call's main arguments and how it ended (ok, x exit 1, ...)
    //     When each background task finished, under the call that started it
    //     From Default.
    //     Shows under each call's full line: tick "Tool calls with outcome" too.      <- NeedsCalls only
    // Parameters: box - the box.
    // Returns the tooltip text. Throws std::bad_alloc only.
    inline std::wstring TranscriptViewBoxTip(const TranscriptViewBox& box)
    {
        std::wstring tip;
        for (const auto& row : kTranscriptLevelGuide)
        {
            if (row.feature == box.feature)
            {
                if (!tip.empty())
                {
                    tip += L"\n";
                }
                tip += row.text;
            }
        }
        if (tip.empty())
        {
            tip = box.label; // no guide row (the harness refuses this); the box still says something
        }
        tip += L"\nFrom ";
        tip += TranscriptLevelLabel(box.level);
        tip += L".";
        switch (box.gate)
        {
        case TranscriptBoxGate::NeedsCalls:
            tip += L"\nShows under each call's full line: tick \"Tool calls with outcome\" too.";
            break;
        case TranscriptBoxGate::WithoutCalls:
            tip += L"\nWith \"Tool calls with outcome\" ticked, each full call line already carries the reason.";
            break;
        case TranscriptBoxGate::Always:
        default:
            break;
        }
        return tip;
    }

    // A box tab's tooltip - its cheat sheet, like a level tab's (TranscriptLevelTip): the tab and its key, the side it shows
    // from Minimal, what it leaves out, and how the boxes work. A level's view gets the level's own cheat sheet.
    // Shape (one item per line):
    //     User Only (key 7)
    //     Your side of each turn, from Minimal: your messages, and what you typed while Claude worked.
    //     Leaves out each turn's final reply, and the messages teammates and other sessions sent - a turn ...
    //     The boxes above the transcript add what Compact, Default, Full and Max show.
    //     Each tab keeps its own boxes, in every window.
    // Parameters: view - the tab's view index; shortcut - the digit key that picks the tab (its 1-based position), or 0 to
    //             leave the key out.
    // Returns the tooltip text. Throws std::bad_alloc only.
    inline std::wstring TranscriptViewTip(int view, size_t shortcut)
    {
        if (!IsTranscriptBoxView(view))
        {
            return TranscriptLevelTip(static_cast<TurnsLevel>(view), shortcut);
        }
        std::wstring tip = TranscriptViewLabel(view);
        if (shortcut > 0)
        {
            tip += L" (key " + std::to_wstring(shortcut) + L")";
        }
        switch (static_cast<TranscriptView>(view))
        {
        case TranscriptView::UserOnly:
            tip += L"\nYour side of each turn, from Minimal: your messages, and what you typed while Claude worked."
                   L"\nLeaves out each turn's final reply, and the messages teammates and other sessions sent - a turn with nothing of "
                   L"yours is skipped, and the others keep their numbers.";
            break;
        case TranscriptView::AssistantOnly:
            tip += L"\nClaude's side of each turn, from Minimal: each turn's final reply."
                   L"\nLeaves out every message that reached a turn - yours, and those teammates and other sessions sent.";
            break;
        case TranscriptView::Custom:
        default:
            tip += L"\nBoth sides of each turn, from Minimal: your messages and each turn's final reply."
                   L"\nEvery box ticked shows exactly what Max shows.";
            break;
        }
        tip += L"\nThe boxes above the transcript add what Compact, Default, Full and Max show."
               L"\nEach tab keeps its own boxes, in every window.";
        return tip;
    }

    // The (i) panel's paragraph about the box tabs (the comparison table covers the five levels only).
    inline constexpr const wchar_t* kTranscriptViewsGuideNote =
        L"Custom, User Only and Assistant Only start from Minimal - Custom with both sides of each turn, User Only with your own "
        L"messages (no final reply, no teammate's message), Assistant Only with Claude's side (no message that reached a turn). "
        L"The boxes above the transcript add any row of this table from Compact to Max; each tab keeps its own boxes, in every window.";
}
