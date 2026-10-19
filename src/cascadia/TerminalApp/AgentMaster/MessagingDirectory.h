// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the DIRECTORY (MESSAGING.md §21.16): ONE pinned message per bot chat, in the chat's main view ("All", the
// General topic of a bot forum), that lists what the chat holds — its DEDICATED topics (🔎 Search today; the list is open for
// more) and every bookmark tag of Agentmaster that an open tab carries, with those tabs — under two rows of fleet counts. The
// user's request (2026-10-17): "a pinned message per bot … a nice list of dedicated topics, out of which we right now have
// Search but will more in future. Also a pin all tags and the tabs in the tag. make sure the message shows titles with coloring
// and when clicked jumps to the topic … Also a two rows of stats"; and, once the first directory was up, "Tags with 0 tabs
// should not be shown" — a tag no open tab carries is left out (§21.16.1). On 2026-10-19 the user asked for one more section, at
// the bottom: "the top 10 tab titles in the order of their recent activity and that are not in stationary status" — the RECENT
// ACTIVITY section (§21.16.8): the ten newest open Claude tabs that are not Idle or Done, in the topic list's MOST RECENT order.
//
// This header is the PURE half: the model the service fills, its two renders — a RICH message whose titles are URL chips in
// the look nearest the tab's color, and the classic HTML the service sends when Telegram refuses the rich one —, the hash that
// decides an edit, and the post / edit decision. Header-only, no I/O, no Win32, no WinRT: the m5 suite pins every rule
// (tests_messaging_directory.cpp); the service (Messaging.cpp, MessagingService::_directoryTick) gathers the facts, sends,
// edits and pins.
//
// The links. A title chip opens https://t.me/<bot>/<topic>?topic=<topic>: the MESSAGE link of the topic's creation message (the
// deep-link spec's form for a topic: "the topic ID is actually the ID of the service message that created the topic") plus
// the `topic` parameter the apps' link code reads. A topic id is the USER-space id the Bot API hands out (MESSAGING.md §5.1),
// so the number is right on the user's side. What a tap does, read from the clients' sources (Desktop 2026-10-15, Android
// 2026-10-13, iOS 2026-10-07) and, for Android, seen by the user (2026-10-17):
//   - Telegram Desktop opens the topic — when it has the bot chat's topic list loaded (showRepliesForMessage needs the topic
//     and its unread count; else it does nothing for a bot).
//   - Telegram iOS opens it through its reply-thread path, which finds the topic in the list the app has loaded
//     (_internal_fetchChannelReplyThreadMessage's local thread info) — read from the code only.
//   - Telegram ANDROID cannot: its link code selects a topic only in a GROUP forum, so for a bot it opens a new chat screen at
//     message <topic> in the "All" view — the topic's first message, which is what the user saw. NO link form changes that
//     (`?topic=`, `?thread=`, tg://resolve, tg://openmessage, the two-number path: all end in the same ChatActivity).
// The DOT. So Android has a way in, each tab's state dot is a callback button (DirectoryFrontCallbackData): a tap makes the bot
// re-post that topic's sticky card, the topic's newest message then. Telegram lists a bot chat's topics by their last message,
// so the topic becomes the FIRST in the thread list on every app, and Android's "All" view puts its own "Continue last thread ›"
// button under the card (ChatMessageCell: the chat's last message, in a topic). The user's choice, 2026-10-17: "Link + dot
// button" (MESSAGING.md §21.16.7).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype> // std::towlower (detail::DirectoryFoldTag)
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "MessagingModels.h" // TabFact, StateGlyph, CardTextHash, detail::TitleLessCi, SessionState
#include "TelegramFormat.h" // TgEscapeHtml, TgTitleColorSquare, TgTitleChipStyle, TgClockText, detail::TgPrefixUnits / TgEscapeHtmlAttribute

namespace Agentmaster::Msg
{
    // ===== the cadence and the limits =================================================================================

    // How often the service rebuilds the directory's model and compares it with what the message shows (a cheap pass: the
    // tag store's cached reads and one lean registry copy).
    inline constexpr int64_t kDirectoryCheckMs = 15 * 1000;
    // The least time between two sends of the directory (its post, its edits). A busy fleet changes the counts every few
    // seconds; one edit per two minutes at most keeps the directory within ~0.5 call a minute of the chat's flood budget, which
    // a burst of edits once turned into a 6542 s hold (MESSAGING.md §21.14.5).
    inline constexpr int64_t kDirectoryEditGapMs = 2 * 60 * 1000;
    // After a send Telegram refused for another reason than flood control, the next try waits this long.
    inline constexpr int64_t kDirectoryRetryMs = 5 * 60 * 1000;
    // A directory message found DELETED (the user deleted it, or an old message id) is posted again only after this long, so a
    // deletion is not answered by a new message at once.
    inline constexpr int64_t kDirectoryRepostGapMs = 30 * 60 * 1000;
    // A send that got no answer for this long is forgotten (an answer lost to an exception in the send loop would otherwise hold
    // the directory for the rest of the run). Every send carries its key, so a send made after this cannot double a queued one.
    inline constexpr int64_t kDirectoryStallMs = 15 * 60 * 1000;
    // The most open tabs one tag lists; the rest are counted ("… and 4 more").
    inline constexpr size_t kDirectoryMaxTabsPerTag = 20;
    // The most tabs the Recent activity section lists (§21.16.8): the user's "top 10". The rest are not counted — the stats rows
    // above already count every state.
    inline constexpr size_t kDirectoryMaxRecentTabs = 10;
    // The most tag sections the message holds: each is one rich block, and Telegram takes at most 500 blocks per message.
    inline constexpr size_t kDirectoryMaxTagSections = 100;
    // The renders' size limits: the rich one is kept under Telegram's rich limit (kRichMessageMaxChars, counted on the HTML as
    // the rest of the service does); the classic fallback must stay ONE message (TgChunkHtml splits past 4096), so it stops
    // short of that with room for the "more tags" line and the footer.
    inline constexpr size_t kDirectoryRichMaxChars = kRichMessageMaxChars;
    inline constexpr size_t kDirectoryClassicMaxChars = 3800;
    // The room a render keeps free for its closing lines (the "… and N more tags" line and the footer) while it adds sections.
    inline constexpr size_t kDirectoryTailReserveChars = 400;

    // ===== the model ===============================================================================================

    /// <summary>
    /// One DEDICATED topic of the chat (§21.16's first section): a topic the service keeps for a purpose rather than for a tab —
    /// 🔎 Search today. The service lists them in a fixed order; a new one only adds an entry.
    /// </summary>
    struct DirectoryTopic
    {
        std::wstring label; // what its chip shows as is ("🔎 Search": its emoji, a space, its name)
        int64_t threadId{ 0 }; // its topic (0 = not made yet: shown as a chip that does nothing)
        std::wstring colorHex; // its icon color ("#6FB9F0"): the chip's look (TgTitleChipStyle of its square)
    };

