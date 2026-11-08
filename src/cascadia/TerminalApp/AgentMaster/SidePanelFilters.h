// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the side panel's FILTERS (SIDE_PANEL.md §2l): what the header's funnel button narrows the card list by,
// as plain data and pure rules.
//
// The user's request of 2026-11-08: "In sidemenu, we need to introduce ability to filter, filter by folder [...] a button
// for FILTER and in it have menu appear on click and then By Folder -> <the picker of +>. The row below the filter with N
// sessions across all window, shall be replaced by a row only shown when filter is on and in that row we show what we
// filter based on. Add other filterables." Their picks the same day (design-ask with mockups):
//   - By Folder opens the "+" launcher's own card in a folder-picking mode (AgentLaunchBar's pick mode);
//   - a folder keeps the folder AND everything under it by default, "and have some way to get Exactly that folder" (the
//     card's "Include subfolders" check box: each picked folder keeps its own choice — SidePanelFolderFilter);
//   - the other filters: By State, By Tag, the four flags (Unsent draft · Unread · Pinned to top · Favorite) and Active
//     within — "Multiselect enabled, menu shouldnt close on clicking" (the funnel's panel stays open while you pick);
//   - the row under the filter box: the count, one chip per filter (its ✕ drops that filter), Clear — shown only while a
//     filter narrows the list; the filters are kept per window (WindowRecord::sidePanelFilters).
//
// THE RULES (one facet per kind of filter):
//   - Different facets narrow TOGETHER (AND): a session is listed only when it passes every active facet.
//   - Picks inside one facet combine with OR: "Running or Waiting for you", "release or bug or untagged", folder A or B.
//   - The four flags are four facets: two checked flags mean both must hold (an unsent draft AND unread).
//   - An inactive facet passes everything. No filter at all = the list as before (only the scope and the typed text).
//   - The typed text in the filter box is a facet too (Text): its matching stays the box's own (ParseSessionQuery over
//     title / folder / branch / id, SidePanelSessionMatches); the matcher only takes its verdict.
//
// WHAT lives here (no WinRT, no XAML, no file IO): the facets and their bits, the six states and the four "active within"
// windows the panel offers, the filter record and its edits, the per-session facts the predicate reads, the compiled
// matcher (folder keys and tag names folded once per list update), the chips the row shows and what each chip's ✕ leaves,
// and the record's JSON (normalized on load: a hand-edited file can never make a filter that hides cards for a reason the
// row cannot show). The app applies it (TerminalPage::_SidePanelRebuildCards, the funnel's panel, the chips row); the m5
// harness pins it (TestSidePanelFilters, tests_sidepanel.cpp).

#pragma once

#include <algorithm>
#include <cstddef> // std::ptrdiff_t — a folder chip's erase offset
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Json.h" // json::Value — the window record's "sidePanelFilters" object
#include "SessionFolderFilter.h" // FolderKeyCovers / IsAbsoluteFolderPath / TrimFolderPath (+ NormDirKey through Persistence.h)
#include "SessionModels.h" // SessionState — the By State facet
#include "SessionStore.h" // NormalizeTagName / FoldTagName — a tag's identity is its folded name (TAGS.md)

namespace Agentmaster
{
    // ===== The facets ===================================================================================================

    // One kind of filter. Each facet is one bit of a facet mask (SidePanelFacetBit): the matcher answers with the mask of the
    // facets a session passes, so the funnel's panel can count "how many would be listed if this were picked" for one facet
    // with every OTHER facet applied (SidePanelPassesAllBut). The order is the chips row's order.
    enum class SidePanelFacet : uint8_t
    {
        Folder, // By Folder (the "+" card in pick mode): the session works in a picked folder (or under it)
        State, // By State: the session's state is one of the picked ones
        Tag, // By Tag: the session carries a picked tag, or carries none while "Untagged" is picked
        Draft, // the flag "Unsent draft": the session's input box holds a draft (the 3 dots)
        Unread, // the flag "Unread": the session is in the unread census (its tab's red ring)
        Pinned, // the flag "Pinned to top": the card is pinned (SIDE_PANEL.md §2j)
        Favorite, // the flag "Favorite": the session carries the ★ (FAVORITES.md)
        Active, // Active within: the session's last activity is younger than the picked window
        Text, // the typed text in the filter box (its verdict is the caller's: SidePanelSessionMatches)
    };
    // How many facets there are (the enum's size); the all-pass mask has this many bits.
    inline constexpr uint32_t kSidePanelFacetCount = 9;

    // The bit of one facet in a facet mask.
    //   facet   - the facet.
    //   returns - 1 << its index.
    //   throws  - nothing.
    [[nodiscard]] constexpr uint32_t SidePanelFacetBit(SidePanelFacet facet) noexcept
    {
        return 1u << static_cast<uint32_t>(facet);
    }

