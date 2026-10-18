// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the NEW THREAD (MESSAGING.md §21.17): a thread the user opens in the bot's chat starts a NEW SESSION. The user's
// request (2026-10-18): "When creating a new thread in telegram and user is typing a prompt, similar to browse, ask user to pick
// workdir. two lists, one of most recently 10 with paging and second is top 10 used overall with paging. once picked, removed and
// that folder is used to create the session. this covers creation of a new tab. Then user can set the title."
//
// The flow (the service half is MessagingService's _onUserThreadCreated / _holdForNewThread / _onNewThreadAction / _adoptBoundThreads):
//   1. The user creates a thread (Telegram sends forum_topic_created) or writes in a thread no session owns: the bot posts a
//      PICKER there — two lists of folders, RECENT (the folder whose newest session was active last, first) and MOST USED (the
//      folder more sessions worked in, first), ten a page, each with its own PAGER row (the page number between a ◀ and a ▶, each
//      arrow only where it leads somewhere) — and holds what the user writes there.
//   2. A tap on a folder (or a folder path the user sends) deletes the picker and opens a new Claude tab there, with a session id
//      the service chose, in the background of the most recently active window.
//   3. When the session is live, the service ADOPTS the user's thread as the session's topic (no new topic is made), renames it like
//      every topic (state glyph, color square, title), and hands it the held messages as if they had been sent in the topic: the
//      first becomes the session's first prompt. A held message the user deleted meanwhile is left out (HeldMessageGone), and
//      each one's 👀 "seen" mark comes off as it is handed over.
//   4. The title: the thread's name when the user named it (forum_topic_created's is_name_implicit false), else the folder-derived
//      default; renaming the thread later renames the session (forum_topic_edited), like /rename.
//
// This header is the PURE half: the folder lists from the registry's facts, the picker's text and buttons, the buttons' callback
// data, the name a button shows, a topic name without its decoration, and a typed folder path. Header-only, no I/O (the folder
// probe is injected), no Win32, no WinRT: the m5 suite pins every rule (tests_messaging.cpp, TestMessagingNewThread).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype> // std::towlower (detail::NewThreadFoldLeaf)
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MessagingModels.h" // StateGlyph (StripTopicNameDecoration), TgShortSpan / TgEscapeHtml via TelegramFormat.h
#include "SessionFolderFilter.h" // SessionFolderFacts, CollectSessionFolders, SortFoldersByRecency / SortFoldersByUse, FolderKeyCovers, TrimFolderPath, IsAbsoluteFolderPath
#include "TelegramFormat.h" // TgEscapeHtml, TgShortSpan, detail::TgPrefixUnits

namespace Agentmaster::Msg
{
    // ===== the limits ================================================================================================

    // Folders per page of each list: the user's "most recently 10 with paging … top 10 used overall with paging".
    inline constexpr size_t kNewThreadPageSize = 10;
    // The most folders a list keeps (ten pages): a picker is a choice, not an index of every folder ever used, and the probe that
    // drops a deleted folder runs once per kept folder.
    inline constexpr size_t kNewThreadListMax = 100;
    // A picked launch whose session has not come up live after this long is given up: the thread is told, and its held messages
    // are not delivered (the user still sees them in the chat). A background tab registers in about a second; three minutes
    // covers a window that was busy re-homing a large workspace.
    inline constexpr int64_t kNewThreadBindTimeoutMs = 3 * 60 * 1000;
    // A picker nobody used for this long is forgotten (its thread can start a new one with the next message): the service keeps
    // the pickers in memory only, so this bounds them.
    inline constexpr int64_t kNewThreadPickerIdleMs = 24 * 60 * 60 * 1000;
    // The most messages one picker holds; a message past it is answered and not held (the picker is a doorway, not a queue).
    inline constexpr size_t kNewThreadHeldMax = 20;
    // The folder checks run on the POLL thread, the one that reads Telegram, so each waits a bounded time (FolderProbe.h): an
    // offline network share answers only after the network's timeout, tens of seconds, and the whole bot froze meanwhile.
    //   A PICK (a picker button, a typed path, the Search topic's /newsession <folder>) asks the folder again and waits this long;
    //   past it the user is told the folder did not answer, and the probe goes on in the background.
    inline constexpr uint32_t kNewThreadPickProbeWaitMs = 2000;
    //   A picker's LISTS wait at most this long per folder…
    inline constexpr uint32_t kNewThreadListProbeEachMs = 250;
    //   …and at most this long for all the folders together; past it the rest are listed unchecked (the pick checks them).
    inline constexpr uint32_t kNewThreadListProbeBudgetMs = 1500;
    //   A list takes an answer up to this old (a minute: a folder deleted that recently is caught by the pick's own check).
    inline constexpr uint32_t kNewThreadListProbeMaxAgeMs = 60 * 1000;
    // The most characters of a folder's name on its button (an ellipsis past it).
    inline constexpr size_t kNewThreadLabelMaxChars = 24;
    // The prefix of every picker button's callback data. "nt:" collides with no other prefix the service reads ("c:" card verbs,
    // "t:" tokens, "d:front:" directory dots, the Search buttons' own).
    inline constexpr std::wstring_view kNewThreadCallbackPrefix = L"nt:";