    /// <summary>One open tab as the directory lists it under a tag.</summary>
    struct DirectoryTab
    {
        std::wstring sessionId;
        std::wstring title; // the tab title ("" = the first 8 characters of the session id)
        SessionState state{ SessionState::Idle }; // the glyph before its chip (StateGlyph, like the topic names)
        std::wstring colorHex; // the tab color (TabFact::colorHex; "" = none: no square, the plain look)
        int64_t threadId{ 0 }; // its topic (0 = none — an idle tab, a Codex tab: a chip that does nothing)
        // The session's MOST RECENT key (TabFact::mostRecentMs = MostRecentSortKeyMs: its newest turn boundary, the key the topic
        // list and the side panel order by). Only the Recent activity section reads it (§21.16.8); 0 = unknown (it ranks last).
        int64_t recentMs{ 0 };
    };

    /// <summary>One bookmark tag and the open tabs that carry it.</summary>
    struct DirectoryTag
    {
        std::wstring name; // the display casing (CollectGlobalTags')
        std::wstring colorHex; // the tag's display color (ResolveTagDisplayHex: the picked one, else the palette's)
        std::vector<DirectoryTab> tabs; // the OPEN tabs carrying it, any order (the render sorts them by title)
    };

    /// <summary>
    /// The two rows of fleet counts (§21.16). Row 1 is the user's own example ("5 running 15 waiting 300 idle"); row 2 is the
    /// rest of the states a tab can be in, the prompts the runner still has to send, and the global pause when it is on. Counted
    /// over the Claude tabs, like the 🔎 Search topic's /board (Codex tabs have no topic, D23).
    /// </summary>
    struct DirectoryStats
    {
        size_t running{ 0 };
        size_t waiting{ 0 }; // Waiting for you
        size_t idle{ 0 };
        size_t needsApproval{ 0 };
        size_t error{ 0 };
        size_t done{ 0 }; // claude exited in the tab
        size_t queued{ 0 }; // Pending prompts of those tabs (the Prompt Queue Runner's backlog)
        bool runnerPaused{ false }; // the Manager's global "Pause Prompt Queue Runner" is on
    };

    /// <summary>Everything the directory shows.</summary>
    struct DirectoryModel
    {
        std::wstring botUsername; // the bot's @username WITHOUT the '@' (the links' host part); "" = links unknown
        DirectoryStats stats;
        std::vector<DirectoryTopic> topics;
        std::vector<DirectoryTag> tags; // any order (the render sorts them by name)
        /// <summary>
        /// The CANDIDATES of the Recent activity section (§21.16.8): every open Claude tab — the population of the stats rows — in
        /// any order. The render keeps the ones whose state is not stationary (DirectoryStateIsStationary), newest first, at most
        /// kDirectoryMaxRecentTabs (PickDirectoryRecentTabs). Empty = the section says that no tab is working or waiting.
        /// </summary>
        std::vector<DirectoryTab> recent;
    };

    /// <summary>The directory as the service sends it.</summary>
    struct DirectoryRender
    {
        std::wstring rich; // rich_message.html
        std::wstring classic; // Telegram HTML for sendMessage / editMessageText, at most kDirectoryClassicMaxChars + the tail
        /// <summary>
        /// CardTextHash of the rich render WITHOUT its "updated" footer: the content, apart from the clock. The service edits the
        /// message only when this moves (or the message's form is not the wanted one), so a quiet fleet costs no call.
        /// </summary>
        uint32_t hash{ 0 };
        size_t tagsShown{ 0 }; // tag sections in the rich render
        size_t tagsHidden{ 0 }; // tags left out of it (the message was full)
        /// <summary>
        /// Tags left out of BOTH renders because no open tab carries them (only closed sessions do, or none: a tag the registry
        /// keeps for re-use, Rule #17) — the user's "Tags with 0 tabs should not be shown". Counted for the service's log only.
        /// </summary>
        size_t tagsWithoutTabs{ 0 };
        size_t tabsShown{ 0 }; // tab chips under the tags in the rich render (the Recent activity rows are counted in recentShown)
        /// <summary>
        /// The rows of the Recent activity section (§21.16.8): the tabs PickDirectoryRecentTabs kept, at most
        /// kDirectoryMaxRecentTabs. Both renders list the same rows: the section is built before the tag sections, which fill the
        /// message only up to the room it leaves, so a full message never drops it.
        /// </summary>
        size_t recentShown{ 0 };
        /// <summary>
        /// The tabs of the rich render whose state dot is a button (DirectoryFrontCallbackData): the listed tabs that have a
        /// topic — under the tags and in the Recent activity section alike (a tab listed in both places has a button in each). A
        /// tab with no topic keeps a plain dot — there is no card to re-post. The classic render has no dot buttons (Telegram HTML
        /// cannot hold an in-text callback), so this counts the rich render only.
        /// </summary>
        size_t dotsShown{ 0 };
    };

    // ===== the pieces ================================================================================================

    /// <summary>
    /// The link of a topic of the bot's private chat: https://t.me/&lt;bot&gt;/&lt;topic&gt;?topic=&lt;topic&gt; (see the header
    /// note: the topic's message link, plus the parameter Telegram Desktop reads). Since §21.17 the one builder is TgTopicUrl
    /// (TelegramFormat.h), which every title link of the bot's messages uses too, so a directory chip and a title chip open the
    /// same place.
    /// </summary>
    /// <param name="botUsername">The bot's username, with or without its leading '@'.</param>
    /// <param name="threadId">The topic's message_thread_id (the Bot API's, which is the user-space id).</param>
    /// <returns>The link; "" when either part is missing (no link can be made).</returns>
    /// <exception cref="std::bad_alloc">If the link cannot be allocated.</exception>
    inline std::wstring DirectoryTopicUrl(std::wstring_view botUsername, int64_t threadId)
    {
        return TgTopicUrl(botUsername, threadId);
    }

    /// <summary>
    /// The prefix of a tab DOT's callback data (§21.16.7): "d:front:" and then the topic's thread id. The service's
    /// _handleCallback reads it back with ParseDirectoryFrontCallbackData. It names the THREAD, not the session: a topic follows
    /// its tab to the next conversation (a /clear or a /resume in the tab rekeys it to another session id) while its thread id
    /// stays, so a dot the directory drew before a rekey still finds its topic — the message is edited at most every two minutes.
    /// </summary>
    inline constexpr std::wstring_view kDirectoryFrontCallbackPrefix = L"d:front:";

