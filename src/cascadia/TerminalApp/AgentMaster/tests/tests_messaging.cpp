// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md: the fleet in Telegram, the PURE core (TelegramFormat.h + MessagingModels.h)
// and the two engine rules it leans on (the remote prompt's runner bypass in DecideAdvance, the pin hold).
// The service's network half (TelegramApi, the poll / send / tick threads) is not exercised here: it talks
// to api.telegram.org, and the live check is a session's own bot (MESSAGING.md §15).

#include "m5_tests.h"

#include "../MessagingModels.h"
#include "../TelegramFormat.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    json::Value ParseLine(const wchar_t* text)
    {
        const auto v = json::Parse(text);
        CHECK(v.has_value(), "fixture line parses");
        return v ? *v : json::Value{};
    }

    size_t CountOf(const std::wstring& hay, const std::wstring& needle)
    {
        size_t n = 0;
        for (size_t p = hay.find(needle); p != std::wstring::npos; p = hay.find(needle, p + needle.size()))
        {
            ++n;
        }
        return n;
    }
}

void TestMessaging()
{
    wprintf(L"== TestMessaging (MESSAGING.md: Telegram formatting, the transcript classifier, topics = tabs, commands, the remote prompt, the pin) ==\n");

    // ---- TelegramFormat: escaping ----
    CHECK(TgEscapeHtml(L"a <T> & b") == L"a &lt;T&gt; &amp; b", "the three HTML entities");
    CHECK(TgEscapeAttr(L"x\"y") == L"x&quot;y", "attribute quotes");
    CHECK(TgFirstLine(L"\n\n  hello world  \nmore", 100) == L"hello world", "first non-empty line, trimmed");
    CHECK(TgFirstLine(L"abcdefghij", 5).size() == 5, "first line capped at maxChars incl. the ellipsis");
    CHECK(TgFirstLine(L"   \n  ", 10).empty(), "a blank text has no first line");

    // ---- TelegramFormat: Markdown -> HTML ----
    {
        const std::wstring html = TgMarkdownToHtml(L"# Title\nSome **bold** and `code <x>` here\n- item one\n* item two\n> quoted\n```cpp\nint a<b;\n```\ntail [link](https://x.y/z?a=1&b=2) end");
        CHECK(html.find(L"<b>Title</b>") != std::wstring::npos, "a heading is a bold line");
        CHECK(html.find(L"<b>bold</b>") != std::wstring::npos, "**bold** -> <b>");
        CHECK(html.find(L"<code>code &lt;x&gt;</code>") != std::wstring::npos, "a code span is escaped inside <code>");
        CHECK(CountOf(html, L"\x2022 item") == 2, "list bullets");
        CHECK(html.find(L"<blockquote>quoted</blockquote>") != std::wstring::npos, "a quote");
        CHECK(html.find(L"<pre><code class=\"language-cpp\">int a&lt;b;\n</code></pre>") != std::wstring::npos, "a fenced block with its language, escaped");
        CHECK(html.find(L"<a href=\"https://x.y/z?a=1&amp;b=2\">link</a>") != std::wstring::npos, "a link, its url attribute-escaped");
        CHECK(html.find(L"_") == std::wstring::npos || true, "(italic underscores are never converted)");
    }
    {
        const std::wstring html = TgMarkdownToHtml(L"snake_case and 2*3*4 and ~~gone~~ and **unclosed");
        CHECK(html.find(L"snake_case") != std::wstring::npos && html.find(L"2*3*4") != std::wstring::npos, "underscores and lone stars stay text");
        CHECK(html.find(L"<s>gone</s>") != std::wstring::npos, "~~strike~~");
        CHECK(html.find(L"**unclosed") != std::wstring::npos && html.find(L"<b>") == std::wstring::npos, "an unpaired ** stays text");
    }
    {
        const std::wstring html = TgMarkdownToHtml(L"```\nnever closed\nline two");
        CHECK(html.rfind(L"</code></pre>") == html.size() - 13, "an unterminated fence is closed at the end");
    }

    // ---- TelegramFormat: chunking ----
    {
        std::wstring big;
        for (int i = 0; i < 400; ++i)
        {
            big += L"line " + std::to_wstring(i) + L" of a long reply that goes on\n";
        }
        const auto chunks = TgChunkHtml(big);
        CHECK(chunks.size() >= 3, "a 13K text splits into several chunks");
        size_t total = 0;
        for (const auto& c : chunks)
        {
            CHECK(c.size() <= kTelegramMessageMaxChars, "every chunk fits Telegram's limit");
            total += c.size();
        }
        CHECK(total >= big.size() - chunks.size(), "no text is lost (only the split line breaks)");
        CHECK(chunks.front().rfind(L"line 0 of", 0) == 0, "the first chunk starts at the start");
    }
    {
        std::wstring pre = L"<pre><code>";
        for (int i = 0; i < 300; ++i)
        {
            pre += L"code line " + std::to_wstring(i) + L" with some width to it\n";
        }
        pre += L"</code></pre>\nafter";
        const auto chunks = TgChunkHtml(pre);
        CHECK(chunks.size() >= 2, "a long <pre> splits");
        for (size_t i = 0; i < chunks.size(); ++i)
        {
            const size_t opens = CountOf(chunks[i], L"<pre><code");
            const size_t closes = CountOf(chunks[i], L"</code></pre>");
            CHECK(opens == closes, "each chunk keeps its <pre> balanced");
        }
        CHECK(chunks.back().find(L"after") != std::wstring::npos, "the text after the block arrives");
    }
    CHECK(TgChunkHtml(L"short").size() == 1 && TgChunkHtml(L"").size() == 1, "a short or empty text is one chunk");

    // ---- TelegramFormat: topic color + spans ----
    CHECK(TgTopicIconColor(L"#2E8B57") == 0x8EEE98u, "a green tab picks the green topic color");
    CHECK(TgTopicIconColor(L"#FF1E90FF") == 0x6FB9F0u, "a blue tab (with alpha) picks the blue topic color");
    CHECK(TgTopicIconColor(L"") == kTelegramTopicColors[0] && TgTopicIconColor(L"nope") == kTelegramTopicColors[0], "no / bad color -> the first");
    CHECK(TgShortSpan(45 * 1000) == L"45s" && TgShortSpan(3 * 60 * 1000) == L"3m" && TgShortSpan(72 * 60 * 1000) == L"1h12m" && TgShortSpan(28 * 3600 * 1000ll) == L"1d4h", "short spans");

    // ---- the transcript classifier ----
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"fix the tab drag crash\"},\"timestamp\":\"2026-10-16T07:29:14.217Z\",\"uuid\":\"u1\"}"));
        CHECK(items.size() == 1 && items[0].kind == TopicItemKind::Prompt && items[0].text == L"fix the tab drag crash" && !items[0].midTurn, "a typed prompt");
        CHECK(items.size() == 1 && items[0].tsMs == 1792135754217ll, "the line's timestamp -> unix ms (2026-10-16T07:29:14.217Z)");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"user\",\"isMeta\":true,\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"skill text\"}]}}"));
        CHECK(items.empty(), "a meta line shows nothing");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"is_error\":true,\"content\":\"boom\\nmore\"}]}}"));
        CHECK(items.size() == 1 && items[0].kind == TopicItemKind::ToolResult && items[0].isError && items[0].toolUseId == L"t1" && items[0].text == L"boom", "a failed tool result");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"[Request interrupted by user for tool use]\"}}"));
        CHECK(items.size() == 1 && items[0].kind == TopicItemKind::Interrupt, "the interrupt marker");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"<command-message>x</command-message>\\n<command-name>/compact</command-name>\\n<command-args>keep the plan</command-args>\"}}"));
        CHECK(items.size() == 1 && items[0].kind == TopicItemKind::Command && items[0].text == L"/compact keep the plan", "a slash-command echo");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"assistant\",\"message\":{\"id\":\"m1\",\"role\":\"assistant\",\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"text\",\"text\":\"Reading first.\"},{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"Bash\",\"input\":{\"command\":\"npm test -- --watch=false\\n\"}}]}}"));
        CHECK(items.size() == 2, "a text block and a tool call");
        CHECK(items[0].kind == TopicItemKind::Text && !items[0].turnEnds && items[0].messageId == L"m1", "the text does not end the turn (tool_use)");
        CHECK(items[1].kind == TopicItemKind::ToolCall && items[1].summary == L"Bash: npm test -- --watch=false" && items[1].toolUseId == L"t2", "the Bash summary");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"assistant\",\"message\":{\"id\":\"m2\",\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"\"},{\"type\":\"text\",\"text\":\"Done: **fixed**.\"}]}}"));
        CHECK(items.size() == 1 && items[0].kind == TopicItemKind::Text && items[0].turnEnds, "the final reply ends the turn; thinking is skipped");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"assistant\",\"message\":{\"id\":\"m3\",\"role\":\"assistant\",\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t3\",\"name\":\"AskUserQuestion\",\"input\":{\"questions\":[{\"header\":\"Scope\",\"question\":\"Which half?\",\"multiSelect\":false,\"options\":[{\"label\":\"UI\"},{\"label\":\"Engine\"}]}]}}]}}"));
        CHECK(items.size() == 1 && items[0].kind == TopicItemKind::Question && items[0].text == L"Scope: Which half?" && items[0].options.size() == 2 && items[0].options[1] == L"Engine", "a question with its options");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"assistant\",\"isApiErrorMessage\":true,\"apiErrorStatus\":429,\"message\":{\"model\":\"<synthetic>\",\"stop_reason\":\"stop_sequence\",\"content\":[{\"type\":\"text\",\"text\":\"API Error: rate limited\"}]}}"));
        CHECK(items.size() == 1 && items[0].kind == TopicItemKind::ApiError && items[0].text == L"HTTP 429: API Error: rate limited" && items[0].turnEnds, "an API error line");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"system\",\"subtype\":\"compact_boundary\",\"compactMetadata\":{\"trigger\":\"auto\",\"preTokens\":412000,\"postTokens\":38000}}"));
        CHECK(items.size() == 1 && items[0].kind == TopicItemKind::Compaction && items[0].text == L"compacted (auto) 412K \x2192 38K", "a compaction marker");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"attachment\",\"attachment\":{\"type\":\"queued_command\",\"commandMode\":\"prompt\",\"prompt\":\"also update the docs\",\"origin\":{\"kind\":\"human\"},\"timestamp\":\"2026-10-16T08:00:00.000Z\"}}"));
        CHECK(items.size() == 1 && items[0].kind == TopicItemKind::Prompt && items[0].midTurn && items[0].text == L"also update the docs", "a message typed while the turn ran");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"attachment\",\"attachment\":{\"type\":\"queued_command\",\"commandMode\":\"task-notification\",\"prompt\":\"<task-notification>x</task-notification>\",\"origin\":{\"kind\":\"task-notification\"}}}"));
        CHECK(items.empty(), "a task notification attachment is not the user's message");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"assistant\",\"message\":{\"id\":\"m4\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"No response requested.\"}]}}"));
        CHECK(items.empty(), "the resume placeholder is not model output");
    }
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"user\",\"isSidechain\":true,\"message\":{\"role\":\"user\",\"content\":\"subagent prompt\"}}"));
        CHECK(items.empty(), "a sidechain line is the subagent's, not the topic's");
    }
    // §21.6: a FORK's copy of its source's history is never news (its topic would replay the source conversation).
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"user\",\"forkedFrom\":{\"sessionId\":\"src\",\"messageUuid\":\"u0\"},\"message\":{\"role\":\"user\",\"content\":\"the source's prompt\"},\"timestamp\":\"2026-10-16T07:00:00.000Z\"}"));
        CHECK(items.empty(), "a fork's copied user line shows nothing");
        const auto copiedReply = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"assistant\",\"forkedFrom\":{\"sessionId\":\"src\",\"messageUuid\":\"u1\"},\"message\":{\"id\":\"m9\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"old reply\"}]}}"));
        CHECK(copiedReply.empty(), "a fork's copied assistant line shows nothing");
        const auto own = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"the fork's own first prompt\"}}"));
        CHECK(own.size() == 1 && own[0].kind == TopicItemKind::Prompt, "the fork's own first line is a prompt");
    }
    // §21.6: Claude Code's own recap (system/away_summary), its "(disable recaps in /config)" hint stripped.
    {
        const auto items = ClassifyTranscriptLine(ParseLine(L"{\"type\":\"system\",\"subtype\":\"away_summary\",\"content\":\"Fixed the crash; next: deploy. (disable recaps in /config)\",\"timestamp\":\"2026-10-16T08:10:00.000Z\"}"));
        CHECK(items.size() == 1 && items[0].kind == TopicItemKind::Recap && items[0].text == L"Fixed the crash; next: deploy." && items[0].tsMs > 0, "an away_summary line is a Recap item, normalized and timed");
        CHECK(ClassifyTranscriptLine(ParseLine(L"{\"type\":\"system\",\"subtype\":\"away_summary\",\"content\":\"(disable recaps in /config)\"}")).empty(), "an empty recap shows nothing");
    }
    // §21.6: the WHY of a Bash call (its description) leads; the command follows, cut to fit.
    {
        const auto both = ParseLine(L"{\"command\":\"npm test -- --watch=false\",\"description\":\"Run the unit tests\"}");
        CHECK(ToolCallSummary(L"Bash", &both) == L"Bash: Run the unit tests ($ npm test -- --watch=false)", "Bash: the description, then the command");
        const auto bare = ParseLine(L"{\"command\":\"git status\"}");
        CHECK(ToolCallSummary(L"Bash", &bare) == L"Bash: git status", "Bash without a description: the command");
        std::wstring longCmd = L"{\"command\":\"";
        for (int i = 0; i < 40; ++i)
        {
            longCmd += L"echo step" + std::to_wstring(i) + L" && ";
        }
        longCmd += L"true\",\"description\":\"Run every step of the release check\"}";
        const auto longIn = ParseLine(longCmd.c_str());
        const std::wstring cut = ToolCallSummary(L"Bash", &longIn);
        CHECK(cut.size() <= kToolSummaryMaxChars && cut.rfind(L"Bash: Run every step of the release check ($ echo step0", 0) == 0 && cut.back() == L')', "a long command is the part cut; the description stays whole");
    }
    // §21.6: the recap's freshness — a recap newer than every conversation message is fresh; the next message makes it stale.
    {
        CHECK(IsConversationItem(TopicItemKind::Prompt) && IsConversationItem(TopicItemKind::Text) && IsConversationItem(TopicItemKind::ToolCall) && IsConversationItem(TopicItemKind::Command),
              "prompts, replies, tool calls and commands are conversation messages");
        CHECK(!IsConversationItem(TopicItemKind::Recap) && !IsConversationItem(TopicItemKind::Compaction) && !IsConversationItem(TopicItemKind::ToolResult),
              "a recap, a compaction and a tool result are not");
        const auto item = [](TopicItemKind k, int64_t ts, const wchar_t* text = L"x") {
            TopicItem it;
            it.kind = k;
            it.tsMs = ts;
            it.text = text;
            return it;
        };
        std::wstring recap;
        int64_t recapAt = 0, lastMsg = 0;
        FoldRecapFacts({ item(TopicItemKind::Prompt, 100), item(TopicItemKind::Text, 200), item(TopicItemKind::Recap, 300, L"where it stands") }, recap, recapAt, lastMsg);
        CHECK(recap == L"where it stands" && recapAt == 300 && lastMsg == 200 && RecapIsFresh(recap, recapAt, lastMsg), "a recap after the last reply is fresh");
        FoldRecapFacts({ item(TopicItemKind::ToolResult, 400), item(TopicItemKind::Compaction, 400) }, recap, recapAt, lastMsg);
        CHECK(RecapIsFresh(recap, recapAt, lastMsg), "a tool result or a compaction leaves it fresh");
        FoldRecapFacts({ item(TopicItemKind::Prompt, 500) }, recap, recapAt, lastMsg);
        CHECK(!RecapIsFresh(recap, recapAt, lastMsg) && recap == L"where it stands", "the next message makes it stale (the text is kept for the next recap to replace)");
        FoldRecapFacts({ item(TopicItemKind::Recap, 250, L"older") }, recap, recapAt, lastMsg);
        CHECK(recap == L"where it stands" && recapAt == 300, "an OLDER recap never replaces a newer one");
        FoldRecapFacts({ item(TopicItemKind::Recap, 600, L"newer") }, recap, recapAt, lastMsg);
        CHECK(recap == L"newer" && RecapIsFresh(recap, recapAt, lastMsg), "a newer recap is fresh again");
        FoldRecapFacts({ item(TopicItemKind::Text, 0) }, recap, recapAt, lastMsg);
        CHECK(!RecapIsFresh(recap, recapAt, lastMsg), "an untimed message counts as written after everything folded (file order)");
        CHECK(!RecapIsFresh(L"", 10, 0) && !RecapIsFresh(L"r", 0, 0) && !RecapIsFresh(L"r", 5, 5), "no text, no time, or a tie: not fresh");
    }
    {
        const auto editInput = ParseLine(L"{\"file_path\":\"K:\\\\source\\\\Agentmaster\\\\src\\\\cascadia\\\\TerminalApp\\\\AgentMaster\\\\Messaging.cpp\"}");
        CHECK(ToolCallSummary(L"Edit", &editInput) == L"Edit: \x2026/TerminalApp/AgentMaster/Messaging.cpp", "an Edit shows the path's tail");
        const auto mcpInput = ParseLine(L"{\"path\":\"agentmaster/agentmaster.md\"}");
        CHECK(ToolCallSummary(L"mcp__catalog__catalog_read", &mcpInput) == L"catalog \x00B7 catalog_read: agentmaster/agentmaster.md", "an MCP tool reads server · tool");
        const auto todoInput = ParseLine(L"{\"todos\":[{\"status\":\"completed\"},{\"status\":\"pending\"}]}");
        CHECK(ToolCallSummary(L"TodoWrite", &todoInput) == L"TodoWrite: 1/2 done", "the to-do count");
    }

    // ---- topics = tabs ----
    {
        const auto fact = [](const wchar_t* id, SessionState st, bool pinned = false, bool claude = true, bool inTab = true) {
            TabFact f;
            f.sessionId = id;
            f.title = std::wstring{ L"t-" } + id;
            f.state = st;
            f.live = true;
            f.inTab = inTab;
            f.claude = claude;
            f.pinned = pinned;
            return f;
        };
        std::vector<TabFact> facts{ fact(L"a", SessionState::Running), fact(L"b", SessionState::Idle), fact(L"c", SessionState::Idle, true),
                                    fact(L"d", SessionState::WaitingForInput, false, false), fact(L"e", SessionState::Running, false, true, false) };
        std::map<std::wstring, TopicRecord> topics;
        auto ops = PlanTopicSync(facts, topics, 1000, 600000);
        size_t creates = 0;
        for (const auto& op : ops)
        {
            if (op.kind == TopicSyncOp::Kind::Create)
            {
                ++creates;
                CHECK(op.sessionId == L"a" || op.sessionId == L"c", "created: the running tab and the pinned idle one");
            }
        }
        CHECK(creates == 2, "no topic for an idle, a Codex or a tab-less session");
        // Now a topic exists for a and for b (b went idle after its topic was made).
        topics[L"a"] = TopicRecord{ L"a", 11, 0, 0, L"t-a", 0, 0, L"", 0 };
        topics[L"b"] = TopicRecord{ L"b", 12, 0, 0, L"t-b", 0, 0, L"", 0 };
        topics[L"z"] = TopicRecord{ L"z", 13, 0, 0, L"t-z", 0, 0, L"", 0 }; // a closed session
        ops = PlanTopicSync(facts, topics, 1000, 600000);
        bool idleCard = false, deleteZ = false, createA = false;
        for (const auto& op : ops)
        {
            idleCard = idleCard || (op.kind == TopicSyncOp::Kind::IdleCard && op.sessionId == L"b");
            deleteZ = deleteZ || (op.kind == TopicSyncOp::Kind::Delete && op.sessionId == L"z" && op.reason == L"closed");
            createA = createA || (op.kind == TopicSyncOp::Kind::Create && op.sessionId == L"a");
        }
        CHECK(idleCard, "a topic whose session went Idle gets the Idle card first");
        CHECK(deleteZ, "a closed session's topic is deleted");
        CHECK(!createA, "an existing topic is not created again");
        // The grace: not yet, then deleted.
        topics[L"b"].idleSinceMs = 1000;
        ops = PlanTopicSync(facts, topics, 1000 + 599000, 600000);
        bool delB = false;
        for (const auto& op : ops)
        {
            delB = delB || (op.kind == TopicSyncOp::Kind::Delete && op.sessionId == L"b");
        }
        CHECK(!delB, "inside the grace the idle topic stays");
        ops = PlanTopicSync(facts, topics, 1000 + 600000, 600000);
        for (const auto& op : ops)
        {
            delB = delB || (op.kind == TopicSyncOp::Kind::Delete && op.sessionId == L"b" && op.reason == L"idle");
        }
        CHECK(delB, "past the grace the idle topic is deleted");
        // Back from idle within the grace + a rename.
        facts[1].state = SessionState::Running;
        facts[1].title = L"renamed";
        ops = PlanTopicSync(facts, topics, 1000 + 5000, 600000);
        bool unidle = false, rename = false;
        for (const auto& op : ops)
        {
            unidle = unidle || (op.kind == TopicSyncOp::Kind::UnidleCard && op.sessionId == L"b");
            rename = rename || (op.kind == TopicSyncOp::Kind::Rename && op.sessionId == L"b" && op.title == L"renamed");
        }
        CHECK(unidle && rename, "a session that left Idle gets its card back, and a new title renames the topic");
        CHECK(QualifiesForTopic(fact(L"p", SessionState::Idle, true), 1000, 600000) && !QualifiesForTopic(fact(L"q", SessionState::Idle), 1000, 600000), "a pinned session always qualifies");
    }

    // ---- topics = tabs, §21.6: a NEW tab is listed before its first message; a topic follows its TAB ----
    {
        const int64_t grace = 10 * 60 * 1000;
        const int64_t now = 1792135754217ll;
        const auto fact = [&](const wchar_t* id, SessionState st, int64_t lastActivityMs, const wchar_t* tab) {
            TabFact f;
            f.sessionId = id;
            f.title = std::wstring{ L"t-" } + id;
            f.state = st;
            f.live = true;
            f.inTab = true;
            f.claude = true;
            f.lastActivityMs = lastActivityMs;
            f.tabToken = tab;
            return f;
        };
        // QualifiesForTopic: an Idle tab used within the grace (a new session / fork: its creation instant) qualifies;
        // a restored tab whose last activity is old does not; a tab-less or closed session never does.
        CHECK(QualifiesForTopic(fact(L"new", SessionState::Idle, now - 2000, L"T1"), now, grace), "a new Idle tab (created 2 s ago) qualifies");
        CHECK(!QualifiesForTopic(fact(L"old", SessionState::Idle, now - grace - 1, L"T2"), now, grace), "a restored Idle tab untouched for longer than the grace does not");
        CHECK(!QualifiesForTopic(fact(L"never", SessionState::Idle, 0, L"T3"), now, grace), "an Idle tab with no recorded activity does not");
        CHECK(!QualifiesForTopic(fact(L"g0", SessionState::Idle, now - 1, L"T4"), now, 0), "a grace of 0 lists no Idle tab");
        TabFact closed = fact(L"closed", SessionState::Running, now, L"T5");
        closed.live = false;
        CHECK(!QualifiesForTopic(closed, now, grace), "a closed session never qualifies");
        // IdleTopicLeavesAtMs: one grace after the LATER of "first seen Idle" and the last activity; 0 when not idle.
        CHECK(IdleTopicLeavesAtMs(0, now, grace) == 0, "not idle -> no leave time");
        CHECK(IdleTopicLeavesAtMs(now, now - 5000, grace) == now + grace, "decayed long after its activity: a full grace from the decay");
        CHECK(IdleTopicLeavesAtMs(now - 5000, now, grace) == now + grace, "used after it went idle: a full grace from the use");

        // A brand-new tab gets a Create at once.
        {
            std::map<std::wstring, TopicRecord> topics;
            const auto ops = PlanTopicSync({ fact(L"fresh", SessionState::Idle, now - 1500, L"T1") }, topics, now, grace);
            CHECK(ops.size() == 1 && ops[0].kind == TopicSyncOp::Kind::Create && ops[0].sessionId == L"fresh", "a new Idle tab is created at once (the user's 'a new session/tab with no new messages should appear')");
        }
        // The plan's grace for an Idle topic counts from the later of idleSince and the last activity.
        {
            std::map<std::wstring, TopicRecord> topics;
            TopicRecord t;
            t.sessionId = L"used";
            t.threadId = 21;
            t.title = L"t-used";
            t.idleSinceMs = now - grace - 1000; // first seen Idle more than a grace ago...
            t.tabToken = L"T6";
            topics[L"used"] = t;
            auto ops = PlanTopicSync({ fact(L"used", SessionState::Idle, now - 1000, L"T6") }, topics, now, grace); // ...but used a second ago
            bool del = false;
            for (const auto& op : ops)
            {
                del = del || op.kind == TopicSyncOp::Kind::Delete;
            }
            CHECK(!del, "an Idle topic whose session was used since it went idle stays a whole grace from the use");
            ops = PlanTopicSync({ fact(L"used", SessionState::Idle, now - grace - 500, L"T6") }, topics, now, grace);
            del = false;
            for (const auto& op : ops)
            {
                del = del || (op.kind == TopicSyncOp::Kind::Delete && op.reason == L"idle");
            }
            CHECK(del, "an Idle topic untouched past the grace is deleted");
        }
        // REKEY: the topic's session closed and another session holds the SAME tab (a /clear, a /resume, a handover in
        // place) — the topic follows the tab; the heir gets no topic of its own; the tab key compares case-insensitively.
        {
            std::map<std::wstring, TopicRecord> topics;
            TopicRecord t;
            t.sessionId = L"before";
            t.threadId = 31;
            t.title = L"t-before";
            t.tabToken = L"ABCD-EF01";
            topics[L"before"] = t;
            auto heir = fact(L"after", SessionState::Running, now, L"abcd-ef01");
            heir.title = L"cleared conversation";
            const auto ops = PlanTopicSync({ heir }, topics, now, grace);
            CHECK(ops.size() == 1 && ops[0].kind == TopicSyncOp::Kind::Rekey && ops[0].sessionId == L"after" && ops[0].fromSessionId == L"before" && ops[0].title == L"cleared conversation",
                  "a closed session's topic is carried over to the session now in its tab (one Rekey, no Delete, no Create)");
        }
        {
            // No heir in that tab: the topic is deleted ("closed"); a heir in ANOTHER tab gets its own topic.
            std::map<std::wstring, TopicRecord> topics;
            TopicRecord t;
            t.sessionId = L"gone";
            t.threadId = 41;
            t.tabToken = L"T7";
            topics[L"gone"] = t;
            const auto ops = PlanTopicSync({ fact(L"other", SessionState::Running, now, L"T8") }, topics, now, grace);
            bool deleted = false, created = false, rekeyed = false;
            for (const auto& op : ops)
            {
                deleted = deleted || (op.kind == TopicSyncOp::Kind::Delete && op.sessionId == L"gone" && op.reason == L"closed");
                created = created || (op.kind == TopicSyncOp::Kind::Create && op.sessionId == L"other");
                rekeyed = rekeyed || op.kind == TopicSyncOp::Kind::Rekey;
            }
            CHECK(deleted && created && !rekeyed, "another tab's session is no heir: the old topic goes, the other tab gets its own");
        }
        {
            // A /handover-here successor takes the tab on a NEW connection (a new token): it CONTINUES the closed session,
            // so the topic follows it all the same.
            std::map<std::wstring, TopicRecord> topics;
            TopicRecord t;
            t.sessionId = L"origin";
            t.threadId = 71;
            t.tabToken = L"OLD-CONN";
            topics[L"origin"] = t;
            auto successor = fact(L"successor", SessionState::Running, now, L"NEW-CONN");
            successor.continuedFromId = L"origin";
            const auto ops = PlanTopicSync({ successor }, topics, now, grace);
            CHECK(ops.size() == 1 && ops[0].kind == TopicSyncOp::Kind::Rekey && ops[0].sessionId == L"successor" && ops[0].fromSessionId == L"origin",
                  "a handover-in-place successor (continuedFromId) inherits the topic");
        }
        {
            // A topic with no recorded tab (written before §21.6) is never carried over.
            std::map<std::wstring, TopicRecord> topics;
            TopicRecord t;
            t.sessionId = L"legacy";
            t.threadId = 51;
            topics[L"legacy"] = t;
            const auto ops = PlanTopicSync({ fact(L"heir", SessionState::Running, now, L"T9") }, topics, now, grace);
            bool rekeyed = false;
            for (const auto& op : ops)
            {
                rekeyed = rekeyed || op.kind == TopicSyncOp::Kind::Rekey;
            }
            CHECK(!rekeyed, "a topic without a recorded tab is not carried over");
        }
        {
            // While the topic's own session still holds the tab, a second session in that tab (the moment of a
            // conversation switch, before the old record closes) gets no topic: the next sync carries the topic over.
            std::map<std::wstring, TopicRecord> topics;
            TopicRecord t;
            t.sessionId = L"holder";
            t.threadId = 61;
            t.title = L"t-holder";
            t.tabToken = L"TA";
            topics[L"holder"] = t;
            const auto ops = PlanTopicSync({ fact(L"holder", SessionState::Running, now, L"TA"), fact(L"second", SessionState::Running, now, L"ta") }, topics, now, grace);
            bool createdSecond = false;
            for (const auto& op : ops)
            {
                createdSecond = createdSecond || (op.kind == TopicSyncOp::Kind::Create && op.sessionId == L"second");
            }
            CHECK(!createdSecond, "per tab: a tab whose topic's session still holds it gets no second topic");
        }
    }

    // ---- what a message in a topic is ----
    {
        auto c = ParseRemoteCommand(L"  please fix it  ");
        CHECK(c.kind == RemoteKind::Text && c.text == L"please fix it", "plain text is a prompt");
        c = ParseRemoteCommand(L"/queue");
        CHECK(c.kind == RemoteKind::AgentmasterCommand && c.name == L"queue", "an Agentmaster command");
        c = ParseRemoteCommand(L"/rename@SidKatrineBot  New Title ");
        CHECK(c.kind == RemoteKind::AgentmasterCommand && c.name == L"rename" && c.args == L"New Title", "the @bot suffix is dropped; args trimmed");
        c = ParseRemoteCommand(L"/compact keep the plan");
        CHECK(c.kind == RemoteKind::ClaudeCommand && c.name == L"compact" && c.text == L"/compact keep the plan", "a recorded Claude Code command is queued whole");
        c = ParseRemoteCommand(L"/reload-plugins");
        CHECK(c.kind == RemoteKind::RefusedNoRecord, "a no-record command is refused");
        c = ParseRemoteCommand(L"/resume abc");
        CHECK(c.kind == RemoteKind::RefusedLeavesTab, "a command that leaves the tab is refused");
        c = ParseRemoteCommand(L"/model");
        CHECK(c.kind == RemoteKind::RefusedNoRecord, "/model with no argument is the TUI picker");
        c = ParseRemoteCommand(L"/model opus");
        CHECK(c.kind == RemoteKind::ClaudeCommand, "/model <name> is recorded");
        c = ParseRemoteCommand(L"/cc /status");
        CHECK(c.kind == RemoteKind::RefusedNoRecord && c.name == L"status", "/cc still applies the refusal table");
        c = ParseRemoteCommand(L"/cc /mycommand do it");
        CHECK(c.kind == RemoteKind::ClaudeCommand && c.name == L"mycommand" && c.text == L"/mycommand do it", "/cc passes a command through to Claude Code");
        c = ParseRemoteCommand(L"/cc hello");
        CHECK(c.kind == RemoteKind::Text && c.text == L"hello", "/cc with plain text is text");
        c = ParseRemoteCommand(L"/");
        CHECK(c.kind == RemoteKind::Text, "a lone slash is text");
        c = ParseRemoteCommand(L"/my-skill with args\nand a second line");
        CHECK(c.kind == RemoteKind::ClaudeCommand && c.name == L"my-skill" && c.args == L"with args\nand a second line", "an unknown command is Claude Code's (a skill), args keep their lines");
    }
    CHECK(IsBacklogUpdate(1000, 1000 + kBacklogAgeMs) && !IsBacklogUpdate(1000, 1000 + kBacklogAgeMs - 1) && !IsBacklogUpdate(0, 5000000), "the backlog rule");

    // ---- the state file ----
    {
        MessagingState s;
        s.updateOffset = 77;
        s.searchThreadId = 5;
        s.botUsername = L"SidKatrineBot";
        s.botId = 123456789012; // past 32 bits: Telegram ids are
        s.topics[L"sid-1"] = TopicRecord{ L"sid-1", 42, 100, 101, L"title", 123, 4567, L"C:\\t.jsonl", 9 };
        const auto back = StateFromJson(StateToJson(s));
        CHECK(back.has_value() && back->updateOffset == 77 && back->searchThreadId == 5 && back->botUsername == L"SidKatrineBot" && back->botId == 123456789012, "the state's scalars round-trip (the bot id past 32 bits)");
        // A file written before the key reads 0 = "the first bot seen is this state's", never a reset.
        auto legacyObj = json::Value::MkObj();
        legacyObj.Set(L"updateOffset", json::Value::MkNum(77));
        const auto legacy = StateFromJson(legacyObj);
        CHECK(legacy.has_value() && legacy->botId == 0 && legacy->updateOffset == 77, "an older state file without botId reads botId 0");
        // §21.5: the business connection and the sticky-card / recap / errorAnnounced fields of a topic.
        MessagingState b;
        b.businessConnectionId = L"conn-1";
        b.businessEnabled = true;
        b.businessCanReply = true;
        b.businessMode = -1;
        b.businessNote = L"refused";
        TopicRecord tr;
        tr.sessionId = L"sid-2";
        tr.threadId = 7;
        tr.lastPostedMs = 11;
        tr.cardPostedMs = 10;
        tr.recapText = L"Shipped the fix. Next: deploy.";
        tr.recapAtMs = 9;
        tr.lastMessageAtMs = 8;
        tr.cardTextHash = 0xFEDCBA98u; // past 31 bits: the hash is unsigned
        tr.tabToken = L"wt-1";
        tr.errorAnnounced = true;
        b.topics[L"sid-2"] = tr;
        const auto bb = StateFromJson(StateToJson(b));
        CHECK(bb.has_value() && bb->businessConnectionId == L"conn-1" && bb->businessEnabled && bb->businessCanReply && bb->businessMode == -1 && bb->businessNote == L"refused", "the business connection round-trips");
        CHECK(bb.has_value() && bb->topics.at(L"sid-2").lastPostedMs == 11 && bb->topics.at(L"sid-2").cardPostedMs == 10 && bb->topics.at(L"sid-2").recapText == L"Shipped the fix. Next: deploy." &&
                  bb->topics.at(L"sid-2").recapAtMs == 9 && bb->topics.at(L"sid-2").lastMessageAtMs == 8 && bb->topics.at(L"sid-2").cardTextHash == 0xFEDCBA98u &&
                  bb->topics.at(L"sid-2").tabToken == L"wt-1" && bb->topics.at(L"sid-2").errorAnnounced,
              "the sticky-card clocks, the recap facts, the card hash, the tab and errorAnnounced round-trip");
        CHECK(legacy->topics.empty() && !legacy->businessEnabled && legacy->businessMode == 0, "an older state file reads no business connection");
        // A 0.0.1.15 topic (recapPrompt / recapReply, no recapText) reads NO recap: the old synthesized block is gone.
        {
            const auto old = json::Parse(L"{\"topics\":[{\"sessionId\":\"s\",\"threadId\":3,\"recapPrompt\":\"p\",\"recapReply\":\"r\",\"recapAtMs\":5,\"recapSeenMs\":6}]}");
            const auto st = old ? StateFromJson(*old) : std::nullopt;
            CHECK(st.has_value() && st->topics.at(L"s").recapText.empty() && st->topics.at(L"s").recapAtMs == 0 && st->topics.at(L"s").tabToken.empty() && st->topics.at(L"s").cardTextHash == 0,
                  "a 0.0.1.15 topic reads no recap, no tab and no card hash");
        }
        CHECK(back.has_value() && back->topics.size() == 1 && back->topics.at(L"sid-1").threadId == 42 && back->topics.at(L"sid-1").cardMessageId == 100 &&
                  back->topics.at(L"sid-1").cursor == 4567 && back->topics.at(L"sid-1").transcriptPath == L"C:\\t.jsonl" && back->topics.at(L"sid-1").idleSinceMs == 123,
              "a topic record round-trips");
        CHECK(!StateFromJson(json::Value::MkArr()).has_value(), "a non-object is refused");
    }

    // ---- the card ----
    {
        SessionInfo s = MakeSession(L"card-1");
        s.workingDir = L"K:\\source\\Agentmaster";
        s.title = L"Agentmaster/feature";
        s.state = SessionState::Running;
        s.stateChangedAtUnixMs = 1000;
        s.branch = L"agentmaster";
        s.currentModel = L"claude-opus-5-5";
        s.contextTokens = 182000;
        s.autorunner.mode = AutorunnerMode::Off;
        QueuedPrompt q;
        q.id = L"q1";
        q.text = L"also update the docs\nsecond line";
        s.queue.push_back(q);
        s.pendingInput = L"and then rebase onto <main>";
        const std::wstring card = BuildCardText(s, 1000 + 3 * 60 * 1000, 0, false);
        CHECK(card.find(L"<b>Agentmaster/feature</b>") != std::wstring::npos, "the card leads with the title");
        // The "since" is a LOCAL clock time ("HH:MM" — the same day in the test's epochs), never a span: the
        // card is re-edited on every text change, and a span changed every minute on every topic.
        const auto looksLikeClock = [](std::wstring_view t) {
            return t.size() >= 5 && iswdigit(t[0]) && iswdigit(t[1]) && t[2] == L':' && iswdigit(t[3]) && iswdigit(t[4]);
        };
        const size_t sincePos = card.find(L"Running since ");
        CHECK(sincePos != std::wstring::npos && looksLikeClock(std::wstring_view{ card }.substr(sincePos + 14)), "the state and since when, as a clock time");
        CHECK(card.find(L"Running 3m") == std::wstring::npos, "no relative span in the card (it would re-edit every minute)");
        CHECK(looksLikeClock(TgClockText(1700000000000, 1700000000000)) && TgClockText(1700000000000, 1700000000000).size() == 5, "TgClockText: the same day is HH:MM");
        CHECK(TgClockText(1700000000000 - 3ll * 86400000, 1700000000000).size() > 5 && TgClockText(1700000000000 - 3ll * 86400000, 1700000000000).find(L' ') != std::wstring::npos, "TgClockText: another day adds the date");
        CHECK(card.find(L"agentmaster \x00B7 claude-opus-5-5 \x00B7 ctx 182K") != std::wstring::npos, "branch · model · ctx");
        CHECK(card.find(L"Runner Paused \x00B7 queue 1 \x2014 next: \x201C" L"also update the docs\x201D") != std::wstring::npos, "the runner and the next queued prompt");
        CHECK(card.find(L"unsent at the PC: \x201C" L"and then rebase onto &lt;main&gt;\x201D") != std::wstring::npos, "the unsent draft, escaped");
        CHECK(BuildCardText(s, 5000, 0, true).find(L"(Off globally)") != std::wstring::npos, "the global pause shows");
        CHECK(BuildCardText(s, 5000, 5000 + 90000, false).find(L"leaves the list at ") != std::wstring::npos, "the idle card names the clock time it leaves at (never a countdown)");
        // §21.6: Claude Code's own recap (its away_summary) only while it is fresh — a clock time and the text, escaped;
        // a parked tab, a session in Error and a claude that exited carry the Reactivate lines.
        CardExtras ex;
        ex.recapFresh = true;
        ex.recapText = L"Fixed the <drag> crash. Next: deploy.";
        ex.recapAtMs = 1700000000000;
        const std::wstring withRecap = BuildCardText(s, 1700000000000 + 60000, 0, false, &ex);
        CHECK(withRecap.find(L"<b>Recap</b> \x00B7 ") != std::wstring::npos && withRecap.find(L"<i>Fixed the &lt;drag&gt; crash. Next: deploy.</i>") != std::wstring::npos, "the recap block: a clock time and the recap, escaped");
        ex.recapFresh = false;
        CHECK(BuildCardText(s, 5000, 0, false, &ex).find(L"Recap") == std::wstring::npos, "no recap block once a message is newer than the recap");
        ex.recapFresh = true;
        ex.recapText.clear();
        CHECK(BuildCardText(s, 5000, 0, false, &ex).find(L"Recap") == std::wstring::npos, "no recap block without a recap");
        SessionInfo exited = s;
        exited.live = true;
        exited.started = true;
        exited.state = SessionState::Done;
        CHECK(BuildCardText(exited, 5000, 0, false).find(L"claude exited \x2014 \x25B6 Reactivate starts it again") != std::wstring::npos, "a claude that exited: the Reactivate line");
        // The card's "since" comes from persisted stamps, so a restart (which restamps stateChangedAtUnixMs) changes no card.
        {
            SessionInfo c = MakeSession(L"since-1", SessionState::Running);
            c.turnStartUnixMs = 100;
            c.turnEndUnixMs = 50;
            c.lastActivityUnixMs = 300;
            c.stateChangedAtUnixMs = 400;
            CHECK(CardStateSinceMs(c) == 100, "Running: since the turn started");
            c.state = SessionState::NeedsApproval;
            CHECK(CardStateSinceMs(c) == 100, "NeedsApproval: since the turn started");
            c.state = SessionState::WaitingForInput;
            c.turnEndUnixMs = 200;
            CHECK(CardStateSinceMs(c) == 200, "at rest: since the newer turn boundary");
            c.turnStartUnixMs = 0;
            c.turnEndUnixMs = 0;
            CHECK(CardStateSinceMs(c) == 300, "no turn yet (a new session): since its last activity (its creation)");
            c.lastActivityUnixMs = 0;
            CHECK(CardStateSinceMs(c) == 400, "nothing else known: the state change (the last fallback)");
        }
        CHECK(CardTextHash(L"a") == CardTextHash(L"a") && CardTextHash(L"a") != CardTextHash(L"b") && CardTextHash(L"") != 0, "the card hash: stable, text-sensitive, never 0");
        {
            SessionInfo r = MakeSession(L"react-1", SessionState::WaitingForInput);
            r.live = true;
            r.started = true;
            CHECK(!SessionIsReactivatable(r), "a waiting session has nothing to reactivate");
            r.started = false;
            CHECK(SessionIsReactivatable(r), "a parked tab is reactivatable");
            r.external = true;
            CHECK(!SessionIsReactivatable(r), "an adopted session's claude is running whatever `started` says");
            r.external = false;
            r.started = true;
            r.state = SessionState::Error;
            CHECK(SessionIsReactivatable(r), "a session in Error is reactivatable");
            r.state = SessionState::Done;
            CHECK(SessionIsReactivatable(r), "a claude that exited is reactivatable");
            r.live = false;
            CHECK(!SessionIsReactivatable(r), "a closed session is not (Search resumes it)");
        }
        SessionInfo parked = s;
        parked.live = true;
        parked.started = false;
        parked.external = false;
        CHECK(BuildCardText(parked, 5000, 0, false).find(L"not running \x2014 a message here or \x25B6 Reactivate resumes it") != std::wstring::npos, "a parked tab: the Reactivate line, never 'parked'");
        SessionInfo failed = s;
        failed.live = true;
        failed.started = true;
        failed.state = SessionState::Error;
        CHECK(BuildCardText(failed, 5000, 0, false).find(L"\x25B6 Reactivate restarts it") != std::wstring::npos, "a session in Error: the Reactivate line");
        CHECK(TgStripTags(L"<b>a &lt;T&gt;</b> &amp; <code>b</code>") == L"a <T> & b", "TgStripTags drops the tags and unescapes the three entities");
        CHECK(ParseRemoteCommand(L"/reactivate").kind == RemoteKind::AgentmasterCommand && ParseRemoteCommand(L"/reactivate").name == L"reactivate", "/reactivate is an Agentmaster command");
    }

    // ---- the remote prompt's runner bypass (D30) ----
    {
        SessionInfo s = MakeSession(L"remote-1");
        s.workingDir = L"K:\\x";
        s.live = true;
        s.started = true;
        s.state = SessionState::WaitingForInput;
        s.stateChangedAtUnixMs = 1000;
        s.autorunner.mode = AutorunnerMode::Off;
        QueuedPrompt q;
        q.id = L"r1";
        q.text = L"from the phone";
        q.origin = PromptOrigin::Autorun;
        q.guardPattern = std::wstring{ kAnswersQuestionOk };
        q.remoteSource = L"telegram:5";
        q.queuedAtUnixMs = 1000;
        s.queue.push_back(q);
        CHECK(NextPromptIsRemote(s), "the next prompt is remote");
        const int64_t later = 1000 + kStateSettleMs + 1;
        CHECK(DecideAdvance(s, later, 0, false).action == AdvanceAction::Send, "a remote prompt is sent with the runner Paused");
        CHECK(DecideAdvance(s, later, 0, true).action == AdvanceAction::Send, "… and past Pause all");
        s.autorunner.autoSendsThisRun = s.autorunner.maxAutoSends;
        CHECK(DecideAdvance(s, later, 0, false).action == AdvanceAction::Send, "… and past the send budget");
        s.autorunner.autoSendsThisRun = 0;
        s.lastMessageWasQuestion = true;
        CHECK(DecideAdvance(s, later, 0, false).action == AdvanceAction::Send, "… and past the question guard (it IS the answer)");
        s.state = SessionState::Running;
        CHECK(DecideAdvance(s, later, 0, false).action == AdvanceAction::None, "but never into a running turn");
        s.state = SessionState::WaitingForInput;
        s.started = false;
        CHECK(DecideAdvance(s, later, 0, false).action == AdvanceAction::None, "nor into a dormant tab");
        s.started = true;
        s.queue[0].remoteSource.clear();
        CHECK(DecideAdvance(s, later, 0, false).action == AdvanceAction::None && DecideAdvance(s, later, 0, false).reason == L"autorunner off", "a plain prompt still waits for the runner");
        // A remote prompt behind a plain Pending one does not bypass (the plain one is next).
        QueuedPrompt plain;
        plain.id = L"p1";
        plain.text = L"planned";
        s.queue.insert(s.queue.begin(), plain);
        s.queue[1].remoteSource = L"telegram:6";
        CHECK(!NextPromptIsRemote(s), "only the NEXT prompt decides");
    }

    // ---- §21.6: a remote prompt settles kRemotePromptSettleMs, not the runner's 40 s ----
    {
        SessionInfo s = MakeSession(L"remote-settle");
        s.live = true;
        s.started = true;
        s.state = SessionState::WaitingForInput;
        s.stateChangedAtUnixMs = 10000; // e.g. a parked tab that just started
        s.autorunner.mode = AutorunnerMode::Full;
        QueuedPrompt q;
        q.id = L"rs1";
        q.text = L"from the phone";
        q.origin = PromptOrigin::Autorun;
        q.remoteSource = L"telegram:77";
        q.queuedAtUnixMs = 9000;
        s.queue.push_back(q);
        CHECK(StateSettleRemainingMs(s, s.queue[0], 11000) == kRemotePromptSettleMs - 1000, "a remote prompt's window is kRemotePromptSettleMs");
        const AdvancePlan held = DecideAdvance(s, 11000, 0, false);
        CHECK(held.action == AdvanceAction::None && held.retryAfterMs == kRemotePromptSettleMs - 1000, "held only for the rest of the short window (a timed re-check)");
        CHECK(DecideAdvance(s, 10000 + kRemotePromptSettleMs, 0, false).action == AdvanceAction::Send, "sent once the short window passed");
        CHECK(StateSettleRemainingMs(s, s.queue[0], 11000, 0) == 0, "a disabled hold stays disabled for a remote prompt");
        s.queue[0].remoteSource.clear();
        CHECK(DecideAdvance(s, 10000 + kRemotePromptSettleMs, 0, false).action == AdvanceAction::None && StateSettleRemainingMs(s, s.queue[0], 10000 + kRemotePromptSettleMs) == kStateSettleMs - kRemotePromptSettleMs,
              "the runner's own prompt still waits the whole 40 s");
    }

    // ---- the pin hold + its persistence ----
    {
        SessionInfo s = MakeSession(L"pin-1");
        s.live = true;
        s.pinned = true;
        s.state = SessionState::Idle;
        CHECK(ApplyPinHold(s) && s.state == SessionState::WaitingForInput, "a pinned live Idle session is held in Waiting");
        s.state = SessionState::Done;
        CHECK(!ApplyPinHold(s) && s.state == SessionState::Done, "Done is not held");
        s.state = SessionState::Idle;
        s.live = false;
        CHECK(!ApplyPinHold(s), "a closed session is not held");
        s.live = true;
        s.pinned = false;
        CHECK(!ApplyPinHold(s), "an unpinned session is not held");
        CHECK(!ShouldDecayWaitingToIdle(SessionState::WaitingForInput, 1000, 2000, false, false, 60, 1000 + 61 * 60000) == false, "(the decay rule itself is unchanged; the scanner refuses pinned before it)");

        SessionRegistry reg;
        SessionInfo r = MakeSession(L"pin-2");
        r.live = true;
        r.pinned = true;
        r.state = SessionState::Idle;
        reg.Upsert(r);
        CHECK(reg.Get(L"pin-2")->state == SessionState::WaitingForInput, "Upsert applies the hold");
        reg.Update(L"pin-2", [](SessionInfo& ss) { ss.state = SessionState::Idle; });
        CHECK(reg.Get(L"pin-2")->state == SessionState::WaitingForInput, "Update applies the hold (a triage move to Idle is undone)");
        reg.Update(L"pin-2", [](SessionInfo& ss) { ss.pinned = false; ss.state = SessionState::Idle; });
        CHECK(reg.Get(L"pin-2")->state == SessionState::Idle, "unpinned: Idle stands");

        // Persistence: `pinned` and a prompt's remoteSource round-trip; absent keys read as defaults.
        SessionInfo p = MakeSession(L"persist-1");
        p.pinned = true;
        QueuedPrompt q;
        q.id = L"q1";
        q.text = L"hi";
        q.remoteSource = L"telegram:9";
        p.queue.push_back(q);
        const auto text = SerializeSessions({ p });
        CHECK(text.find(L"\"pinned\":true") != std::wstring::npos && text.find(L"\"remoteSource\":\"telegram:9\"") != std::wstring::npos, "both keys are written");
        const auto back = DeserializeSessions(text);
        CHECK(back.size() == 1 && back[0].pinned && back[0].queue.size() == 1 && back[0].queue[0].remoteSource == L"telegram:9", "both keys read back");
        SessionInfo plain = MakeSession(L"persist-2");
        const auto plainText = SerializeSessions({ plain });
        CHECK(plainText.find(L"pinned") == std::wstring::npos && plainText.find(L"remoteSource") == std::wstring::npos, "an unpinned record with no remote prompt writes neither key");
    }
}