    // ===== the folder lists ==========================================================================================

    /// <summary>One folder of a picker list: its spelling and how the registry's sessions used it.</summary>
    struct NewThreadFolder
    {
        std::wstring path; // the display spelling (TrimFolderPath: '\' separators, no trailing one but a drive root's)
        int sessions{ 0 }; // the sessions that worked there (launched there, or inferred there)
        int64_t lastActivityMs{ 0 }; // the newest activity among them (Unix ms; 0 = unknown)
    };

    /// <summary>The picker's two lists, each in its own order and capped at kNewThreadListMax.</summary>
    struct NewThreadLists
    {
        std::vector<NewThreadFolder> recent; // SortFoldersByRecency: the folder active most recently first
        std::vector<NewThreadFolder> mostUsed; // SortFoldersByUse: the folder more sessions worked in first
    };

    /// <summary>
    /// The picker's folder lists (§21.17) from the registry's sessions: every folder a session was launched in or worked in
    /// (CollectSessionFolders — the Sessions page's folder filter's universe, one count per session per folder), minus the folders
    /// under a scratch root (the machine's temp folders: a session started there is a probe, never a place to work) and the
    /// folders the probe says are gone. Each list keeps its first kNewThreadListMax survivors.
    /// </summary>
    /// <param name="sessions">One entry per registry session (archived ones too: "top 10 used overall").</param>
    /// <param name="excludedRootKeys">NormDirKey of the roots whose folders (and the roots themselves) are left out — the
    /// service passes the machine's temp roots.</param>
    /// <param name="folderExists">The probe: true when the folder is an existing directory. Called at most once per folder (its
    /// answers are kept for the second list); an empty function keeps every folder.</param>
    /// <param name="maxPerList">The cap per list (kNewThreadListMax by default; a test passes a small one).</param>
    /// <returns>The two lists.</returns>
    /// <exception cref="std::bad_alloc">If the lists cannot be allocated.</exception>
    /// <remarks>The probe may block on a network share; the service calls this off the UI thread (the poll thread), once per
    /// picker.</remarks>
    inline NewThreadLists BuildNewThreadLists(const std::vector<SessionFolderFacts>& sessions,
                                              const std::vector<std::wstring>& excludedRootKeys,
                                              const std::function<bool(const std::wstring&)>& folderExists,
                                              size_t maxPerList = kNewThreadListMax)
    {
        std::vector<SessionFolderUse> all = CollectSessionFolders(sessions);
        // The scratch roots first (no probe for them), so a temp folder that still exists never reaches a list.
        all.erase(std::remove_if(all.begin(), all.end(), [&](const SessionFolderUse& f) {
                      return std::any_of(excludedRootKeys.begin(), excludedRootKeys.end(), [&](const std::wstring& root) { return FolderKeyCovers(root, f.key, true); });
                  }),
                  all.end());
        // One probe per folder, shared by the two lists (a folder ranks in both).
        std::unordered_map<std::wstring, bool> exists;
        const auto keep = [&](const SessionFolderUse& f) {
            if (!folderExists)
            {
                return true;
            }
            const auto [it, isNew] = exists.try_emplace(f.key, false);
            if (isNew)
            {
                it->second = folderExists(f.path);
            }
            return it->second;
        };
        const auto take = [&](std::vector<SessionFolderUse> sorted) {
            std::vector<NewThreadFolder> out;
            for (const auto& f : sorted)
            {
                if (out.size() >= maxPerList)
                {
                    break;
                }
                if (keep(f))
                {
                    out.push_back(NewThreadFolder{ f.path, f.sessions, f.lastActivityMs });
                }
            }
            return out;
        };
        NewThreadLists lists;
        std::vector<SessionFolderUse> byRecency = all;
        SortFoldersByRecency(byRecency);
        lists.recent = take(std::move(byRecency));
        SortFoldersByUse(all);
        lists.mostUsed = take(std::move(all));
        return lists;
    }