    /// <summary>
    /// The callback data of a tab's dot button: "d:front:&lt;thread id&gt;" — at most 27 characters, well under the Bot API's
    /// limit of 64 bytes for callback data.
    /// </summary>
    /// <param name="threadId">The topic's message_thread_id.</param>
    /// <returns>The data; "" for an id that is not positive (no topic: the dot stays a plain glyph).</returns>
    /// <exception cref="std::bad_alloc">If the data cannot be allocated.</exception>
    inline std::wstring DirectoryFrontCallbackData(int64_t threadId)
    {
        if (threadId <= 0)
        {
            return {};
        }
        return std::wstring{ kDirectoryFrontCallbackPrefix } + std::to_wstring(threadId);
    }

    /// <summary>
    /// Reads a dot button's callback data back (DirectoryFrontCallbackData). Any other data — another button's, a malformed or a
    /// forged one — reads as 0: only decimal digits may follow the prefix, at most 18 of them, so the value always fits an
    /// int64 and is never negative; a 0 id (or a prefix with nothing after it) is no topic either.
    /// </summary>
    /// <param name="data">The callback query's data.</param>
    /// <returns>The thread id (&gt; 0), or 0 when the data is not a dot's.</returns>
    inline int64_t ParseDirectoryFrontCallbackData(std::wstring_view data) noexcept
    {
        if (data.size() <= kDirectoryFrontCallbackPrefix.size() || data.substr(0, kDirectoryFrontCallbackPrefix.size()) != kDirectoryFrontCallbackPrefix)
        {
            return 0;
        }
        const std::wstring_view digits = data.substr(kDirectoryFrontCallbackPrefix.size());
        if (digits.size() > 18)
        {
            return 0; // 19+ digits could overflow int64: not data this service ever wrote
        }
        int64_t value = 0;
        for (const wchar_t c : digits)
        {
            if (c < L'0' || c > L'9')
            {
                return 0;
            }
            value = value * 10 + (c - L'0');
        }
        return value;
    }

    /// <summary>
    /// Counts the two stats rows over the sync's facts: every LIVE session in a tab that is a Claude session (the /board
    /// population), by its state.
    /// </summary>
    /// <param name="facts">The last topic sync's facts (MessagingService::_lastFacts).</param>
    /// <param name="queued">The Pending prompts of those sessions (the service counts them from the registry).</param>
    /// <param name="runnerPaused">The global pause (Scheduler::GlobalPaused).</param>
    /// <returns>The counts.</returns>
    inline DirectoryStats CountDirectoryStats(const std::vector<TabFact>& facts, size_t queued, bool runnerPaused) noexcept
    {
        DirectoryStats s;
        s.queued = queued;
        s.runnerPaused = runnerPaused;
        for (const auto& f : facts)
        {
            if (!(f.live && f.inTab && f.claude))
            {
                continue;
            }
            switch (f.state)
            {
            case SessionState::Running:
                ++s.running;
                break;
            case SessionState::WaitingForInput:
                ++s.waiting;
                break;
            case SessionState::NeedsApproval:
                ++s.needsApproval;
                break;
            case SessionState::Error:
                ++s.error;
                break;
            case SessionState::Done:
                ++s.done;
                break;
            case SessionState::Idle:
            default:
                ++s.idle;
                break;
            }
        }
        return s;
    }

    /// <summary>
    /// Whether a tab's state is STATIONARY — the user's word (2026-10-19) for the tabs the Recent activity section leaves out
    /// (§21.16.8): nothing happens in the tab and nothing is asked of the user. Idle (no turn in flight, nothing waiting) and Done
    /// (claude ended in the tab) are; Running (a turn in flight), Waiting for you (its turn is over and it waits for the next
    /// message), Needs approval and Error (each waits for the user's answer) are not. ⚠ A state added to SessionState is NOT
    /// stationary until it is named here, so a new state shows in the section rather than vanishing from it.
    /// </summary>
    /// <param name="state">The tab's state.</param>
    /// <returns>True for Idle and Done.</returns>
    inline constexpr bool DirectoryStateIsStationary(SessionState state) noexcept
    {
        return state == SessionState::Idle || state == SessionState::Done;
    }

    /// <summary>
    /// The tabs of the Recent activity section (§21.16.8, the user's "the top 10 tab titles in the order of their recent activity
    /// and that are not in stationary status"): the candidates whose state is not stationary (DirectoryStateIsStationary), in the
    /// TOPIC LIST's order — TopicRanksAbove over DirectoryTab::recentMs, the side panel's MOST RECENT key, so the section lists
    /// the tabs in the order their threads stand in Telegram's thread list (§21.8) —, at most `max`. The key is a turn boundary
    /// (a prompt sent, a turn finished), never a tool call, so the order moves only when a turn starts or ends: the moments the
    /// state glyph and the stats rows change anyway, so the section costs no directory edit of its own. Two tabs the order cannot
    /// tell apart (an equal key and the same title, case-insensitively) go by session id, so the pick never depends on the order
    /// of the candidates.
    /// </summary>
    /// <param name="candidates">Every open Claude tab (DirectoryModel::recent), any order. A tab with no title is ranked by its
    /// session id.</param>
    /// <param name="max">The most tabs to keep (kDirectoryMaxRecentTabs; 0 keeps none).</param>
    /// <returns>Copies of the kept tabs, the newest first.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::vector<DirectoryTab> PickDirectoryRecentTabs(const std::vector<DirectoryTab>& candidates, size_t max)
    {
        std::vector<const DirectoryTab*> moving;
        moving.reserve(candidates.size());
        for (const auto& t : candidates)
        {
            if (!DirectoryStateIsStationary(t.state))
            {
                moving.push_back(&t);
            }
        }
        // Pointers are sorted, and only the kept ones copied: a fleet of hundreds of waiting tabs costs pointer moves only.
        std::stable_sort(moving.begin(), moving.end(), [](const DirectoryTab* a, const DirectoryTab* b) {
            const std::wstring& ta = a->title.empty() ? a->sessionId : a->title;
            const std::wstring& tb = b->title.empty() ? b->sessionId : b->title;
            if (TopicRanksAbove(a->recentMs, ta, b->recentMs, tb))
            {
                return true;
            }
            if (TopicRanksAbove(b->recentMs, tb, a->recentMs, ta))
            {
                return false;
            }
            return a->sessionId < b->sessionId;
        });
        const size_t kept = (std::min)(moving.size(), max); // (std::min): the <windows.h> macro
        std::vector<DirectoryTab> out;
        out.reserve(kept);
        for (size_t i = 0; i < kept; ++i)
        {
            out.push_back(*moving[i]);
        }
        return out;
    }

