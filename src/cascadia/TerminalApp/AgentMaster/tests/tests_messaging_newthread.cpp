// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.17: the user's three requests of 2026-10-18, their pure halves —
//   1. every title the bot prints LINKS to its session's topic (TelegramFormat.h: the token's THREAD part, TgTopicUrl, the linked
//      classic render, the URL chip; the card, the change line and the Search cards name their thread), and a "👤 You" line has the
//      session's title as its SECOND ROW (UserLineTitleRow, the builders, RenderHistory / PlanHistoryPosts);
//   1.5 a turn's prompt line replies to the previous turn's — the history replay's chain survives a restart on the topic record
//      (TopicRecord::historyPromptMessageId), and the card's link is a FORM like its rich-ness (TopicRecord::cardLinked);
//   2. a thread the user opens starts a NEW SESSION: the folder picker (MessagingNewThread.h — the two lists from the registry's
//      facts, the pages, the text, the keyboard, the buttons' data, a typed folder, a topic name without its decoration), the
//      registry's lean read behind it (SessionRegistry::PlaceSamples).
// The service's sends, the binding of the thread and the launch with a chosen id need Telegram and a window: the live check is the
// dev bot (BuildClaudeSpawn's chosen id is pinned in tests_spawn_sched.cpp).

#include "m5_tests.h"

#include <algorithm> // std::find / find_if / any_of / none_of
#include <cstdio> // swprintf (the test folders' names)

#include "../MessagingDirectory.h" // DirectoryTopicUrl (one link builder with TgTopicUrl)
#include "../MessagingModels.h"
#include "../MessagingNewThread.h"
#include "../SessionRegistry.h" // PlaceSamples
#include "../TelegramFormat.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    // How often `needle` occurs in `hay` (non-overlapping).
    size_t NtCount(const std::wstring& hay, const std::wstring& needle)
    {
        size_t n = 0;
        for (size_t p = hay.find(needle); p != std::wstring::npos; p = hay.find(needle, p + needle.size()))
        {
            ++n;
        }
        return n;
    }

    // A transcript item of the history replay.
    TopicItem NtItem(TopicItemKind kind, const wchar_t* text, int64_t tsMs = 0, bool turnEnds = false)
    {
        TopicItem it;
        it.kind = kind;
        it.text = text;
        it.tsMs = tsMs;
        it.turnEnds = turnEnds;
        return it;
    }

    // A folder fact: a session launched in `dir` (and inferred in `inferred`), last active at `ms`.
    SessionFolderFacts NtFacts(const std::wstring& dir, const std::wstring& inferred, int64_t ms)
    {
        SessionFolderFacts f;
        f.workingDir = dir;
        f.inferredDir = inferred;
        f.lastActivityMs = ms;
        return f;
    }
}