    // ===== the pages =================================================================================================

    /// <summary>How many pages a list of `items` folders has: at least one (an empty list shows one empty page).</summary>
    /// <param name="items">The list's size.</param>
    /// <returns>The page count, &gt;= 1.</returns>
    inline size_t NewThreadPageCount(size_t items) noexcept
    {
        return items == 0 ? 1 : (items + kNewThreadPageSize - 1) / kNewThreadPageSize;
    }

    /// <summary>A page index kept inside a list's pages (a button from an older render can name a page that no longer exists).</summary>
    /// <param name="page">The 0-based page asked for.</param>
    /// <param name="items">The list's size.</param>
    /// <returns>The page, at most the last one.</returns>
    inline size_t ClampNewThreadPage(size_t page, size_t items) noexcept
    {
        const size_t pages = NewThreadPageCount(items);
        return page < pages ? page : pages - 1;
    }

    /// <summary>
    /// The text of a pager's page LABEL ("2 / 4"): the page shown and how many there are, so the user sees at once how far a list
    /// goes. Only a list of two pages or more has a pager (BuildNewThreadPagingRow).
    /// </summary>
    /// <param name="page">The 0-based page shown.</param>
    /// <param name="pages">The list's page count (NewThreadPageCount).</param>
    /// <returns>The 1-based page, " / ", the count.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NewThreadPageLabel(size_t page, size_t pages)
    {
        return std::to_wstring(page + 1) + L" / " + std::to_wstring(pages);
    }

    /// <summary>
    /// The TOAST a tap on a pager's page label answers with (the label is not a button that does anything: Telegram still shows a
    /// spinner on it until the tap is answered, so the answer says where the user is): "Recent: page 2 of 4 (folders 11–20 of 34)".
    /// Plain text, well under answerCallbackQuery's 200 characters.
    /// </summary>
    /// <param name="listName">The list's name ("Recent", "Most used").</param>
    /// <param name="page">The 0-based page the label shows (clamped here: the list can be shorter than when the label was drawn).</param>
    /// <param name="items">The list's size.</param>
    /// <returns>The toast; for an empty list, that it has no folders.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NewThreadPageToast(std::wstring_view listName, size_t page, size_t items)
    {
        if (items == 0)
        {
            return std::wstring{ listName } + L": no folders known yet.";
        }
        page = ClampNewThreadPage(page, items);
        const size_t first = page * kNewThreadPageSize;
        const size_t last = (std::min)(items, first + kNewThreadPageSize); // (std::min): the <windows.h> macro
        return std::wstring{ listName } + L": page " + std::to_wstring(page + 1) + L" of " + std::to_wstring(NewThreadPageCount(items)) + L" (folders " +
               std::to_wstring(first + 1) + L"\x2013" + std::to_wstring(last) + L" of " + std::to_wstring(items) + L")";
    }

    // ===== the buttons' callback data ================================================================================

    /// <summary>What a picker button asks for (§21.17). The data names the list and a GLOBAL index (or a page), never a path: a
    /// path can pass the Bot API's 64-byte limit for callback data, and the lists live in the service's picker state.</summary>
    struct NewThreadAction
    {
        enum class Kind
        {
            None, // not a picker button (or a malformed one)
            PickRecent, // value = the folder's 0-based index in the Recent list
            PickUsed, // value = the folder's 0-based index in the Most used list
            PageRecent, // value = the Recent list's 0-based page to show
            PageUsed, // value = the Most used list's 0-based page to show
            PageLabelRecent, // the Recent pager's page label: nothing changes, the toast says the page; value = the 0-based page it shows
            PageLabelUsed, // the Most used pager's page label (as PageLabelRecent)
            Cancel, // the picker goes; the held messages are dropped
        };
        Kind kind{ Kind::None };
        size_t value{ 0 };
    };

