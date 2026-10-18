// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §5.4 / §21.7: the 🔎 Search topic's PURE rules (MessagingModels.h). The user's three
// rules of 2026-10-16 night are pinned here:
//   1. the Search conversation is one for the whole app and can be CLEARED like the Sessions page's (/clear, /reset);
//   2. it is not a session: nothing in the Search topic wakes, resumes, forks or pins it;
//   3. an answer's buttons PICK a session, and a second menu asks Resume / Fork (no fork-and-pin).
// And (§21.10) the user's answer format of 2026-10-17, "C1": the model's verdict, then one EVIDENCE CARD per session
// with the model's why as its one quote, and the pick menu that repeats the card in full (section 5).
// The agent itself (MessagingSearchBridge.cpp) is app-only (it needs the SDK unity file), so the bridge's
// one-conversation lifetime is checked by review and live, not here.

#include "m5_tests.h"

#include "../MessagingModels.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

void TestMessagingSearch()
{
    wprintf(L"== TestMessagingSearch (MESSAGING.md 5.4 / 21.7: the Search topic's verbs, pick buttons, where a picked session stands) ==\n");

    // ---- 1. what a message in the Search topic is ----
    {
        const auto request = ClassifySearchTopicMessage(L"  the session where I fixed the tab drag crash ");
        CHECK(request.verb == SearchTopicVerb::Request, "plain words are a search request");
        CHECK(request.cmd.text == L"the session where I fixed the tab drag crash", "the request's words are trimmed (what the agent is asked)");

        // /clear and /reset clear THE SEARCH conversation in this topic, in any case, with a bot suffix, with words after.
        CHECK(ClassifySearchTopicMessage(L"/clear").verb == SearchTopicVerb::Clear, "/clear clears the search conversation");
        CHECK(ClassifySearchTopicMessage(L"/reset").verb == SearchTopicVerb::Clear, "/reset clears it too (the old help text's word)");
        CHECK(ClassifySearchTopicMessage(L"/CLEAR@ClaudeSessionsGateBot").verb == SearchTopicVerb::Clear, "a bot suffix and upper case still clear");
        CHECK(ClassifySearchTopicMessage(L"  /reset now please").verb == SearchTopicVerb::Clear, "words after /reset do not change it");
        // ...which is why the Search topic must read them itself: everywhere else they are Claude Code's own commands.
        CHECK(ParseRemoteCommand(L"/reset").kind == RemoteKind::ClaudeCommand, "ParseRemoteCommand reads /reset as Claude Code's (a session's topic queues it)");
        CHECK(ParseRemoteCommand(L"/clear").kind == RemoteKind::ClaudeCommand, "ParseRemoteCommand reads /clear as Claude Code's (a session's topic queues it)");
        CHECK(ClassifySearchTopicMessage(L"/clearance").verb == SearchTopicVerb::ClaudeCommand, "a longer word that starts with clear is not /clear");

        // The commands the Search topic runs.
        const wchar_t* fleet[] = { L"/help", L"/start", L"/status", L"/idle", L"/board", L"/newsession K:\\source\\x opus", L"/pauseall", L"/resumeall" };
        for (const auto* text : fleet)
        {
            CHECK(ClassifySearchTopicMessage(text).verb == SearchTopicVerb::FleetCommand, "a fleet command runs in the Search topic");
        }
        const auto ns = ClassifySearchTopicMessage(L"/newsession \"K:\\my folder\" sonnet");
        CHECK(ns.verb == SearchTopicVerb::FleetCommand && ns.cmd.name == L"newsession" && ns.cmd.args == L"\"K:\\my folder\" sonnet", "/newsession keeps its arguments for the launch");

        // The user's rule 2: the Search conversation is not a session — every session verb is refused here.
        const wchar_t* sessionVerbs[] = { L"/wake", L"/pin", L"/unpin", L"/forksession", L"/forksession opus", L"/reactivate", L"/activate",
                                          L"/restartsession", L"/deactivate", L"/transcript max", L"/summary", L"/queue", L"/rename x" };
        for (const auto* text : sessionVerbs)
        {
            CHECK(ClassifySearchTopicMessage(text).verb == SearchTopicVerb::SessionCommand, "a session's command is refused in the Search topic");
        }
        const auto wake = ClassifySearchTopicMessage(L"/wake");
        CHECK(wake.cmd.name == L"wake", "the refusal can name the command that was typed");
        // /cc has no session to pass through to here, whatever follows it — and a forced "/cc /clear" is not a clear.
        const auto ccClear = ClassifySearchTopicMessage(L"/cc /clear");
        CHECK(ccClear.verb == SearchTopicVerb::SessionCommand && ccClear.cmd.name == L"cc", "/cc /clear is /cc (a session's command), not a clear");
        const auto ccWords = ClassifySearchTopicMessage(L"/cc find the crash");
        CHECK(ccWords.verb == SearchTopicVerb::SessionCommand && ccWords.cmd.name == L"cc", "/cc words is /cc, not a search for the words");

        // Claude Code's commands (recorded, no record, leaving the tab) have no session in this topic.
        CHECK(ClassifySearchTopicMessage(L"/compact").verb == SearchTopicVerb::ClaudeCommand, "a recorded Claude Code command is refused here");
        CHECK(ClassifySearchTopicMessage(L"/reload-plugins").verb == SearchTopicVerb::ClaudeCommand, "a no-record Claude Code command is refused here");
        CHECK(ClassifySearchTopicMessage(L"/resume 1e0f7ab2").verb == SearchTopicVerb::ClaudeCommand, "Claude Code's /resume is refused here (Resume is a pick's menu)");
        CHECK(ClassifySearchTopicMessage(L"/fork").verb == SearchTopicVerb::ClaudeCommand, "Claude Code's /fork is refused here (Fork is a pick's menu)");
    }

    // ---- 2. the pick buttons' callback data ----
    {
        const std::wstring uuid = L"0b1f8635-4c6e-4b5a-9a1f-3c2d1e0f7ab2";
        const std::wstring data = SearchPickCallbackData(3, uuid);
        CHECK(data == L"p:3:" + uuid, "a pick carries its row number and the session id");
        CHECK(data.size() <= 64, "a pick's callback data fits Telegram's 64 bytes (ASCII: one byte per character)");
        CHECK(SearchPickCallbackData(99, uuid).size() <= 64, "two digits still fit");
        const auto back = ParseSearchPickCallbackData(data);
        CHECK(back.has_value() && back->number == 3 && back->sessionId == uuid, "a pick reads back");
        CHECK(SearchPickCallbackData(0, uuid).empty() && SearchPickCallbackData(100, uuid).empty(), "a row number outside 1..99 gets no button");
        CHECK(SearchPickCallbackData(1, L"short").empty(), "an id that is too short gets no button");
        CHECK(SearchPickCallbackData(1, L"0b1f8635:4c6e").empty(), "an id with a foreign character gets no button");
        CHECK(SearchPickCallbackData(1, std::wstring(41, L'a')).empty(), "an id longer than 40 characters gets no button");
        // Callback data comes from Telegram: anything malformed is not a pick.
        const wchar_t* bad[] = { L"", L"p:", L"p:3", L"p:3:", L"p::0b1f8635", L"p:0:0b1f8635", L"p:123:0b1f8635", L"p:x:0b1f8635",
                                 L"p:3:0b1f8635/../x", L"p:3:0b1f863", L"q:3:0b1f8635", L"c:pin:0b1f8635", L"t:abc", L"s:clear" };
        for (const auto* d : bad)
        {
            CHECK(!ParseSearchPickCallbackData(d).has_value(), "malformed or foreign callback data is not a pick");
        }
        CHECK(kSearchClearCallbackData.size() <= 64 && !ParseSearchPickCallbackData(kSearchClearCallbackData).has_value(), "the Clear button's data is no pick");
    }

    // ---- 3. the pick keyboard: numbers, five to a row, at most ten ----
    {
        CHECK(SearchPickKeyboardLayout(0).empty(), "no rows, no pick buttons");
        const auto three = SearchPickKeyboardLayout(3);
        CHECK(three.size() == 1 && three[0] == std::vector<size_t>({ 1, 2, 3 }), "three rows: one line of three numbers");
        const auto five = SearchPickKeyboardLayout(5);
        CHECK(five.size() == 1 && five[0].size() == 5, "five fit one line");
        const auto seven = SearchPickKeyboardLayout(7);
        CHECK(seven.size() == 2 && seven[0].size() == 5 && seven[1] == std::vector<size_t>({ 6, 7 }), "the sixth starts a second line");
        const auto many = SearchPickKeyboardLayout(37);
        CHECK(many.size() == 2 && many[1].back() == kSearchAnswerMaxRows, "at most kSearchAnswerMaxRows picks (the answer says how many more)");
    }

    // ---- 4. where a picked session stands (the row's word, the menu's line, what Resume does) ----
    {
        CHECK(ClassifySearchPickState(false, false, false, SessionState::Idle, false) == SearchPickState::Closed, "a session the registry never knew is closed");
        CHECK(ClassifySearchPickState(true, false, true, SessionState::WaitingForInput, false) == SearchPickState::Closed, "an archived session is closed");
        CHECK(ClassifySearchPickState(true, true, false, SessionState::Running, false) == SearchPickState::Closed, "a live record without a tab is closed");
        CHECK(ClassifySearchPickState(true, true, true, SessionState::Idle, false) == SearchPickState::IdleTab, "an open, idle, unpinned tab is an idle tab (Resume wakes it)");
        CHECK(ClassifySearchPickState(true, true, true, SessionState::Idle, true) == SearchPickState::Listed, "a pinned tab never idles: it has its topic");
        const SessionState listedStates[] = { SessionState::Running, SessionState::WaitingForInput, SessionState::NeedsApproval, SessionState::Error, SessionState::Done };
        for (const auto st : listedStates)
        {
            CHECK(ClassifySearchPickState(true, true, true, st, false) == SearchPickState::Listed, "every state but Idle is listed (it has its topic)");
        }
        CHECK(std::wstring{ SearchPickStateWord(SearchPickState::Closed) } == L"closed" && std::wstring{ SearchPickStateWord(SearchPickState::IdleTab) } == L"idle tab" && std::wstring{ SearchPickStateWord(SearchPickState::Listed) } == L"has a topic", "the answer's row words are the old ones");
        const std::wstring lines[] = { SearchPickStateLine(SearchPickState::Closed), SearchPickStateLine(SearchPickState::IdleTab), SearchPickStateLine(SearchPickState::Listed) };
        CHECK(!lines[0].empty() && lines[0] != lines[1] && lines[1] != lines[2] && lines[0] != lines[2], "each state has its own menu line");
        CHECK(lines[0].find(L"Resume") != std::wstring::npos && lines[1].find(L"Resume") != std::wstring::npos && lines[2].find(L"Resume") != std::wstring::npos, "every menu line says what Resume does there");
    }

    // ---- 5. the answer as EVIDENCE CARDS (§21.10, the user's "C1" of 2026-10-17) ----
    {
        // Local wall-clock instants, so the days and clock times below are the same on every machine.
        const auto localMs = [](int year, int month, int day, int hour, int minute) {
            std::tm t{};
            t.tm_year = year - 1900;
            t.tm_mon = month - 1;
            t.tm_mday = day;
            t.tm_hour = hour;
            t.tm_min = minute;
            t.tm_isdst = -1;
            return static_cast<int64_t>(std::mktime(&t)) * 1000;
        };
        const int64_t now = localMs(2026, 10, 17, 12, 0);

        // The day a card names, and when a menu says it ran.
        CHECK(SearchDayText(localMs(2026, 10, 17, 9, 30), now) == L"today" && SearchDayText(localMs(2026, 10, 16, 23, 59), now) == L"yesterday", "card day: today, yesterday");
        CHECK(SearchDayText(localMs(2026, 10, 11, 14, 2), now) == L"11 Oct" && SearchDayText(localMs(2025, 10, 11, 14, 2), now) == L"11 Oct 2025" && SearchDayText(0, now).empty(), "card day: day first, the year only when it is not this one, nothing when unknown");
        CHECK(SearchDayText(localMs(2026, 1, 1, 10, 0), localMs(2026, 1, 2, 8, 0)) == L"yesterday" && SearchDayText(localMs(2025, 12, 31, 10, 0), localMs(2026, 1, 1, 8, 0)) == L"yesterday", "card day: yesterday across a month and a year");
        CHECK(SearchSpanText(localMs(2026, 10, 11, 14, 2), localMs(2026, 10, 11, 16, 40), now) == L"11 Oct 14:02 \x2192 16:40", "menu span: one day names its date once");
        CHECK(SearchSpanText(localMs(2026, 10, 10, 9, 0), localMs(2026, 10, 11, 16, 40), now) == L"10 Oct 09:00 \x2192 11 Oct 16:40", "menu span: across days, both dates");
        CHECK(SearchSpanText(localMs(2026, 10, 17, 10, 0), localMs(2026, 10, 17, 11, 0), now) == L"10:00 \x2192 11:00", "menu span: today, clock times only");
        CHECK(SearchSpanText(0, localMs(2026, 10, 11, 16, 40), now) == L"active 11 Oct 16:40" && SearchSpanText(localMs(2026, 10, 11, 14, 2), 0, now) == L"11 Oct 14:02" && SearchSpanText(0, 0, now).empty(), "menu span: what is known of it");

        // The answer: the verdict, then one card per session — number, glyph, title; folder · day · prompts; the quote.
        SearchCard release;
        release.number = 1;
        release.sessionId = L"a1a1a1a1-0000-4000-8000-000000000001";
        release.title = L"Release v0.7.9";
        release.dir = L"K:\\source\\Agentmaster\\";
        release.branch = L"agentmaster";
        release.createdMs = localMs(2026, 10, 11, 14, 2);
        release.lastActivityMs = localMs(2026, 10, 11, 16, 40);
        release.prompts = 14;
        release.why = L"You asked: \"cut v0.7.9 as a pre-release\"";
        release.state = SearchPickState::Closed;
        SearchCard bare;
        bare.number = 2;
        bare.sessionId = L"b2b2b2b2-0000-4000-8000-000000000002";
        bare.prompts = 1;
        bare.state = SearchPickState::IdleTab;
        const std::wstring answer = TgRenderTitleTokensClassic(BuildSearchAnswerHtml(L"Two sessions are about <releasing> Agentmaster.", { release, bare }, 3, now)); // §21.15: the classic render
        CHECK(answer == L"Two sessions are about &lt;releasing&gt; Agentmaster.\n\n"
                        L"<b>1</b> \U0001F4E6 <b>Release v0.7.9</b>\nAgentmaster \x00B7 11 Oct \x00B7 14 prompts\n<blockquote>You asked: \"cut v0.7.9 as a pre-release\"</blockquote>\n\n"
                        L"<b>2</b> \U0001F4A4 <b>b2b2b2b2</b>\n1 prompt\n\n"
                        L"<i>(3 more not listed: ask to narrow it down.)</i>",
              "answer: the verdict escaped, a card per session with its one quote, the short id for a session with no title, and the count not listed");
        CHECK(BuildSearchAnswerHtml(L"", {}, 0, now) == L"Nothing found." && BuildSearchAnswerHtml(L"No session is about that.", {}, 0, now) == L"No session is about that.", "answer: nothing at all says so; a verdict alone is the answer");
        SearchCard escaped = release;
        escaped.title = L"a <b>bold</b> & title";
        escaped.why = L"Claude: \"x < y & z\"";
        const std::wstring escapedAnswer = TgRenderTitleTokensClassic(BuildSearchAnswerHtml(L"", { escaped }, 0, now));
        CHECK(escapedAnswer.find(L"a &lt;b&gt;bold&lt;/b&gt; &amp; title") != std::wstring::npos && escapedAnswer.find(L"x &lt; y &amp; z") != std::wstring::npos && escapedAnswer.rfind(L"<b>1</b>", 0) == 0,
              "answer: a title and a quote are escaped (the model's text is never markup)");
        CHECK(std::wstring{ SearchPickStateGlyph(SearchPickState::Closed) } != SearchPickStateGlyph(SearchPickState::IdleTab) && std::wstring{ SearchPickStateGlyph(SearchPickState::IdleTab) } != SearchPickStateGlyph(SearchPickState::Listed),
              "answer: each state has its own glyph");

        // The pick menu: the whole folder and the branch, when it ran, the quote, where it stands now.
        CHECK(TgRenderTitleTokensClassic(BuildSearchPickMenuHtml(release, now)) == L"<b>1 \x00B7 Release v0.7.9</b>\nK:\\source\\Agentmaster\\ \x00B7 agentmaster\n11 Oct 14:02 \x2192 16:40 \x00B7 14 prompts\n<blockquote>You asked: \"cut v0.7.9 as a pre-release\"</blockquote>\n"
                                                L"<i>\U0001F4E6 Closed: Resume opens it in a background tab, and its topic appears.</i>",
              "pick menu: the card's facts in full, its quote, and the state line");
        CHECK(TgRenderTitleTokensClassic(BuildSearchPickMenuHtml(bare, now)) == L"<b>2 \x00B7 b2b2b2b2</b>\n1 prompt\n<i>\U0001F4A4 Open in a tab, idle: Resume brings its topic back.</i>", "pick menu: a session known only by its id (a pick after a restart) shows what there is");

        // §21.13: a card and a menu name the session with its tab's color square, like every title mention ("State, then
        // color" on the card, as in the topic names); a session with no color reads as above.
        SearchCard colored = release;
        colored.colorSquare = L"\U0001F7E6";
        CHECK(TgRenderTitleTokensClassic(BuildSearchAnswerHtml(L"", { colored }, 0, now)).rfind(L"<b>1</b> \U0001F4E6 \U0001F7E6 <b>Release v0.7.9</b>\n", 0) == 0, "answer: the glyph, then the color square, then the title");
        CHECK(TgRenderTitleTokensClassic(BuildSearchPickMenuHtml(colored, now)).rfind(L"<b>1 \x00B7 \U0001F7E6 Release v0.7.9</b>\n", 0) == 0, "pick menu: the color square before the title");

        // §21.15: in a rich answer the title is a chip in the tab's look and the quote stays a block; the menu's head keeps its
        // number in bold, then the chip.
        CHECK(TgRichHtmlFromClassic(BuildSearchAnswerHtml(L"", { colored }, 0, now)) ==
                  L"<p><b>1</b> \U0001F4E6 <tg-button type=\"copy_text\" style=\"primary\" text=\"Release v0.7.9\">\U0001F7E6 Release v0.7.9</tg-button><br>Agentmaster \x00B7 11 Oct \x00B7 14 prompts</p>"
                  L"<blockquote>You asked: \"cut v0.7.9 as a pre-release\"</blockquote>",
              "answer, rich: the number, the glyph, the chip; the folder line; the quote as a block");
        CHECK(TgRichHtmlFromClassic(BuildSearchPickMenuHtml(colored, now)).rfind(L"<p><b>1 \x00B7 </b><tg-button type=\"copy_text\" style=\"primary\" text=\"Release v0.7.9\">\U0001F7E6 Release v0.7.9</tg-button><br>", 0) == 0,
              "pick menu, rich: the number in bold, then the chip");
    }

    // ---- §21.19: /search-tab jumps to the Search topic and /search sends a search there, from any topic ----
    {
        // /search-tab: the user's spelling and the "/" menu's (search_tab), in any case, with a bot suffix, with words after it.
        CHECK(IsSearchTabCommand(L"/search-tab") && IsSearchTabCommand(L"/search_tab"), "/search-tab and /search_tab are the jump");
        CHECK(IsSearchTabCommand(L"  /SEARCH-TAB@ClaudeSessionsGateBot  ") && IsSearchTabCommand(L"/search_tab now please"), "the jump in any case, with a bot suffix, with words after it");
        CHECK(!IsSearchTabCommand(L"/search") && !IsSearchTabCommand(L"/search the crash") && !IsSearchTabCommand(L"/searchtab") && !IsSearchTabCommand(L"search-tab") &&
                  !IsSearchTabCommand(L""),
              "not the jump: /search (its own command), a third spelling, plain words, nothing");
        CHECK(!IsSearchTabCommand(L"/cc /search-tab"), "/cc forces /search-tab through to Claude Code (the user's choice)");
        // A session's topic reads all three as Agentmaster's own, never as Claude Code commands to queue.
        CHECK(ParseRemoteCommand(L"/search-tab").kind == RemoteKind::AgentmasterCommand && ParseRemoteCommand(L"/search_tab").kind == RemoteKind::AgentmasterCommand &&
                  ParseRemoteCommand(L"/search the crash").kind == RemoteKind::AgentmasterCommand,
              "a session's topic reads /search-tab, /search_tab and /search as Agentmaster's commands");

        // /search <words>: its words (the rest of the line and every further line, trimmed); "" for a bare /search.
        CHECK(SearchCommandWords(L"/search the session where I fixed the tab drag crash") == std::optional<std::wstring>{ L"the session where I fixed the tab drag crash" },
              "/search: its words");
        CHECK(SearchCommandWords(L"/Search@ClaudeSessionsGateBot   drag crash  \n  in the strip ") == std::optional<std::wstring>{ L"drag crash  \n  in the strip" },
              "/search in any case, with a bot suffix: every line, trimmed");
        CHECK(SearchCommandWords(L"/search") == std::optional<std::wstring>{ L"" }, "a bare /search has no words (it is the jump)");
        CHECK(!SearchCommandWords(L"/search-tab") && !SearchCommandWords(L"/searching x") && !SearchCommandWords(L"search x") && !SearchCommandWords(L"/cc /search x"),
              "not /search: the jump, a longer word, plain words, a forced /cc");

        // The tables: every name of both commands is Agentmaster's in a session's topic and a fleet command in the Search topic.
        std::vector<std::wstring_view> names{ std::begin(kSearchTabCommands), std::end(kSearchTabCommands) };
        names.push_back(kSearchCommand);
        for (const std::wstring_view name : names)
        {
            CHECK(::Agentmaster::Msg::detail::InList(name, std::begin(kAgentmasterCommands), std::end(kAgentmasterCommands)), "each name is Agentmaster's in a session's topic (never queued for Claude Code)");
            CHECK(::Agentmaster::Msg::detail::InList(name, std::begin(kSearchTopicFleetCommands), std::end(kSearchTopicFleetCommands)), "each name is a fleet command in the Search topic");
        }

        // In the Search topic: the jump (and a bare /search) is a fleet command; "/search <words>" is a request of the words alone.
        CHECK(ClassifySearchTopicMessage(L"/search-tab").verb == SearchTopicVerb::FleetCommand && ClassifySearchTopicMessage(L"/search_tab").verb == SearchTopicVerb::FleetCommand &&
                  ClassifySearchTopicMessage(L"/search").verb == SearchTopicVerb::FleetCommand,
              "in the Search topic the jump and a bare /search are fleet commands");
        const auto here = ClassifySearchTopicMessage(L"/search the tab drag crash");
        CHECK(here.verb == SearchTopicVerb::Request && here.cmd.text == L"the tab drag crash", "/search <words> in the Search topic is a request: the agent is asked the words, not the command");

        // The relayed request as the bot posts it in the Search topic: You, the clock, where it was asked; the topic it came
        // from (a title token); the words, escaped — and never a "/search" a tap would send.
        const std::wstring row = UserLineTitleRow(L"Agentmaster/feature", L"sid-1", L"\U0001F7E6", 26785);
        const std::wstring line = TgRenderTitleTokensClassic(BuildRelayedSearchLineHtml(1792273200000, L"the <drag> crash", row, false));
        CHECK(line.rfind(L"<b>\U0001F464 You</b> \x00B7 ", 0) == 0 && line.find(L" \x00B7 \U0001F50E search, from a topic\n") != std::wstring::npos, "relayed: You, the clock, where it was asked");
        CHECK(line.find(L"Agentmaster/feature") != std::wstring::npos && line.find(L"\nthe &lt;drag&gt; crash") != std::wstring::npos, "relayed: the topic it came from, then the words, escaped");
        CHECK(line.find(L"/search") == std::wstring::npos, "relayed: no /search a tap would send again");
        CHECK(BuildRelayedSearchLineHtml(0, L"x", L"", true) == L"<b>\U0001F464 You</b> \x00B7 \U0001F50E search, from the main view\nx", "relayed from the main view: no clock when unknown, no row");
    }
}