    // The mask of a session that passes every facet: what SidePanelFilterMatcher::Passed answers for a listed session.
    inline constexpr uint32_t kSidePanelAllFacets = (1u << kSidePanelFacetCount) - 1u;

    // Does a session pass every facet except `facet`? The question behind the funnel panel's counts: "how many sessions would
    // be listed if this pick were added to the facet", with the other facets as they are.
    //   passed  - SidePanelFilterMatcher::Passed for the session.
    //   facet   - the facet whose own verdict is ignored.
    //   returns - true when every other facet passes.
    //   throws  - nothing.
    [[nodiscard]] constexpr bool SidePanelPassesAllBut(uint32_t passed, SidePanelFacet facet) noexcept
    {
        return (passed | SidePanelFacetBit(facet)) == kSidePanelAllFacets;
    }

    // ===== The states (By State) ========================================================================================

    // One state the By State section offers: the session state, its bit in SidePanelFilters::states, the words the panel and the
    // chip show, and the token the window record stores (never the words: a renamed label must not lose a saved filter).
    struct SidePanelStateChoice
    {
        SessionState state; // the session state this entry matches
        uint32_t bit; // its bit in SidePanelFilters::states
        const wchar_t* label; // the panel's and the chip's words ("Waiting for you")
        const wchar_t* token; // the window record's stable token ("waiting")
    };
    // The six states, in the board's column order (Running · Waiting-for-you · Needs-approval · Error · Idle · Done), the order
    // the panel lists them and the chip names them in.
    inline constexpr SidePanelStateChoice kSidePanelStateChoices[] = {
        { SessionState::Running, 1u << 0, L"Running", L"running" },
        { SessionState::WaitingForInput, 1u << 1, L"Waiting for you", L"waiting" },
        { SessionState::NeedsApproval, 1u << 2, L"Needs approval", L"approval" },
        { SessionState::Error, 1u << 3, L"Error", L"error" },
        { SessionState::Idle, 1u << 4, L"Idle", L"idle" },
        { SessionState::Done, 1u << 5, L"Done", L"done" },
    };
    // Every state bit at once: a stored mask is cut to it on load (a bit no state owns can never hide a card).
    inline constexpr uint32_t kSidePanelAllStateBits = 0x3Fu;

    // The bit of a session state in SidePanelFilters::states.
    //   state   - the session's state.
    //   returns - its bit; 0 for a value no entry owns (none today — a future state would match no pick, never every pick).
    //   throws  - nothing.
    [[nodiscard]] constexpr uint32_t SidePanelStateBit(SessionState state) noexcept
    {
        for (const auto& choice : kSidePanelStateChoices)
        {
            if (choice.state == state)
            {
                return choice.bit;
            }
        }
        return 0;
    }

    // ===== Active within ================================================================================================

    // One window the Active within section offers: its length, the panel's words and the chip's words.
    struct SidePanelActiveChoice
    {
        uint32_t minutes; // the window, in minutes (SidePanelFilters::activeWithinMinutes)
        const wchar_t* label; // the panel's words ("1 hour")
        const wchar_t* chipText; // the chip's words ("active in 1 h")
    };
    // The four windows, shortest first. "Any time" (0) is the panel's own first entry, not one of these.
    inline constexpr SidePanelActiveChoice kSidePanelActiveChoices[] = {
        { 15u, L"15 minutes", L"active in 15 min" },
        { 60u, L"1 hour", L"active in 1 h" },
        { 1440u, L"24 hours", L"active in 24 h" },
        { 10080u, L"7 days", L"active in 7 days" },
    };
    // The longest window a stored record may hold (a year): a hand-edited value past it, or a negative one, reads as "any
    // time", so a corrupt number can never hide every card.
    inline constexpr uint32_t kSidePanelActiveMaxMinutes = 366u * 24u * 60u;

    // Is a session's last activity inside an "active within" window?
    //   lastActivityMs - the session's last activity, Unix ms (the card's "active … ago": the transcript's last line, else the
    //                    hook anchor); 0 or less = unknown, which is never "within" a window.
    //   nowMs          - the clock, Unix ms.
    //   minutes        - the window; 0 = any time (always true).
    //   returns        - true when the activity is at most `minutes` old. A stamp in the future (clock skew) counts as now.
    //   throws         - nothing.
    [[nodiscard]] constexpr bool SidePanelActiveWithin(int64_t lastActivityMs, int64_t nowMs, uint32_t minutes) noexcept
    {
        if (minutes == 0)
        {
            return true;
        }
        if (lastActivityMs <= 0)
        {
            return false;
        }
        return nowMs - lastActivityMs <= static_cast<int64_t>(minutes) * 60'000;
    }