    /// <summary>The callback data of a picker button: "nt:r:3", "nt:m:0", "nt:rp:1", "nt:mp:2", "nt:rl:1", "nt:ml:0", "nt:x".</summary>
    /// <param name="kind">The action (None gives "").</param>
    /// <param name="value">The index or page (ignored for Cancel).</param>
    /// <returns>The data, at most 26 characters ("nt:", a three-character tag, a 64-bit value's 20 digits): far under the Bot API's
    /// 64 bytes. Only a value of at most 9 digits parses back (ParseNewThreadCallbackData), which every real index and page is.</returns>
    /// <exception cref="std::bad_alloc">If the data cannot be allocated.</exception>
    inline std::wstring NewThreadCallbackData(NewThreadAction::Kind kind, size_t value)
    {
        const std::wstring prefix{ kNewThreadCallbackPrefix };
        switch (kind)
        {
        case NewThreadAction::Kind::PickRecent:
            return prefix + L"r:" + std::to_wstring(value);
        case NewThreadAction::Kind::PickUsed:
            return prefix + L"m:" + std::to_wstring(value);
        case NewThreadAction::Kind::PageRecent:
            return prefix + L"rp:" + std::to_wstring(value);
        case NewThreadAction::Kind::PageUsed:
            return prefix + L"mp:" + std::to_wstring(value);
        case NewThreadAction::Kind::PageLabelRecent:
            return prefix + L"rl:" + std::to_wstring(value);
        case NewThreadAction::Kind::PageLabelUsed:
            return prefix + L"ml:" + std::to_wstring(value);
        case NewThreadAction::Kind::Cancel:
            return prefix + L"x";
        case NewThreadAction::Kind::None:
        default:
            return {};
        }
    }

    /// <summary>Reads a picker button's callback data back (NewThreadCallbackData's inverse).</summary>
    /// <param name="data">The callback data.</param>
    /// <returns>The action; Kind::None for data that is not a well-formed picker button's (a number with anything but digits, or
    /// more than 9 of them, is malformed).</returns>
    inline NewThreadAction ParseNewThreadCallbackData(std::wstring_view data) noexcept
    {
        NewThreadAction a;
        if (data.size() <= kNewThreadCallbackPrefix.size() || data.substr(0, kNewThreadCallbackPrefix.size()) != kNewThreadCallbackPrefix)
        {
            return a;
        }
        data.remove_prefix(kNewThreadCallbackPrefix.size());
        if (data == L"x")
        {
            a.kind = NewThreadAction::Kind::Cancel;
            return a;
        }
        const size_t colon = data.find(L':');
        if (colon == std::wstring_view::npos)
        {
            return a;
        }
        const std::wstring_view what = data.substr(0, colon);
        const std::wstring_view digits = data.substr(colon + 1);
        if (digits.empty() || digits.size() > 9)
        {
            return a;
        }
        size_t value = 0;
        for (const wchar_t c : digits)
        {
            if (c < L'0' || c > L'9')
            {
                return a;
            }
            value = value * 10 + static_cast<size_t>(c - L'0');
        }
        if (what == L"r")
        {
            a.kind = NewThreadAction::Kind::PickRecent;
        }
        else if (what == L"m")
        {
            a.kind = NewThreadAction::Kind::PickUsed;
        }
        else if (what == L"rp")
        {
            a.kind = NewThreadAction::Kind::PageRecent;
        }
        else if (what == L"mp")
        {
            a.kind = NewThreadAction::Kind::PageUsed;
        }
        else if (what == L"rl")
        {
            a.kind = NewThreadAction::Kind::PageLabelRecent;
        }
        else if (what == L"ml")
        {
            a.kind = NewThreadAction::Kind::PageLabelUsed;
        }
        else
        {
            return a;
        }
        a.value = value;
        return a;
    }

    // ===== the picker message ========================================================================================

    namespace detail
    {
        /// <summary>A folder's last segment ("Agentmaster" for "K:\source\Agentmaster"; "K:" for the drive root "K:\").</summary>
        /// <param name="path">The display spelling.</param>
        /// <returns>The segment (the text itself when it has no separator).</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        inline std::wstring NewThreadLeaf(std::wstring_view path)
        {
            while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/'))
            {
                path.remove_suffix(1);
            }
            const size_t sep = path.find_last_of(L"\\/");
            return std::wstring{ sep == std::wstring_view::npos ? path : path.substr(sep + 1) };
        }

        /// <summary>A folder's last TWO segments ("source\Agentmaster"), for two shown folders that share a last segment.</summary>
        /// <param name="path">The display spelling.</param>
        /// <returns>The parent's segment, '\', the leaf; the leaf alone when there is no parent.</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        inline std::wstring NewThreadParentAndLeaf(std::wstring_view path)
        {
            while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/'))
            {
                path.remove_suffix(1);
            }
            const size_t sep = path.find_last_of(L"\\/");
            if (sep == std::wstring_view::npos || sep == 0)
            {
                return NewThreadLeaf(path);
            }
            return NewThreadLeaf(path.substr(0, sep)) + L"\\" + std::wstring{ path.substr(sep + 1) };
        }