    namespace detail
    {
        /// <summary>A tag's case-insensitive identity: each UTF-16 unit lower-cased with towlower — FoldTagName's rule
        /// (SessionStore.cpp), which this pure header cannot call; the m5 suite pins the two to each other.</summary>
        /// <param name="name">A tag name.</param>
        /// <returns>The folded name.</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        inline std::wstring DirectoryFoldTag(std::wstring_view name)
        {
            std::wstring out;
            out.reserve(name.size());
            for (const wchar_t c : name)
            {
                out.push_back(static_cast<wchar_t>(std::towlower(c)));
            }
            return out;
        }

        /// <summary>
        /// A label for a chip: one line, trimmed, cut at kTgTitleChipLabelMaxChars with an ellipsis (never between the halves of
        /// a surrogate pair). The line breaks follow the title rule of every other place the bot prints a title (TgOneLineTitle,
        /// 2026-10-18: a run of breaks and the blanks around it become ONE space — a CR LF pair used to become two), and a tab
        /// becomes a space.
        /// </summary>
        /// <param name="text">The raw text (a tab title, a tag name, a dedicated topic's label).</param>
        /// <returns>The label, raw (the caller escapes it); "" for a blank text.</returns>
        /// <exception cref="std::bad_alloc">If the label cannot be allocated.</exception>
        inline std::wstring DirectoryLabel(std::wstring_view text)
        {
            // The breaks first, by the title rule; the loop below then meets only tabs (its CR / LF arms stay as a belt).
            std::wstring line = TgOneLineTitle(text);
            for (auto& c : line)
            {
                if (c == L'\r' || c == L'\n' || c == L'\t')
                {
                    c = L' ';
                }
            }
            const size_t a = line.find_first_not_of(L' ');
            if (a == std::wstring::npos)
            {
                return {};
            }
            line = line.substr(a, line.find_last_not_of(L' ') - a + 1);
            if (line.size() > kTgTitleChipLabelMaxChars)
            {
                line = std::wstring{ TgPrefixUnits(line, kTgTitleChipLabelMaxChars - 1) } + L"\x2026";
            }
            return line;
        }

        /// <summary>A tab's label: its color square, a space and its title (the session id's first 8 characters when it has none).</summary>
        /// <param name="t">The tab.</param>
        /// <returns>The raw label.</returns>
        /// <exception cref="std::bad_alloc">If the label cannot be allocated.</exception>
        inline std::wstring DirectoryTabLabel(const DirectoryTab& t)
        {
            std::wstring title = DirectoryLabel(t.title);
            if (title.empty())
            {
                title = t.sessionId.substr(0, 8);
            }
            const std::wstring square = TgTitleColorSquare(t.colorHex);
            return square.empty() ? title : square + L" " + title;
        }

        /// <summary>
        /// An in-text chip of the rich render: a URL button in the look of `colorSquare`'s family (TgTitleChipStyle: 🟦 primary,
        /// 🟩 success, 🟥 danger, else the app's plain button), or — with no link — a disabled button with the same look, which
        /// shows the title but opens nothing (a tab without a topic).
        /// </summary>
        /// <param name="label">The raw label (escaped here).</param>
        /// <param name="colorSquare">The square that picks the look ("" = the plain look).</param>
        /// <param name="url">The link ("" = a disabled chip).</param>
        /// <returns>One &lt;tg-button&gt; element.</returns>
        /// <exception cref="std::bad_alloc">If the element cannot be allocated.</exception>
        inline std::wstring DirectoryChipHtml(std::wstring_view label, std::wstring_view colorSquare, std::wstring_view url)
        {
            std::wstring chip = url.empty() ? std::wstring{ L"<tg-button type=\"disabled\"" } : std::wstring{ L"<tg-button type=\"url\"" };
            if (const wchar_t* style = TgTitleChipStyle(colorSquare); *style)
            {
                chip += L" style=\"";
                chip += style;
                chip += L"\"";
            }
            if (!url.empty())
            {
                chip += L" url=\"" + TgEscapeHtmlAttribute(url) + L"\"";
            }
            chip += L">" + TgEscapeHtml(label) + L"</tg-button>";
            return chip;
        }

        /// <summary>The classic render's form of a chip: a link (or the bare label when there is no link).</summary>
        /// <param name="label">The raw label (escaped here).</param>
        /// <param name="url">The link ("" = the label as text).</param>
        /// <returns>Telegram HTML.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring DirectoryLinkHtml(std::wstring_view label, std::wstring_view url)
        {
            if (url.empty())
            {
                return TgEscapeHtml(label);
            }
            return L"<a href=\"" + TgEscapeHtmlAttribute(url) + L"\">" + TgEscapeHtml(label) + L"</a>";
        }

        /// <summary>
        /// A tab's state DOT in the rich render (§21.16.7). With a topic it is an in-text CALLBACK button showing the glyph: a tap
        /// makes the bot put that topic first in the thread list (DirectoryFrontCallbackData), the way into a topic that Telegram
        /// Android has, since no link opens one there. With no topic it is the plain glyph: there is no card to re-post. No style
        /// attribute — the app's plain button — so the dot keeps its own state color.
        /// </summary>
        /// <param name="state">The tab's state (the glyph: StateGlyph, like the topic names).</param>
        /// <param name="threadId">The tab's topic (0 = none).</param>
        /// <returns>One &lt;tg-button&gt; element, or the bare glyph.</returns>
        /// <exception cref="std::bad_alloc">If the element cannot be allocated.</exception>
        inline std::wstring DirectoryDotHtml(SessionState state, int64_t threadId)
        {
            const std::wstring glyph{ StateGlyph(state) };
            const std::wstring data = DirectoryFrontCallbackData(threadId);
            if (data.empty())
            {
                return glyph;
            }
            return L"<tg-button type=\"callback_data\" data=\"" + TgEscapeHtmlAttribute(data) + L"\">" + glyph + L"</tg-button>";
        }

        /// <summary>
        /// One tab's row in the RICH render: its state dot (a callback button when it has a topic, §21.16.7) and its title chip (a
        /// URL button to its topic in the look of its color's square, disabled without one). The tag sections and the Recent
        /// activity section (§21.16.8) share it, so a tab reads the same wherever the directory lists it.
        /// </summary>
        /// <param name="tab">The tab.</param>
        /// <param name="botUsername">The bot's username (the links' host part; "" = no links: disabled chips).</param>
        /// <returns>Telegram rich HTML, no line break (the caller puts the &lt;br&gt; between rows).</returns>
        /// <exception cref="std::bad_alloc">If the row cannot be allocated.</exception>
        inline std::wstring DirectoryTabRowRich(const DirectoryTab& tab, std::wstring_view botUsername)
        {
            const std::wstring label = DirectoryTabLabel(tab);
            const std::wstring url = DirectoryTopicUrl(botUsername, tab.threadId);
            return DirectoryDotHtml(tab.state, tab.threadId) + L" " + DirectoryChipHtml(label, TgTitleColorSquare(tab.colorHex), url);
        }

