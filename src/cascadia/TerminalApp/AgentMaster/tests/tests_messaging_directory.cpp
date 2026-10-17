// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.16: the pinned DIRECTORY, its pure half (MessagingDirectory.h) — the topic links, the two
// stats rows, the join of the tags with the open tabs, both renders (the rich message with URL chips in the tab colors and the
// tabs' state dots as callback buttons, and the classic HTML fallback), the content hash that decides an edit, the post / edit
// decision and the "message is gone" rule, the dot's callback data and its toast (§21.16.7) — plus the state file's new fields,
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
}

void TestMessagingDirectory()
{
    wprintf(L"== TestMessagingDirectory (MESSAGING.md 21.16: the pinned directory - links, stats, tags x tabs, renders, hash, decision, state, palette) ==\n");

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
    }
    // The footer: the relative date-time entity, apps keep it current; the PC's clock time is its text.
    {
        const std::wstring mark = L"updated <tg-time unix=\"" + std::to_wstring(updatedAt / 1000) + L"\" format=\"r\">" + TgClockText(updatedAt, updatedAt) + L"</tg-time>";
        CHECK(DirEndsWith(r.rich, L"<footer>" + mark + L"</footer>"), "the rich message ends with the footer");
        CHECK(DirEndsWith(r.classic, L"\n\n<i>" + mark + L"</i>"), "the classic render ends with the same mark, in italics");
    }
    // The structure is balanced, and nothing private reaches Telegram.
    CHECK(DirCount(r.rich, L"<tg-button ") == DirCount(r.rich, L"</tg-button>") && DirCount(r.rich, L"<p>") == DirCount(r.rich, L"</p>") &&
              DirCount(r.rich, L"<b>") == DirCount(r.rich, L"</b>") && DirCount(r.rich, L"<h3>") == 2,
          "the rich HTML's elements are balanced");
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
