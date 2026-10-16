// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §5.4 / §21.7: the 🔎 Search topic's PURE rules (MessagingModels.h). The user's three
// rules of 2026-10-16 night are pinned here:
//   1. the Search conversation is one for the whole app and can be CLEARED like the Sessions page's (/clear, /reset);
//   2. it is not a session: nothing in the Search topic wakes, resumes, forks or pins it;
//   3. an answer's buttons PICK a session, and a second menu asks Resume / Fork (no fork-and-pin).
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
}