    // The chip's words for a window: the offered entry's, else "active in N min" for a hand-edited value.
    //   minutes - SidePanelFilters::activeWithinMinutes (non-zero).
    //   returns - the words.
    //   throws  - std::bad_alloc.
    [[nodiscard]] inline std::wstring SidePanelActiveChipText(uint32_t minutes)
    {
        for (const auto& choice : kSidePanelActiveChoices)
        {
            if (choice.minutes == minutes)
            {
                return choice.chipText;
            }
        }
        return L"active in " + std::to_wstring(minutes) + L" min";
    }

    // ===== The filter record ============================================================================================
    // The record itself — SidePanelFolderFilter and SidePanelFilters — is plain data in SessionModels.h, beside the
    // WindowRecord that persists it (WindowRecord::sidePanelFilters); this header holds its rules. The record is kept
    // normalized by every edit below and by the loader: folders absolute and unique by NormDirKey, tags normalized and unique
    // by FoldTagName, states cut to kSidePanelAllStateBits, the window either 0 or in (0, kSidePanelActiveMaxMinutes].

    // Does any MENU filter narrow the list? (The typed text is the caller's to add: it is not part of the record.)
    //   filters - the record.
    //   returns - true when at least one facet other than Text is active.
    //   throws  - nothing.
    [[nodiscard]] inline bool SidePanelFiltersActive(const SidePanelFilters& filters) noexcept
    {
        return !filters.folders.empty() || filters.states != 0 || !filters.tags.empty() || filters.untagged || filters.draft ||
               filters.unread || filters.pinned || filters.favorite || filters.activeWithinMinutes != 0;
    }

    // The mask of the facets that constrain the list (an inactive facet is left out).
    //   filters    - the record.
    //   textActive - the filter box holds a query (the caller's ParseSessionQuery found at least one term).
    //   returns    - the facet bits.
    //   throws     - nothing.
    [[nodiscard]] inline uint32_t SidePanelActiveFacets(const SidePanelFilters& filters, bool textActive) noexcept
    {
        uint32_t mask = 0;
        mask |= filters.folders.empty() ? 0u : SidePanelFacetBit(SidePanelFacet::Folder);
        mask |= filters.states == 0 ? 0u : SidePanelFacetBit(SidePanelFacet::State);
        mask |= (filters.tags.empty() && !filters.untagged) ? 0u : SidePanelFacetBit(SidePanelFacet::Tag);
        mask |= filters.draft ? SidePanelFacetBit(SidePanelFacet::Draft) : 0u;
        mask |= filters.unread ? SidePanelFacetBit(SidePanelFacet::Unread) : 0u;
        mask |= filters.pinned ? SidePanelFacetBit(SidePanelFacet::Pinned) : 0u;
        mask |= filters.favorite ? SidePanelFacetBit(SidePanelFacet::Favorite) : 0u;
        mask |= filters.activeWithinMinutes == 0 ? 0u : SidePanelFacetBit(SidePanelFacet::Active);
        mask |= textActive ? SidePanelFacetBit(SidePanelFacet::Text) : 0u;
        return mask;
    }

    // ===== Edits (each keeps the record normalized; each returns whether it changed anything) ==========================

    // Add a folder to By Folder — the folder card's Filter. A folder already picked (the same NormDirKey, whatever its
    // spelling or case) keeps its place and takes the new Include-subfolders choice, so picking it again with the box
    // unchecked turns it into "exactly".
    //   filters           - the record to edit.
    //   rawPath           - the folder as typed or picked; trimmed here (TrimFolderPath).
    //   includeSubfolders - the card's check box.
    //   returns           - true when the record changed; false for a path that is not an absolute folder (nothing added)
    //                       or a pick identical to one already there.
    //   throws            - std::bad_alloc.
    inline bool SidePanelAddFolderFilter(SidePanelFilters& filters, std::wstring_view rawPath, bool includeSubfolders)
    {
        std::wstring path = TrimFolderPath(rawPath);
        if (path.empty() || !IsAbsoluteFolderPath(path))
        {
            return false;
        }
        const std::wstring key = NormDirKey(path);
        for (auto& existing : filters.folders)
        {
            if (NormDirKey(existing.path) == key)
            {
                if (existing.includeSubfolders == includeSubfolders)
                {
                    return false;
                }
                existing.includeSubfolders = includeSubfolders;
                return true;
            }
        }
        filters.folders.push_back(SidePanelFolderFilter{ std::move(path), includeSubfolders });
        return true;
    }

    // Toggle one state of By State.
    //   filters - the record to edit.
    //   bit     - a SidePanelStateChoice::bit; a value outside kSidePanelAllStateBits changes nothing.
    //   returns - true when the record changed.
    //   throws  - nothing.
    inline bool SidePanelToggleState(SidePanelFilters& filters, uint32_t bit) noexcept
    {
        bit &= kSidePanelAllStateBits;
        if (bit == 0)
        {
            return false;
        }
        filters.states ^= bit;
        return true;
    }

