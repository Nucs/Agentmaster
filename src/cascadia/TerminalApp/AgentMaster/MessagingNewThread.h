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
//   2. A tap on a folder (or a folder path the user sends) leads through the WIZARD's further steps (§21.25,
//      MessagingNewThreadWizard.h): a typed folder that does not exist is offered for creation (checked case-insensitively), then
//      an optional model (the right-click menu's list, or a typed id), then a window by number (the default: the window of the
//      newest tab working in that folder). The launch deletes the picker and opens a new Claude tab, with a session id the
//      service chose, in the background of the chosen window, right after that newest tab.
//   3. When the session is live, the service ADOPTS the user's thread as the session's topic (no new topic is made), renames it like
//      every topic (state glyph, color square, title), and hands it the held messages as if they had been sent in the topic: the
//      first becomes the session's first prompt. A held message the user deleted meanwhile is left out (HeldMessageGone), and
//      each one's 👀 "seen" mark comes off as it is handed over.
//   4. The title: the thread's name when the user named it (forum_topic_created's is_name_implicit false), else the folder-derived
//      default; renaming the thread later renames the session (forum_topic_edited), like /rename — and since §21.25 a rename
//      during the wizard or while the launch runs is the title too.
//   5. §21.25: ABORTING the creation deletes the thread (the user's rule) — ✖ Cancel at any step, or a wizard nobody touched for
//      kNewThreadPickerIdleMs. A refused launch is not an abort: the wizard goes back to the step that can fix it. The wizards
//      and launches are persisted in state.json, so a restart resumes them where they stood.
//
// This header is the PURE half: the folder lists from the registry's facts, the picker's text and buttons, the buttons' callback
// data, the name a button shows, a topic name without its decoration, and a typed folder path. Header-only, no I/O (the folder
// probe is injected), no Win32, no WinRT: the m5 suite pins every rule (tests_messaging.cpp, TestMessagingNewThread).
// §21.25.9: the ➕ message's buttons ("nt:a…", MessagingNewThreadWizard.h's extra folders) are this header's kinds too, and its add
// picker draws the folder step's two lists through the same builders (detail::AppendNewThreadFolderSections, BuildNewThreadFolderRows).
// §21.48 (2026-11-07): a new session from ANYWHERE — the opener follow (NewThreadOpenerFollow: Telegram for Android 13.0.x sends the
// message that opened a thread from "All" without the thread's id, so it is taken into the thread it opened), the name of a thread
// the service opens for a main-view message (NewThreadNameForMessage), /newsession's arguments (ParseNewSessionArgs), and the
// commands the chat's main view runs itself (kMainViewFleetCommands).

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
#include <utility> // std::pair (ParseNewThreadCallbackData's table of bare tags)
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
    // A picked launch whose session has not come up live after this long is given up: the thread is told and goes back to its
    // wizard's window step (§21.25), with the messages it held. A background tab registers in about a second; three minutes
    // covers a window that was busy re-homing a large workspace. While windows are still restoring their tabs at a start, a
    // launch restored from state.json is not given up (its session may only be waiting to be re-homed).
    inline constexpr int64_t kNewThreadBindTimeoutMs = 3 * 60 * 1000;
    // §21.25: a wizard nobody used for this long is ABORTED — its thread is deleted (the user's rule: an aborted creation deletes
    // its new topic), and a silent line in the chat's main view keeps what the user had written there. The wizards are persisted
    // with the state file, so the clock counts across restarts. Before §21.25 an idle picker was only forgotten (in memory).
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

    /// <summary>What a picker button asks for (§21.17; the wizard's steps, §21.25). The data names the list and a GLOBAL index (or a
    /// page), never a path or a model id: a path can pass the Bot API's 64-byte limit for callback data, and the lists live in the
    /// service's picker state (persisted with it, so a button of a picker from before a restart still maps to what it showed).</summary>
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
            Cancel, // §21.25: the creation is aborted at any step — the THREAD is deleted with everything in it (the user's rule)
            // §21.25, the wizard's steps after the folder (MessagingNewThreadWizard.h):
            CreateFolder, // the CreateFolder step: make the typed folder that does not exist, and go on in it
            BackToFolders, // back to the folder lists (from the CreateFolder or the model step)
            ModelDefault, // the model step: Default (the settings model, else Claude Code's own) — skipping the step
            ModelPick, // value = the model step's 0-based choice (NewThreadWizard::modelChoices, as the step showed them)
            ModelSpecify, // the model step: ✏ Specify… — the next message is a typed model id
            BackToModels, // back to the model step (from the model-id prompt or the window step)
            WindowPick, // value = the window step's 0-based window (NewThreadWizard::windows, as the step showed them)
            WindowDefault, // the window step: its default (the window of the newest tab in the chosen folder)
            WindowRefresh, // the window step: read the windows again
            // §21.25.9, the ➕ MESSAGE above the model step (MessagingNewThreadWizard.h, "the extra folders"): more folders for the
            // session, each sent to Claude Code as "/add-dir <path>" when the session starts, before its first prompt. Its buttons
            // carry the same "nt:" prefix, so the one picker route (_handleCallback → _onNewThreadAction) reads them; the reducer
            // then checks them against the ➕ message's own state (NewThreadAddStepTakes), not only against the wizard's step.
            AddFolder, // ➕: the message turns into a folder picker (the add flow's Pick state)
            AddPickRecent, // value = the folder's 0-based index in the Recent list (the add picker shows the folder step's lists)
            AddPickUsed, // value = the folder's 0-based index in the Most used list
            AddPageRecent, // value = the add picker's Recent page to show
            AddPageUsed, // value = the add picker's Most used page to show
            AddPageLabelRecent, // the add picker's Recent page label: nothing changes, the toast says the page; value = the page it shows
            AddPageLabelUsed, // the add picker's Most used page label (as AddPageLabelRecent)
            AddScope, // value = the /add-dir dialog's option for the picked folder: 1 for this session, 2 remember it (asks first), 3 No
            AddConfirm, // the "remember it?" confirmation's Yes: the folder is added with option 2
            AddBack, // one state back in the add flow: the folder picker to the ➕, the confirmation to the three options
            AddRemove, // value = the 0-based index of an added folder (NewThreadWizard::extraFolders) to take off the list again
        };
        Kind kind{ Kind::None };
        size_t value{ 0 };
    };

    /// <summary>
    /// The callback data of a picker button: "nt:r:3", "nt:m:0", "nt:rp:1", "nt:mp:2", "nt:rl:1", "nt:ml:0", "nt:x"; the wizard's
    /// (§21.25) "nt:cf", "nt:bf", "nt:md", "nt:mo:4", "nt:ms", "nt:bm", "nt:w:1", "nt:wd", "nt:wr"; the ➕ message's (§21.25.9)
    /// "nt:af", "nt:ar:3", "nt:au:0", "nt:arp:1", "nt:aup:2", "nt:arl:1", "nt:aul:0", "nt:as:2", "nt:ac", "nt:ab", "nt:ax:1".
    /// </summary>
    /// <param name="kind">The action (None gives "").</param>
    /// <param name="value">The index, page or option (ignored by the kinds that carry none: Cancel, CreateFolder, BackToFolders,
    /// ModelDefault, ModelSpecify, BackToModels, WindowDefault, WindowRefresh, AddFolder, AddConfirm, AddBack).</param>
    /// <returns>The data, at most 27 characters ("nt:", a tag of up to four characters, a 64-bit value's 20 digits): far under the
    /// Bot API's 64 bytes. Only a value of at most 9 digits parses back (ParseNewThreadCallbackData), which every real index, page
    /// and option is.</returns>
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
        case NewThreadAction::Kind::CreateFolder:
            return prefix + L"cf";
        case NewThreadAction::Kind::BackToFolders:
            return prefix + L"bf";
        case NewThreadAction::Kind::ModelDefault:
            return prefix + L"md";
        case NewThreadAction::Kind::ModelPick:
            return prefix + L"mo:" + std::to_wstring(value);
        case NewThreadAction::Kind::ModelSpecify:
            return prefix + L"ms";
        case NewThreadAction::Kind::BackToModels:
            return prefix + L"bm";
        case NewThreadAction::Kind::WindowPick:
            return prefix + L"w:" + std::to_wstring(value);
        case NewThreadAction::Kind::WindowDefault:
            return prefix + L"wd";
        case NewThreadAction::Kind::WindowRefresh:
            return prefix + L"wr";
        // §21.25.9: the ➕ message's buttons. Every tag starts with "a" and none equals a tag above, so the parse below never reads
        // one kind as another ("ar" is not "r", "arp" is not "rp").
        case NewThreadAction::Kind::AddFolder:
            return prefix + L"af";
        case NewThreadAction::Kind::AddPickRecent:
            return prefix + L"ar:" + std::to_wstring(value);
        case NewThreadAction::Kind::AddPickUsed:
            return prefix + L"au:" + std::to_wstring(value);
        case NewThreadAction::Kind::AddPageRecent:
            return prefix + L"arp:" + std::to_wstring(value);
        case NewThreadAction::Kind::AddPageUsed:
            return prefix + L"aup:" + std::to_wstring(value);
        case NewThreadAction::Kind::AddPageLabelRecent:
            return prefix + L"arl:" + std::to_wstring(value);
        case NewThreadAction::Kind::AddPageLabelUsed:
            return prefix + L"aul:" + std::to_wstring(value);
        case NewThreadAction::Kind::AddScope:
            return prefix + L"as:" + std::to_wstring(value);
        case NewThreadAction::Kind::AddConfirm:
            return prefix + L"ac";
        case NewThreadAction::Kind::AddBack:
            return prefix + L"ab";
        case NewThreadAction::Kind::AddRemove:
            return prefix + L"ax:" + std::to_wstring(value);
        case NewThreadAction::Kind::None:
        default:
            return {};
        }
    }

    /// <summary>Reads a picker button's callback data back (NewThreadCallbackData's inverse).</summary>
    /// <param name="data">The callback data.</param>
    /// <returns>The action; Kind::None for data that is not a well-formed picker button's (a number with anything but digits, or
    /// more than 9 of them, is malformed; so is a value on a kind that carries none, or a valued kind without its value).</returns>
    inline NewThreadAction ParseNewThreadCallbackData(std::wstring_view data) noexcept
    {
        NewThreadAction a;
        if (data.size() <= kNewThreadCallbackPrefix.size() || data.substr(0, kNewThreadCallbackPrefix.size()) != kNewThreadCallbackPrefix)
        {
            return a;
        }
        data.remove_prefix(kNewThreadCallbackPrefix.size());
        // The kinds that carry no value: the whole rest is their tag ("x", "cf", …). A tag followed by ":<digits>" is malformed
        // for them — it falls through to the valued tags below, none of which matches, and reads as None.
        static constexpr std::pair<std::wstring_view, NewThreadAction::Kind> kBare[] = {
            { L"x", NewThreadAction::Kind::Cancel },
            { L"cf", NewThreadAction::Kind::CreateFolder },
            { L"bf", NewThreadAction::Kind::BackToFolders },
            { L"md", NewThreadAction::Kind::ModelDefault },
            { L"ms", NewThreadAction::Kind::ModelSpecify },
            { L"bm", NewThreadAction::Kind::BackToModels },
            { L"wd", NewThreadAction::Kind::WindowDefault },
            { L"wr", NewThreadAction::Kind::WindowRefresh },
            { L"af", NewThreadAction::Kind::AddFolder }, // §21.25.9: the ➕ message's valueless buttons
            { L"ac", NewThreadAction::Kind::AddConfirm },
            { L"ab", NewThreadAction::Kind::AddBack },
        };
        for (const auto& [tag, kind] : kBare)
        {
            if (data == tag)
            {
                a.kind = kind;
                return a;
            }
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
        else if (what == L"mo")
        {
            a.kind = NewThreadAction::Kind::ModelPick; // §21.25
        }
        else if (what == L"w")
        {
            a.kind = NewThreadAction::Kind::WindowPick; // §21.25
        }
        // §21.25.9: the ➕ message's valued buttons. AddScope's option is NOT range-checked here (a digit outside 1-3 parses, and
        // the reducer answers it as a stale tap): the parse only reads the data's shape, the reducer owns what a value may mean.
        else if (what == L"ar")
        {
            a.kind = NewThreadAction::Kind::AddPickRecent;
        }
        else if (what == L"au")
        {
            a.kind = NewThreadAction::Kind::AddPickUsed;
        }
        else if (what == L"arp")
        {
            a.kind = NewThreadAction::Kind::AddPageRecent;
        }
        else if (what == L"aup")
        {
            a.kind = NewThreadAction::Kind::AddPageUsed;
        }
        else if (what == L"arl")
        {
            a.kind = NewThreadAction::Kind::AddPageLabelRecent;
        }
        else if (what == L"aul")
        {
            a.kind = NewThreadAction::Kind::AddPageLabelUsed;
        }
        else if (what == L"as")
        {
            a.kind = NewThreadAction::Kind::AddScope;
        }
        else if (what == L"ax")
        {
            a.kind = NewThreadAction::Kind::AddRemove;
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

    namespace detail
    {
        /// <summary>
        /// Appends the two folder lists' CURRENT PAGES to a picker's text — the folder step's own picker (§21.17,
        /// BuildNewThreadPickerHtml) and the ➕ message's add picker (§21.25.9) show the same lists, numbered like their buttons:
        /// each list's heading with the range shown ("🕘 Recent · 1–10 of 34"), then one line per folder — its number, its full
        /// path and, for Recent, how long ago it was used, for Most used how many sessions worked there — and after it whatever the
        /// caller's `note` says about that folder (the add picker marks the session's own folder and the folders added already, so
        /// a tap that would only be refused is visible before it is made).
        /// </summary>
        /// <param name="html">The text to append to (classic Telegram HTML).</param>
        /// <param name="lists">The two lists.</param>
        /// <param name="recentPage">The Recent page shown (clamped here).</param>
        /// <param name="usedPage">The Most used page shown (clamped here).</param>
        /// <param name="nowMs">The clock the "ago" texts count against (Unix ms).</param>
        /// <param name="note">Called with each shown folder's path; returns HTML to put at the end of its line ("" = nothing). An
        /// empty function notes nothing (the folder step's picker, byte for byte as before §21.25.9).</param>
        /// <exception cref="std::bad_alloc">If the text cannot grow.</exception>
        /// <exception cref="std::exception">Whatever `note` throws, unchanged.</exception>
        inline void AppendNewThreadFolderSections(std::wstring& html, const NewThreadLists& lists, size_t recentPage, size_t usedPage, int64_t nowMs,
                                                  const std::function<std::wstring(const std::wstring&)>& note)
        {
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
                    if (note)
                    {
                        html += note(f.path);
                    }
                }
            };
            section(L"\U0001F558", L"Recent", lists.recent, recentPage, true);
            section(L"\U0001F3C6", L"Most used", lists.mostUsed, usedPage, false);
        }
    }

    /// <summary>
    /// The PICKER's text (§21.17): what it is for, what happens to the user's messages, how the title is chosen, the steps that
    /// follow and what Cancel does (§21.25), then the two lists' current pages — each line the folder's number (the button's), its
    /// full path and, for Recent, how long ago it was used, for Most used how many sessions worked there. Classic Telegram HTML (no
    /// title token: it is sent and edited as is). The wizard's folder step (MessagingNewThreadWizard.h BuildNewThreadView) shows it
    /// under its notice line.
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
        // §21.25: the folder is the wizard's FIRST step — say what follows, and that the creation's abort takes the thread with it
        // (the user's rule: an aborted creation deletes its new topic), so a Cancel never surprises.
        html += L"\n<i>Then a model (optional) and a window. \x2716 Cancel deletes this thread.</i>";
        detail::AppendNewThreadFolderSections(html, lists, recentPage, usedPage, nowMs, nullptr);
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
    /// The button kinds of ONE folder picker's two lists (§21.25.9): the folder step's own picker and the ➕ message's add picker
    /// show the same lists with the same layout, and differ only in what their buttons ask for — a tap on the folder step's
    /// "🕘 1" chooses the session's folder, the same tap on the add picker asks about an EXTRA folder.
    /// </summary>
    struct NewThreadPickerKinds
    {
        NewThreadAction::Kind pickRecent; // a Recent folder's button (its value: the folder's index)
        NewThreadAction::Kind pickUsed; // a Most used folder's button
        NewThreadAction::Kind pageRecent; // the Recent pager's arrows (their value: the page they turn to)
        NewThreadAction::Kind pageUsed; // the Most used pager's arrows
        NewThreadAction::Kind labelRecent; // the Recent pager's page label (a toast only)
        NewThreadAction::Kind labelUsed; // the Most used pager's page label
    };

    /// <summary>The folder step's picker (§21.17): its buttons choose the session's own folder.</summary>
    inline constexpr NewThreadPickerKinds kNewThreadFolderStepKinds{ NewThreadAction::Kind::PickRecent,     NewThreadAction::Kind::PickUsed,
                                                                     NewThreadAction::Kind::PageRecent,     NewThreadAction::Kind::PageUsed,
                                                                     NewThreadAction::Kind::PageLabelRecent, NewThreadAction::Kind::PageLabelUsed };

    /// <summary>The ➕ message's add picker (§21.25.9): its buttons ask about an extra folder for the session.</summary>
    inline constexpr NewThreadPickerKinds kNewThreadAddPickerKinds{ NewThreadAction::Kind::AddPickRecent,     NewThreadAction::Kind::AddPickUsed,
                                                                    NewThreadAction::Kind::AddPageRecent,     NewThreadAction::Kind::AddPageUsed,
                                                                    NewThreadAction::Kind::AddPageLabelRecent, NewThreadAction::Kind::AddPageLabelUsed };

    /// <summary>
    /// The FOLDER ROWS of a picker's keyboard (§21.17; shared with the ➕ message's add picker since §21.25.9): the Recent page's
    /// folders two to a row ("🕘 1 · Agentmaster"), its pager row (BuildNewThreadPagingRow: the page label between the arrows
    /// that lead somewhere), the Most used page's folders ("🏆 1 · Agentmaster") and pager row. The caller adds its own last row
    /// (the folder step's ✖ Cancel, the add picker's ◀ Back).
    /// </summary>
    /// <param name="lists">The picker's lists.</param>
    /// <param name="recentPage">The Recent page shown (clamped here).</param>
    /// <param name="usedPage">The Most used page shown (clamped here).</param>
    /// <param name="kinds">What the buttons ask for (kNewThreadFolderStepKinds or kNewThreadAddPickerKinds).</param>
    /// <returns>The rows, top first (none for two empty lists).</returns>
    /// <exception cref="std::bad_alloc">If the rows cannot be allocated.</exception>
    inline std::vector<std::vector<NewThreadButton>> BuildNewThreadFolderRows(const NewThreadLists& lists, size_t recentPage, size_t usedPage, const NewThreadPickerKinds& kinds)
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
        folderRows(L"\U0001F558", lists.recent, rFirst, rLast, kinds.pickRecent);
        pagingRow(L"Recent", lists.recent.size(), recentPage, kinds.pageRecent, kinds.labelRecent);
        folderRows(L"\U0001F3C6", lists.mostUsed, mFirst, mLast, kinds.pickUsed);
        pagingRow(L"Most used", lists.mostUsed.size(), usedPage, kinds.pageUsed, kinds.labelUsed);
        return rows;
    }

    /// <summary>
    /// The picker's KEYBOARD (§21.17): the folder rows of the folder step (BuildNewThreadFolderRows with kNewThreadFolderStepKinds —
    /// the Recent page's folders two to a row, its pager row, the Most used page's folders and pager row), and ✖ Cancel.
    /// </summary>
    /// <param name="lists">The picker's lists.</param>
    /// <param name="recentPage">The Recent page shown (clamped here).</param>
    /// <param name="usedPage">The Most used page shown (clamped here).</param>
    /// <returns>The rows, top first.</returns>
    /// <exception cref="std::bad_alloc">If the rows cannot be allocated.</exception>
    inline std::vector<std::vector<NewThreadButton>> BuildNewThreadPickerKeyboard(const NewThreadLists& lists, size_t recentPage, size_t usedPage)
    {
        std::vector<std::vector<NewThreadButton>> rows = BuildNewThreadFolderRows(lists, recentPage, usedPage, kNewThreadFolderStepKinds);
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

    // ===== a new session from anywhere (§21.48) ======================================================================

    // The name of a thread the SERVICE opens for a new session (/newsession in the main view or in the 🔎 Search topic), and the
    // temporary name a thread Telegram named after a "/newsession" message gets instead of the command: either way the thread is
    // renamed to the session's title at the bind, like every topic. U+1F195 is the "NEW" button emoji.
    inline constexpr std::wstring_view kNewSessionThreadName = L"\U0001F195 New session";
    // The most UTF-16 units of the first line of a main-view message that names the thread the service opens for it (Telegram's
    // own implicit names come from the message the same way; a topic name may hold 128).
    inline constexpr size_t kNewThreadNameMaxChars = 64;
    // A thread-less message Telegram stamped at most this many seconds after a thread's creation — or after the message the follow
    // adopted before it — is that thread's OPENER (NewThreadOpenerFollow). Both stamps are Telegram's own `date`, whole seconds of
    // the moment it was SENT, so the window holds in a backlog too (a creation and its opener read after a restart).
    inline constexpr int64_t kNewThreadOpenerFollowSec = 5;
    // How many MOVED messages the follow remembers: an edit of one comes without the thread's id as well, and the outbox drops a
    // reply's quote of one inside its thread (the message is not there). Old ones fall out first.
    inline constexpr size_t kNewThreadMovedMemory = 64;
    // The member a moved message's copy carries (a json::Value Bool): the service took this message into a thread it was not sent
    // in — the opener Telegram Android leaves in "All", or a main-view message a thread the service opened took. It travels with
    // the copy into the wizard's held messages, which are persisted, so a restart still knows the message is not in its thread.
    inline constexpr std::wstring_view kNewThreadMovedKey = L"am_moved_to_thread";
    // The Agentmaster commands the chat's MAIN VIEW runs (MessagingService::_runMainViewCommand): the help, and the fleet commands
    // the 🔎 Search topic runs that act on the whole fleet. /newsession and the /search pair have their own handling; every other
    // Agentmaster command belongs in a session's topic.
    inline constexpr std::wstring_view kMainViewFleetCommands[] = {
        L"start", L"help", L"status", L"idle", L"board", L"pauseall", L"resumeall", L"verbose", L"recreate"
    };

    /// <summary>
    /// §21.48: the FOLLOW of a thread the user opens from the chat's "All" view, and the memory of every message the service MOVED
    /// into a thread. Telegram for Android 13.0.x (DrKLO/Telegram#2179, seen in the owner's chat 2026-11-07) creates the thread —
    /// forum_topic_created arrives with the thread's id — and then sends the message that named it WITHOUT message_thread_id: the
    /// message stays in "All" (the General topic), the new thread stays empty ("No messages" in Telegram itself), and a bot reads
    /// it as a message of the main view. Desktop clients stamp it. The follow takes a thread-less message as the new thread's when
    /// Telegram stamped it at most kNewThreadOpenerFollowSec after the creation, or after the message it adopted before (a long
    /// opener the client splits into several messages) — the time rule NousResearch/hermes-agent#141751 settled on after an
    /// id-adjacency rule misrouted (the message ids are one sequence across every private chat of the bot). A message with a
    /// thread id ends the follow, and so does a thread-less one past the window. An EDIT never adopts by time: only a message the
    /// follow adopted (or the service moved, Remember) is routed again — its edit carries no thread id either.
    /// </summary>
    /// <remarks>
    /// Not thread-safe: the service keeps one under its `_mtx`. In memory only: a restart forgets a follow in progress (the
    /// creation and its opener come in one getUpdates answer, so the gap is a stop between two updates of one batch) and refills
    /// the moved messages from the wizards' persisted held messages (their kNewThreadMovedKey). A follow is never started for a
    /// thread the bot itself created: its forum_topic_created comes back with the bot as the sender, which the service skips.
    /// </remarks>
    struct NewThreadOpenerFollow
    {
        int64_t threadId{ 0 }; // the thread a thread-less message is adopted into now (0 = no follow)
        int64_t lastDateSec{ 0 }; // Telegram's stamp of the creation, then of the newest adopted message (Unix seconds)
        std::vector<std::pair<int64_t, int64_t>> moved; // (message id, thread id) of the moved messages, oldest first, at most kNewThreadMovedMemory

        /// <summary>Forgets the follow and every moved message — a new bot (the token changed) owns another chat.</summary>
        void Reset() noexcept
        {
            threadId = 0;
            lastDateSec = 0;
            moved.clear();
        }

        /// <summary>
        /// The owner created a thread (forum_topic_created): thread-less messages stamped within the window are its opener from now
        /// on. A creation replaces a follow still running — the user wrote in "All" again, and Telegram opened another thread.
        /// </summary>
        /// <param name="thread">The new thread's id (0 ends any follow).</param>
        /// <param name="dateSec">The creation's `date` (Unix seconds); 0 when Telegram sent none, which starts no follow: without
        /// a stamp the window cannot be measured, and adopting by guess could move an unrelated main-view message.</param>
        void OnThreadCreated(int64_t thread, int64_t dateSec) noexcept
        {
            if (thread <= 0 || dateSec <= 0)
            {
                threadId = 0;
                lastDateSec = 0;
                return;
            }
            threadId = thread;
            lastDateSec = dateSec;
        }

        /// <summary>
        /// Remembers that `messageId` was moved into `thread` (the follow's own adoptions, and a main-view message the thread the
        /// service opened took), so its edits find the thread and the outbox never quotes it from inside the thread. Remembering a
        /// message again updates its thread and makes it the newest; past kNewThreadMovedMemory the oldest is forgotten.
        /// </summary>
        /// <param name="messageId">The moved message (0 is ignored: nothing can name it).</param>
        /// <param name="thread">The thread it was moved into (0 is ignored).</param>
        /// <exception cref="std::bad_alloc">If the memory cannot grow; the follow's state is unchanged then.</exception>
        void Remember(int64_t messageId, int64_t thread)
        {
            if (messageId == 0 || thread <= 0)
            {
                return;
            }
            const auto same = std::find_if(moved.begin(), moved.end(), [messageId](const auto& m) { return m.first == messageId; });
            if (same != moved.end())
            {
                moved.erase(same); // re-added below as the newest, with the thread it has now
            }
            if (moved.size() >= kNewThreadMovedMemory)
            {
                moved.erase(moved.begin());
            }
            moved.emplace_back(messageId, thread);
        }

        /// <summary>The thread a remembered message was moved into.</summary>
        /// <param name="messageId">The message.</param>
        /// <returns>The thread; 0 when the message was never moved (or has been forgotten).</returns>
        int64_t MovedThreadOf(int64_t messageId) const noexcept
        {
            if (messageId == 0)
            {
                return 0;
            }
            for (auto it = moved.rbegin(); it != moved.rend(); ++it)
            {
                if (it->first == messageId)
                {
                    return it->second;
                }
            }
            return 0;
        }

        /// <summary>
        /// Where a message from the owner belongs, as the follow reads it — and the follow's next state. A message with a thread
        /// id belongs to that thread and ENDS the follow (it was sent from inside a thread, so the opener has been read or never
        /// came thread-less). A thread-less message stamped within the window of the creation (or of the message adopted before
        /// it) is ADOPTED: it belongs to the followed thread, is remembered as moved, and the window runs on from its stamp. A
        /// thread-less message past the window ends the follow and stays in the main view. An edit only ever goes back to the
        /// thread a moved message went to, and touches neither the follow nor the memory.
        /// </summary>
        /// <param name="thread">The message's message_thread_id (0 = none: the main view, or a thread-less opener).</param>
        /// <param name="dateSec">The message's `date` (Unix seconds; for an edit, of the original message).</param>
        /// <param name="messageId">The message's id.</param>
        /// <param name="edited">True for an edited_message update.</param>
        /// <returns>The thread the message belongs to: `thread` itself for a stamped message; the followed thread for an adopted
        /// one; the moved thread for an edit of a moved message; 0 for a main-view message.</returns>
        /// <exception cref="std::bad_alloc">If an adopted message cannot be remembered; the message is still adopted then (the
        /// follow moved on before the memory grew), and only its later edit or quote is not caught.</exception>
        int64_t Route(int64_t thread, int64_t dateSec, int64_t messageId, bool edited)
        {
            if (edited)
            {
                return thread != 0 ? thread : MovedThreadOf(messageId);
            }
            if (thread != 0)
            {
                threadId = 0;
                lastDateSec = 0;
                return thread;
            }
            if (threadId == 0)
            {
                return 0;
            }
            const int64_t gap = dateSec - lastDateSec;
            if (dateSec <= 0 || gap < 0 || gap > kNewThreadOpenerFollowSec)
            {
                // Too late (or unstamped, or stamped before the creation, which no opener can be): the follow is over, and the
                // message is the main view's.
                threadId = 0;
                lastDateSec = 0;
                return 0;
            }
            const int64_t adopted = threadId;
            lastDateSec = dateSec; // the next chunk of a split opener is measured from this one
            Remember(messageId, adopted);
            return adopted;
        }
    };

    /// <summary>
    /// Whether a Telegram message carries something the user SENT — text, a caption, or media — as opposed to a service message
    /// (a topic created, renamed or pinned). Only such messages take part in the opener follow: a service message in the main view
    /// right after a creation (a pin the user made) must neither be adopted nor end the follow.
    /// </summary>
    /// <param name="msg">The Telegram Message object.</param>
    /// <returns>True for text, a caption, or any of the media kinds a user sends.</returns>
    inline bool NewThreadMessageCarriesContent(const json::Value& msg)
    {
        if (msg.type != json::Value::Type::Obj)
        {
            return false;
        }
        if (!msg.StrAt(L"text").empty() || !msg.StrAt(L"caption").empty())
        {
            return true;
        }
        static constexpr std::wstring_view kMediaKinds[] = { L"voice", L"audio",    L"video_note", L"video",   L"photo", L"document", L"animation",
                                                             L"sticker", L"location", L"venue",      L"contact", L"poll",  L"dice",     L"story" };
        return std::any_of(std::begin(kMediaKinds), std::end(kMediaKinds), [&msg](std::wstring_view kind) { return msg.Find(kind) != nullptr; });
    }

    /// <summary>
    /// The name of the thread the service opens for a message written in the main view (§21.48): the message's first non-empty
    /// line, cut to kNewThreadNameMaxChars UTF-16 units with an ellipsis (never inside a surrogate pair: a lone surrogate is
    /// invalid UTF-16, which Telegram refuses) — the way Telegram itself names a thread opened from "All". A message with no
    /// line (a blank text) gets kNewSessionThreadName.
    /// </summary>
    /// <param name="text">The message's text.</param>
    /// <returns>The thread's name, never empty.</returns>
    /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
    inline std::wstring NewThreadNameForMessage(std::wstring_view text)
    {
        const std::wstring line = TgFirstLine(text, 0);
        if (line.empty())
        {
            return std::wstring{ kNewSessionThreadName };
        }
        if (line.size() <= kNewThreadNameMaxChars)
        {
            return line;
        }
        return std::wstring{ detail::TgPrefixUnits(line, kNewThreadNameMaxChars - 1) } + L"\x2026";
    }

    /// <summary>
    /// The silent line the service posts where a new session was asked for — the main view or the 🔎 Search topic — once the
    /// thread it opened for it exists (§21.48): what the thread is for, its name, and a link that opens it ("open it"; Telegram
    /// Desktop opens the thread, Android lands in "All", §21.16.3) — or, before the bot's username is known, where to find it
    /// (a new thread is first in the thread list).
    /// </summary>
    /// <param name="botUsername">The bot's username (with or without '@'; "" = not known yet: no link).</param>
    /// <param name="threadId">The thread the service opened.</param>
    /// <param name="name">The thread's name as the service gave it.</param>
    /// <param name="heldWords">True: the user's WORDS opened it, and wait there as the session's first prompt; false: /newsession.</param>
    /// <returns>The line, HTML (the name escaped).</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring BuildNewSessionThreadOpenedHtml(std::wstring_view botUsername, int64_t threadId, std::wstring_view name, bool heldWords)
    {
        std::wstring html = heldWords ? L"\U0001F195 Your message opened a new thread, \x201C" + TgEscapeHtml(name) +
                                            L"\x201D: it waits there as the new session's first prompt while the thread asks for the folder, the model and the window" :
                                        L"\U0001F195 New thread \x201C" + TgEscapeHtml(name) + L"\x201D: it asks for the new session's folder, model and window";
        const std::wstring url = TgTopicUrl(botUsername, threadId);
        if (url.empty())
        {
            html += L" (first in the thread list).";
        }
        else
        {
            html += L" \x2014 <a href=\"" + detail::TgEscapeHtmlAttribute(url) + L"\">open it</a>.";
        }
        return html;
    }

    /// <summary>/newsession's arguments as a new thread's wizard takes them (§21.48).</summary>
    struct NewSessionArgs
    {
        std::wstring folder; // the folder to pre-pick ("" = the folder step asks)
        std::wstring model; // the model id to pre-pick after it ("" = the model step asks; the wizard checks the id)
    };

    /// <summary>
    /// Splits /newsession's arguments the way the 🔎 Search topic's /newsession &lt;folder&gt; [model] always has: a "quoted
    /// folder" (it may hold spaces) or the first word is the folder, and whatever follows is the model. Nothing is checked here:
    /// the wizard resolves the folder (a missing one is only offered for creation, never made without the user's tap) and
    /// refuses a model id it cannot pass to claude.
    /// </summary>
    /// <param name="args">The text after "/newsession" (RemoteCommand::args).</param>
    /// <returns>The folder and the model, each trimmed ("" when absent). An unclosed quote takes the rest as the folder.</returns>
    /// <exception cref="std::bad_alloc">If the strings cannot be allocated.</exception>
    inline NewSessionArgs ParseNewSessionArgs(std::wstring_view args)
    {
        const auto trim = [](std::wstring_view v) {
            const size_t a = v.find_first_not_of(L" \t\r\n");
            if (a == std::wstring_view::npos)
            {
                return std::wstring_view{};
            }
            return v.substr(a, v.find_last_not_of(L" \t\r\n") - a + 1);
        };
        NewSessionArgs out;
        const std::wstring_view all = trim(args);
        if (all.empty())
        {
            return out;
        }
        if (all.front() == L'"')
        {
            const size_t close = all.find(L'"', 1);
            if (close == std::wstring_view::npos)
            {
                out.folder = std::wstring{ trim(all.substr(1)) };
                return out;
            }
            out.folder = std::wstring{ trim(all.substr(1, close - 1)) };
            out.model = std::wstring{ trim(all.substr(close + 1)) };
            return out;
        }
        const size_t ws = all.find_first_of(L" \t\r\n");
        out.folder = std::wstring{ all.substr(0, ws) };
        if (ws != std::wstring_view::npos)
        {
            out.model = std::wstring{ trim(all.substr(ws)) };
        }
        return out;
    }

    /// <summary>Whether a parsed command is one the chat's main view runs itself (kMainViewFleetCommands).</summary>
    /// <param name="cmd">The command (ParseRemoteCommand).</param>
    /// <returns>True for an Agentmaster command of that list; false for every other command and for plain text.</returns>
    inline bool IsMainViewFleetCommand(const RemoteCommand& cmd) noexcept
    {
        return cmd.kind == RemoteKind::AgentmasterCommand && detail::InList(cmd.name, std::begin(kMainViewFleetCommands), std::end(kMainViewFleetCommands));
    }
}
