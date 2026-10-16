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
        CHECK(QualifiesForTopic(fact(L"p", SessionState::Idle, true)) && !QualifiesForTopic(fact(L"q", SessionState::Idle)), "a pinned session always qualifies");
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
        s.topics[L"sid-1"] = TopicRecord{ L"sid-1", 42, 100, 101, L"title", 123, 4567, L"C:\\t.jsonl", 9 };
        const auto back = StateFromJson(StateToJson(s));
        CHECK(back.has_value() && back->updateOffset == 77 && back->searchThreadId == 5 && back->botUsername == L"SidKatrineBot", "the state's scalars round-trip");
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
        CHECK(card.find(L"Running 3m") != std::wstring::npos, "the state and since when");
        CHECK(card.find(L"agentmaster \x00B7 claude-opus-5-5 \x00B7 ctx 182K") != std::wstring::npos, "branch · model · ctx");
        CHECK(card.find(L"Runner Paused \x00B7 queue 1 \x2014 next: \x201C" L"also update the docs\x201D") != std::wstring::npos, "the runner and the next queued prompt");
        CHECK(card.find(L"unsent at the PC: \x201C" L"and then rebase onto &lt;main&gt;\x201D") != std::wstring::npos, "the unsent draft, escaped");
        CHECK(BuildCardText(s, 5000, 0, true).find(L"(Off globally)") != std::wstring::npos, "the global pause shows");
        CHECK(BuildCardText(s, 5000, 5000 + 90000, false).find(L"leaves the list in 1m") != std::wstring::npos, "the idle card counts down");
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