    // Toggle one tag of By Tag: picked -> dropped, else added (in the spelling given, normalized).
    //   filters - the record to edit.
    //   tag     - the tag's display name; matched to a picked one case-insensitively (FoldTagName).
    //   returns - true when the record changed; false for a name that normalizes to nothing.
    //   throws  - std::bad_alloc.
    inline bool SidePanelToggleTag(SidePanelFilters& filters, const std::wstring& tag)
    {
        const std::wstring name = NormalizeTagName(tag);
        if (name.empty())
        {
            return false;
        }
        const std::wstring folded = FoldTagName(name);
        const auto it = std::find_if(filters.tags.begin(), filters.tags.end(), [&](const std::wstring& t) { return FoldTagName(t) == folded; });
        if (it != filters.tags.end())
        {
            filters.tags.erase(it);
        }
        else
        {
            filters.tags.push_back(name);
        }
        return true;
    }

    // Put "Active within" on a window.
    //   filters - the record to edit.
    //   minutes - the window (0 = any time); a value past kSidePanelActiveMaxMinutes reads as 0.
    //   returns - true when the record changed.
    //   throws  - nothing.
    inline bool SidePanelSetActiveWithin(SidePanelFilters& filters, uint32_t minutes) noexcept
    {
        const uint32_t value = minutes > kSidePanelActiveMaxMinutes ? 0u : minutes;
        if (filters.activeWithinMinutes == value)
        {
            return false;
        }
        filters.activeWithinMinutes = value;
        return true;
    }

    // ===== The facts of one session, and the matcher ====================================================================

    // What the predicate reads about one session. The caller fills only what the active facets need (the matcher says which:
    // NeedsTags / NeedsFavorite); a fact a facet does not read may stay at its default.
    struct SidePanelFilterFacts
    {
        std::wstring launchKey; // NormDirKey of the session's working folder (SessionInfo::workingDir); "" = none
        std::wstring inferredKey; // NormDirKey of its inferred folder (SessionInfo::inferredWorkingDir); "" = none or the same folder
        SessionState state{ SessionState::Idle }; // the state the card shows
        const std::vector<std::wstring>* tags{ nullptr }; // the session's tags (display spellings), or null = none; read only by the Tag facet
        bool draft{ false }; // the input box holds an unsent draft (SessionInfo::pendingInput)
        bool unread{ false }; // the session is in the unread census (CurrentUnreadSessionIds)
        bool pinned{ false }; // the card is pinned to the top (IsSidePanelPinStamp)
        bool favorite{ false }; // the session carries the ★ (IsSessionFavorite)
        int64_t lastActivityMs{ 0 }; // the card's "active … ago" source (Unix ms; 0 = unknown)
    };

    // A filter record COMPILED for one list update: the picked folders' keys and the picked tags' folded names computed once,
    // so judging a session costs a few comparisons per facet. Build one per update (it holds the clock of that update).
    class SidePanelFilterMatcher
    {
    public:
        // Compile `filters`.
        //   filters - the window's record (normalized or not: a folder that is not absolute and an empty tag are skipped).
        //   nowMs   - the clock the Active facet judges against (Unix ms).
        //   throws  - std::bad_alloc.
        SidePanelFilterMatcher(const SidePanelFilters& filters, int64_t nowMs) :
            _states{ filters.states & kSidePanelAllStateBits },
            _untagged{ filters.untagged },
            _draft{ filters.draft },
            _unread{ filters.unread },
            _pinned{ filters.pinned },
            _favorite{ filters.favorite },
            _activeMinutes{ filters.activeWithinMinutes > kSidePanelActiveMaxMinutes ? 0u : filters.activeWithinMinutes },
            _nowMs{ nowMs }
        {
            for (const auto& folder : filters.folders)
            {
                const std::wstring path = TrimFolderPath(folder.path);
                if (!path.empty() && IsAbsoluteFolderPath(path))
                {
                    _folders.push_back(Folder{ NormDirKey(path), folder.includeSubfolders });
                }
            }
            for (const auto& tag : filters.tags)
            {
                const std::wstring name = NormalizeTagName(tag);
                if (!name.empty())
                {
                    _tags.push_back(FoldTagName(name));
                }
            }
        }

