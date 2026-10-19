// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.16: the pinned DIRECTORY, its pure half (MessagingDirectory.h) — the topic links, the two
// stats rows, the join of the tags with the open tabs, both renders (the rich message with URL chips in the tab colors and the
// tabs' state dots as callback buttons, and the classic HTML fallback), the content hash that decides an edit, the post / edit
// decision and the "message is gone" rule, the dot's callback data and its toast (§21.16.7), the Recent activity section at the
// bottom (§21.16.8: the stationary states, the pick, its place, its room in a full message) — plus the state file's new fields,
// the census kind of its edits, and the tag palette the directory shares with the app (SessionStore.h, Rule #17). The service's
// sends, pins and the dot's re-post (Messaging.cpp, Messaging.Inbound.cpp) need Telegram; the live check is the dev bot.

#include "m5_tests.h"

#include <algorithm> // std::reverse (the order-independence check)
#include <map>

#include "../MessagingDirectory.h"
#include "../MessagingModels.h"
#include "../SessionStore.h" // FoldTagName, TagPaletteIndexFor, TagPaletteHex, ResolveTagDisplayHex
#include "../TelegramFormat.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    // How often `needle` occurs in `hay` (non-overlapping).
    size_t DirCount(const std::wstring& hay, const std::wstring& needle)
    {
        size_t n = 0;
        for (size_t p = hay.find(needle); p != std::wstring::npos; p = hay.find(needle, p + needle.size()))
        {
            ++n;
        }
        return n;
    }

    // Whether `text` ends with `tail`.
    bool DirEndsWith(const std::wstring& text, const std::wstring& tail)
    {
        return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
    }

    // A live tab's fact, as the topic sync fills it.
    TabFact DirFact(const wchar_t* id, SessionState state, bool claude = true, bool live = true, bool inTab = true)
    {
        TabFact f;
        f.sessionId = id;
        f.title = id;
        f.state = state;
        f.claude = claude;
        f.live = live;
        f.inTab = inTab;
        return f;
    }

    // A tab of the directory.
    DirectoryTab DirTab(const wchar_t* id, const wchar_t* title, SessionState state, const wchar_t* colorHex, int64_t threadId)
    {
        DirectoryTab t;
        t.sessionId = id;
        t.title = title;
        t.state = state;
        t.colorHex = colorHex;
        t.threadId = threadId;
        return t;
    }

    // A candidate of the Recent activity section (§21.16.8): a tab with its MOST RECENT key.
    DirectoryTab DirRecentTab(const wchar_t* id, const wchar_t* title, SessionState state, const wchar_t* colorHex, int64_t threadId, int64_t recentMs)
    {
        DirectoryTab t = DirTab(id, title, state, colorHex, threadId);
        t.recentMs = recentMs;
        return t;
    }
}