        /// <summary>A leaf folded for the "same name" test (lower case: Windows folders differ in case only by spelling).</summary>
        /// <param name="leaf">The segment.</param>
        /// <returns>The folded segment.</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        inline std::wstring NewThreadFoldLeaf(std::wstring_view leaf)
        {
            std::wstring f{ leaf };
            for (auto& c : f)
            {
                c = static_cast<wchar_t>(std::towlower(c));
            }
            return f;
        }
    }

    /// <summary>
    /// The name a folder's BUTTON shows: its last segment ("Agentmaster"), or its last two ("source\Agentmaster") when another
    /// shown folder has the same last segment (two checkouts of one repository), cut at kNewThreadLabelMaxChars with an ellipsis.
    /// The full path is in the message text above the buttons, numbered like them.
    /// </summary>
    /// <param name="path">The folder.</param>
    /// <param name="shownPaths">Every folder the keyboard shows (both lists' current pages); `path` among them.</param>
    /// <returns>The label.</returns>
    /// <exception cref="std::bad_alloc">If the label cannot be allocated.</exception>
    inline std::wstring NewThreadFolderLabel(std::wstring_view path, const std::vector<std::wstring>& shownPaths)
    {
        const std::wstring leaf = detail::NewThreadLeaf(path);
        const std::wstring folded = detail::NewThreadFoldLeaf(leaf);
        const std::wstring ownKey = NormDirKey(TrimFolderPath(path));
        bool shared = false;
        for (const auto& other : shownPaths)
        {
            if (NormDirKey(TrimFolderPath(other)) != ownKey && detail::NewThreadFoldLeaf(detail::NewThreadLeaf(other)) == folded)
            {
                shared = true;
                break;
            }
        }
        std::wstring label = shared ? detail::NewThreadParentAndLeaf(path) : leaf;
        if (label.size() > kNewThreadLabelMaxChars)
        {
            label = std::wstring{ ::Agentmaster::Msg::detail::TgPrefixUnits(label, kNewThreadLabelMaxChars - 1) } + L"\x2026";
        }
        return label;
    }

    /// <summary>
    /// The PICKER's text (§21.17): what it is for, what happens to the user's messages, how the title is chosen, then the two
    /// lists' current pages — each line the folder's number (the button's), its full path and, for Recent, how long ago it was
    /// used, for Most used how many sessions worked there. Classic Telegram HTML (no title token: it is sent and edited as is).
    /// </summary>
    /// <param name="lists">The picker's lists.</param>
    /// <param name="recentPage">The Recent page shown (clamped here).</param>
    /// <param name="usedPage">The Most used page shown (clamped here).</param>
    /// <param name="heldCount">How many of the user's messages the picker holds (0 = none yet).</param>
    /// <param name="topicName">The thread's name when the user gave it one ("" = Telegram named it, or it is not known).</param>
    /// <param name="nowMs">The clock the "ago" texts are counted against (Unix ms).</param>
    /// <returns>The text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildNewThreadPickerHtml(const NewThreadLists& lists, size_t recentPage, size_t usedPage, size_t heldCount, std::wstring_view topicName, int64_t nowMs)
    {
        std::wstring html = L"\U0001F4C2 <b>New session</b> \x2014 pick the folder it works in.";
        if (heldCount == 0)
        {
            html += L"\n<i>A message you send here waits for the session and becomes its first prompt.</i>";
        }
        else if (heldCount == 1)
        {
            html += L"\n<i>Your message waits here and becomes the session's first prompt.</i>";
        }
        else
        {
            html += L"\n<i>Your " + std::to_wstring(heldCount) + L" messages wait here and go to the session in order.</i>";
        }
        html += topicName.empty() ? std::wstring{ L"\n<i>Its title: the folder's name \x2014 rename this thread to change it.</i>" }
                                  : L"\n<i>Its title: <b>" + TgEscapeHtml(TgTitleLine(topicName, 120)) + L"</b> \x2014 rename this thread to change it.</i>"; // a title: on one line (TgTitleLine), like every title the bot prints
        html += L"\n<i>Or send a folder's full path.</i>";
        const auto section = [&](const wchar_t* glyph, const wchar_t* name, const std::vector<NewThreadFolder>& list, size_t page, bool recency) {
            html += L"\n\n";
            html += glyph;
            html += L" <b>";
            html += name;
            html += L"</b>";
            if (list.empty())
            {
                html += L"\n<i>No folders known yet.</i>";
                return;
            }
            page = ClampNewThreadPage(page, list.size());
            const size_t first = page * kNewThreadPageSize;
            const size_t last = (std::min)(list.size(), first + kNewThreadPageSize); // (std::min): the <windows.h> macro
            html += L" \x00B7 " + std::to_wstring(first + 1) + L"\x2013" + std::to_wstring(last) + L" of " + std::to_wstring(list.size());
            for (size_t i = first; i < last; ++i)
            {
                const auto& f = list[i];
                html += L"\n<b>" + std::to_wstring(i + 1) + L"</b> " + TgEscapeHtml(f.path);
                if (recency)
                {
                    if (f.lastActivityMs > 0 && nowMs >= f.lastActivityMs)
                    {
                        html += L" \x00B7 " + TgShortSpan(nowMs - f.lastActivityMs) + L" ago";
                    }
                }
                else
                {
                    html += L" \x00B7 " + std::to_wstring(f.sessions) + (f.sessions == 1 ? L" session" : L" sessions");
                }
            }
        };
        section(L"\U0001F558", L"Recent", lists.recent, recentPage, true);
        section(L"\U0001F3C6", L"Most used", lists.mostUsed, usedPage, false);
        return html;
    }