        // Does the Tag facet read the session's tags? (The caller then reads them for every session it judges.)
        //   returns - true while By Tag has a pick (a tag or Untagged).
        //   throws  - nothing.
        [[nodiscard]] bool NeedsTags() const noexcept { return !_tags.empty() || _untagged; }
        // Does the Favorite flag read the session's ★? (The caller then reads it for every session it judges.)
        //   returns - true while the flag is checked.
        //   throws  - nothing.
        [[nodiscard]] bool NeedsFavorite() const noexcept { return _favorite; }
        // Does the Unread flag read the unread census?
        //   returns - true while the flag is checked.
        //   throws  - nothing.
        [[nodiscard]] bool NeedsUnread() const noexcept { return _unread; }

        // The mask of the facets a session passes (an inactive facet always passes).
        //   facts  - the session's facts (see SidePanelFilterFacts for what each facet reads).
        //   textOk - the typed text's verdict for this session (true when the box is empty).
        //   returns - the facet bits passed; kSidePanelAllFacets = listed.
        //   throws  - std::bad_alloc (a tag fold).
        [[nodiscard]] uint32_t Passed(const SidePanelFilterFacts& facts, bool textOk) const
        {
            uint32_t passed = 0;
            passed |= _FolderOk(facts) ? SidePanelFacetBit(SidePanelFacet::Folder) : 0u;
            passed |= (_states == 0 || (_states & SidePanelStateBit(facts.state)) != 0) ? SidePanelFacetBit(SidePanelFacet::State) : 0u;
            passed |= _TagOk(facts) ? SidePanelFacetBit(SidePanelFacet::Tag) : 0u;
            passed |= (!_draft || facts.draft) ? SidePanelFacetBit(SidePanelFacet::Draft) : 0u;
            passed |= (!_unread || facts.unread) ? SidePanelFacetBit(SidePanelFacet::Unread) : 0u;
            passed |= (!_pinned || facts.pinned) ? SidePanelFacetBit(SidePanelFacet::Pinned) : 0u;
            passed |= (!_favorite || facts.favorite) ? SidePanelFacetBit(SidePanelFacet::Favorite) : 0u;
            passed |= SidePanelActiveWithin(facts.lastActivityMs, _nowMs, _activeMinutes) ? SidePanelFacetBit(SidePanelFacet::Active) : 0u;
            passed |= textOk ? SidePanelFacetBit(SidePanelFacet::Text) : 0u;
            return passed;
        }

        // Is the session listed? (Every facet passes.)
        //   facts, textOk - as for Passed.
        //   returns       - true when listed.
        //   throws        - std::bad_alloc (a tag fold).
        [[nodiscard]] bool Matches(const SidePanelFilterFacts& facts, bool textOk) const
        {
            return Passed(facts, textOk) == kSidePanelAllFacets;
        }

        // Does the session carry the (already folded) tag? The funnel panel counts each tag with it.
        //   facts       - the session's facts (its tags).
        //   foldedTag   - FoldTagName of the tag.
        //   returns     - true when one of the session's tags folds to it.
        //   throws      - std::bad_alloc.
        [[nodiscard]] static bool HasTag(const SidePanelFilterFacts& facts, const std::wstring& foldedTag)
        {
            if (!facts.tags)
            {
                return false;
            }
            return std::any_of(facts.tags->begin(), facts.tags->end(), [&](const std::wstring& t) { return FoldTagName(t) == foldedTag; });
        }

    private:
        // One picked folder, compiled.
        struct Folder
        {
            std::wstring key; // NormDirKey of the picked folder
            bool includeSubfolders{ true }; // the pick's own choice
        };

        // By Folder: no pick passes; else ANY pick covers the session's working or inferred folder (OR).
        bool _FolderOk(const SidePanelFilterFacts& facts) const noexcept
        {
            if (_folders.empty())
            {
                return true;
            }
            return std::any_of(_folders.begin(), _folders.end(), [&](const Folder& f) {
                return FolderKeyCovers(f.key, facts.launchKey, f.includeSubfolders) || FolderKeyCovers(f.key, facts.inferredKey, f.includeSubfolders);
            });
        }

        // By Tag: no pick passes; else a picked tag on the session, or no tag at all while Untagged is picked (OR).
        bool _TagOk(const SidePanelFilterFacts& facts) const
        {
            if (_tags.empty() && !_untagged)
            {
                return true;
            }
            const bool hasNone = !facts.tags || facts.tags->empty();
            if (_untagged && hasNone)
            {
                return true;
            }
            return std::any_of(_tags.begin(), _tags.end(), [&](const std::wstring& folded) { return HasTag(facts, folded); });
        }

        std::vector<Folder> _folders; // By Folder's picks, compiled
        std::vector<std::wstring> _tags; // By Tag's picks, folded
        uint32_t _states{ 0 }; // By State's bits
        bool _untagged{ false }; // By Tag's Untagged
        bool _draft{ false }; // the four flags
        bool _unread{ false };
        bool _pinned{ false };
        bool _favorite{ false };
        uint32_t _activeMinutes{ 0 }; // Active within (0 = any time)
        int64_t _nowMs{ 0 }; // the update's clock
    };