        /// <summary>
        /// One tab's row in the CLASSIC render: the plain state glyph (Telegram HTML has no in-text callback button) and the title
        /// as a link to its topic — the bare label when it has none. Shared like DirectoryTabRowRich.
        /// </summary>
        /// <param name="tab">The tab.</param>
        /// <param name="botUsername">The bot's username ("" = no links: bare labels).</param>
        /// <returns>Telegram HTML, no line break (the caller puts the "\n" between rows).</returns>
        /// <exception cref="std::bad_alloc">If the row cannot be allocated.</exception>
        inline std::wstring DirectoryTabRowClassic(const DirectoryTab& tab, std::wstring_view botUsername)
        {
            return std::wstring{ StateGlyph(tab.state) } + L" " + DirectoryLinkHtml(DirectoryTabLabel(tab), DirectoryTopicUrl(botUsername, tab.threadId));
        }

        /// <summary>"&lt;b&gt;5&lt;/b&gt; running": a bold count and its word, the plural word for every count but 1.</summary>
        /// <param name="n">The count.</param>
        /// <param name="one">The word after 1.</param>
        /// <param name="many">The word after any other count.</param>
        /// <returns>Telegram HTML (valid in both renders).</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring DirectoryCount(size_t n, const wchar_t* one, const wchar_t* many)
        {
            return L"<b>" + std::to_wstring(n) + L"</b> " + (n == 1 ? one : many);
        }

        /// <summary>The first stats row: "🔵 5 running · 🟡 15 waiting · ⚪ 300 idle" (the user's own example).</summary>
        /// <param name="s">The counts.</param>
        /// <returns>Telegram HTML (no line breaks).</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring DirectoryStatsRow1(const DirectoryStats& s)
        {
            return std::wstring{ StateGlyph(SessionState::Running) } + L" " + DirectoryCount(s.running, L"running", L"running") + L" \x00B7 " +
                   StateGlyph(SessionState::WaitingForInput) + L" " + DirectoryCount(s.waiting, L"waiting", L"waiting") + L" \x00B7 " +
                   StateGlyph(SessionState::Idle) + L" " + DirectoryCount(s.idle, L"idle", L"idle");
        }

        /// <summary>
        /// The second stats row: "🟠 2 need approval · 🔴 1 error · 🟢 3 done · ⏳ 12 queued", and "· ⏸ runner paused" while the
        /// global pause is on — the states row 1 leaves out, the runner's backlog, and the one switch that stops it.
        /// </summary>
        /// <param name="s">The counts.</param>
        /// <returns>Telegram HTML (no line breaks).</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring DirectoryStatsRow2(const DirectoryStats& s)
        {
            std::wstring row = std::wstring{ StateGlyph(SessionState::NeedsApproval) } + L" " + DirectoryCount(s.needsApproval, L"needs approval", L"need approval") +
                               L" \x00B7 " + StateGlyph(SessionState::Error) + L" " + DirectoryCount(s.error, L"error", L"errors") + L" \x00B7 " +
                               StateGlyph(SessionState::Done) + L" " + DirectoryCount(s.done, L"done", L"done") + L" \x00B7 \x23F3 " +
                               DirectoryCount(s.queued, L"queued", L"queued");
            if (s.runnerPaused)
            {
                row += L" \x00B7 \x23F8 runner paused";
            }
            return row;
        }

        /// <summary>
        /// The "updated" mark both renders end with: a date-time entity in the RELATIVE format, which each app renders and keeps
        /// current by itself ("5 minutes ago"), so the clock never needs an edit; the text inside is the PC's clock time
        /// (TgClockText), shown by an app that does not know the entity.
        /// </summary>
        /// <param name="updatedAtMs">When the content was rendered (Unix ms).</param>
        /// <param name="nowMs">The clock TgClockText's "same day" test uses (Unix ms).</param>
        /// <returns>Telegram HTML: "updated &lt;tg-time …&gt;19:04&lt;/tg-time&gt;".</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring DirectoryUpdatedMark(int64_t updatedAtMs, int64_t nowMs)
        {
            return L"updated <tg-time unix=\"" + std::to_wstring(updatedAtMs / 1000) + L"\" format=\"r\">" + TgEscapeHtml(TgClockText(updatedAtMs, nowMs)) + L"</tg-time>";
        }
    }

    /// <summary>
    /// The toast a dot's tap answers with (answerCallbackQuery's text; Telegram shows it at the top of the chat and caps it at 200
    /// characters): which thread goes first in the thread list. The re-post itself follows on the service's next tick, so it says
    /// "goes", not "is". A topic whose HISTORY is still being replayed keeps its card above the history until the replay is in
    /// (the sticky card's rule, §21.9), so its toast says the move waits for that. The title is one line, trimmed and cut at the
    /// chip's length (detail::DirectoryLabel), so the toast always fits.
    /// </summary>
    /// <param name="title">The tab's title ("" = "This thread").</param>
    /// <param name="historyPending">The topic's history replay is not done (TopicBackfillPending).</param>
    /// <returns>Plain text, under 200 characters.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring DirectoryFrontAnswer(std::wstring_view title, bool historyPending)
    {
        const std::wstring label = detail::DirectoryLabel(title);
        const std::wstring who = label.empty() ? std::wstring{ L"This thread" } : L"\x201C" + label + L"\x201D";
        return L"\x2191 " + who + (historyPending ? L" goes first in the thread list once its history is in" : L" goes first in the thread list");
    }