    /// <summary>One button of the picker's keyboard: its label and its callback data.</summary>
    struct NewThreadButton
    {
        std::wstring text;
        std::wstring data;
    };

    /// <summary>
    /// One list's PAGER row (§21.17; the user's layout of 2026-10-18: "We start with one button to the right and after moving one
    /// page there are 2 buttons and one label in middle for the page number. reaching end shows only to the left"): the page label
    /// ("2 / 4", NewThreadPageLabel) with "◀ Recent" on its left from the second page on and "Recent ▶" on its right until the last
    /// page — the first page [1 / 4] [Recent ▶], a middle page [◀ Recent] [2 / 4] [Recent ▶], the last [◀ Recent] [4 / 4]. The label
    /// is there on every page: it places a lone arrow on its side (a lone button would fill the whole row) and says from the first
    /// page how far the list goes. A list of one page, or none, has no pager.
    /// </summary>
    /// <param name="listName">The list's name the arrows carry ("Recent", "Most used"): the picker shows two pagers.</param>
    /// <param name="items">The list's size.</param>
    /// <param name="page">The page shown (clamped here).</param>
    /// <param name="turn">An arrow's action (PageRecent / PageUsed); its value is the page the arrow turns to.</param>
    /// <param name="label">The label's action (PageLabelRecent / PageLabelUsed); its value is the page the label shows.</param>
    /// <returns>The row's buttons, left to right; empty when the list fits on one page.</returns>
    /// <exception cref="std::bad_alloc">If the row cannot be allocated.</exception>
    inline std::vector<NewThreadButton> BuildNewThreadPagingRow(std::wstring_view listName, size_t items, size_t page, NewThreadAction::Kind turn, NewThreadAction::Kind label)
    {
        std::vector<NewThreadButton> row;
        const size_t pages = NewThreadPageCount(items);
        if (pages < 2)
        {
            return row; // nothing to turn to
        }
        page = ClampNewThreadPage(page, items);
        if (page > 0)
        {
            row.push_back(NewThreadButton{ L"\x25C0 " + std::wstring{ listName }, NewThreadCallbackData(turn, page - 1) });
        }
        row.push_back(NewThreadButton{ NewThreadPageLabel(page, pages), NewThreadCallbackData(label, page) });
        if (page + 1 < pages)
        {
            row.push_back(NewThreadButton{ std::wstring{ listName } + L" \x25B6", NewThreadCallbackData(turn, page + 1) });
        }
        return row;
    }