    // ===== The chips row ================================================================================================

    // What one chip of the row stands for — and so what its ✕ drops (SidePanelFiltersWithoutChip).
    enum class SidePanelChipKind : uint8_t
    {
        Folder, // one picked folder (its index in SidePanelFilters::folders)
        State, // the whole By State facet
        Tag, // the whole By Tag facet (tags and Untagged)
        Draft, // the Unsent draft flag
        Unread, // the Unread flag
        Pinned, // the Pinned to top flag
        Favorite, // the Favorite flag
        Active, // the Active within window
        Text, // the typed text (its ✕ empties the filter box — the caller's)
    };

    // One chip of the row: its kind, the folder it stands for, its words and its tooltip. The glyph and the color are the
    // UI's (a folder icon, the first picked state's dot, the first picked tag's ribbon…).
    struct SidePanelFilterChip
    {
        SidePanelChipKind kind{ SidePanelChipKind::Text };
        size_t folderIndex{ 0 }; // Folder chips: the index in SidePanelFilters::folders (0 for every other kind)
        std::wstring text; // the chip's words ("Agentmaster", "Running or Waiting for you", "“login”")
        std::wstring tip; // the full words, for the chip's tooltip ("Folder K:\x and every folder under it")
    };

    // The last component of a folder path, for a folder chip (a drive root names itself: "K:\").
    //   path    - a display spelling (TrimFolderPath's form).
    //   returns - the leaf; the whole path when it has no separator past a drive root.
    //   throws  - std::bad_alloc.
    [[nodiscard]] inline std::wstring SidePanelFolderLeaf(const std::wstring& path)
    {
        std::wstring trimmed = path;
        while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
        {
            trimmed.pop_back();
        }
        const auto sep = trimmed.find_last_of(L"\\/");
        if (sep == std::wstring::npos || sep + 1 >= trimmed.size())
        {
            return trimmed; // "K:\" or a bare name
        }
        return trimmed.substr(sep + 1);
    }

    // The chips the row shows, in the facets' order: one per folder, one for the states, one for the tags, one per flag, one
    // for the window, one for the typed text.
    //   filters   - the window's record.
    //   typedText - the filter box's text, trimmed by the caller ("" = no text chip).
    //   returns   - the chips (empty = the row hides).
    //   throws    - std::bad_alloc.
    [[nodiscard]] inline std::vector<SidePanelFilterChip> SidePanelFilterChips(const SidePanelFilters& filters, std::wstring_view typedText)
    {
        std::vector<SidePanelFilterChip> chips;
        for (size_t i = 0; i < filters.folders.size(); ++i)
        {
            const auto& folder = filters.folders[i];
            SidePanelFilterChip chip;
            chip.kind = SidePanelChipKind::Folder;
            chip.folderIndex = i;
            chip.text = SidePanelFolderLeaf(folder.path) + (folder.includeSubfolders ? L"" : L" (exactly)");
            chip.tip = folder.includeSubfolders ? (L"Folder " + folder.path + L" and every folder under it.") :
                                                  (L"Exactly the folder " + folder.path + L" (its subfolders are not included).");
            chips.push_back(std::move(chip));
        }
        if ((filters.states & kSidePanelAllStateBits) != 0)
        {
            SidePanelFilterChip chip;
            chip.kind = SidePanelChipKind::State;
            for (const auto& choice : kSidePanelStateChoices)
            {
                if ((filters.states & choice.bit) != 0)
                {
                    chip.text += (chip.text.empty() ? L"" : L" or ") + std::wstring{ choice.label };
                }
            }
            chip.tip = L"Sessions that are " + chip.text + L" now.";
            chips.push_back(std::move(chip));
        }
        if (!filters.tags.empty() || filters.untagged)
        {
            SidePanelFilterChip chip;
            chip.kind = SidePanelChipKind::Tag;
            for (const auto& tag : filters.tags)
            {
                chip.text += (chip.text.empty() ? L"" : L" or ") + tag;
            }
            if (filters.untagged)
            {
                chip.text += chip.text.empty() ? L"untagged" : L" or untagged";
            }
            chip.tip = L"Sessions tagged " + chip.text + L".";
            chips.push_back(std::move(chip));
        }
        const auto flag = [&](bool on, SidePanelChipKind kind, const wchar_t* text, const wchar_t* tip) {
            if (on)
            {
                chips.push_back(SidePanelFilterChip{ kind, 0, text, tip });
            }
        };
        flag(filters.draft, SidePanelChipKind::Draft, L"Unsent draft", L"Sessions whose input box holds a draft you have not sent (the 3 dots).");
        flag(filters.unread, SidePanelChipKind::Unread, L"Unread", L"Sessions with news you have not looked at (the red ring on their tab).");
        flag(filters.pinned, SidePanelChipKind::Pinned, L"Pinned to top", L"Cards you pinned to the top of this list.");
        flag(filters.favorite, SidePanelChipKind::Favorite, L"Favorite", L"Sessions you marked with the \x2605.");
        if (filters.activeWithinMinutes != 0)
        {
            SidePanelFilterChip chip;
            chip.kind = SidePanelChipKind::Active;
            chip.text = SidePanelActiveChipText(filters.activeWithinMinutes);
            chip.tip = L"Sessions whose last activity is at most that old.";
            chips.push_back(std::move(chip));
        }
        if (!typedText.empty())
        {
            SidePanelFilterChip chip;
            chip.kind = SidePanelChipKind::Text;
            chip.text = L"\x201C" + std::wstring{ typedText } + L"\x201D";
            chip.tip = L"The text typed in the filter box. \x2715 empties the box.";
            chips.push_back(std::move(chip));
        }
        return chips;
    }