void TestMessagingDirectory()
{
    wprintf(L"== TestMessagingDirectory (MESSAGING.md 21.16: the pinned directory - links, stats, tags x tabs, recent activity, renders, hash, decision, state, palette) ==\n");

    // ---- the links ----
    CHECK(DirectoryTopicUrl(L"ClaudeSessionsDebugGateBot", 25027) == L"https://t.me/ClaudeSessionsDebugGateBot/25027?topic=25027",
          "a topic link: the topic's message link plus the topic parameter Telegram Desktop reads");
    CHECK(DirectoryTopicUrl(L"@Bot", 7) == L"https://t.me/Bot/7?topic=7", "a leading @ of the username is dropped");
    CHECK(DirectoryTopicUrl(L"Bot", 0).empty() && DirectoryTopicUrl(L"", 7).empty() && DirectoryTopicUrl(L"@", 7).empty(),
          "no topic, or no username: no link (a chip that opens nothing)");

    // ---- the counts: the /board population (live Claude sessions in a tab), by state ----
    {
        std::vector<TabFact> facts = {
            DirFact(L"r1", SessionState::Running),
            DirFact(L"r2", SessionState::Running),
            DirFact(L"w1", SessionState::WaitingForInput),
            DirFact(L"i1", SessionState::Idle),
            DirFact(L"a1", SessionState::NeedsApproval),
            DirFact(L"e1", SessionState::Error),
            DirFact(L"d1", SessionState::Done),
            DirFact(L"codex", SessionState::Running, /*claude*/ false),
            DirFact(L"closed", SessionState::Running, true, /*live*/ false),
            DirFact(L"notab", SessionState::Running, true, true, /*inTab*/ false),
        };
        const DirectoryStats s = CountDirectoryStats(facts, 12, true);
        CHECK(s.running == 2 && s.waiting == 1 && s.idle == 1 && s.needsApproval == 1 && s.error == 1 && s.done == 1,
              "each state of a live Claude tab is counted once; a Codex tab, a closed session and one with no tab are not");
        CHECK(s.queued == 12 && s.runnerPaused, "the backlog and the global pause pass through");
    }

    // ---- the join: tags x open tabs ----
    {
        const std::vector<std::pair<std::wstring, std::wstring>> universe = {
            { L"Release", L"#FFCA28" }, { L"hotfix", L"#E06C75" }, { L"empty", L"#AB47BC" }, { L"RELEASE", L"#000000" }, { L"", L"#000000" } };
        std::vector<std::pair<DirectoryTab, std::vector<std::wstring>>> open;
        open.emplace_back(DirTab(L"s1", L"one", SessionState::Running, L"#61AFEF", 11), std::vector<std::wstring>{ L"release", L"HOTFIX" });
        open.emplace_back(DirTab(L"s2", L"two", SessionState::Idle, L"#98C379", 0), std::vector<std::wstring>{ L"Release", L"rElEaSe", L"unknown" });
        const auto tags = JoinDirectoryTags(universe, open);
        CHECK(tags.size() == 3, "one tag per case-insensitive name; an empty name is dropped");
        CHECK(tags.size() == 3 && tags[0].name == L"Release" && tags[0].colorHex == L"#FFCA28", "a name given twice keeps its first spelling and color");
        CHECK(tags.size() == 3 && tags[0].tabs.size() == 2 && tags[0].tabs[0].sessionId == L"s1" && tags[0].tabs[1].sessionId == L"s2",
              "a tab is listed under its tag case-insensitively, once even when its list repeats the tag");
        CHECK(tags.size() == 3 && tags[1].tabs.size() == 1 && tags[1].tabs[0].sessionId == L"s1", "a tab is listed under each of its tags");
        CHECK(tags.size() == 3 && tags[2].name == L"empty" && tags[2].tabs.empty(),
              "the JOIN keeps a tag no open tab carries, with no tabs (the render leaves it out and counts it)");
        // The pure fold must stay the tag store's identity (the join and the store must agree on what one tag is).
        bool foldOk = true;
        for (const wchar_t* n : { L"Bug", L"RELEASE", L"Tag With Spaces", L"\x00C9\x0043OLE", L"mIxEd-42" })
        {
            foldOk = foldOk && Agentmaster::Msg::detail::DirectoryFoldTag(n) == FoldTagName(n); // qualified: Agentmaster has a detail namespace too
        }
        CHECK(foldOk, "the directory's tag fold is FoldTagName's (SessionStore.cpp)");
    }

    // ---- the renders ----
    const std::wstring bot = L"Bot";
    DirectoryModel m;
    m.botUsername = bot;
    m.stats.running = 5;
    m.stats.waiting = 15;
    m.stats.idle = 300;
    m.stats.needsApproval = 2;
    m.stats.error = 1;
    m.stats.done = 3;
    m.stats.queued = 12;
    {
        DirectoryTopic search;
        search.label = L"\U0001F50E Search";
        search.threadId = 25027;
        search.colorHex = L"#6FB9F0";
        m.topics.push_back(search);
    }
    {
        DirectoryTag release;
        release.name = L"release";
        release.colorHex = L"#FFCA28"; // amber: a yellow square
        release.tabs.push_back(DirTab(L"zzzz1111", L"zeta work", SessionState::WaitingForInput, L"#98C379", 31));
        release.tabs.push_back(DirTab(L"aaaa2222", L"Agentmaster", SessionState::Running, L"#61AFEF", 26785));
        release.tabs.push_back(DirTab(L"bbbb3333", L"idle tab", SessionState::Idle, L"#98C379", 0));
        release.tabs.push_back(DirTab(L"cccc4444", L"orange <x> & \"q\"", SessionState::Error, L"#D19A66", 40));
        DirectoryTag alpha;
        alpha.name = L"Alpha";
        alpha.colorHex = L"#AB47BC"; // purple
        DirectoryTag codex;
        codex.name = L"beta";
        codex.colorHex = L"#E06C75";
        codex.tabs.push_back(DirTab(L"dddd5555", L"", SessionState::Done, L"#E06C75", 50));
        m.tags = { release, alpha, codex };
    }
    const int64_t updatedAt = 1792200000000ll;
    const DirectoryRender r = RenderDirectory(m, updatedAt, updatedAt);
    {
        const std::wstring row1 = L"\U0001F535 <b>5</b> running \x00B7 \U0001F7E1 <b>15</b> waiting \x00B7 \x26AA <b>300</b> idle";
        const std::wstring row2 = L"\U0001F7E0 <b>2</b> need approval \x00B7 \U0001F534 <b>1</b> error \x00B7 \U0001F7E2 <b>3</b> done \x00B7 \x23F3 <b>12</b> queued";
        CHECK(r.rich.rfind(L"<p>" + row1 + L"</p><p>" + row2 + L"</p>", 0) == 0,
              "the rich message STARTS with the two stats rows (the pinned bar shows the start: the user's \"5 running 15 waiting 300 idle\")");
        CHECK(r.classic.rfind(row1 + L"\n" + row2 + L"\n", 0) == 0, "the classic render starts with the same two rows");
        CHECK(r.rich.find(L"runner paused") == std::wstring::npos, "no pause note while the runner runs");
    }
    {
        DirectoryModel paused = m;
        paused.stats.runnerPaused = true;
        paused.stats.needsApproval = 1;
        paused.stats.error = 2;
        const DirectoryRender p = RenderDirectory(paused, updatedAt, updatedAt);
        CHECK(p.rich.find(L"<b>1</b> needs approval") != std::wstring::npos && p.rich.find(L"<b>2</b> errors") != std::wstring::npos,
              "the singular and plural words of row 2");
        CHECK(p.rich.find(L"\x00B7 \x23F8 runner paused</p>") != std::wstring::npos, "row 2 says the global pause is on");
    }
    // The dedicated topics.
    CHECK(r.rich.find(L"<h3>\U0001F9ED Dedicated topics</h3><p><tg-button type=\"url\" style=\"primary\" url=\"https://t.me/Bot/25027?topic=25027\">\U0001F50E Search</tg-button></p>") !=
              std::wstring::npos,
          "the Search topic is a blue URL chip to its topic (its icon color picks the look)");
    CHECK(r.classic.find(L"<b>\U0001F9ED Dedicated topics</b>\n<a href=\"https://t.me/Bot/25027?topic=25027\">\U0001F50E Search</a>") != std::wstring::npos,
          "the classic render links the Search topic");
    {
        DirectoryModel early = m;
        early.topics[0].threadId = 0;
        const DirectoryRender e = RenderDirectory(early, updatedAt, updatedAt);
        CHECK(e.rich.find(L"<tg-button type=\"disabled\" style=\"primary\">\U0001F50E Search</tg-button>") != std::wstring::npos,
              "a dedicated topic not made yet is a chip that opens nothing");
        DirectoryModel none = m;
        none.topics.clear();
        CHECK(RenderDirectory(none, updatedAt, updatedAt).rich.find(L"<h3>\U0001F9ED Dedicated topics</h3><p><i>none yet</i></p>") != std::wstring::npos,
              "no dedicated topic: the section says so");
    }
    // The tags: by name, case-insensitively; each tab under its tag by title, its state glyph first, its chip in its color's look.
    {
        const size_t betaAt = r.rich.find(L"<b>\U0001F7E5 beta</b>");
        const size_t releaseAt = r.rich.find(L"<b>\U0001F7E8 release</b>");
        CHECK(betaAt != std::wstring::npos && releaseAt != std::wstring::npos && betaAt < releaseAt, "the tags in order of their names, each with its color square");
        {
            // "Zulu" sorts AFTER "release" case-insensitively; an ordinal sort ('Z' < 'b') would put it first.
            DirectoryModel cased = m;
            DirectoryTag zulu;
            zulu.name = L"Zulu";
            zulu.colorHex = L"#61AFEF";
            zulu.tabs.push_back(DirTab(L"eeee6666", L"z tab", SessionState::Idle, L"#61AFEF", 60));
            cased.tags.push_back(zulu);
            const DirectoryRender c = RenderDirectory(cased, updatedAt, updatedAt);
            const size_t cRelease = c.rich.find(L"<b>\U0001F7E8 release</b>");
            const size_t cZulu = c.rich.find(L"<b>\U0001F7E6 Zulu</b>");
            CHECK(cRelease != std::wstring::npos && cZulu != std::wstring::npos && cRelease < cZulu, "the tag order is case-insensitive");
        }
        CHECK(r.rich.find(L"Alpha") == std::wstring::npos && r.classic.find(L"Alpha") == std::wstring::npos && r.rich.find(L"no open tab") == std::wstring::npos,
              "a tag no open tab carries is not shown at all (the user's \"Tags with 0 tabs should not be shown\")");
        CHECK(r.rich.find(L"<b>\U0001F7E8 release</b> \x00B7 4 tabs<br>") != std::wstring::npos && r.rich.find(L"\U0001F7E5 beta</b> \x00B7 1 tab<br>") != std::wstring::npos,
              "a tag's count of open tabs, singular and plural");
        // §21.16.7: a tab WITH a topic has its state dot as a callback button (it puts the topic first in the thread list: the
        // way in on Telegram Android, which opens no topic from a link), then its title chip; a tab without one keeps a plain dot.
        const std::wstring agentmaster = L"<br><tg-button type=\"callback_data\" data=\"d:front:26785\">\U0001F535</tg-button> "
                                         L"<tg-button type=\"url\" style=\"primary\" url=\"https://t.me/Bot/26785?topic=26785\">\U0001F7E6 Agentmaster</tg-button>";
        const std::wstring idleTab = L"<br>\x26AA <tg-button type=\"disabled\" style=\"success\">\U0001F7E9 idle tab</tg-button>";
        const std::wstring orange = L"<br><tg-button type=\"callback_data\" data=\"d:front:40\">\U0001F534</tg-button> "
                                    L"<tg-button type=\"url\" url=\"https://t.me/Bot/40?topic=40\">\U0001F7E7 orange &lt;x&gt; &amp; \"q\"</tg-button>";
        const std::wstring zeta = L"<br><tg-button type=\"callback_data\" data=\"d:front:31\">\U0001F7E1</tg-button> "
                                  L"<tg-button type=\"url\" style=\"success\" url=\"https://t.me/Bot/31?topic=31\">\U0001F7E9 zeta work</tg-button>";
        const size_t a = r.rich.find(agentmaster);
        const size_t i = r.rich.find(idleTab);
        const size_t o = r.rich.find(orange);
        const size_t z = r.rich.find(zeta);
        CHECK(a != std::wstring::npos, "a blue tab: its state dot as a dot button, then a primary URL chip to its topic, the square in its label");
        CHECK(i != std::wstring::npos, "a tab with no topic (idle): a plain dot and a disabled chip, still in its color's look");
        CHECK(o != std::wstring::npos, "an orange tab: the plain chip (no style), its title escaped");
        CHECK(z != std::wstring::npos, "a green tab: a success chip");
        CHECK(a < i && i < o && o < z, "the tabs of a tag in case-insensitive order of their titles");
        CHECK(r.rich.find(L"<tg-button type=\"callback_data\" data=\"d:front:50\">\U0001F7E2</tg-button> "
                          L"<tg-button type=\"url\" style=\"danger\" url=\"https://t.me/Bot/50?topic=50\">\U0001F7E5 dddd5555</tg-button>") != std::wstring::npos,
              "a tab with no title shows its session id's first 8 characters; a red tab is a danger chip");
        CHECK(r.classic.find(L"\n\U0001F535 <a href=\"https://t.me/Bot/26785?topic=26785\">\U0001F7E6 Agentmaster</a>") != std::wstring::npos &&
                  r.classic.find(L"\n\x26AA \U0001F7E9 idle tab") != std::wstring::npos,
              "the classic render: a plain dot and a link per tab, the bare label for one with no topic");
        CHECK(r.classic.find(L"callback_data") == std::wstring::npos && r.classic.find(L"<tg-button") == std::wstring::npos,
              "the classic render has no dot buttons (Telegram HTML has no in-text callback button)");
        CHECK(r.tagsShown == 2 && r.tagsHidden == 0 && r.tagsWithoutTabs == 1 && r.tabsShown == 5 && r.dotsShown == 4,
              "the counts of a render that fits: 2 tags listed, the one no open tab carries counted apart, a dot button for each tab with a topic");
        CHECK(DescribeDirectoryRender(r) == L"2 tag(s), 5 tab chip(s), 4 dot button(s), 1 tag(s) on no open tab not listed", "the log's words for a render");
        // The legend says which does what, under the Tags heading, rich only.
        const std::wstring legend = L"<p><i>Tap a title to open its thread (Desktop, iPhone). Tap its dot to put the thread first in the list (any app).</i></p>";
        CHECK(r.rich.find(L"<h3>\U0001F3F7\xFE0F Tags</h3>" + legend) != std::wstring::npos, "the legend comes right under the Tags heading");
        CHECK(r.classic.find(L"Tap its dot") == std::wstring::npos, "the classic render has no legend (its dots are not buttons)");
        // A directory whose listed tabs have no topic has no dot button, and so no legend.
        DirectoryModel dotless = m;
        for (auto& tag : dotless.tags)
        {
            for (auto& tab : tag.tabs)
            {
                tab.threadId = 0;
            }
        }
        const DirectoryRender dl = RenderDirectory(dotless, updatedAt, updatedAt);
        CHECK(dl.dotsShown == 0 && dl.rich.find(L"callback_data") == std::wstring::npos && dl.rich.find(legend) == std::wstring::npos && dl.tabsShown == 5,
              "no tab with a topic: plain dots, no legend");
        CHECK(DescribeDirectoryRender(dl) == L"2 tag(s), 5 tab chip(s), 1 tag(s) on no open tab not listed", "no dot button: the log's words do not mention them");
    }
    // ---- the dot's callback data and its toast (§21.16.7) ----
    {
        CHECK(DirectoryFrontCallbackData(26785) == L"d:front:26785" && DirectoryFrontCallbackData(0).empty() && DirectoryFrontCallbackData(-3).empty(),
              "a dot's data names its topic's thread; no topic, no data");
        CHECK(DirectoryFrontCallbackData(9223372036854775807ll).size() <= 64, "the longest data fits the Bot API's 64-byte limit");
        CHECK(ParseDirectoryFrontCallbackData(DirectoryFrontCallbackData(25027)) == 25027 && ParseDirectoryFrontCallbackData(L"d:front:1") == 1,
              "the data reads back to its thread");
        CHECK(ParseDirectoryFrontCallbackData(L"d:front:") == 0 && ParseDirectoryFrontCallbackData(L"d:front:0") == 0 &&
                  ParseDirectoryFrontCallbackData(L"d:front:-5") == 0 && ParseDirectoryFrontCallbackData(L"d:front:12x") == 0 &&
                  ParseDirectoryFrontCallbackData(L"d:front:1 ") == 0 && ParseDirectoryFrontCallbackData(L"D:front:5") == 0 &&
                  ParseDirectoryFrontCallbackData(L"c:runner:5") == 0 && ParseDirectoryFrontCallbackData(L"") == 0,
              "data that is not a dot's - empty, zero, signed, with junk, another button's - reads as no topic");
        CHECK(ParseDirectoryFrontCallbackData(L"d:front:999999999999999999") == 999999999999999999ll && ParseDirectoryFrontCallbackData(L"d:front:9999999999999999999") == 0,
              "18 digits read back; 19 could overflow and read as no topic");
        CHECK(DirectoryFrontAnswer(L"Agentmaster", false) == L"\x2191 \x201C" L"Agentmaster\x201D goes first in the thread list", "the toast names the thread");
        CHECK(DirectoryFrontAnswer(L"Agentmaster", true) == L"\x2191 \x201C" L"Agentmaster\x201D goes first in the thread list once its history is in",
              "a topic still replaying its history moves once the history is in");
        CHECK(DirectoryFrontAnswer(L"  ", false) == L"\x2191 This thread goes first in the thread list", "no title: \"This thread\"");
        const std::wstring longTitle(500, L'x');
        CHECK(DirectoryFrontAnswer(longTitle, true).size() < 200, "a long title is cut, so the toast stays under Telegram's 200 characters");
        // A two-line tab title (its line break a lone CR, as the tab's rename box writes it) is one line, like every title the bot
        // prints (TgOneLineTitle, 2026-10-18): a CR LF pair is ONE space now, where the label's own loop made it two.
        CHECK(DirectoryFrontAnswer(L"BetterClipboard\ron-launch", false) == L"\x2191 \x201C" L"BetterClipboard on-launch\x201D goes first in the thread list" &&
                  DirectoryFrontAnswer(L"a\r\nb", false) == L"\x2191 \x201C" L"a b\x201D goes first in the thread list",
              "a multi-line title is one line in the directory's toast (and in its chips: the same label)");
    }
    // The footer: the relative date-time entity, apps keep it current; the PC's clock time is its text.
    {
        const std::wstring mark = L"updated <tg-time unix=\"" + std::to_wstring(updatedAt / 1000) + L"\" format=\"r\">" + TgClockText(updatedAt, updatedAt) + L"</tg-time>";
        CHECK(DirEndsWith(r.rich, L"<footer>" + mark + L"</footer>"), "the rich message ends with the footer");
        CHECK(DirEndsWith(r.classic, L"\n\n<i>" + mark + L"</i>"), "the classic render ends with the same mark, in italics");
    }
    // The structure is balanced, and nothing private reaches Telegram.
    CHECK(DirCount(r.rich, L"<tg-button ") == DirCount(r.rich, L"</tg-button>") && DirCount(r.rich, L"<p>") == DirCount(r.rich, L"</p>") &&
              DirCount(r.rich, L"<b>") == DirCount(r.rich, L"</b>") && DirCount(r.rich, L"<h3>") == 3,
          "the rich HTML's elements are balanced (three headings: Dedicated topics, Tags, Recent activity)");
    CHECK(r.rich.find(kTgTitleTokenOpen) == std::wstring::npos && r.classic.find(kTgTitleTokenOpen) == std::wstring::npos, "no title token in either render");
    CHECK(r.classic.size() <= kDirectoryClassicMaxChars + kDirectoryTailReserveChars && TgChunkHtml(r.classic).size() == 1, "the classic render is ONE message");
    {
        DirectoryModel noTags = m;
        noTags.tags.clear();
        const DirectoryRender nt = RenderDirectory(noTags, updatedAt, updatedAt);
        CHECK(nt.rich.find(L"<h3>\U0001F3F7\xFE0F Tags</h3><p><i>No open tab carries a tag. A tab's right-click menu \x25B8 Tags adds one.</i></p>") != std::wstring::npos &&
                  nt.classic.find(L"<b>\U0001F3F7\xFE0F Tags</b>\n<i>No open tab carries a tag. A tab's right-click menu \x25B8 Tags adds one.</i>") != std::wstring::npos,
              "no tag at all: one line says how a tab gets one");
        DirectoryModel onlyEmpty = m;
        onlyEmpty.tags = { m.tags[1] }; // "Alpha": no open tab carries it
        const DirectoryRender oe = RenderDirectory(onlyEmpty, updatedAt, updatedAt);
        CHECK(oe.rich == nt.rich && oe.hash == nt.hash && oe.tagsShown == 0 && oe.tagsWithoutTabs == 1,
              "tags that no open tab carries render exactly like no tag at all: the same line, the same hash");
        CHECK(DescribeDirectoryRender(oe) == L"0 tag(s), 0 tab chip(s), 1 tag(s) on no open tab not listed" && DescribeDirectoryRender(nt) == L"0 tag(s), 0 tab chip(s)",
              "the log's words count the tags left out, and say nothing of them when there are none");
    }
    // The hash: the content, never the clock.
    {
        CHECK(RenderDirectory(m, updatedAt + 3600 * 1000, updatedAt + 3600 * 1000).hash == r.hash, "the same content an hour later hashes the same (no edit for the clock)");
        DirectoryModel counted = m;
        counted.stats.running = 6;
        CHECK(RenderDirectory(counted, updatedAt, updatedAt).hash != r.hash, "a count that moved changes the hash");
        DirectoryModel moved = m;
        moved.tags[0].tabs[1].state = SessionState::WaitingForInput;
        CHECK(RenderDirectory(moved, updatedAt, updatedAt).hash != r.hash, "a listed tab's state that moved changes the hash");
        DirectoryModel reordered = m;
        std::reverse(reordered.tags.begin(), reordered.tags.end());
        std::reverse(reordered.tags[2].tabs.begin(), reordered.tags[2].tabs.end());
        CHECK(RenderDirectory(reordered, updatedAt, updatedAt).hash == r.hash, "the model's order does not matter: the render sorts");
        // A tag nobody sees here never edits the message: one added, renamed or recolored with no open tab keeps the hash.
        DirectoryModel ghosted = m;
        DirectoryTag ghost;
        ghost.name = L"ghost";
        ghost.colorHex = L"#E06C75";
        ghosted.tags.push_back(ghost);
        ghosted.tags[1].name = L"Alpha renamed";
        ghosted.tags[1].colorHex = L"#61AFEF";
        CHECK(RenderDirectory(ghosted, updatedAt, updatedAt).hash == r.hash, "a tag no open tab carries - added, renamed or recolored - does not change the hash");
        // Its first open tab makes it appear.
        DirectoryModel carried = m;
        carried.tags[1].tabs.push_back(DirTab(L"ffff7777", L"alpha tab", SessionState::Running, L"#61AFEF", 70));
        const DirectoryRender cr = RenderDirectory(carried, updatedAt, updatedAt);
        CHECK(cr.hash != r.hash && cr.tagsShown == 3 && cr.tagsWithoutTabs == 0 && cr.rich.find(L"<p><b>\U0001F7EA Alpha</b> \x00B7 1 tab<br>") != std::wstring::npos,
              "a tag's first open tab makes it appear, and changes the hash");
    }
    // The limits: many tabs under one tag, and more tags than the message holds.
    {
        DirectoryModel many = m;
        many.tags.resize(1);
        many.tags[0].tabs.clear();
        for (int k = 0; k < 30; ++k)
        {
            const std::wstring id = L"id" + std::to_wstring(100 + k);
            many.tags[0].tabs.push_back(DirTab(id.c_str(), id.c_str(), SessionState::Running, L"#61AFEF", 1000 + k));
        }
        const DirectoryRender mr = RenderDirectory(many, updatedAt, updatedAt);
        CHECK(DirCount(mr.rich, L"<tg-button type=\"url\" style=\"primary\" url=\"https://t.me/Bot/1") == kDirectoryMaxTabsPerTag,
              "one tag lists at most kDirectoryMaxTabsPerTag tabs");
        CHECK(mr.rich.find(L"<i>\x2026 and 10 more</i></p>") != std::wstring::npos && mr.rich.find(L"\x00B7 30 tabs") != std::wstring::npos,
              "the rest are counted, and the head counts them all");

        DirectoryModel huge = m;
        huge.tags.clear();
        for (int t = 0; t < 60; ++t)
        {
            DirectoryTag tag;
            tag.name = L"tag" + std::to_wstring(100 + t);
            tag.colorHex = L"#61AFEF";
            for (int k = 0; k < 20; ++k)
            {
                const std::wstring id = L"s" + std::to_wstring(t * 100 + k);
                tag.tabs.push_back(DirTab(id.c_str(), L"a tab title that takes some room in its chip", SessionState::Idle, L"#61AFEF", 5000 + t * 100 + k));
            }
            huge.tags.push_back(std::move(tag));
        }
        const DirectoryRender hr = RenderDirectory(huge, updatedAt, updatedAt);
        CHECK(hr.rich.size() <= kDirectoryRichMaxChars, "a model past the rich limit still renders within it");
        CHECK(hr.tagsHidden > 0 && hr.tagsShown + hr.tagsHidden == 60 && hr.tagsWithoutTabs == 0 && hr.rich.find(L" more tags (the message is full)</i></p>") != std::wstring::npos,
              "the tags that do not fit are counted in one line");
        CHECK(DescribeDirectoryRender(hr).find(L" tag(s) left out: the message is full") != std::wstring::npos, "the log's words count the tags the full message left out");
        CHECK(hr.rich.find(L"<b>\U0001F7E6 tag100</b>") != std::wstring::npos && hr.rich.find(L"<b>\U0001F7E6 tag159</b>") == std::wstring::npos,
              "the tags in name order until the message is full: the first are shown, the last are counted");
        CHECK(hr.classic.size() <= kDirectoryClassicMaxChars + kDirectoryTailReserveChars && TgChunkHtml(hr.classic).size() == 1 &&
                  hr.classic.find(L" more tags (the message is full)</i>") != std::wstring::npos,
              "the classic render stops sooner and stays one message");
    }

    // ---- the Recent activity section (§21.16.8): the ten newest tabs that are not stationary, at the bottom ----
    {
        CHECK(DirectoryStateIsStationary(SessionState::Done), "Done is stationary: claude ended in the tab");
        CHECK(!DirectoryStateIsStationary(SessionState::Idle) && !DirectoryStateIsStationary(SessionState::Running) &&
                  !DirectoryStateIsStationary(SessionState::WaitingForInput) && !DirectoryStateIsStationary(SessionState::NeedsApproval) &&
                  !DirectoryStateIsStationary(SessionState::Error),
              "Idle (the user's \"Do include idle ones please\"), Running, Waiting for you, Needs approval and Error are not");

        // The pick: the stationary tabs out, the newest MOST RECENT key first, at most ten.
        std::vector<DirectoryTab> cands;
        for (int k = 0; k < 12; ++k)
        {
            const std::wstring id = L"rec" + std::to_wstring(100 + k);
            cands.push_back(DirRecentTab(id.c_str(), id.c_str(), k % 2 == 0 ? SessionState::Running : SessionState::WaitingForInput, L"#61AFEF", 9000 + k, 1000 + k));
        }
        cands.push_back(DirRecentTab(L"idlenew", L"idle but newest", SessionState::Idle, L"#61AFEF", 9100, 99999));
        cands.push_back(DirRecentTab(L"donenew", L"done but newer", SessionState::Done, L"#61AFEF", 9101, 88888));
        const auto picked = PickDirectoryRecentTabs(cands, kDirectoryMaxRecentTabs);
        bool newestFirst = picked.size() == 10 && picked[0].sessionId == L"idlenew";
        for (size_t i = 1; newestFirst && i < picked.size(); ++i)
        {
            newestFirst = picked[i].recentMs == 1012 - static_cast<int64_t>(i);
        }
        CHECK(newestFirst,
              "ten tabs, the newest key first: the idle tab is listed (the newest), the done one left out though newer than the rest, the three oldest drop off");
        const std::vector<DirectoryTab> reversedCands(cands.rbegin(), cands.rend());
        const auto pickedReversed = PickDirectoryRecentTabs(reversedCands, kDirectoryMaxRecentTabs);
        bool sameOrder = pickedReversed.size() == picked.size();
        for (size_t i = 0; sameOrder && i < picked.size(); ++i)
        {
            sameOrder = pickedReversed[i].sessionId == picked[i].sessionId;
        }
        CHECK(sameOrder, "the pick does not depend on the order of the candidates");
        CHECK(PickDirectoryRecentTabs(cands, 0).empty() && PickDirectoryRecentTabs({}, kDirectoryMaxRecentTabs).empty(), "no room, or no candidate: nothing");
        // Equal keys: by title, case-insensitively (the topic list's TopicRanksAbove; a tab with no title by its session id), then
        // by session id.
        const std::vector<DirectoryTab> ties = {
            DirRecentTab(L"s3", L"Beta", SessionState::Running, L"", 0, 500),
            DirRecentTab(L"s2", L"SAME", SessionState::Running, L"", 0, 500),
            DirRecentTab(L"s1", L"same", SessionState::Running, L"", 0, 500),
            DirRecentTab(L"s0", L"alpha", SessionState::Running, L"", 0, 500),
            DirRecentTab(L"aaa", L"", SessionState::Running, L"", 0, 500),
        };
        const auto tied = PickDirectoryRecentTabs(ties, kDirectoryMaxRecentTabs);
        CHECK(tied.size() == 5 && tied[0].sessionId == L"aaa" && tied[1].sessionId == L"s0" && tied[2].sessionId == L"s3" && tied[3].sessionId == L"s1" &&
                  tied[4].sessionId == L"s2",
              "equal keys go by title case-insensitively (no title: the session id), then by session id");

        // The render: the section at the bottom, right before the footer; each row the dot and chip of a tagged tab.
        const std::wstring legendText = L"<p><i>Tap a title to open its thread (Desktop, iPhone). Tap its dot to put the thread first in the list (any app).</i></p>";
        DirectoryModel withRecent = m;
        withRecent.recent = {
            DirRecentTab(L"aaaa2222", L"Agentmaster", SessionState::Running, L"#61AFEF", 26785, 5000), // also listed under the "release" tag
            DirRecentTab(L"wwww1111", L"docs", SessionState::WaitingForInput, L"#98C379", 31, 9000),
            DirRecentTab(L"apap3333", L"hotfix review", SessionState::NeedsApproval, L"#AB47BC", 0, 7000), // no topic yet
            DirRecentTab(L"erer4444", L"zeta error", SessionState::Error, L"#E06C75", 77, 3000),
            DirRecentTab(L"idid5555", L"idle newest", SessionState::Idle, L"#61AFEF", 81, 99999),
            DirRecentTab(L"dodo6666", L"done tab", SessionState::Done, L"#61AFEF", 82, 88888),
        };
        const DirectoryRender wr = RenderDirectory(withRecent, updatedAt, updatedAt);
        const std::wstring docsRow = L"<tg-button type=\"callback_data\" data=\"d:front:31\">\U0001F7E1</tg-button> "
                                     L"<tg-button type=\"url\" style=\"success\" url=\"https://t.me/Bot/31?topic=31\">\U0001F7E9 docs</tg-button>";
        const std::wstring hotfixRow = L"\U0001F7E0 <tg-button type=\"disabled\">\U0001F7EA hotfix review</tg-button>";
        const std::wstring agentRow = L"<tg-button type=\"callback_data\" data=\"d:front:26785\">\U0001F535</tg-button> "
                                      L"<tg-button type=\"url\" style=\"primary\" url=\"https://t.me/Bot/26785?topic=26785\">\U0001F7E6 Agentmaster</tg-button>";
        const std::wstring zetaRow = L"<tg-button type=\"callback_data\" data=\"d:front:77\">\U0001F534</tg-button> "
                                     L"<tg-button type=\"url\" style=\"danger\" url=\"https://t.me/Bot/77?topic=77\">\U0001F7E5 zeta error</tg-button>";
        const std::wstring idleRow = L"<tg-button type=\"callback_data\" data=\"d:front:81\">\x26AA</tg-button> "
                                     L"<tg-button type=\"url\" style=\"primary\" url=\"https://t.me/Bot/81?topic=81\">\U0001F7E6 idle newest</tg-button>";
        const std::wstring section = L"<h3>\U0001F558 Recent activity</h3><p>" + idleRow + L"<br>" + docsRow + L"<br>" + hotfixRow + L"<br>" + agentRow + L"<br>" + zetaRow +
                                     L"</p>";
        CHECK(wr.rich.find(section + L"<footer>") != std::wstring::npos,
              "the rich section: the five tabs that are not stationary (the idle one too), newest first, each with its dot and title chip, right before the footer");
        const std::wstring classicSection = L"\n\n<b>\U0001F558 Recent activity</b>"
                                            L"\n\x26AA <a href=\"https://t.me/Bot/81?topic=81\">\U0001F7E6 idle newest</a>"
                                            L"\n\U0001F7E1 <a href=\"https://t.me/Bot/31?topic=31\">\U0001F7E9 docs</a>"
                                            L"\n\U0001F7E0 \U0001F7EA hotfix review"
                                            L"\n\U0001F535 <a href=\"https://t.me/Bot/26785?topic=26785\">\U0001F7E6 Agentmaster</a>"
                                            L"\n\U0001F534 <a href=\"https://t.me/Bot/77?topic=77\">\U0001F7E5 zeta error</a>";
        CHECK(wr.classic.find(classicSection + L"\n\n<i>updated ") != std::wstring::npos, "the classic section: plain dots and links, right before the footer");
        CHECK(wr.rich.find(L"done tab") == std::wstring::npos && wr.classic.find(L"done tab") == std::wstring::npos,
              "a stationary tab (Done: claude ended) is not listed, however new");
        CHECK(wr.rich.find(L"<h3>\U0001F558 Recent activity</h3>") > wr.rich.find(L"<b>\U0001F7E8 release</b>") && DirCount(wr.rich, L"<h3>") == 3,
              "the section comes after the tag sections");
        CHECK(DirCount(wr.rich, legendText) == 1 && wr.rich.find(L"<h3>\U0001F3F7\xFE0F Tags</h3>" + legendText) != std::wstring::npos,
              "the legend once, under the Tags heading, when the tag sections have dot buttons");
        CHECK(wr.recentShown == 5 && wr.tabsShown == 5 && wr.dotsShown == 4 + 4,
              "the counts: five recent rows, the tag chips as before, a dot button for every listed tab with a topic in either place");
        CHECK(DescribeDirectoryRender(wr) == L"2 tag(s), 5 tab chip(s), 5 recent tab(s), 8 dot button(s), 1 tag(s) on no open tab not listed",
              "the log's words count the recent rows");
        CHECK(DirCount(wr.rich, L"<tg-button ") == DirCount(wr.rich, L"</tg-button>") && DirCount(wr.rich, L"<p>") == DirCount(wr.rich, L"</p>"),
              "the rich HTML stays balanced");
        // No tag section with a dot button: the legend goes under the Recent activity heading instead.
        DirectoryModel dotlessTags = withRecent;
        for (auto& tag : dotlessTags.tags)
        {
            for (auto& tab : tag.tabs)
            {
                tab.threadId = 0;
            }
        }
        const DirectoryRender dt = RenderDirectory(dotlessTags, updatedAt, updatedAt);
        CHECK(dt.rich.find(L"<h3>\U0001F558 Recent activity</h3>" + legendText + L"<p>") != std::wstring::npos && DirCount(dt.rich, legendText) == 1,
              "no tag section has a dot button: the legend goes under the Recent activity heading, once");
        // Nothing to list.
        const std::wstring emptyRich = L"<h3>\U0001F558 Recent activity</h3><p><i>No tab to list: none is open, or every open one has ended.</i></p><footer>";
        const std::wstring emptyClassic = L"\n\n<b>\U0001F558 Recent activity</b>\n<i>No tab to list: none is open, or every open one has ended.</i>\n\n<i>updated ";
        CHECK(r.rich.find(emptyRich) != std::wstring::npos && r.classic.find(emptyClassic) != std::wstring::npos && r.recentShown == 0,
              "no candidate: one line says there is no tab to list");
        DirectoryModel allEnded = m;
        allEnded.recent = { DirRecentTab(L"x1", L"ended one", SessionState::Done, L"#61AFEF", 5, 100), DirRecentTab(L"x2", L"ended two", SessionState::Done, L"#61AFEF", 6, 200) };
        const DirectoryRender ended = RenderDirectory(allEnded, updatedAt, updatedAt);
        CHECK(ended.rich == r.rich && ended.hash == r.hash, "only stationary (Done) candidates render exactly like none: the same line, the same hash");
        DirectoryModel onlyIdle = m;
        onlyIdle.recent = { DirRecentTab(L"y1", L"quiet tab", SessionState::Idle, L"#61AFEF", 0, 100) };
        const DirectoryRender oi = RenderDirectory(onlyIdle, updatedAt, updatedAt);
        const std::wstring quietSection = L"<h3>\U0001F558 Recent activity</h3><p>\x26AA <tg-button type=\"disabled\" style=\"primary\">\U0001F7E6 quiet tab</tg-button></p>";
        CHECK(oi.recentShown == 1 && oi.rich.find(quietSection) != std::wstring::npos,
              "an idle tab alone is listed: a plain dot and a chip that opens nothing while it has no topic");

        // The hash follows what the section shows, and nothing else.
        DirectoryModel plusEnded = withRecent;
        plusEnded.recent.push_back(DirRecentTab(L"zzzz9999", L"another ended", SessionState::Done, L"#61AFEF", 90, 777777));
        CHECK(RenderDirectory(plusEnded, updatedAt, updatedAt).hash == wr.hash, "a stationary (Done) candidate, however new, does not change the hash");
        DirectoryModel recentReversed = withRecent;
        std::reverse(recentReversed.recent.begin(), recentReversed.recent.end());
        CHECK(RenderDirectory(recentReversed, updatedAt, updatedAt).hash == wr.hash, "the candidates' order does not matter");
        DirectoryModel stateMoved = withRecent;
        stateMoved.recent[1].state = SessionState::Running; // docs: Waiting for you -> Running
        CHECK(RenderDirectory(stateMoved, updatedAt, updatedAt).hash != wr.hash, "a listed tab's state that moved changes the hash");
        DirectoryModel wentIdle = withRecent;
        wentIdle.recent[3].state = SessionState::Idle; // zeta error -> Idle
        const DirectoryRender wi = RenderDirectory(wentIdle, updatedAt, updatedAt);
        CHECK(wi.hash != wr.hash && wi.recentShown == 5 && wi.rich.find(L"data=\"d:front:77\">\x26AA</tg-button>") != std::wstring::npos,
              "a tab that went idle stays listed, its dot now the idle one (and the hash moves)");
        DirectoryModel wentDone = withRecent;
        wentDone.recent[3].state = SessionState::Done; // zeta error: claude ended in the tab
        const DirectoryRender wd = RenderDirectory(wentDone, updatedAt, updatedAt);
        CHECK(wd.hash != wr.hash && wd.recentShown == 4 && wd.rich.find(L"zeta error") == std::wstring::npos, "a tab whose claude ended leaves the section");
        DirectoryModel reranked = withRecent;
        reranked.recent[3].recentMs = 9500; // zeta error's turn ended: its key is now newer than every tab but the idle one
        const DirectoryRender rk = RenderDirectory(reranked, updatedAt, updatedAt);
        CHECK(rk.hash != wr.hash && rk.rich.find(L"<h3>\U0001F558 Recent activity</h3><p>" + idleRow + L"<br>" + zetaRow + L"<br>" + docsRow) != std::wstring::npos,
              "a new turn boundary moves its tab up, and changes the hash");

        // At most ten; one older than the ten listed changes nothing.
        DirectoryModel eleven = m;
        for (int k = 0; k < 11; ++k)
        {
            const std::wstring id = L"many" + std::to_wstring(10 + k);
            eleven.recent.push_back(DirRecentTab(id.c_str(), id.c_str(), SessionState::Running, L"#61AFEF", 9000 + k, 1000 + k));
        }
        const DirectoryRender er = RenderDirectory(eleven, updatedAt, updatedAt);
        const size_t erHead = er.rich.find(L"<h3>\U0001F558 Recent activity</h3>");
        CHECK(er.recentShown == kDirectoryMaxRecentTabs && erHead != std::wstring::npos && er.rich.find(L"d:front:9010", erHead) != std::wstring::npos &&
                  er.rich.find(L"d:front:9001", erHead) != std::wstring::npos && er.rich.find(L"d:front:9000", erHead) == std::wstring::npos,
              "at most ten tabs: the oldest of eleven is left out");
        DirectoryModel olderStill = eleven;
        olderStill.recent.push_back(DirRecentTab(L"older", L"older", SessionState::Running, L"#61AFEF", 9999, 10));
        CHECK(RenderDirectory(olderStill, updatedAt, updatedAt).hash == er.hash, "a tab older than the ten listed does not change the hash");

        // A full message keeps the section whole: the tag sections fill only the room it leaves.
        DirectoryModel full = m;
        full.tags.clear();
        for (int t = 0; t < 60; ++t)
        {
            DirectoryTag tag;
            tag.name = L"tag" + std::to_wstring(100 + t);
            tag.colorHex = L"#61AFEF";
            for (int k = 0; k < 20; ++k)
            {
                const std::wstring id = L"s" + std::to_wstring(t * 100 + k);
                tag.tabs.push_back(DirTab(id.c_str(), L"a tab title that takes some room in its chip", SessionState::Idle, L"#61AFEF", 5000 + t * 100 + k));
            }
            full.tags.push_back(std::move(tag));
        }
        full.recent = eleven.recent;
        const DirectoryRender fr = RenderDirectory(full, updatedAt, updatedAt);
        const size_t frRichHead = fr.rich.find(L"<h3>\U0001F558 Recent activity</h3>");
        const size_t frClassicHead = fr.classic.find(L"<b>\U0001F558 Recent activity</b>");
        CHECK(fr.recentShown == kDirectoryMaxRecentTabs && fr.tagsHidden > 0 && fr.rich.size() <= kDirectoryRichMaxChars && frRichHead != std::wstring::npos &&
                  DirCount(fr.rich.substr(frRichHead), L"d:front:90") == kDirectoryMaxRecentTabs,
              "a full rich message: tags left out, the ten recent rows kept whole, within the rich limit");
        CHECK(frClassicHead != std::wstring::npos && DirCount(fr.classic.substr(frClassicHead), L"https://t.me/Bot/90") == kDirectoryMaxRecentTabs &&
                  fr.classic.size() <= kDirectoryClassicMaxChars + kDirectoryTailReserveChars && TgChunkHtml(fr.classic).size() == 1,
              "the classic render keeps the ten rows too, and stays one message");
    }

    // ---- the decision ----
    {
        DirectoryShown none;
        CHECK(DecideDirectoryAction(none, 7, true, false, 1000, 0, 0) == DirectoryAction::Post, "no message: post one");
        CHECK(DecideDirectoryAction(none, 7, true, true, 1000, 0, 0) == DirectoryAction::None, "a send in flight: nothing");
        CHECK(DecideDirectoryAction(none, 7, true, false, 1000, 0, 5000) == DirectoryAction::None, "a back-off holds even the post");
        CHECK(DecideDirectoryAction(none, 7, true, false, 6000, 0, 5000) == DirectoryAction::Post, "the back-off over: post");
        DirectoryShown shown;
        shown.messageId = 42;
        shown.hash = 7;
        shown.rich = true;
        CHECK(DecideDirectoryAction(shown, 7, true, false, 10 * 60 * 1000, 0, 0) == DirectoryAction::None, "the same content in the wanted form: nothing");
        const int64_t t0 = 100 * 60 * 1000;
        CHECK(DecideDirectoryAction(shown, 8, true, false, t0, t0 - kDirectoryEditGapMs, 0) == DirectoryAction::Edit, "a changed content after the gap: edit");
        CHECK(DecideDirectoryAction(shown, 8, true, false, t0, t0 - kDirectoryEditGapMs + 1, 0) == DirectoryAction::None, "a changed content inside the gap: wait");
        CHECK(DecideDirectoryAction(shown, 7, false, false, t0, 0, 0) == DirectoryAction::Edit, "the rich switch turned off: edit into the classic form");
        DirectoryShown classic = shown;
        classic.rich = false;
        CHECK(DecideDirectoryAction(classic, 7, true, false, t0, 0, 0) == DirectoryAction::Edit, "a classic message while rich is wanted: edit into the rich form");
        classic.richRefusedHash = 7;
        CHECK(DecideDirectoryAction(classic, 7, true, false, t0, 0, 0) == DirectoryAction::None, "a content whose rich form Telegram refused stays classic");
        CHECK(DecideDirectoryAction(classic, 9, true, false, t0, 0, 0) == DirectoryAction::Edit, "a changed content tries the rich form again");
    }
    CHECK(DirectoryEditFoundNoMessage(false, 400, L"Bad Request: message to edit not found") && DirectoryEditFoundNoMessage(false, 400, L"Bad Request: MESSAGE_ID_INVALID") &&
              DirectoryEditFoundNoMessage(false, 400, L"Bad Request: message can't be edited"),
          "the answers that say the message is gone");
    CHECK(!DirectoryEditFoundNoMessage(false, 400, L"Bad Request: message is not modified: specified new message content and reply markup are exactly the same") &&
              !DirectoryEditFoundNoMessage(false, 400, L"Bad Request: can't parse entities") && !DirectoryEditFoundNoMessage(false, 429, L"Too Many Requests: retry after 5") &&
              !DirectoryEditFoundNoMessage(true, 200, L""),
          "\"not modified\", another refusal, a 429 and a success do not");

    // ---- the state file and the census ----
    {
        MessagingState s;
        s.directoryMessageId = 1234;
        s.directoryHash = 0xDEADBEEFu;
        s.directoryRich = true;
        s.directoryPinned = true;
        const auto back = StateFromJson(StateToJson(s));
        CHECK(back && back->directoryMessageId == 1234 && back->directoryHash == 0xDEADBEEFu && back->directoryRich && back->directoryPinned,
              "the directory's fields round-trip through state.json");
        const auto old = StateFromJson(*json::Parse(L"{\"version\":1,\"searchThreadId\":5}"));
        CHECK(old && old->directoryMessageId == 0 && old->directoryHash == 0 && !old->directoryRich && !old->directoryPinned,
              "an older state file has no directory yet (the service posts and pins one)");
        const auto bad = StateFromJson(*json::Parse(L"{\"directoryMessageId\":-5}"));
        CHECK(bad && bad->directoryMessageId == 0, "a negative message id reads as none");
    }
    CHECK(std::wstring{ EditKindOfKey(L"dir:edit") } == L"directory" && std::wstring{ EditKindOfKey(L"card:x") } == L"card",
          "the traffic census counts the directory's edits as their own kind");

    // ---- the tag palette the directory shares with the app (Rule #17) ----
    CHECK(TagPaletteIndexFor(L"release") == 4 && TagPaletteIndexFor(L"Release") == 4 && TagPaletteIndexFor(L"bug") == 5 && TagPaletteIndexFor(L"hotfix") == 1 &&
              TagPaletteIndexFor(L"docs") == 10 && TagPaletteIndexFor(L"telegram") == 0,
          "the name hash picks the palette entries it always picked (pinned against an independent FNV-1a)");
    CHECK(TagPaletteHex(0) == L"#FF4FC3F7" && TagPaletteHex(4) == L"#FFFFCA28" && TagPaletteHex(12) == L"#FF4FC3F7", "a palette entry as #FFRRGGBB (the index wraps)");
    {
        const std::map<std::wstring, std::wstring> stored = { { L"release", L"#FF112233" }, { L"broken", L"#12" } };
        CHECK(ResolveTagDisplayHex(L"RELEASE", stored) == L"#FF112233", "a picked color wins, found case-insensitively");
        CHECK(ResolveTagDisplayHex(L"broken", stored) == TagPaletteHex(TagPaletteIndexFor(L"broken")), "a malformed stored color falls back to the hash");
        CHECK(ResolveTagDisplayHex(L"bug", stored) == L"#FF26A69A", "no picked color: the palette entry of the hash");
        CHECK(std::wstring{ TgTitleColorSquare(ResolveTagDisplayHex(L"release", {})) } == L"\U0001F7E8", "a tag's square comes from its display color (amber: yellow)");
    }
}