    /// <summary>
    /// The picker's KEYBOARD (§21.17): the Recent page's folders two to a row ("🕘 1 · Agentmaster"), its pager row
    /// (BuildNewThreadPagingRow: the page label between the arrows that lead somewhere), the Most used page's folders
    /// ("🏆 1 · Agentmaster") and pager row, and Cancel.
    /// </summary>
    /// <param name="lists">The picker's lists.</param>
    /// <param name="recentPage">The Recent page shown (clamped here).</param>
    /// <param name="usedPage">The Most used page shown (clamped here).</param>
    /// <returns>The rows, top first.</returns>
    /// <exception cref="std::bad_alloc">If the rows cannot be allocated.</exception>
    inline std::vector<std::vector<NewThreadButton>> BuildNewThreadPickerKeyboard(const NewThreadLists& lists, size_t recentPage, size_t usedPage)
    {
        recentPage = ClampNewThreadPage(recentPage, lists.recent.size());
        usedPage = ClampNewThreadPage(usedPage, lists.mostUsed.size());
        const auto pageRange = [](const std::vector<NewThreadFolder>& list, size_t page) {
            const size_t first = page * kNewThreadPageSize;
            return std::make_pair((std::min)(first, list.size()), (std::min)(list.size(), first + kNewThreadPageSize)); // (std::min): the <windows.h> macro
        };
        const auto [rFirst, rLast] = pageRange(lists.recent, recentPage);
        const auto [mFirst, mLast] = pageRange(lists.mostUsed, usedPage);
        // The labels look at every folder the keyboard shows, so two shown folders with one name both get their parent.
        std::vector<std::wstring> shown;
        for (size_t i = rFirst; i < rLast; ++i)
        {
            shown.push_back(lists.recent[i].path);
        }
        for (size_t i = mFirst; i < mLast; ++i)
        {
            shown.push_back(lists.mostUsed[i].path);
        }
        std::vector<std::vector<NewThreadButton>> rows;
        const auto folderRows = [&](const wchar_t* glyph, const std::vector<NewThreadFolder>& list, size_t first, size_t last, NewThreadAction::Kind pick) {
            std::vector<NewThreadButton> row;
            for (size_t i = first; i < last; ++i)
            {
                row.push_back(NewThreadButton{ std::wstring{ glyph } + L" " + std::to_wstring(i + 1) + L" \x00B7 " + NewThreadFolderLabel(list[i].path, shown), NewThreadCallbackData(pick, i) });
                if (row.size() == 2)
                {
                    rows.push_back(std::move(row));
                    row.clear();
                }
            }
            if (!row.empty())
            {
                rows.push_back(std::move(row));
            }
        };
        const auto pagingRow = [&](const wchar_t* name, size_t items, size_t page, NewThreadAction::Kind turn, NewThreadAction::Kind label) {
            std::vector<NewThreadButton> row = BuildNewThreadPagingRow(name, items, page, turn, label);
            if (!row.empty())
            {
                rows.push_back(std::move(row));
            }
        };
        folderRows(L"\U0001F558", lists.recent, rFirst, rLast, NewThreadAction::Kind::PickRecent);
        pagingRow(L"Recent", lists.recent.size(), recentPage, NewThreadAction::Kind::PageRecent, NewThreadAction::Kind::PageLabelRecent);
        folderRows(L"\U0001F3C6", lists.mostUsed, mFirst, mLast, NewThreadAction::Kind::PickUsed);
        pagingRow(L"Most used", lists.mostUsed.size(), usedPage, NewThreadAction::Kind::PageUsed, NewThreadAction::Kind::PageLabelUsed);
        rows.push_back({ NewThreadButton{ L"\x2716 Cancel", NewThreadCallbackData(NewThreadAction::Kind::Cancel, 0) } });
        return rows;
    }

    // ===== the user's own words ======================================================================================

    /// <summary>
    /// A FOLDER the user typed instead of tapping one (§21.17): a message that is one line naming an absolute folder ("K:\src\x",
    /// "K:/src/x", "\\server\share\x", quoted or not — IsAbsoluteFolderPath after TrimFolderPath). Anything else is a prompt.
    /// Whether the folder exists is the service's check.
    /// </summary>
    /// <param name="text">The message's text.</param>
    /// <returns>The folder's display spelling; nullopt when the text is not a lone folder path.</returns>
    /// <exception cref="std::bad_alloc">If the spelling cannot be allocated.</exception>
    inline std::optional<std::wstring> NewThreadTypedFolder(std::wstring_view text)
    {
        std::wstring folder = TrimFolderPath(text);
        if (folder.empty() || folder.find(L'\n') != std::wstring::npos || folder.find(L'\r') != std::wstring::npos || !IsAbsoluteFolderPath(folder))
        {
            return std::nullopt;
        }
        return folder;
    }