    // The record without what one chip stands for: its ✕. A Text chip leaves the record as it is (the caller empties the box).
    //   filters - the record.
    //   chip    - the chip clicked (a Folder chip whose index is out of range — the record changed since the row was drawn —
    //             drops nothing).
    //   returns - the edited copy.
    //   throws  - std::bad_alloc.
    [[nodiscard]] inline SidePanelFilters SidePanelFiltersWithoutChip(SidePanelFilters filters, const SidePanelFilterChip& chip)
    {
        switch (chip.kind)
        {
        case SidePanelChipKind::Folder:
            if (chip.folderIndex < filters.folders.size())
            {
                filters.folders.erase(filters.folders.begin() + static_cast<std::ptrdiff_t>(chip.folderIndex));
            }
            break;
        case SidePanelChipKind::State:
            filters.states = 0;
            break;
        case SidePanelChipKind::Tag:
            filters.tags.clear();
            filters.untagged = false;
            break;
        case SidePanelChipKind::Draft:
            filters.draft = false;
            break;
        case SidePanelChipKind::Unread:
            filters.unread = false;
            break;
        case SidePanelChipKind::Pinned:
            filters.pinned = false;
            break;
        case SidePanelChipKind::Favorite:
            filters.favorite = false;
            break;
        case SidePanelChipKind::Active:
            filters.activeWithinMinutes = 0;
            break;
        case SidePanelChipKind::Text:
        default:
            break;
        }
        return filters;
    }

    // ===== The window record's JSON =====================================================================================

    // The record normalized: folders trimmed, absolute only and unique by NormDirKey (the first spelling and its choice kept);
    // tags normalized and unique by FoldTagName (the first spelling kept); states cut to kSidePanelAllStateBits; a window past
    // kSidePanelActiveMaxMinutes read as 0. What the loader returns, so a hand-edited record can never carry a filter the row
    // cannot show or the panel cannot uncheck.
    //   filters - the record as read or edited.
    //   returns - the normalized copy.
    //   throws  - std::bad_alloc.
    [[nodiscard]] inline SidePanelFilters NormalizeSidePanelFilters(const SidePanelFilters& filters)
    {
        SidePanelFilters out;
        std::vector<std::wstring> keys;
        for (const auto& folder : filters.folders)
        {
            std::wstring path = TrimFolderPath(folder.path);
            if (path.empty() || !IsAbsoluteFolderPath(path))
            {
                continue;
            }
            std::wstring key = NormDirKey(path);
            if (std::find(keys.begin(), keys.end(), key) != keys.end())
            {
                continue;
            }
            keys.push_back(std::move(key));
            out.folders.push_back(SidePanelFolderFilter{ std::move(path), folder.includeSubfolders });
        }
        std::vector<std::wstring> folded;
        for (const auto& tag : filters.tags)
        {
            std::wstring name = NormalizeTagName(tag);
            if (name.empty())
            {
                continue;
            }
            std::wstring f = FoldTagName(name);
            if (std::find(folded.begin(), folded.end(), f) != folded.end())
            {
                continue;
            }
            folded.push_back(std::move(f));
            out.tags.push_back(std::move(name));
        }
        out.states = filters.states & kSidePanelAllStateBits;
        out.untagged = filters.untagged;
        out.draft = filters.draft;
        out.unread = filters.unread;
        out.pinned = filters.pinned;
        out.favorite = filters.favorite;
        out.activeWithinMinutes = filters.activeWithinMinutes > kSidePanelActiveMaxMinutes ? 0u : filters.activeWithinMinutes;
        return out;
    }