void TestMessagingNewThread()
{
    wprintf(L"== TestMessagingNewThread (MESSAGING.md 21.17: title links, the You lines' title row, the turn chain fields, the new-thread picker) ==\n");
    const std::wstring blue = L"\U0001F7E6";
    const std::wstring green = L"\U0001F7E9";
    const std::wstring url42 = L"https://t.me/bot/42?topic=42";

    // ===== 1. the topic link ====================================================================================================
    {
        CHECK(TgTopicUrl(L"@bot", 9) == L"https://t.me/bot/9?topic=9" && TgTopicUrl(L"bot", 9) == L"https://t.me/bot/9?topic=9", "TgTopicUrl drops the @ and builds the topic link");
        CHECK(TgTopicUrl(L"", 9).empty() && TgTopicUrl(L"@", 9).empty() && TgTopicUrl(L"bot", 0).empty() && TgTopicUrl(L"bot", -3).empty(), "no link without a username or a thread");
        CHECK(DirectoryTopicUrl(L"@bot", 9) == TgTopicUrl(L"@bot", 9), "the directory's chips and the title links use one builder");
    }

    // ===== 1. a token that names its thread ===================================================================================
    {
        const std::wstring plain = TgTitleToken(TgTitleForm::Plain, blue, L"a <b> & c", 42);
        // Without the username: the pre-21.17 render byte for byte (every hash is taken this way).
        CHECK(TgRenderTitleTokensClassic(plain) == blue + L" a &lt;b&gt; &amp; c", "a threaded token renders like before without the username");
        CHECK(TgRenderTitleTokensClassic(plain) == TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Plain, blue, L"a <b> & c")), "the thread does not change the hashed render");
        CHECK(TgRenderTitleTokensClassic(plain, L"bot") == blue + L" <a href=\"" + url42 + L"\">a &lt;b&gt; &amp; c</a>", "classic + username: the title is a link, the square outside it");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Bold, blue, L"t", 7), L"@Bot") == blue + L" <b><a href=\"https://t.me/Bot/7?topic=7\">t</a></b>",
              "the Bold form links inside the bold");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Quoted, blue, L"t", 42), L"bot") == L"\x201C" + blue + L" <a href=\"" + url42 + L"\">t</a>\x201D",
              "the Quoted form links inside the quotes");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Square, green, L"t", 42), L"bot") == green, "the bare square names no title and links nowhere");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Plain, L"", L"t", 42), L"bot") == L"<a href=\"" + url42 + L"\">t</a>", "no square: the link alone");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Plain, blue, L"t"), L"bot") == blue + L" t", "a token without a thread never links");
        // A stray THREAD marker in a title is dropped like the other framing characters; the token still parses.
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Plain, L"", std::wstring{ L"a" } + kTgTitleTokenThread + L"b", 5), L"bot") ==
                  L"<a href=\"https://t.me/bot/5?topic=5\">ab</a>",
              "a THREAD marker inside a title is dropped");
        // A malformed thread part (not digits): no link, the title as before.
        const std::wstring bad = std::wstring{ kTgTitleTokenOpen } + L"p" + blue + kTgTitleTokenThread + L"4x2" + kTgTitleTokenMid + L"t" + kTgTitleTokenClose;
        CHECK(TgRenderTitleTokensClassic(bad, L"bot") == blue + L" t", "a thread part that is not a number links nowhere");
        const std::wstring tooLong = std::wstring{ kTgTitleTokenOpen } + L"p" + kTgTitleTokenThread + L"1234567890123456789" + kTgTitleTokenMid + L"t" + kTgTitleTokenClose;
        CHECK(TgRenderTitleTokensClassic(tooLong, L"bot") == L"t", "a thread of 19 digits (an overflow) links nowhere");
        // Rich: a URL chip with the look and the label of the copy chip; without the username the copy chip of 21.15.
        CHECK(TgRichHtmlFromClassic(L"x " + plain + L" y", L"bot") ==
                  L"<p>x <tg-button type=\"url\" style=\"primary\" url=\"" + url42 + L"\">" + blue + L" a &lt;b&gt; &amp; c</tg-button> y</p>",
              "rich + username: the title is a URL chip to its topic");
        CHECK(TgRichHtmlFromClassic(L"x " + plain + L" y") == L"<p>x <tg-button type=\"copy_text\" style=\"primary\" text=\"a &lt;b&gt; &amp; c\">" + blue + L" a &lt;b&gt; &amp; c</tg-button> y</p>",
              "rich without the username: the 21.15 copy chip");
        CHECK(TgRichHtmlFromClassic(TgTitleToken(TgTitleForm::Plain, blue, L"t"), L"bot").find(L"type=\"copy_text\"") != std::wstring::npos, "rich: a token without a thread stays a copy chip");
        CHECK(TgRichHtmlFromClassic(L"<blockquote>" + plain + L"</blockquote>", L"bot").find(L"type=\"url\"") != std::wstring::npos, "a title inside a quote links too");
        CHECK(TgRichHtmlFromClassic(L"<pre>" + plain + L"</pre>", L"bot").find(L"<a href") == std::wstring::npos, "a title inside code stays text");
        CHECK(TgTitleChipHtml(blue, L"t", url42) == L"<tg-button type=\"url\" style=\"primary\" url=\"" + url42 + L"\">" + blue + L" t</tg-button>", "TgTitleChipHtml with a url is a URL button");
        CHECK(TgTitleChipHtml(L"", L"t", L"https://t.me/b/1?topic=1&x=\"y\"").find(L"url=\"https://t.me/b/1?topic=1&amp;x=&quot;y&quot;\"") != std::wstring::npos, "the url attribute is escaped");
    }

    // ===== 1. the card, the change line, the Search cards ======================================================================
    {
        SessionInfo s = MakeSession(L"abcdef12-3456-7890-abcd-ef1234567890", SessionState::WaitingForInput);
        s.title = L"tab";
        CardExtras ex;
        ex.colorSquare = blue;
        ex.threadId = 42;
        const std::wstring card = BuildCardText(s, 1'800'000'000'000, 0, false, &ex);
        CardExtras noThread = ex;
        noThread.threadId = 0;
        const std::wstring cardBefore = BuildCardText(s, 1'800'000'000'000, 0, false, &noThread);
        CHECK(TgRenderTitleTokensClassic(card) == TgRenderTitleTokensClassic(cardBefore), "the card's hashed render does not move with its thread");
        CHECK(TgRenderTitleTokensClassic(card, L"bot").find(L"<b><a href=\"" + url42 + L"\">tab</a></b>") != std::wstring::npos, "the card's title links to its topic");
        const std::wstring notice = BuildTopicChangeNoticeHtml(L"old", blue, L"new", blue, 42);
        CHECK(TgRenderTitleTokensClassic(notice) == TgRenderTitleTokensClassic(BuildTopicChangeNoticeHtml(L"old", blue, L"new", blue)), "the change line renders like before without the username");
        CHECK(NtCount(TgRenderTitleTokensClassic(notice, L"bot"), L"<a href=\"" + url42 + L"\">") == 2, "both titles of the change line link to the topic");
        SearchCard sc;
        sc.number = 3;
        sc.sessionId = s.id;
        sc.title = L"found";
        sc.colorSquare = blue;
        sc.threadId = 42;
        sc.state = SearchPickState::Listed;
        CHECK(TgRenderTitleTokensClassic(BuildSearchPickMenuHtml(sc, 1'800'000'000'000), L"bot").find(L"<a href=\"" + url42 + L"\">found</a>") != std::wstring::npos, "the pick menu's title links to the topic");
        CHECK(TgRenderTitleTokensClassic(BuildSearchAnswerHtml(L"one", { sc }, 0, 1'800'000'000'000), L"bot").find(L"<a href=\"" + url42 + L"\">found</a>") != std::wstring::npos, "a Search card's title links to the topic");
        sc.threadId = 0;
        CHECK(TgRenderTitleTokensClassic(BuildSearchAnswerHtml(L"one", { sc }, 0, 1'800'000'000'000), L"bot").find(L"<a href") == std::wstring::npos, "a session with no topic: no link");
    }

    // ===== 1. the "You" lines' title row ======================================================================================
    {
        const std::wstring row = UserLineTitleRow(L"my tab", L"12345678-aaaa-bbbb-cccc-dddddddddddd", blue, 42);
        CHECK(row == TgTitleToken(TgTitleForm::Plain, blue, L"my tab", 42), "UserLineTitleRow: the title as a Plain token naming the topic");
        CHECK(UserLineTitleRow(L"", L"12345678-aaaa", L"", 0) == TgTitleToken(TgTitleForm::Plain, L"", L"12345678", 0), "no title: the short id, no link");
        // 2026-10-18: a title is printed on ONE line (TgTitleLine), no longer cut to its first line.
        CHECK(UserLineTitleRow(L"line one\nline two", L"x", L"", 1) == TgTitleToken(TgTitleForm::Plain, L"", L"line one line two", 1) &&
                  UserLineTitleRow(L"line one\rline two", L"x", L"", 1) == TgTitleToken(TgTitleForm::Plain, L"", L"line one line two", 1),
              "a two-line title: both lines, joined by a space (an LF, or the lone CR the tab's rename box writes)");
        const std::wstring without = BuildPromptLineHtml(0, PromptSource::Pc, L"hello", PromptTurnStatus::Running, false);
        const std::wstring with = BuildPromptLineHtml(0, PromptSource::Pc, L"hello", PromptTurnStatus::Running, false, row);
        const size_t nl = without.find(L'\n');
        CHECK(nl != std::wstring::npos && with == without.substr(0, nl) + L"\n" + row + without.substr(nl), "the prompt line: header, the title row, the prompt");
        CHECK(BuildPromptLineHtml(0, PromptSource::Pc, L"hello", PromptTurnStatus::Running, false, L"") == without, "no row: the 21.12 line byte for byte");
        CHECK(TgRenderTitleTokensClassic(with, L"bot").find(L"\n" + blue + L" <a href=\"" + url42 + L"\">my tab</a>\nhello") != std::wstring::npos, "the row renders as a linked title on line 2");
        const std::wstring rich = TgRichHtmlFromClassic(with, L"bot");
        CHECK(rich.find(L"<br><tg-button type=\"url\"") != std::wstring::npos && rich.find(L"</tg-button><br>hello") != std::wstring::npos, "rich: the row is a URL chip on its own line");
        const std::wstring answer = BuildAnswerLineHtml(0, L"yes", row);
        CHECK(answer.find(L"\n" + row + L"\n") != std::wstring::npos && BuildAnswerLineHtml(0, L"yes") == BuildAnswerLineHtml(0, L"yes", L""), "the answer line has the row; none without it");
        const std::wstring command = BuildCommandLineHtml(0, L"/compact", row);
        CHECK(command.find(L"\n" + row + L"\n<code>/compact</code>") != std::wstring::npos, "the command line has the row above the command");
        // The history replay: every "You" line of the bot carries the row; the assistant's posts do not.
        // (A first turn gets a day line before its prompt, so the prompt is looked up, not taken as the first post.)
        int64_t day = 0;
        const auto posts = RenderHistory({ NtItem(TopicItemKind::Prompt, L"go", 1'800'000'000'000), NtItem(TopicItemKind::Text, L"Done.", 0, true) }, false, day, {}, false, row);
        const auto prompt = std::find_if(posts.begin(), posts.end(), [](const HistoryPost& p) { return p.turnPrompt; });
        CHECK(prompt != posts.end() && prompt->fromUser && prompt->html.find(row) != std::wstring::npos, "the replayed prompt line carries the row");
        CHECK(!posts.empty() && posts.back().turnReply && posts.back().html.find(row) == std::wstring::npos, "the reply carries no row");
        int64_t day2 = 0;
        const auto plainPosts = RenderHistory({ NtItem(TopicItemKind::Prompt, L"go", 1'800'000'000'000), NtItem(TopicItemKind::Text, L"Done.", 0, true) }, false, day2, {}, false);
        CHECK(!plainPosts.empty() && std::none_of(plainPosts.begin(), plainPosts.end(), [](const HistoryPost& p) { return TgHasTitleTokens(p.html); }),
              "no row: the replay's lines carry no token");
        HistoryWindow w;
        w.items = { NtItem(TopicItemKind::Prompt, L"go", 1'800'000'000'000), NtItem(TopicItemKind::Text, L"Done.", 0, true) };
        w.itemAt = { 0, 10 };
        w.consumedTo = 20;
        int64_t day3 = 0;
        const auto planned = PlanHistoryPosts(w, day3, {}, false, row);
        CHECK(std::any_of(planned.begin(), planned.end(), [&](const HistoryPlanPost& p) { return p.post.turnPrompt && p.post.html.find(row) != std::wstring::npos; }),
              "PlanHistoryPosts passes the row to the replay");
    }

    // ===== 1.5 the topic record's new fields ==================================================================================
    {
        MessagingState st;
        TopicRecord t;
        t.sessionId = L"s1";
        t.threadId = 5;
        t.cardLinked = true;
        t.historyPromptMessageId = 77;
        st.topics[L"s1"] = t;
        TopicRecord bare;
        bare.sessionId = L"s2";
        bare.threadId = 6;
        st.topics[L"s2"] = bare;
        const auto parsed = json::Parse(json::Dump(StateToJson(st)));
        const auto back = parsed ? StateFromJson(*parsed) : std::nullopt;
        CHECK(back && back->topics.count(L"s1") && back->topics.at(L"s1").cardLinked && back->topics.at(L"s1").historyPromptMessageId == 77, "cardLinked and historyPromptMessageId round-trip");
        CHECK(back && back->topics.count(L"s2") && !back->topics.at(L"s2").cardLinked && back->topics.at(L"s2").historyPromptMessageId == 0, "absent: not linked, no chain yet");
        CHECK(json::Dump(StateToJson(st)).find(L"historyPromptMessageId\":0") == std::wstring::npos, "an empty chain is not written");
    }

    // ===== 2. the picker's lists ==============================================================================================
    const int64_t now = 1'800'000'000'000;
    std::vector<SessionFolderFacts> facts;
    for (int i = 1; i <= 23; ++i)
    {
        wchar_t name[32];
        swprintf(name, 32, L"K:\\w\\f%02d", i);
        facts.push_back(NtFacts(name, L"", now - i * 60'000)); // fNN active NN minutes ago
    }
    facts.push_back(NtFacts(L"K:\\w\\f05", L"", now - 100 * 60'000)); // f05: 3 sessions, the most used
    facts.push_back(NtFacts(L"K:\\w\\f05", L"", now - 200 * 60'000));
    facts.push_back(NtFacts(L"K:\\w\\f09", L"", now - 300 * 60'000)); // f09: 2 sessions
    facts.push_back(NtFacts(L"C:\\Temp\\probe", L"", now)); // the newest, under a scratch root: never offered
    facts.push_back(NtFacts(L"K:\\w\\gone", L"", now - 30'000)); // the probe says it is gone
    facts.push_back(NtFacts(L"K:\\w\\f01", L"K:\\w\\deep", now - 45'000)); // f01: 2 sessions; deep: inferred only
    size_t probes = 0;
    std::vector<std::wstring> probed;
    const auto probe = [&](const std::wstring& path) {
        ++probes;
        probed.push_back(path);
        return path != L"K:\\w\\gone";
    };
    const NewThreadLists lists = BuildNewThreadLists(facts, { NormDirKey(L"C:\\Temp") }, probe);
    {
        CHECK(lists.recent.size() == 24 && lists.mostUsed.size() == 24, "24 folders in each list (23 + the inferred one; temp and gone left out)");
        CHECK(!lists.recent.empty() && lists.recent[0].path == L"K:\\w\\f01" && lists.recent[1].path == L"K:\\w\\deep" && lists.recent[2].path == L"K:\\w\\f02",
              "Recent: the newest activity first, a tie by sessions");
        CHECK(!lists.mostUsed.empty() && lists.mostUsed[0].path == L"K:\\w\\f05" && lists.mostUsed[0].sessions == 3 && lists.mostUsed[1].path == L"K:\\w\\f01" &&
                  lists.mostUsed[2].path == L"K:\\w\\f09" && lists.mostUsed[3].path == L"K:\\w\\deep",
              "Most used: the most sessions first, a tie by recency");
        bool noTemp = true;
        for (const auto& f : lists.recent)
        {
            noTemp = noTemp && f.path.find(L"Temp") == std::wstring::npos && f.path != L"K:\\w\\gone";
        }
        CHECK(noTemp, "no scratch folder and no gone folder in Recent");
        CHECK(probes == 25 && std::find(probed.begin(), probed.end(), L"C:\\Temp\\probe") == probed.end(), "one probe per folder (25), none for a scratch root");
        const NewThreadLists capped = BuildNewThreadLists(facts, {}, nullptr, 5);
        CHECK(capped.recent.size() == 5 && capped.mostUsed.size() == 5 && capped.recent[0].path == L"C:\\Temp\\probe", "the cap per list; no probe and no roots keep everything");
        CHECK(BuildNewThreadLists({}, {}, probe).recent.empty(), "no sessions: empty lists");
    }

    // ===== 2. pages and the buttons' data ======================================================================================
    {
        CHECK(NewThreadPageCount(0) == 1 && NewThreadPageCount(10) == 1 && NewThreadPageCount(11) == 2 && NewThreadPageCount(24) == 3, "page counts (10 a page, at least 1)");
        CHECK(ClampNewThreadPage(5, 24) == 2 && ClampNewThreadPage(1, 0) == 0 && ClampNewThreadPage(1, 24) == 1, "a page is kept inside the list");
        CHECK(NewThreadCallbackData(NewThreadAction::Kind::PickRecent, 3) == L"nt:r:3" && NewThreadCallbackData(NewThreadAction::Kind::PickUsed, 0) == L"nt:m:0" &&
                  NewThreadCallbackData(NewThreadAction::Kind::PageRecent, 1) == L"nt:rp:1" && NewThreadCallbackData(NewThreadAction::Kind::PageUsed, 2) == L"nt:mp:2" &&
                  NewThreadCallbackData(NewThreadAction::Kind::Cancel, 9) == L"nt:x" && NewThreadCallbackData(NewThreadAction::Kind::None, 1).empty(),
              "the buttons' callback data");
        CHECK(NewThreadCallbackData(NewThreadAction::Kind::PageUsed, 999999999).size() <= 64, "within the Bot API's 64 bytes");
        const auto back = ParseNewThreadCallbackData(L"nt:m:17");
        CHECK(back.kind == NewThreadAction::Kind::PickUsed && back.value == 17, "parse: a pick");
        CHECK(ParseNewThreadCallbackData(L"nt:rp:2").kind == NewThreadAction::Kind::PageRecent && ParseNewThreadCallbackData(L"nt:x").kind == NewThreadAction::Kind::Cancel, "parse: a page, cancel");
        CHECK(ParseNewThreadCallbackData(L"nt:r:").kind == NewThreadAction::Kind::None && ParseNewThreadCallbackData(L"nt:r:3a").kind == NewThreadAction::Kind::None &&
                  ParseNewThreadCallbackData(L"nt:q:1").kind == NewThreadAction::Kind::None && ParseNewThreadCallbackData(L"nt:r:1234567890").kind == NewThreadAction::Kind::None &&
                  ParseNewThreadCallbackData(L"c:r:1").kind == NewThreadAction::Kind::None && ParseNewThreadCallbackData(L"nt:").kind == NewThreadAction::Kind::None,
              "parse: malformed data is no action");
        CHECK(ParseNewThreadCallbackData(L"d:front:5").kind == NewThreadAction::Kind::None, "a directory dot is not a picker button");
        // The pager's page label (2026-10-18): its own data, so a tap only answers which page it is.
        CHECK(NewThreadCallbackData(NewThreadAction::Kind::PageLabelRecent, 1) == L"nt:rl:1" && NewThreadCallbackData(NewThreadAction::Kind::PageLabelUsed, 0) == L"nt:ml:0",
              "the pager labels' callback data");
        CHECK(NewThreadCallbackData(NewThreadAction::Kind::PageLabelUsed, static_cast<size_t>(-1)).size() <= 26, "the longest data: 26 characters at most");
        const auto label = ParseNewThreadCallbackData(L"nt:ml:3");
        CHECK(label.kind == NewThreadAction::Kind::PageLabelUsed && label.value == 3 && ParseNewThreadCallbackData(L"nt:rl:0").kind == NewThreadAction::Kind::PageLabelRecent,
              "parse: a pager label");
        CHECK(ParseNewThreadCallbackData(L"nt:rl:").kind == NewThreadAction::Kind::None && ParseNewThreadCallbackData(L"nt:ml:2x").kind == NewThreadAction::Kind::None &&
                  ParseNewThreadCallbackData(L"nt:l:1").kind == NewThreadAction::Kind::None && ParseNewThreadCallbackData(L"nt:rlx:1").kind == NewThreadAction::Kind::None,
              "parse: a malformed label is no action");
        CHECK(NewThreadPageLabel(0, 4) == L"1 / 4" && NewThreadPageLabel(3, 4) == L"4 / 4" && NewThreadPageLabel(1, 2) == L"2 / 2", "the page label: the 1-based page / the pages");
        CHECK(NewThreadPageToast(L"Recent", 1, 34) == L"Recent: page 2 of 4 (folders 11\x2013" L"20 of 34)", "the label's toast: the page, the pages, the folders shown");
        CHECK(NewThreadPageToast(L"Most used", 9, 34) == L"Most used: page 4 of 4 (folders 31\x2013" L"34 of 34)", "a label past the end tells the last page");
        CHECK(NewThreadPageToast(L"Recent", 0, 0) == L"Recent: no folders known yet." && NewThreadPageToast(L"Recent", 0, 7) == L"Recent: page 1 of 1 (folders 1\x2013" L"7 of 7)",
              "an empty list says so; a short list is one page");
    }

    // ===== 2. the pager rows (2026-10-18) ======================================================================================
    // The user's layout: "We start with one button to the right and after moving one page there are 2 buttons and one label in
    // middle for the page number. reaching end shows only to the left".
    {
        using K = NewThreadAction::Kind;
        CHECK(BuildNewThreadPagingRow(L"Recent", 0, 0, K::PageRecent, K::PageLabelRecent).empty() && BuildNewThreadPagingRow(L"Recent", 10, 0, K::PageRecent, K::PageLabelRecent).empty(),
              "one page, or none: no pager");
        const auto firstPage = BuildNewThreadPagingRow(L"Recent", 34, 0, K::PageRecent, K::PageLabelRecent);
        CHECK(firstPage.size() == 2 && firstPage[0].text == L"1 / 4" && firstPage[0].data == L"nt:rl:0" && firstPage[1].text == L"Recent \x25B6" && firstPage[1].data == L"nt:rp:1",
              "the first page: the label, then the one arrow, on the right");
        const auto middlePage = BuildNewThreadPagingRow(L"Recent", 34, 1, K::PageRecent, K::PageLabelRecent);
        CHECK(middlePage.size() == 3 && middlePage[0].text == L"\x25C0 Recent" && middlePage[0].data == L"nt:rp:0" && middlePage[1].text == L"2 / 4" &&
                  middlePage[1].data == L"nt:rl:1" && middlePage[2].text == L"Recent \x25B6" && middlePage[2].data == L"nt:rp:2",
              "a middle page: the two arrows with the label between them");
        const auto lastPage = BuildNewThreadPagingRow(L"Most used", 34, 3, K::PageUsed, K::PageLabelUsed);
        CHECK(lastPage.size() == 2 && lastPage[0].text == L"\x25C0 Most used" && lastPage[0].data == L"nt:mp:2" && lastPage[1].text == L"4 / 4" && lastPage[1].data == L"nt:ml:3",
              "the last page: the one arrow, on the left, then the label");
        const auto pastEnd = BuildNewThreadPagingRow(L"Recent", 34, 99, K::PageRecent, K::PageLabelRecent);
        CHECK(pastEnd.size() == 2 && pastEnd[0].data == L"nt:rp:2" && pastEnd[1].text == L"4 / 4" && pastEnd[1].data == L"nt:rl:3", "a page past the end: the last page's pager");
        const auto twoPages = BuildNewThreadPagingRow(L"Recent", 20, 1, K::PageRecent, K::PageLabelRecent);
        CHECK(twoPages.size() == 2 && twoPages[0].data == L"nt:rp:0" && twoPages[1].text == L"2 / 2", "two pages (dev's 20 folders): the second page is the last");
    }

    // ===== 2. the labels ======================================================================================================
    {
        CHECK(NewThreadFolderLabel(L"K:\\a\\proj", { L"K:\\a\\proj", L"K:\\b\\other" }) == L"proj", "a label is the folder's name");
        CHECK(NewThreadFolderLabel(L"K:\\a\\proj", { L"K:\\a\\proj", L"Q:\\b\\Proj" }) == L"a\\proj", "two shown folders with one name: the parent too");
        CHECK(NewThreadFolderLabel(L"K:\\a\\proj", { L"K:\\a\\proj", L"k:/a/proj/" }) == L"proj", "the same folder spelled twice is no clash");
        CHECK(NewThreadFolderLabel(L"K:\\", {}) == L"K:", "a drive root");
        const std::wstring longLabel = NewThreadFolderLabel(L"K:\\x\\" + std::wstring(40, L'n'), {});
        CHECK(longLabel.size() == kNewThreadLabelMaxChars && longLabel.back() == L'\x2026', "a long name is cut with an ellipsis");
    }

    // ===== 2. the picker's text ===============================================================================================
    {
        const std::wstring html = BuildNewThreadPickerHtml(lists, 1, 0, 0, L"", now);
        CHECK(html.find(L"\U0001F4C2 <b>New session</b>") == 0, "the picker starts with what it is for");
        CHECK(html.find(L"\U0001F558 <b>Recent</b> \x00B7 11\x2013" L"20 of 24") != std::wstring::npos, "Recent's page 2 shows 11-20 of 24");
        CHECK(html.find(L"\n<b>11</b> K:\\w\\f10 \x00B7 10m ago") != std::wstring::npos, "a Recent line: number, path, how long ago");
        CHECK(html.find(L"\U0001F3C6 <b>Most used</b> \x00B7 1\x2013" L"10 of 24") != std::wstring::npos, "Most used's first page");
        CHECK(html.find(L"\n<b>1</b> K:\\w\\f05 \x00B7 3 sessions") != std::wstring::npos && html.find(L"K:\\w\\f02 \x00B7 1 session\n") != std::wstring::npos,
              "a Most used line: number, path, sessions (singular for one)");
        CHECK(html.find(L"A message you send here waits") != std::wstring::npos && html.find(L"the folder's name \x2014 rename this thread") != std::wstring::npos,
              "no message held, no name: what happens and how the title is chosen");
        const std::wstring two = BuildNewThreadPickerHtml(lists, 0, 0, 2, L"my <tab>", now);
        CHECK(two.find(L"Your 2 messages wait here") != std::wstring::npos && two.find(L"Its title: <b>my &lt;tab&gt;</b>") != std::wstring::npos, "held messages counted; the name escaped");
        CHECK(BuildNewThreadPickerHtml(lists, 0, 0, 1, L"", now).find(L"Your message waits here") != std::wstring::npos, "one held message");
        const std::wstring empty = BuildNewThreadPickerHtml(NewThreadLists{}, 0, 0, 0, L"", now);
        CHECK(NtCount(empty, L"No folders known yet.") == 2 && empty.find(L"send a folder's full path") != std::wstring::npos, "empty lists say so; a path still works");
        CHECK(BuildNewThreadPickerHtml(lists, 99, 0, 0, L"", now).find(L"21\x2013" L"24 of 24") != std::wstring::npos, "a page past the end shows the last one");
    }

    // ===== 2. the keyboard ====================================================================================================
    {
        const auto rows = BuildNewThreadPickerKeyboard(lists, 1, 0);
        CHECK(rows.size() == 13, "5 + pager + 5 + pager + cancel rows");
        if (rows.size() == 13)
        {
            CHECK(rows[0].size() == 2 && rows[0][0].text == L"\U0001F558 11 \x00B7 f10" && rows[0][0].data == L"nt:r:10" && rows[0][1].data == L"nt:r:11", "Recent's folders two to a row, by global index");
            CHECK(rows[5].size() == 3 && rows[5][0].data == L"nt:rp:0" && rows[5][0].text == L"\x25C0 Recent" && rows[5][1].text == L"2 / 3" && rows[5][1].data == L"nt:rl:1" &&
                      rows[5][2].data == L"nt:rp:2" && rows[5][2].text == L"Recent \x25B6",
                  "Recent's page 2 of 3: both arrows around its page label");
            CHECK(rows[6][0].text == L"\U0001F3C6 1 \x00B7 f05" && rows[6][0].data == L"nt:m:0", "Most used's first folder");
            CHECK(rows[11].size() == 2 && rows[11][0].text == L"1 / 3" && rows[11][0].data == L"nt:ml:0" && rows[11][1].data == L"nt:mp:1" && rows[11][1].text == L"Most used \x25B6",
                  "Most used's first page: its label, then the arrow forward");
            CHECK(rows[12].size() == 1 && rows[12][0].data == L"nt:x", "Cancel last");
        }
        const auto lastRows = BuildNewThreadPickerKeyboard(lists, 2, 2);
        CHECK(lastRows.size() == 7, "both last pages (folders 21-24): 2 + pager + 2 + pager + cancel rows");
        if (lastRows.size() == 7)
        {
            CHECK(lastRows[2].size() == 2 && lastRows[2][0].text == L"\x25C0 Recent" && lastRows[2][0].data == L"nt:rp:1" && lastRows[2][1].text == L"3 / 3" && lastRows[2][1].data == L"nt:rl:2",
                  "Recent's last page: the arrow back, then its label");
            CHECK(lastRows[5].size() == 2 && lastRows[5][0].text == L"\x25C0 Most used" && lastRows[5][1].text == L"3 / 3" && lastRows[5][1].data == L"nt:ml:2",
                  "Most used's last page: the arrow back, then its label");
        }
        const NewThreadLists few = BuildNewThreadLists({ NtFacts(L"K:\\a\\one", L"", now), NtFacts(L"K:\\b\\one", L"", now - 1), NtFacts(L"K:\\c\\two", L"", now - 2) }, {}, nullptr);
        const auto fewRows = BuildNewThreadPickerKeyboard(few, 0, 0); // (never "small": a <windows.h> macro)
        CHECK(fewRows.size() == 5, "3 folders: 2 rows + 2 rows + cancel, no paging rows");
        if (fewRows.size() == 5)
        {
            CHECK(fewRows[0][0].text == L"\U0001F558 1 \x00B7 a\\one" && fewRows[0][1].text == L"\U0001F558 2 \x00B7 b\\one" && fewRows[1][0].text == L"\U0001F558 3 \x00B7 two",
                  "shown folders with one name get their parent; the others their name");
        }
        CHECK(BuildNewThreadPickerKeyboard(NewThreadLists{}, 0, 0).size() == 1, "no folders: Cancel only");
    }

    // ===== 2. the user's own words ============================================================================================
    {
        CHECK(NewThreadTypedFolder(L"  \"K:/src/x\" ") == std::optional<std::wstring>{ L"K:\\src\\x" }, "a quoted forward-slash path is a folder");
        CHECK(NewThreadTypedFolder(L"\\\\server\\share\\x") == std::optional<std::wstring>{ L"\\\\server\\share\\x" }, "a UNC path is a folder");
        CHECK(!NewThreadTypedFolder(L"fix the build") && !NewThreadTypedFolder(L"K:\\a\nmore") && !NewThreadTypedFolder(L"src\\x") && !NewThreadTypedFolder(L""),
              "a prompt, two lines, a relative path, nothing: no folder");
        CHECK(StripTopicNameDecoration(L"\U0001F7E1 \U0001F7E6 my title") == L"my title" && StripTopicNameDecoration(L"my title") == L"my title", "glyph and square go; a bare name stays");
        CHECK(StripTopicNameDecoration(L"\U0001F535 x") == L"x" && StripTopicNameDecoration(L"\U0001F7E6 x") == L"x" && StripTopicNameDecoration(L"\x26AA ") == L"", "each part alone; nothing left");
        CHECK(StripTopicNameDecoration(L"\U0001F7E6\U0001F7E6 x") == L"\U0001F7E6 x" && StripTopicNameDecoration(L"  plain  ") == L"plain", "one square only; edges trimmed");
    }

    // ===== 2. the registry's lean read ========================================================================================
    {
        SessionRegistry reg;
        SessionInfo a = MakeSession(L"a1");
        a.workingDir = L"K:\\x";
        a.inferredWorkingDir = L"K:\\y";
        a.lastActivityUnixMs = 5;
        a.convLastActivityUnixMs = 9;
        reg.Upsert(a);
        SessionInfo b = MakeSession(L"b2");
        b.workingDir = L"K:\\z";
        b.lastActivityUnixMs = 7;
        b.live = false; // archived records count ("top 10 used overall")
        reg.Upsert(b);
        const auto samples = reg.PlaceSamples();
        CHECK(samples.size() == 2 && samples[0].workingDir == L"K:\\x" && samples[0].inferredWorkingDir == L"K:\\y" && samples[0].lastActivityUnixMs == 9 &&
                  samples[1].workingDir == L"K:\\z" && samples[1].lastActivityUnixMs == 7,
              "PlaceSamples: every record in insertion order, the newer activity stamp");
    }
}