    /// <summary>
    /// What a pick says when the folder's check did not answer within kNewThreadPickProbeWaitMs (§21.17.4, FolderProbe.h) — an
    /// offline network share, a disk spinning up. The folder comes LAST: a picker button's toast is cut at 190 characters, so a
    /// long path must not push the reason out of it.
    /// </summary>
    /// <param name="folderShown">The folder as the reply shows it: plain for a toast or an escaped reply, or already marked up
    /// (`&lt;code&gt;…&lt;/code&gt;`, escaped) for an HTML reply. The rest of the text holds no HTML special character, so it is
    /// safe either way.</param>
    /// <returns>The sentence, naming the wait in whole seconds.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NewThreadFolderNoAnswerText(std::wstring_view folderShown)
    {
        return L"The folder did not answer within " + std::to_wstring(kNewThreadPickProbeWaitMs / 1000) +
               L" s (an offline network share?) \x2014 try again once it is reachable: " + std::wstring{ folderShown };
    }

    /// <summary>
    /// A topic NAME without the decoration the service puts before the title (§21.11, §21.13): one leading state glyph
    /// (StateGlyph's six) and one leading color square (TgTitleColorSquare's nine), each with the space after it — so a thread the
    /// user renamed to "🟡 🟦 my title" (the decoration kept, the title changed) or to "my title" reads "my title" either way.
    /// Surrounding spaces are trimmed.
    /// </summary>
    /// <param name="name">The name from forum_topic_created / forum_topic_edited.</param>
    /// <returns>The bare title ("" when nothing else is left).</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring StripTopicNameDecoration(std::wstring_view name)
    {
        const auto trim = [](std::wstring_view v) {
            while (!v.empty() && (v.front() == L' ' || v.front() == L'\t'))
            {
                v.remove_prefix(1);
            }
            while (!v.empty() && (v.back() == L' ' || v.back() == L'\t'))
            {
                v.remove_suffix(1);
            }
            return v;
        };
        name = trim(name);
        static constexpr SessionState kStates[] = { SessionState::Running, SessionState::WaitingForInput, SessionState::NeedsApproval,
                                                    SessionState::Error, SessionState::Done, SessionState::Idle };
        for (const SessionState st : kStates)
        {
            const std::wstring_view glyph{ StateGlyph(st) };
            if (name.substr(0, glyph.size()) == glyph)
            {
                name = trim(name.substr(glyph.size()));
                break;
            }
        }
        static constexpr std::wstring_view kSquares[] = { L"\U0001F7E5", L"\U0001F7E7", L"\U0001F7E8", L"\U0001F7E9", L"\U0001F7E6",
                                                          L"\U0001F7EA", L"\U0001F7EB", L"\x2B1B", L"\x2B1C" };
        for (const auto square : kSquares)
        {
            if (name.substr(0, square.size()) == square)
            {
                name = trim(name.substr(square.size()));
                break;
            }
        }
        return std::wstring{ name };
    }

    // ===== the held messages =========================================================================================

    /// <summary>
    /// Whether Telegram's answer to a reaction on a HELD message (§21.17.4) says the message is GONE — the user deleted it
    /// before the new session took it. Telegram sends a bot no update when a user deletes a message, so the hand-over asks with
    /// the reaction call that takes the message's 👀 off anyway (the hold's "seen" mark): a message the Bot API no longer has
    /// is answered 400 "message to react not found" (the server's "&lt;what&gt; not found" wording; "message to edit not found"
    /// is its edit twin), or MESSAGE_ID_INVALID. Every other answer is NOT proof — a success, a flood refusal, a lost connection,
    /// a missing topic or chat, a refused reaction — and the message is handed over: a lost prompt costs more than a stray one.
    /// </summary>
    /// <param name="ok">Telegram's `ok`.</param>
    /// <param name="httpStatus">The HTTP status (0 when no response came back).</param>
    /// <param name="description">Telegram's `description`.</param>
    /// <returns>True only for an answer that says the message itself does not exist.</returns>
    inline bool HeldMessageGone(bool ok, int httpStatus, std::wstring_view description) noexcept
    {
        if (ok || httpStatus != 400)
        {
            return false;
        }
        if (description.find(L"MESSAGE_ID_INVALID") != std::wstring_view::npos)
        {
            return true;
        }
        // "message … not found", but not the TOPIC ("message thread not found") nor the chat ("chat not found"): those say
        // nothing about one message, and the topic's own handling reads them.
        return description.find(L"message") != std::wstring_view::npos && description.find(L"not found") != std::wstring_view::npos &&
               description.find(L"thread") == std::wstring_view::npos && description.find(L"chat not found") == std::wstring_view::npos;
    }
}