    // The record as the window record's "sidePanelFilters" object. Only what is set is written (an absent member reads as
    // unset), so a window with one filter writes one member.
    //   filters - the record (written as given: normalize first when it may be raw).
    //   returns - the object; with no filter active, an empty object (the caller then omits the key altogether).
    //   throws  - std::bad_alloc.
    [[nodiscard]] inline json::Value SidePanelFiltersToJson(const SidePanelFilters& filters)
    {
        json::Value o = json::Value::MkObj();
        if (!filters.folders.empty())
        {
            json::Value arr = json::Value::MkArr();
            for (const auto& folder : filters.folders)
            {
                json::Value f = json::Value::MkObj();
                f.Set(L"path", json::Value::MkStr(folder.path));
                f.Set(L"subfolders", json::Value::MkBool(folder.includeSubfolders));
                arr.Push(std::move(f));
            }
            o.Set(L"folders", std::move(arr));
        }
        if ((filters.states & kSidePanelAllStateBits) != 0)
        {
            json::Value arr = json::Value::MkArr();
            for (const auto& choice : kSidePanelStateChoices)
            {
                if ((filters.states & choice.bit) != 0)
                {
                    arr.Push(json::Value::MkStr(choice.token)); // tokens, never the words: a relabeled state keeps its saved pick
                }
            }
            o.Set(L"states", std::move(arr));
        }
        if (!filters.tags.empty())
        {
            json::Value arr = json::Value::MkArr();
            for (const auto& tag : filters.tags)
            {
                arr.Push(json::Value::MkStr(tag));
            }
            o.Set(L"tags", std::move(arr));
        }
        const auto flag = [&](const wchar_t* key, bool on) {
            if (on)
            {
                o.Set(key, json::Value::MkBool(true));
            }
        };
        flag(L"untagged", filters.untagged);
        flag(L"draft", filters.draft);
        flag(L"unread", filters.unread);
        flag(L"pinned", filters.pinned);
        flag(L"favorite", filters.favorite);
        if (filters.activeWithinMinutes != 0)
        {
            o.Set(L"activeWithinMinutes", json::Value::MkNum(static_cast<double>(filters.activeWithinMinutes)));
        }
        return o;
    }

    // The record from the window record's "sidePanelFilters" object, normalized. Tolerant: a value of the wrong type, an
    // unknown state token, a folder that is not absolute, a negative or fractional window — each reads as "not set" for that
    // one part, never as a filter the panel cannot show.
    //   v       - the object (anything else — absent, null, an array — reads as no filter).
    //   returns - the record.
    //   throws  - std::bad_alloc.
    [[nodiscard]] inline SidePanelFilters SidePanelFiltersFromJson(const json::Value& v)
    {
        SidePanelFilters raw;
        if (v.type != json::Value::Type::Obj)
        {
            return raw;
        }
        if (const auto* folders = v.Find(L"folders"); folders && folders->type == json::Value::Type::Arr)
        {
            for (const auto& f : folders->arr)
            {
                if (f.type != json::Value::Type::Obj)
                {
                    continue;
                }
                SidePanelFolderFilter folder;
                folder.path = f.StrAt(L"path");
                // Absent or not a bool = the default (subfolders included): the user's pick for every folder.
                folder.includeSubfolders = f.BoolAt(L"subfolders", true);
                raw.folders.push_back(std::move(folder));
            }
        }
        if (const auto* states = v.Find(L"states"); states && states->type == json::Value::Type::Arr)
        {
            for (const auto& s : states->arr)
            {
                const std::wstring token = s.AsStr();
                for (const auto& choice : kSidePanelStateChoices)
                {
                    if (token == choice.token)
                    {
                        raw.states |= choice.bit;
                    }
                }
            }
        }
        if (const auto* tags = v.Find(L"tags"); tags && tags->type == json::Value::Type::Arr)
        {
            for (const auto& t : tags->arr)
            {
                if (t.type == json::Value::Type::Str)
                {
                    raw.tags.push_back(t.str);
                }
            }
        }
        raw.untagged = v.BoolAt(L"untagged", false);
        raw.draft = v.BoolAt(L"draft", false);
        raw.unread = v.BoolAt(L"unread", false);
        raw.pinned = v.BoolAt(L"pinned", false);
        raw.favorite = v.BoolAt(L"favorite", false);
        {
            // A whole, positive number of minutes up to a year; anything else (negative, fractional, huge, a string) is 0.
            const double minutes = v.NumAt(L"activeWithinMinutes", 0.0);
            raw.activeWithinMinutes = (minutes >= 1.0 && minutes <= static_cast<double>(kSidePanelActiveMaxMinutes) && minutes == static_cast<double>(static_cast<uint32_t>(minutes))) ?
                                          static_cast<uint32_t>(minutes) :
                                          0u;
        }
        return NormalizeSidePanelFilters(raw);
    }
}