    /// <summary>
    /// Joins the tag universe with the open tabs: one DirectoryTag per tag, carrying the open tabs whose tag list names it
    /// (case-insensitively, the tag store's identity). A tag no open tab carries is kept, with no tabs: the RENDER leaves it out
    /// (RenderDirectory, the user's "Tags with 0 tabs should not be shown") and counts it for the service's log. A tab is listed
    /// under each of its tags; a tag a tab names that is not in the universe is ignored.
    /// </summary>
    /// <param name="tags">The universe: one (display name, color) per tag, any order (CollectGlobalTags' names with
    /// ResolveTagDisplayHex's colors). A name given twice (case-insensitively) is kept once, its first spelling.</param>
    /// <param name="openTabs">Every open tab, each with the names of its tags (the session store's tag list).</param>
    /// <returns>The tags, in the order of `tags`, each with its tabs in the order of `openTabs`.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::vector<DirectoryTag> JoinDirectoryTags(const std::vector<std::pair<std::wstring, std::wstring>>& tags,
                                                       const std::vector<std::pair<DirectoryTab, std::vector<std::wstring>>>& openTabs)
    {
        std::vector<DirectoryTag> out;
        std::unordered_map<std::wstring, size_t> byKey; // folded name -> index into out
        for (const auto& [name, colorHex] : tags)
        {
            if (name.empty())
            {
                continue;
            }
            if (byKey.emplace(detail::DirectoryFoldTag(name), out.size()).second)
            {
                DirectoryTag t;
                t.name = name;
                t.colorHex = colorHex;
                out.push_back(std::move(t));
            }
        }
        for (const auto& [tab, names] : openTabs)
        {
            // A tab whose tag list repeats a tag (another casing) is listed under it once.
            std::vector<size_t> seen;
            for (const auto& n : names)
            {
                const auto it = byKey.find(detail::DirectoryFoldTag(n));
                if (it == byKey.end() || std::find(seen.begin(), seen.end(), it->second) != seen.end())
                {
                    continue;
                }
                seen.push_back(it->second);
                out[it->second].tabs.push_back(tab);
            }
        }
        return out;
    }

    /// <summary>
    /// Renders the directory, rich and classic (§21.16):
    /// <code>
    /// 🔵 5 running · 🟡 15 waiting · ⚪ 300 idle
    /// 🟠 2 need approval · 🔴 1 error · 🟢 3 done · ⏳ 12 queued
    /// 🧭 Dedicated topics
    /// [🔎 Search]
    /// 🏷 Tags
    /// Tap a title to open its thread (Desktop, iPhone). Tap its dot to put the thread first in the list (any app).
    /// 🟪 release · 3 tabs
    /// [🔵] [🟦 Agentmaster]
    /// [🟡] [🟩 docs]
    /// ⚪ [🟦 an idle tab]        (a plain dot and a chip that opens nothing: the tab has no topic)
    /// 🕘 Recent activity
    /// [🔵] [🟦 Agentmaster]      (the ten newest tabs that are not stationary, newest first)
    /// [🟠] [🟪 hotfix review]
    /// updated 5 minutes ago
    /// </code>
    /// The stats rows come FIRST: a pinned message's bar shows the start of its text, so the counts read at the top of the chat.
    /// The Recent activity section comes LAST, before the footer (the user's "At the bottom", §21.16.8): at most
    /// kDirectoryMaxRecentTabs open Claude tabs whose state is not stationary (Idle and Done are; PickDirectoryRecentTabs), in
    /// the topic list's MOST RECENT order, each row the same dot and chip as under a tag (detail::DirectoryTabRowRich /
    /// DirectoryTabRowClassic); one italic line when none is listed. It is built BEFORE the tag sections, which fill the message
    /// only up to the room it leaves, so a full message never drops it; the legend goes under its heading only when no tag
    /// section has a dot button (it goes under the first heading whose rows have one).
    /// Tags go in case-insensitive order of their names, the tabs of a tag in case-insensitive order of their titles — orders
    /// that do not move while the fleet works, so the content changes only when a count, a state, a title or a tag does. In the
    /// rich render each title is a chip (a URL button) in the look nearest its color, and the state dot before it is a
    /// callback button that puts the tab's topic first in the thread list (§21.16.7: Telegram Android opens no topic from a
    /// link) — one italic line under the Tags heading says which does what, when any listed tab has a topic. In the classic
    /// render a title is a link with the color square first and the dot is plain text (Telegram HTML has no in-text callback
    /// button). Only a tag that an open tab carries is listed: a tag with no open tab (carried by closed
    /// sessions only, or by none) is left out of both renders — the user's "Tags with 0 tabs should not be shown" (2026-10-17)
    /// — and counted in DirectoryRender::tagsWithoutTabs, so it changes neither the message nor its hash. A tag lists at most
    /// kDirectoryMaxTabsPerTag tabs ("… and 4 more"); when a render is full, the tags that do not fit are counted in one line
    /// ("… and 3 more tags"); when no tag is listed, one line says how a tab gets one.
    /// </summary>
    /// <param name="m">The model.</param>
    /// <param name="updatedAtMs">When the content was rendered (the footer's date-time entity, Unix ms).</param>
    /// <param name="nowMs">The clock the footer's text uses (TgClockText's "same day" test).</param>
    /// <returns>The two renders, the content hash (without the footer), the truncation counts, the count of the tags left out
    /// for having no open tab, the count of the Recent activity rows and the count of the dot buttons.</returns>
    /// <exception cref="std::bad_alloc">If a render cannot be allocated.</exception>
    inline DirectoryRender RenderDirectory(const DirectoryModel& m, int64_t updatedAtMs, int64_t nowMs)
    {
        DirectoryRender r;
        const std::wstring row1 = detail::DirectoryStatsRow1(m.stats);
        const std::wstring row2 = detail::DirectoryStatsRow2(m.stats);
        std::wstring rich = L"<p>" + row1 + L"</p><p>" + row2 + L"</p>";
        std::wstring classic = row1 + L"\n" + row2;

        // The dedicated topics: one chip per line, in the service's order (they are few, and fixed).
        rich += L"<h3>\U0001F9ED Dedicated topics</h3><p>";
        classic += L"\n\n<b>\U0001F9ED Dedicated topics</b>";
        bool firstTopic = true;
        for (const auto& t : m.topics)
        {
            const std::wstring label = detail::DirectoryLabel(t.label);
            if (label.empty())
            {
                continue;
            }
            const std::wstring url = DirectoryTopicUrl(m.botUsername, t.threadId);
            rich += (firstTopic ? L"" : L"<br>") + detail::DirectoryChipHtml(label, TgTitleColorSquare(t.colorHex), url);
            classic += L"\n" + detail::DirectoryLinkHtml(label, url);
            firstTopic = false;
        }
        if (firstTopic)
        {
            rich += L"<i>none yet</i>";
            classic += L"\n<i>none yet</i>";
        }
        rich += L"</p>";

        // The tags, by name; each section is built whole, then added while it fits.
        rich += L"<h3>\U0001F3F7\xFE0F Tags</h3>";
        classic += L"\n\n<b>\U0001F3F7\xFE0F Tags</b>";
        std::vector<const DirectoryTag*> tags;
        tags.reserve(m.tags.size());
        for (const auto& t : m.tags)
        {
            if (t.name.empty())
            {
                continue;
            }
            if (t.tabs.empty())
            {
                // No open tab carries it: the user's "Tags with 0 tabs should not be shown". Left out of BOTH renders, and so of
                // the hash, so the rename, recolor or deletion of a tag nobody sees here never edits the message.
                ++r.tagsWithoutTabs;
                continue;
            }
            tags.push_back(&t);
        }
        std::stable_sort(tags.begin(), tags.end(), [](const DirectoryTag* a, const DirectoryTag* b) { return detail::TitleLessCi(a->name, b->name); });
        // The legend (§21.16.7), rich only and only when a dot is a button: the title and the dot do different things, and the
        // phone's way in is the dot (Telegram Android opens no topic from a link). Before the sections, so the size checks below
        // count it.
        const bool anyDot = std::any_of(tags.begin(), tags.end(), [](const DirectoryTag* t) {
            return std::any_of(t->tabs.begin(), t->tabs.end(), [](const DirectoryTab& tab) { return tab.threadId > 0; });
        });
        const std::wstring legend = L"<p><i>Tap a title to open its thread (Desktop, iPhone). Tap its dot to put the thread first in the list (any app).</i></p>";
        if (anyDot)
        {
            rich += legend;
        }
        if (tags.empty())
        {
            // ONE line for "no tag at all" and "tags, but none on an open tab": it is true of both, and a separate line for the
            // second would let a tag no open tab carries change the message after all.
            rich += L"<p><i>No open tab carries a tag. A tab's right-click menu \x25B8 Tags adds one.</i></p>";
            classic += L"\n<i>No open tab carries a tag. A tab's right-click menu \x25B8 Tags adds one.</i>";
        }

        // The Recent activity section (§21.16.8, the user's "the top 10 tab titles in the order of their recent activity and that
        // are not in stationary status"). It is the LAST section, but it is built HERE, before the tag sections: they fill the
        // message only up to the room it leaves (their size checks below add it), so a fleet with many tags never pushes the rows
        // the user asked for out of a full message. Both strings carry their own heading.
        const std::vector<DirectoryTab> recent = PickDirectoryRecentTabs(m.recent, kDirectoryMaxRecentTabs);
        const size_t recentDots = static_cast<size_t>(std::count_if(recent.begin(), recent.end(), [](const DirectoryTab& t) { return t.threadId > 0; }));
        std::wstring richRecent = L"<h3>\U0001F558 Recent activity</h3>";
        std::wstring classicRecent = L"\n\n<b>\U0001F558 Recent activity</b>";
        if (recentDots > 0 && !anyDot)
        {
            // The legend goes under the first heading whose rows have dot buttons: here, when no tag section has one.
            richRecent += legend;
        }
        if (recent.empty())
        {
            // ONE line for "no open tab" and "every open tab is stationary": the reader is told the same either way.
            richRecent += L"<p><i>No tab is working or waiting for you right now.</i></p>";
            classicRecent += L"\n<i>No tab is working or waiting for you right now.</i>";
        }
        else
        {
            richRecent += L"<p>";
            for (size_t i = 0; i < recent.size(); ++i)
            {
                richRecent += (i == 0 ? L"" : L"<br>") + detail::DirectoryTabRowRich(recent[i], m.botUsername);
                classicRecent += L"\n" + detail::DirectoryTabRowClassic(recent[i], m.botUsername);
            }
            richRecent += L"</p>";
        }

        size_t richHidden = 0;
        size_t classicHidden = 0;
        bool richFull = false;
        bool classicFull = false;
        for (const DirectoryTag* tag : tags)
        {
            std::vector<const DirectoryTab*> tabs;
            tabs.reserve(tag->tabs.size());
            for (const auto& tab : tag->tabs)
            {
                tabs.push_back(&tab);
            }
            std::stable_sort(tabs.begin(), tabs.end(), [](const DirectoryTab* a, const DirectoryTab* b) {
                // By TITLE, not by label (the label starts with the color square, which would group the tabs by color), then by
                // session id when two titles are equal case-insensitively: an order that holds from one render to the next.
                const std::wstring& ta = a->title.empty() ? a->sessionId : a->title;
                const std::wstring& tb = b->title.empty() ? b->sessionId : b->title;
                if (detail::TitleLessCi(ta, tb))
                {
                    return true;
                }
                if (detail::TitleLessCi(tb, ta))
                {
                    return false;
                }
                return a->sessionId < b->sessionId;
            });
            const std::wstring square = TgTitleColorSquare(tag->colorHex);
            // Every tag listed here has at least one open tab (the others were left out above), so the head always counts tabs.
            const std::wstring head = L"<b>" + (square.empty() ? std::wstring{} : square + L" ") + TgEscapeHtml(detail::DirectoryLabel(tag->name)) + L"</b> \x00B7 " +
                                      std::to_wstring(tabs.size()) + (tabs.size() == 1 ? L" tab" : L" tabs");
            std::wstring richSection = L"<p>" + head;
            std::wstring classicSection = L"\n\n" + head;
            const size_t listed = (std::min)(tabs.size(), kDirectoryMaxTabsPerTag); // (std::min): the <windows.h> macro
            size_t sectionDots = 0; // counted into r.dotsShown only when the section is added (it may not fit)
            for (size_t i = 0; i < listed; ++i)
            {
                const DirectoryTab& tab = *tabs[i];
                // Rich: the dot is the callback button (a tab with a topic), then the title chip; classic: the plain glyph. The
                // Recent activity section builds its rows with the same two functions, so a tab reads the same in both places.
                richSection += L"<br>" + detail::DirectoryTabRowRich(tab, m.botUsername);
                classicSection += L"\n" + detail::DirectoryTabRowClassic(tab, m.botUsername);
                if (tab.threadId > 0)
                {
                    ++sectionDots;
                }
            }
            if (tabs.size() > listed)
            {
                const std::wstring more = L"<i>\x2026 and " + std::to_wstring(tabs.size() - listed) + L" more</i>";
                richSection += L"<br>" + more;
                classicSection += L"\n" + more;
            }
            richSection += L"</p>";
            // The Recent activity section still to come is counted in (its room is kept), as are the closing lines (the reserve).
            if (!richFull && r.tagsShown < kDirectoryMaxTagSections &&
                rich.size() + richSection.size() + richRecent.size() + kDirectoryTailReserveChars <= kDirectoryRichMaxChars)
            {
                rich += richSection;
                ++r.tagsShown;
                r.tabsShown += listed;
                r.dotsShown += sectionDots;
            }
            else
            {
                richFull = true; // the order is by name: a later, shorter section must not jump the queue
                ++richHidden;
            }
            if (!classicFull && classic.size() + classicSection.size() + classicRecent.size() <= kDirectoryClassicMaxChars)
            {
                classic += classicSection;
            }
            else
            {
                classicFull = true;
                ++classicHidden;
            }
        }
        if (richHidden > 0)
        {
            rich += L"<p><i>\x2026 and " + std::to_wstring(richHidden) + (richHidden == 1 ? L" more tag" : L" more tags") + L" (the message is full)</i></p>";
        }
        if (classicHidden > 0)
        {
            classic += L"\n<i>\x2026 and " + std::to_wstring(classicHidden) + (classicHidden == 1 ? L" more tag" : L" more tags") + L" (the message is full)</i>";
        }
        // The Recent activity section at the bottom (the user's "At the bottom"): after the tags and their "more tags" line, before
        // the footer — inside the content the hash covers, so a change of the list edits the message.
        rich += richRecent;
        classic += classicRecent;
        r.recentShown = recent.size();
        r.dotsShown += recentDots;
        r.tagsHidden = richHidden;
        r.hash = CardTextHash(rich); // the content: everything but the footer's clock
        const std::wstring mark = detail::DirectoryUpdatedMark(updatedAtMs, nowMs);
        r.rich = rich + L"<footer>" + mark + L"</footer>";
        r.classic = classic + L"\n\n<i>" + mark + L"</i>";
        return r;
    }

    /// <summary>
    /// What a render lists, in words, for the service's log lines — the directory's post and the first edit of a run, the only
    /// places on the PC where the content of a sent directory can be read back: "2 tag(s), 5 tab chip(s)", then, only when there
    /// are any, the rows of the Recent activity section (§21.16.8), the dot buttons of the rich render (§21.16.7), the tags left
    /// out for having no open tab and the tags the full message could not hold. The FORM (rich / HTML) is not in it: the post logs
    /// the form it asks for, the edit the form Telegram took — and only the rich form carries the dot buttons.
    /// </summary>
    /// <param name="r">The render (RenderDirectory's counts).</param>
    /// <returns>The words, e.g. "2 tag(s), 5 tab chip(s), 10 recent tab(s), 14 dot button(s), 6 tag(s) on no open tab not
    /// listed".</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring DescribeDirectoryRender(const DirectoryRender& r)
    {
        std::wstring s = std::to_wstring(r.tagsShown) + L" tag(s), " + std::to_wstring(r.tabsShown) + L" tab chip(s)";
        if (r.recentShown > 0)
        {
            s += L", " + std::to_wstring(r.recentShown) + L" recent tab(s)";
        }
        if (r.dotsShown > 0)
        {
            s += L", " + std::to_wstring(r.dotsShown) + L" dot button(s)";
        }
        if (r.tagsWithoutTabs > 0)
        {
            s += L", " + std::to_wstring(r.tagsWithoutTabs) + L" tag(s) on no open tab not listed";
        }
        if (r.tagsHidden > 0)
        {
            s += L", " + std::to_wstring(r.tagsHidden) + L" tag(s) left out: the message is full";
        }
        return s;
    }

    // ===== the post / edit decision ======================================================================================

    /// <summary>What the service does with the directory on one check.</summary>
    enum class DirectoryAction
    {
        None, // nothing: the message shows this content already, a send is in flight, or a gap / back-off holds
        Post, // send a new directory message (and pin it): there is none
        Edit, // edit the message in place: its content or its form is not the wanted one
    };

    /// <summary>What the directory message shows now, as the service last learned it.</summary>
    struct DirectoryShown
    {
        int64_t messageId{ 0 }; // MessagingState::directoryMessageId (0 = no message)
        uint32_t hash{ 0 }; // MessagingState::directoryHash: the content hash of what it shows
        bool rich{ false }; // MessagingState::directoryRich: it is a rich message
        /// <summary>
        /// The content hash whose RICH form Telegram refused (the classic render went in its place), 0 = none. While the wanted
        /// content is that one, the classic form is the wanted form: a refusal is not tried again every edit gap; a changed
        /// content tries the rich form again. Kept by the service for the run (not persisted).
        /// </summary>
        uint32_t richRefusedHash{ 0 };
    };

    /// <summary>
    /// The decision of one check (pure; the service holds the clocks): nothing while a send is in flight or the back-off holds; a
    /// POST when there is no message; an EDIT when the content hash or the form differs from the wanted one and the last send is
    /// kDirectoryEditGapMs old; else nothing.
    /// </summary>
    /// <param name="shown">What the message shows.</param>
    /// <param name="wantHash">The content hash of the fresh render (DirectoryRender::hash).</param>
    /// <param name="wantRich">Rich messages are wanted (the cog's rich switch on, and the Bot API has them this run).</param>
    /// <param name="inFlight">A post or an edit has not been answered yet.</param>
    /// <param name="nowMs">The clock.</param>
    /// <param name="lastSendMs">When the last post or edit was queued (0 = none this run).</param>
    /// <param name="notBeforeMs">A back-off after a refusal (0 = none): nothing is sent before it.</param>
    /// <returns>The action.</returns>
    inline DirectoryAction DecideDirectoryAction(const DirectoryShown& shown, uint32_t wantHash, bool wantRich, bool inFlight, int64_t nowMs, int64_t lastSendMs, int64_t notBeforeMs) noexcept
    {
        if (inFlight || (notBeforeMs > 0 && nowMs < notBeforeMs))
        {
            return DirectoryAction::None;
        }
        if (shown.messageId <= 0)
        {
            return DirectoryAction::Post;
        }
        const bool richWanted = wantRich && shown.richRefusedHash != wantHash;
        if (shown.hash == wantHash && shown.rich == richWanted)
        {
            return DirectoryAction::None;
        }
        if (lastSendMs > 0 && nowMs - lastSendMs < kDirectoryEditGapMs)
        {
            return DirectoryAction::None;
        }
        return DirectoryAction::Edit;
    }

    /// <summary>
    /// Whether an answer to the directory's EDIT says the message is gone — deleted by the user, or an id that names no message of
    /// this chat — so the service forgets it and posts a new one (after kDirectoryRepostGapMs). "message is not modified" is not
    /// that (the message shows this content already), nor is any other refusal (the edit is tried again later).
    /// </summary>
    /// <param name="ok">Telegram's ok.</param>
    /// <param name="httpStatus">The HTTP status.</param>
    /// <param name="description">Telegram's description of the refusal.</param>
    /// <returns>True when the message is gone.</returns>
    inline bool DirectoryEditFoundNoMessage(bool ok, int httpStatus, std::wstring_view description) noexcept
    {
        if (ok || httpStatus != 400)
        {
            return false;
        }
        return description.find(L"message to edit not found") != std::wstring_view::npos || description.find(L"MESSAGE_ID_INVALID") != std::wstring_view::npos ||
               description.find(L"message can't be edited") != std::wstring_view::npos;
    }
}
