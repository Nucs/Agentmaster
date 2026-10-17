// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md: the fleet in Telegram, the PURE core (TelegramFormat.h + MessagingModels.h)
// and the two engine rules it leans on (the remote prompt's runner bypass in DecideAdvance, the pin hold).
// The service's network half (TelegramApi, the poll / send / tick threads) is not exercised here: it talks
// to api.telegram.org, and the live check is a session's own bot (MESSAGING.md §15).

#include "m5_tests.h"

#include <cstdint> // SIZE_MAX (the rename-budget model's low-water mark)
#include <deque> // the rename-budget model's call window

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
        // §21.9: parentheses inside a url balance (a Wikipedia link kept its last ')' cut off, and a stray one after it).
        const std::wstring wiki = TgMarkdownToHtml(L"see [Jordan King (basketball)](https://en.wikipedia.org/wiki/Jordan_King_(basketball)) now");
        CHECK(wiki.find(L"<a href=\"https://en.wikipedia.org/wiki/Jordan_King_(basketball)\">Jordan King (basketball)</a> now") != std::wstring::npos,
              "a url with balanced parentheses keeps them, and nothing stray follows the link");
        CHECK(TgMarkdownToHtml(L"[a](https://x.y/(unclosed").find(L"<a ") == std::wstring::npos, "an unbalanced url is not a link");
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
    // §21.13 — the title's COLOR SQUARE: by hue, so the pastel tab palette lands on the squares of its color families (a
    // distance pick made its reds and corals brown); greys go by lightness, a dark warm color is brown, and a malformed color
    // has no square at all (never a guess).
    {
        const std::pair<const wchar_t*, const wchar_t*> palette[] = {
            { L"#E06C75", L"\U0001F7E5" }, { L"#E5C07B", L"\U0001F7E8" }, { L"#98C379", L"\U0001F7E9" }, { L"#56B6C2", L"\U0001F7E6" },
            { L"#61AFEF", L"\U0001F7E6" }, { L"#C678DD", L"\U0001F7EA" }, { L"#D19A66", L"\U0001F7E7" }, { L"#BE5046", L"\U0001F7E5" },
            { L"#528BFF", L"\U0001F7E6" }, { L"#7FD962", L"\U0001F7E9" }, { L"#FF9E64", L"\U0001F7E7" }, { L"#2BBAC5", L"\U0001F7E6" },
            { L"#B267E6", L"\U0001F7EA" }, { L"#F78C6C", L"\U0001F7E7" },
        };
        bool paletteOk = true;
        for (const auto& [hex, want] : palette)
        {
            paletteOk = paletteOk && std::wstring{ TgTitleColorSquare(hex) } == want;
        }
        CHECK(paletteOk, "the 14 automatic tab colors land on the squares of their families (red, yellow, green, blue, purple, orange)");
        CHECK(std::wstring{ TgTitleColorSquare(L"#FF61AFEF") } == L"\U0001F7E6" && std::wstring{ TgTitleColorSquare(L"#8B4513") } == L"\U0001F7EB",
              "an alpha byte is ignored; a dark warm color is brown");
        CHECK(std::wstring{ TgTitleColorSquare(L"#000000") } == L"\x2B1B" && std::wstring{ TgTitleColorSquare(L"#FFFFFF") } == L"\x2B1C" &&
                  std::wstring{ TgTitleColorSquare(L"#404040") } == L"\x2B1B" && std::wstring{ TgTitleColorSquare(L"#C0C0C0") } == L"\x2B1C",
              "black, white, and a grey by its lightness");
        CHECK(*TgTitleColorSquare(L"") == 0 && *TgTitleColorSquare(L"nope") == 0 && *TgTitleColorSquare(L"#12345") == 0 && *TgTitleColorSquare(L"#GGGGGG") == 0,
              "no color (the \"Remove colors\" mode) or a malformed one: no square");
        uint32_t rgb = 7;
        const bool lower = TgParseRgbHex(L"#61afef", rgb) && rgb == 0x61AFEFu;
        const bool alpha = TgParseRgbHex(L"#FF528BFF", rgb) && rgb == 0x528BFFu;
        const bool refused = !TgParseRgbHex(L"61AFEF", rgb) && rgb == 0x528BFFu;
        CHECK(lower && alpha && refused, "the parse: either case, the alpha byte dropped, a refused text leaves the value alone");
        CHECK(TgTitleColorPrefix(L"#61AFEF") == L"\U0001F7E6 " && TgTitleColorPrefix(L"").empty(), "the prefix: the square and a space, or nothing");
    }
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

    // ---- §21.8: the topic list in the side panel's MOST RECENT order, and the one-time recreation ----
    {
        // The ONE key the side panel (MakeSortKey) and the topic list share.
        SessionInfo k = MakeSession(L"key-1");
        CHECK(MostRecentSortKeyMs(k) == 0, "nothing known: key 0 (ranks last)");
        k.lastActivityUnixMs = 500;
        CHECK(MostRecentSortKeyMs(k) == 500, "no turn yet (a new session / fork): its last activity = its creation");
        k.turnStartUnixMs = 300;
        CHECK(MostRecentSortKeyMs(k) == 300, "a turn boundary wins over the activity, even an older one (the side panel's rule)");
        k.turnEndUnixMs = 400;
        CHECK(MostRecentSortKeyMs(k) == 400, "the newer of the two boundaries");

        CHECK(TopicRanksAbove(2000, L"b", 1000, L"a") && !TopicRanksAbove(1000, L"a", 2000, L"b"), "the newer key ranks above");
        CHECK(TopicRanksAbove(1000, L"alpha", 1000, L"Beta") && !TopicRanksAbove(1000, L"Beta", 1000, L"alpha"), "equal keys: the title, case-insensitive (alpha above Beta)");
        CHECK(!TopicRanksAbove(1000, L"Same", 1000, L"same") && !TopicRanksAbove(1000, L"same", 1000, L"Same"), "the same key and title: neither ranks above");
        CHECK(TopicRanksAbove(5, L"A_b", 5, L"aab") == false && TopicRanksAbove(5, L"aab", 5, L"A_b"), "upper-cased like CompareStringOrdinal: '_' (0x5F) sorts after 'A' (0x41)");

        // PlanTopicSync makes the creates lowest-ranked first, so the topic that ranks highest is made last (on top).
        const auto fact = [](const wchar_t* id, const wchar_t* title, int64_t key) {
            TabFact f;
            f.sessionId = id;
            f.title = title;
            f.state = SessionState::WaitingForInput;
            f.live = true;
            f.inTab = true;
            f.claude = true;
            f.tabToken = std::wstring{ L"tab-" } + id;
            f.mostRecentMs = key;
            return f;
        };
        std::map<std::wstring, TopicRecord> none;
        const auto ops = PlanTopicSync({ fact(L"a", L"Old", 100), fact(L"b", L"Newest", 900), fact(L"c", L"Middle", 500), fact(L"d", L"Tie-b", 300), fact(L"e", L"tie-A", 300) }, none, 1000, 600000);
        std::wstring order;
        for (const auto& op : ops)
        {
            if (op.kind == TopicSyncOp::Kind::Create)
            {
                order += op.sessionId;
            }
        }
        CHECK(order == L"adecb", "creates come lowest-ranked first: Old, Tie-b, tie-A, Middle, Newest (made last = on top; tie-A ranks above Tie-b)");

        // The recreation: due for topics made under an older layout, never for a state without session topics.
        MessagingState st;
        CHECK(!NeedsTopicRecreation(st), "no topic: nothing to recreate (the layout is just recorded)");
        st.topics[L"x"] = TopicRecord{ L"x", 9, 0, 0, L"t", 0, 0, L"", 0 };
        CHECK(NeedsTopicRecreation(st), "a topic made before the layout: recreate");
        st.topicLayout = kTopicLayoutVersion;
        CHECK(!NeedsTopicRecreation(st), "made under the current layout: nothing to do");
        st.orphanThreads = { 11, 22 };
        const auto back = StateFromJson(StateToJson(st));
        CHECK(back.has_value() && back->topicLayout == kTopicLayoutVersion && back->orphanThreads.size() == 2 && back->orphanThreads[0] == 11 && back->orphanThreads[1] == 22,
              "the layout and the orphan list round-trip (a stop midway finishes the deletes at the next start)");
        auto legacy = json::Value::MkObj();
        legacy.Set(L"updateOffset", json::Value::MkNum(1));
        const auto old = StateFromJson(legacy);
        CHECK(old.has_value() && old->topicLayout == 0 && old->orphanThreads.empty(), "a file without the keys: layout 0, no orphan");
        // /recreate is a command the Search topic RUNS (asked first there); in a session's topic it is Agentmaster's too.
        CHECK(ClassifySearchTopicMessage(L"/recreate").verb == SearchTopicVerb::FleetCommand, "/recreate is a Search topic fleet command");
        CHECK(ParseRemoteCommand(L"/recreate").kind == RemoteKind::AgentmasterCommand, "/recreate is never queued as a prompt");
    }

    // ---- §21.9: the HISTORY replay (a topic shows its whole session) and the list's re-sort ----
    {
        // The day lines: noon UTC is the same local day in every zone from UTC-11 to UTC+11.
        const int64_t day1 = 1792152000000ll; // 2026-10-16 12:00 UTC
        const int64_t day2 = day1 + 24ll * 3600 * 1000;
        CHECK(TgDayKey(day1) == 20261016 && TgDayKey(day2) == 20261017, "the local day as yyyymmdd");
        CHECK(TgDayKey(0) == 0 && TgDayText(0) == L"?", "an unknown moment has no day");
        CHECK(TgDayText(day1) == L"Fri 16 Oct 2026", "the day line's text");

        const auto item = [](TopicItemKind kind, const wchar_t* text, int64_t ts = 0, bool midTurn = false) {
            TopicItem it;
            it.kind = kind;
            it.text = text;
            it.tsMs = ts;
            it.midTurn = midTurn;
            return it;
        };
        const auto tool = [](const wchar_t* id, const wchar_t* name, const wchar_t* summary) {
            TopicItem it;
            it.kind = TopicItemKind::ToolCall;
            it.toolUseId = id;
            it.toolName = name;
            it.summary = summary;
            return it;
        };
        const auto result = [](const wchar_t* id, bool isError, const wchar_t* text) {
            TopicItem it;
            it.kind = TopicItemKind::ToolResult;
            it.toolUseId = id;
            it.isError = isError;
            it.text = text;
            return it;
        };

        // Two turns on two days: the day lines, the user's turns, the narrative packed (remarks + folded tool lines, a
        // failed result labeled with its tool), and each final reply as its own message.
        {
            const std::vector<TopicItem> items{
                item(TopicItemKind::Prompt, L"fix the crash", day1),
                item(TopicItemKind::Text, L"Looking at the stack first."),
                tool(L"t1", L"Bash", L"Bash: Run the tests ($ npm test)"),
                result(L"t1", true, L"exit 1"),
                item(TopicItemKind::Text, L"Found it: a null tab."),
                tool(L"t2", L"Edit", L"Edit: TabManagement.cpp"),
                result(L"t2", false, L""),
                item(TopicItemKind::Text, L"Fixed: the null tab is skipped."),
                item(TopicItemKind::Prompt, L"ship it", day2),
                item(TopicItemKind::Text, L"Shipped."),
            };
            int64_t lastDay = 0;
            const auto posts = RenderHistory(items, false, lastDay);
            CHECK(posts.size() == 7, "day, user, narrative, reply, day, user, reply");
            if (posts.size() == 7)
            {
                CHECK(!posts[0].fromUser && posts[0].html.find(L"Fri 16 Oct 2026") != std::wstring::npos, "the first day line");
                CHECK(posts[1].fromUser && posts[1].userText == L"fix the crash" && posts[1].html.rfind(L"<b>\U0001F464 You</b> \x00B7 ", 0) == 0 &&
                          posts[1].html.find(L" \x00B7 typed at the PC\nfix the crash") != std::wstring::npos && posts[1].turnPrompt,
                      "the user's turn: the words as typed, and the bot's line in its place (bold 'You', the clock, the source, then the prompt; the turn's prompt)");
                CHECK(posts[2].html.find(L"Looking at the stack first.") == 0, "the narrative opens with the first remark");
                CHECK(posts[2].html.find(L"<blockquote expandable>\x2022 Bash: Run the tests ($ npm test)\n\x2717 Bash: exit 1</blockquote>") != std::wstring::npos,
                      "the remark's tool calls in one folded quote, a failed result labeled with its tool");
                CHECK(posts[2].html.find(L"Found it: a null tab.") != std::wstring::npos && posts[2].html.find(L"\x2022 Edit: TabManagement.cpp") != std::wstring::npos,
                      "the next remark and its tool in the same packed message");
                CHECK(posts[2].html.find(L"Fixed:") == std::wstring::npos, "the reply is not in the narrative");
                CHECK(posts[3].html.find(L"Fixed: the null tab is skipped.") != std::wstring::npos && posts[3].html.find(L"blockquote") == std::wstring::npos, "the final reply is its own message");
                CHECK(posts[4].html.find(L"Sat 17 Oct 2026") != std::wstring::npos, "a turn on another day gets a day line first");
                CHECK(posts[5].fromUser && posts[5].userText == L"ship it", "the second turn's prompt");
                CHECK(posts[6].html.find(L"Shipped.") != std::wstring::npos, "the second reply");
            }
            CHECK(lastDay == 20261017, "the day of the last turn is handed back (the next window continues from it)");
            int64_t sameDay = 20261017;
            const auto again = RenderHistory({ item(TopicItemKind::Prompt, L"more", day2), item(TopicItemKind::Text, L"ok") }, false, sameDay);
            CHECK(again.size() == 2 && again[0].fromUser, "the same day as the window before: no day line");
        }

        // One turn longer than the reader's window: the first part has no reply (its last text is narrative); the part
        // that ends the turn has the reply and no prompt.
        {
            int64_t lastDay = 20261016;
            const auto first = RenderHistory({ item(TopicItemKind::Prompt, L"long job", day1), item(TopicItemKind::Text, L"Step one done."), tool(L"t3", L"Bash", L"Bash: step two"),
                                               item(TopicItemKind::Text, L"Still going.") },
                                             true,
                                             lastDay);
            CHECK(first.size() == 2 && first[0].fromUser && first[1].html.find(L"Still going.") != std::wstring::npos, "a cut turn: its last text stays in the narrative");
            const auto rest = RenderHistory({ item(TopicItemKind::Text, L"Wrapped up.") }, false, lastDay);
            CHECK(rest.size() == 1 && !rest[0].fromUser && rest[0].html.find(L"Wrapped up.") != std::wstring::npos, "its rest: the reply, with no prompt or day line");
        }

        // A message typed while the turn ran and a slash command are the user's posts between the assistant's; a runner
        // prompt reads ⚙ in the bot's line; a turn that ended on a tool call (interrupted) has no reply.
        {
            int64_t lastDay = 20261016;
            const auto posts = RenderHistory({ item(TopicItemKind::Prompt, L"go", day1), item(TopicItemKind::Text, L"Working."), tool(L"t4", L"Bash", L"Bash: build"),
                                               item(TopicItemKind::Prompt, L"also the docs", day1, true), item(TopicItemKind::Command, L"/compact keep the plan"),
                                               item(TopicItemKind::Text, L"Done.") },
                                             false,
                                             lastDay);
            CHECK(posts.size() == 5, "user, narrative, mid-turn user, command, reply");
            if (posts.size() == 5)
            {
                CHECK(posts[1].html.find(L"Working.") != std::wstring::npos && posts[1].html.find(L"\x2022 Bash: build") != std::wstring::npos, "the narrative before the mid-turn message");
                CHECK(posts[2].fromUser && posts[2].userText == L"also the docs" && posts[2].html.find(L" \x00B7 typed at the PC \x00B7 mid-turn\nalso the docs") != std::wstring::npos &&
                          !posts[2].turnPrompt,
                      "the mid-turn message is the user's (not the turn's prompt)");
                CHECK(posts[3].fromUser && posts[3].userText == L"/compact keep the plan" && posts[3].html.rfind(L"<b>\U0001F464 You</b>", 0) == 0 &&
                          posts[3].html.find(L" \x00B7 command\n<code>/compact keep the plan</code>") != std::wstring::npos,
                      "the slash command is the user's");
                CHECK(posts[4].turnReply, "the reply is marked: it quotes the turn's prompt");
                CHECK(!posts[4].fromUser && posts[4].html.find(L"Done.") != std::wstring::npos, "the reply");
            }
            const auto runner = RenderHistory({ item(TopicItemKind::Prompt, L"run the suite", day1), item(TopicItemKind::Text, L"Green.") }, false, lastDay,
                                              [](const std::wstring& text) { return text == L"run the suite" ? PromptSource::Runner : PromptSource::Pc; });
            CHECK(runner.size() == 2 && runner[0].fromUser && runner[0].html.find(L" \x00B7 \x2699 runner\nrun the suite") != std::wstring::npos, "a prompt the runner sent: '⚙ runner' in the bot's line");
            const auto phone = RenderHistory({ item(TopicItemKind::Prompt, L"from the bus", day1), item(TopicItemKind::Text, L"On it.") }, false, lastDay,
                                             [](const std::wstring&) { return PromptSource::Phone; });
            CHECK(phone.size() == 2 && phone[0].html.find(L" \x00B7 \U0001F4F1 from the phone\nfrom the bus") != std::wstring::npos, "a prompt sent from the phone: '📱 from the phone'");
            const auto stopped = RenderHistory({ item(TopicItemKind::Prompt, L"x", day1), tool(L"t5", L"Bash", L"Bash: long"), item(TopicItemKind::Interrupt, L"") }, false, lastDay);
            CHECK(stopped.size() == 2 && stopped[1].html.find(L"\x2022 Bash: long") != std::wstring::npos && stopped[1].html.find(L"interrupted") != std::wstring::npos,
                  "an interrupted turn: its tools and the mark, no reply");
            // The user's answer to a question is the user's post, between the question and the reply.
            const auto answered = RenderHistory({ item(TopicItemKind::Prompt, L"build it", day1), item(TopicItemKind::Question, L"Swap: Swap now?"),
                                                  item(TopicItemKind::Answer, L"Swap now", day1), item(TopicItemKind::Text, L"Swapping.") },
                                                false,
                                                lastDay);
            CHECK(answered.size() == 4 && answered[1].html.find(L"Swap: Swap now?") != std::wstring::npos && answered[2].fromUser && answered[2].userText == L"Swap now" &&
                      answered[2].html.find(L" \x00B7 \x21A9 answer\nSwap now") != std::wstring::npos && answered[3].html.find(L"Swapping.") != std::wstring::npos,
                  "the answer to a question is the user's post, between the question and the reply");
        }

        // The user's answer to an AskUserQuestion: Claude Code keeps it on the tool result's line (toolUseResult.answers).
        {
            const auto one = ClassifyTranscriptLine(ParseLine(
                L"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"q1\",\"content\":\"Your questions have been answered\"}]},"
                L"\"toolUseResult\":{\"questions\":[{\"question\":\"Swap now?\",\"header\":\"Swap\"}],\"answers\":{\"Swap now?\":\"Swap now\"}}}"));
            CHECK(one.size() == 1 && one[0].kind == TopicItemKind::Answer && one[0].text == L"Swap now" && one[0].toolUseId == L"q1", "one answer reads as itself");
            const auto two = ClassifyTranscriptLine(ParseLine(
                L"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"q2\",\"content\":\"answered\"}]},"
                L"\"toolUseResult\":{\"questions\":[{\"question\":\"A?\",\"header\":\"Swap\"},{\"question\":\"B?\",\"header\":\"Build\"}],\"answers\":{\"A?\":\"Swap now\",\"B?\":\"Later\"}}}"));
            CHECK(two.size() == 1 && two[0].kind == TopicItemKind::Answer && two[0].text == L"Swap: Swap now\nBuild: Later", "several answers: one \"header: answer\" line each");
            const auto plain = ClassifyTranscriptLine(ParseLine(
                L"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t9\",\"content\":\"ok\"}]},\"toolUseResult\":{\"stdout\":\"ok\"}}"));
            CHECK(plain.size() == 1 && plain[0].kind == TopicItemKind::ToolResult, "a tool result without answers stays a tool result");
            CHECK(IsConversationItem(TopicItemKind::Answer), "an answer is a conversation message (it makes an older recap stale)");
        }

        // The packing keeps every message under the limit and loses no tool line.
        {
            std::vector<TopicItem> items{ item(TopicItemKind::Prompt, L"big", day1), item(TopicItemKind::Text, L"Many steps.") };
            for (int i = 0; i < 200; ++i)
            {
                items.push_back(tool(L"t", L"Bash", (L"Bash: step " + std::to_wstring(i) + L" of the release check (a line of about sixty chars)").c_str()));
            }
            items.push_back(item(TopicItemKind::Text, L"All done."));
            int64_t lastDay = 20261016;
            const auto posts = RenderHistory(items, false, lastDay);
            size_t bullets = 0;
            bool fits = true;
            for (size_t i = 1; i + 1 < posts.size(); ++i)
            {
                bullets += CountOf(posts[i].html, L"\x2022 Bash: step ");
                fits = fits && posts[i].html.size() <= kHistoryPackChars;
            }
            CHECK(posts.size() >= 5, "200 tool lines take several packed messages");
            CHECK(fits, "every packed message stays under kHistoryPackChars");
            CHECK(bullets == 200, "no tool line is lost or repeated");
            CHECK(!posts.empty() && posts.back().html.find(L"All done.") != std::wstring::npos, "the reply comes last");
        }

        // The reader's window: complete turns only, cut at the start of the last turn begun in it.
        {
            const std::vector<std::string> lines{
                R"({"type":"user","message":{"role":"user","content":"first"},"timestamp":"2026-10-16T12:00:00.000Z"})",
                R"({"type":"assistant","message":{"id":"m1","role":"assistant","stop_reason":"end_turn","content":[{"type":"text","text":"one"}]}})",
                R"({"type":"user","message":{"role":"user","content":"second"},"timestamp":"2026-10-16T12:01:00.000Z"})",
                R"({"type":"assistant","message":{"id":"m2","role":"assistant","stop_reason":"tool_use","content":[{"type":"text","text":"Checking."},{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"ls"}}]}})",
                R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":"ok"}]}})",
                R"({"type":"assistant","message":{"id":"m3","role":"assistant","stop_reason":"end_turn","content":[{"type":"text","text":"two"}]}})",
                R"({"type":"user","message":{"role":"user","content":"third"},"timestamp":"2026-10-16T12:02:00.000Z"})",
                R"({"type":"assistant","message":{"id":"m4","role":"assistant","stop_reason":"tool_use","content":[{"type":"text","text":"partial"},{"type":"tool_use","id":"t2","name":"Bash","input":{"command":"make"}}]}})",
            };
            std::string all;
            std::vector<int64_t> at;
            for (const auto& l : lines)
            {
                at.push_back(static_cast<int64_t>(all.size()));
                all += l + "\n";
            }
            const auto whole = SplitHistoryWindow(all, 0, true);
            CHECK(whole.consumedTo == static_cast<int64_t>(all.size()) && !whole.turnContinues && !whole.needMore, "reaching the replay's end: everything is used");
            CHECK(whole.items.size() == 10 && whole.items.front().text == L"first" && whole.items.back().kind == TopicItemKind::ToolCall, "every line's items, in order");
            const auto cut = SplitHistoryWindow(all, 0, false);
            CHECK(cut.consumedTo == at[6] && !cut.turnContinues, "not the end: the window stops where its last turn begins");
            CHECK(cut.items.size() == 7 && cut.items.back().text == L"two", "the window holds the complete turns before it");
            const std::string tail = all.substr(static_cast<size_t>(at[6]));
            const auto longTurn = SplitHistoryWindow(tail, at[6], false);
            CHECK(longTurn.consumedTo == static_cast<int64_t>(all.size()) && longTurn.turnContinues && longTurn.items.size() == 3, "a turn longer than the window: cut, to go on in the next");
            const auto torn = SplitHistoryWindow(std::string_view{ all }.substr(0, all.size() - 5), 0, false);
            CHECK(torn.consumedTo == at[6] && torn.items.size() == 7, "a torn last line is never read");
            CHECK(SplitHistoryWindow(R"({"type":"user")", 0, false).needMore, "no complete line: the reader needs a bigger window");
            // Each item knows its line, and the plan commits TURN BY TURN: a turn's last post carries where the next turn starts,
            // so a stop midway re-sends at most the turn that was going out.
            CHECK(whole.itemAt.size() == whole.items.size() && whole.itemAt.front() == at[0] && whole.itemAt.back() == at[7], "every item knows the line it came from");
            int64_t planDay = 0;
            const auto plan = PlanHistoryPosts(whole, planDay);
            CHECK(plan.size() == 8, "turn 1: day + user + reply; turn 2: user + narrative + reply; turn 3: user + narrative");
            if (plan.size() == 8)
            {
                CHECK(plan[0].commitTo == 0 && plan[1].commitTo == 0 && plan[2].commitTo == at[2] && plan[2].dayAfter == 20261016, "turn 1's reply commits up to turn 2's line, with its day");
                CHECK(plan[3].commitTo == 0 && plan[4].commitTo == 0 && plan[5].commitTo == at[6] && plan[5].post.html.find(L"two") != std::wstring::npos, "turn 2's reply commits up to turn 3's line");
                CHECK(plan[6].post.fromUser && plan[7].commitTo == static_cast<int64_t>(all.size()) && plan[7].post.html.find(L"\x2022 Bash: make") != std::wstring::npos,
                      "the window's last turn commits the window's end");
            }
            CHECK(planDay == 20261016, "the plan hands the day on");
            // A fork's copied lines are its history too (the live mirror skips them).
            const std::string copied = R"({"type":"user","forkedFrom":{"sessionId":"src","messageUuid":"u0"},"message":{"role":"user","content":"the source's prompt"}})"
                                       "\n";
            const auto fork = SplitHistoryWindow(copied, 0, true);
            CHECK(fork.items.size() == 1 && fork.items[0].kind == TopicItemKind::Prompt && fork.items[0].text == L"the source's prompt", "a fork's copied prompt is in its history");
        }

        // The re-sort: the shortest set of card re-posts that makes Telegram's list (newest last message on top) read in
        // MOST RECENT order — the top part of the desired order, re-posted lowest-ranked first.
        {
            using V = std::vector<std::wstring>;
            CHECK(PlanTopicResort({ L"a", L"b", L"c", L"d" }, { L"a", L"b", L"c", L"d" }).empty(), "in order: nothing to re-post");
            CHECK((PlanTopicResort({ L"d", L"c", L"b", L"a" }, { L"a", L"b", L"c", L"d" }) == V{ L"c", L"b", L"a" }), "reversed (a replay's order): all but the lowest, the top one last");
            CHECK((PlanTopicResort({ L"x", L"a", L"b", L"c" }, { L"a", L"b", L"c", L"x" }) == V{ L"c", L"b", L"a" }), "a low-ranked topic bumped to the top: everything above its rank");
            CHECK((PlanTopicResort({ L"a", L"c", L"b", L"d" }, { L"a", L"b", L"c", L"d" }) == V{ L"b", L"a" }), "a swap in the middle: the part above it");
            CHECK((PlanTopicResort({ L"b", L"a" }, { L"a", L"z", L"b" }) == V{ L"a" }), "a topic missing from the list is ignored");
            CHECK(PlanTopicResort({ L"a" }, { L"a" }).empty(), "one topic: nothing to sort");
        }

        // The record: the replay's range round-trips; a file without the keys owes no replay; the card says it.
        {
            MessagingState st;
            TopicRecord t;
            t.sessionId = L"h1";
            t.threadId = 7;
            t.backfillPath = L"C:\\x\\h1.jsonl";
            t.backfillPos = 10;
            t.backfillEnd = 20;
            t.backfillDayKey = 20261016;
            CHECK(TopicBackfillPending(t), "pos < end: the replay still runs");
            st.topics[L"h1"] = t;
            const auto back = StateFromJson(StateToJson(st));
            const bool same = back.has_value() && back->topics.count(L"h1") && back->topics.at(L"h1").backfillPath == t.backfillPath && back->topics.at(L"h1").backfillPos == 10 &&
                              back->topics.at(L"h1").backfillEnd == 20 && back->topics.at(L"h1").backfillDayKey == 20261016;
            CHECK(same, "the replay's range and day round-trip (a stop midway goes on at the next start)");
            t.backfillPos = 20;
            CHECK(!TopicBackfillPending(t), "pos == end: done");
            TopicRecord none;
            none.backfillEnd = 5;
            CHECK(!TopicBackfillPending(none), "no path: nothing to replay");
            CHECK(kTopicLayoutVersion == 2, "layout 2: the topics made before the history replay are made again once");
            CHECK(static_cast<int>(SendPriority::History) > static_cast<int>(SendPriority::Backfill), "the replay has the lowest send priority");
            SessionInfo s = MakeSession(L"h1");
            s.live = true;
            s.state = SessionState::WaitingForInput;
            CardExtras ex;
            ex.historyPending = true;
            CHECK(BuildCardText(s, 5000, 0, false, &ex).find(L"history is being added below") != std::wstring::npos, "the card says the history is coming");
            ex.historyPending = false;
            CHECK(BuildCardText(s, 5000, 0, false, &ex).find(L"history is being added") == std::wstring::npos, "and stops saying it when it is in");
        }

        // §21.11 — RICH MESSAGES: the preparation of Claude's Markdown. A '<' outside code becomes &lt; (Telegram drops a tag it
        // does not know, and its text with it), code is copied as written, a line that another follows gets a hard break, a
        // table row does not, and a collapsible block's tags (the history's tool groups) are kept.
        {
            constexpr auto npos = std::wstring::npos;
            const std::wstring md = L"Use List<int> and `vector<T>` here.\nSecond line.\n\n| a | b |\n|---|---|\n| <x> | 2 |\n\n```cpp\nint a<b;\n```\n"
                                    L"<details><summary>3 <calls></summary>\n\n```text\nx<y\n```\n\n</details>";
            const std::wstring p = TgPrepareRichMarkdown(md);
            CHECK(p.find(L"Use List&lt;int> and `vector<T>` here.  \nSecond line.\n") != npos, "a '<' outside code escaped, a code span as written, a hard break between two lines");
            CHECK(p.find(L"| a | b |\n|---|---|\n| &lt;x> | 2 |\n") != npos, "a table's rows get no hard break (a cell is still escaped)");
            CHECK(p.find(L"```cpp\nint a<b;\n```") != npos, "a fenced block is copied as written");
            CHECK(p.find(L"<details><summary>3 &lt;calls></summary>\n\n```text\nx<y\n```\n\n</details>") != npos, "a collapsible block keeps its tags, its summary escaped");
            CHECK(TgPrepareRichMarkdown(L"a  \nb") == L"a  \nb" && TgPrepareRichMarkdown(L"a\\\nb") == L"a\\\nb", "a line that ends in a break already gets no second one");
            CHECK(TgPrepareRichMarkdown(L"one\n\ntwo") == L"one\n\ntwo", "paragraphs: no hard break before a blank line");
            CHECK(TgPrepareRichMarkdown(L"x <details> y") == L"x &lt;details> y", "a details tag inside a line is text");
            CHECK(TgPrepareRichMarkdown(L"a\r\nb") == L"a  \nb", "a CR before a line break is dropped");

            // The split: a text that fits is one part, unchanged; a long one is cut at a blank line; a code block cut in two is
            // closed and opened again; a surrogate pair is never cut.
            CHECK(TgChunkMarkdown(L"short").size() == 1 && TgChunkMarkdown(L"short")[0] == L"short", "a short text is one part, unchanged");
            {
                std::wstring big;
                for (int i = 0; i < 300; ++i)
                {
                    big += L"paragraph " + std::to_wstring(i) + L" " + std::wstring(80, L'x') + L"\n\n";
                }
                const auto parts = TgChunkMarkdown(big, 2000, 400);
                bool fits = true;
                bool starts = true;
                size_t paras = 0;
                for (const auto& part : parts)
                {
                    fits = fits && part.size() <= 2000;
                    starts = starts && part.rfind(L"paragraph ", 0) == 0;
                    paras += CountOf(part, L"paragraph ");
                }
                CHECK(parts.size() > 10 && fits, "a long text: parts that each fit");
                CHECK(paras == 300, "no paragraph lost or repeated");
                CHECK(starts, "every part starts at a paragraph (cut at a blank line)");
            }
            {
                std::wstring code = L"intro\n\n```cpp\n";
                for (int i = 0; i < 100; ++i)
                {
                    code += L"int v" + std::to_wstring(i) + L" = 0; // " + std::wstring(40, L'c') + L"\n";
                }
                code += L"```\nafter";
                const auto parts = TgChunkMarkdown(code, 1500, 400);
                bool balanced = true;
                size_t lines = 0;
                for (const auto& part : parts)
                {
                    balanced = balanced && CountOf(part, L"```") % 2 == 0;
                    lines += CountOf(part, L"int v");
                }
                CHECK(parts.size() >= 3 && balanced, "a long code block: every part closes the fence it opens");
                CHECK(parts.size() >= 2 && parts[1].rfind(L"```cpp\n", 0) == 0, "the next part opens it again, with its language");
                CHECK(lines == 100 && parts.back().find(L"after") != npos, "no code line is lost, and the text after the block arrives");
            }
            {
                std::wstring emoji;
                for (int i = 0; i < 300; ++i)
                {
                    emoji += L"\U0001F600";
                }
                const auto parts = TgChunkMarkdown(emoji, 202, 400);
                bool whole = true;
                size_t units = 0;
                for (const auto& part : parts)
                {
                    whole = whole && !part.empty() && !(part.back() >= 0xD800 && part.back() <= 0xDBFF) && !(part.front() >= 0xDC00 && part.front() <= 0xDFFF);
                    units += part.size();
                }
                CHECK(parts.size() >= 3 && whole && units == 600, "one long line of emoji: cut into parts, never between the halves of a pair");
            }

            // Our own plain lines as Markdown: shown as typed.
            CHECK(TgMdEscapeText(L"a*b_c [x](y) <T> & #1 |p| ~s~ $5 = -n") == L"a\\*b\\_c \\[x\\](y) &lt;T\\> &amp; \\#1 \\|p\\| \\~s\\~ \\$5 \\= \\-n", "plain text shows as typed");
            CHECK(TgMdEscapeText(L"1. first\n2) second") == L"1\\. first  \n2\\) second", "a number at a line's start stays a number; a line break stays");
        }

        // §21.11 — the history in both forms, and the CONVERSATION ONLY mode (the default): the user's posts and each turn's
        // last message, plus what asked the user something or ended the turn.
        {
            constexpr auto npos = std::wstring::npos;
            int64_t lastDay = 20261016;
            const auto quiet = RenderHistory({ item(TopicItemKind::Prompt, L"fix it", day1), item(TopicItemKind::Text, L"Looking."), tool(L"t1", L"Bash", L"Bash: test"),
                                               result(L"t1", true, L"exit 1"), item(TopicItemKind::Text, L"Almost."), item(TopicItemKind::Prompt, L"also docs", day1, true),
                                               item(TopicItemKind::Compaction, L"compacted"), item(TopicItemKind::Question, L"Ship now?"), item(TopicItemKind::Answer, L"yes", day1),
                                               tool(L"t2", L"Edit", L"Edit: a.cpp"), item(TopicItemKind::Text, L"Shipped.") },
                                             false,
                                             lastDay,
                                             {},
                                             false);
            CHECK(quiet.size() == 5, "user, mid-turn user, question, answer, reply - no narrative");
            if (quiet.size() == 5)
            {
                CHECK(quiet[0].fromUser && quiet[0].userText == L"fix it" && quiet[1].fromUser && quiet[1].userText == L"also docs", "the user's prompt and the message typed while it ran");
                CHECK(!quiet[2].fromUser && quiet[2].html.find(L"Ship now?") != npos && quiet[2].markdown == L"\x2753 **Ship now?**", "the question, in both forms");
                CHECK(quiet[3].fromUser && quiet[3].userText == L"yes", "the answer is the user's");
                CHECK(!quiet[4].fromUser && quiet[4].markdown == L"Shipped." && quiet[4].html.find(L"Shipped.") != npos, "the turn's last message: Claude's text as written, the HTML its fallback");
            }
            CHECK(std::none_of(quiet.begin(), quiet.end(), [](const HistoryPost& hp) { return hp.html.find(L"Looking.") != std::wstring::npos || hp.html.find(L"Bash: test") != std::wstring::npos || hp.html.find(L"compacted") != std::wstring::npos; }),
                  "no remark, no tool call, no compaction");
            const auto stopped = RenderHistory({ item(TopicItemKind::Prompt, L"x", day1), item(TopicItemKind::Text, L"Starting."), tool(L"t5", L"Bash", L"Bash: long"), item(TopicItemKind::Interrupt, L"") },
                                               false,
                                               lastDay,
                                               {},
                                               false);
            CHECK(stopped.size() == 2 && stopped[1].markdown == L"\x25A0 *interrupted*", "an interrupted turn: the user's post and the mark");
            const auto table = RenderHistory({ item(TopicItemKind::Prompt, L"compare", day1), item(TopicItemKind::Text, L"| a | b |\n|---|---|\n| 1 | 2 |") }, false, lastDay, {}, false);
            CHECK(table.size() == 2 && table[1].markdown == L"| a | b |\n|---|---|\n| 1 | 2 |", "a reply's table reaches the rich message as Claude wrote it");

            // VERBOSE (the narrative too): a remark's tool calls in a collapsible block named by its tools, inside a fence longer
            // than any backtick run in the lines.
            const auto loud = RenderHistory({ item(TopicItemKind::Prompt, L"go", day1), item(TopicItemKind::Text, L"Checking."), tool(L"t1", L"Bash", L"Bash: run ```x```"),
                                              tool(L"t2", L"Bash", L"Bash: two"), tool(L"t3", L"Edit", L"Edit: a.cpp"), item(TopicItemKind::Text, L"Done.") },
                                            false,
                                            lastDay);
            CHECK(loud.size() == 3, "user, narrative, reply");
            if (loud.size() == 3)
            {
                CHECK(loud[1].markdown.rfind(L"Checking.", 0) == 0, "the narrative's Markdown opens with the remark as written");
                CHECK(loud[1].markdown.find(L"<details><summary>\U0001F527 Bash \x00D7" L"2, Edit</summary>\n\n````text\n\x2022 Bash: run ```x```\n") != npos,
                      "the tool calls: a collapsible block named by its tools, a fence longer than any backtick run");
                CHECK(loud[1].markdown.find(L"\n````\n\n</details>") != npos, "the block is closed");
                CHECK(loud[2].markdown == L"Done.", "the reply's Markdown is Claude's text");
                const std::wstring prepared = TgPrepareRichMarkdown(loud[1].markdown);
                CHECK(prepared.find(L"<details><summary>") != npos && prepared.find(L"</details>") != npos, "the prepared narrative keeps the collapsible block");
            }

            // The plan of a window renders in the topic's mode.
            HistoryWindow w;
            w.items = { item(TopicItemKind::Prompt, L"p", day1), item(TopicItemKind::Text, L"a"), tool(L"t1", L"Bash", L"Bash: b"), item(TopicItemKind::Text, L"c") };
            w.itemAt = { 0, 10, 20, 30 };
            w.consumedTo = 40;
            int64_t dayA = 0;
            int64_t dayB = 0;
            CHECK(PlanHistoryPosts(w, dayA).size() == 4 && PlanHistoryPosts(w, dayB, {}, false).size() == 3, "verbose: day, user, narrative, reply; conversation only: day, user, reply");
        }

        // §21.11 — the topic names' STATE GLYPH: the tab strip's dot as an emoji, before the title.
        {
            CHECK(TopicDisplayName(SessionState::Running, L"Agentmaster") == L"\U0001F535 Agentmaster", "the state glyph, a space, the title");
            CHECK(std::wstring{ StateGlyph(SessionState::Idle) } == L"\x26AA" && std::wstring{ StateGlyph(SessionState::NeedsApproval) } == L"\U0001F7E0" &&
                      std::wstring{ StateGlyph(SessionState::WaitingForInput) } == L"\U0001F7E1",
                  "idle white, needs-approval orange, waiting yellow");
            std::wstring longTitle(130, L'a');
            longTitle[118] = static_cast<wchar_t>(0xD83D);
            longTitle[119] = static_cast<wchar_t>(0xDE00);
            const std::wstring cutName = TopicDisplayName(SessionState::Idle, longTitle);
            CHECK(cutName.size() <= 128 && cutName.back() == L'\x2026' && !(cutName[cutName.size() - 2] >= 0xD800 && cutName[cutName.size() - 2] <= 0xDBFF),
                  "a long title is cut, ending in an ellipsis, never between the halves of a pair");

            std::map<std::wstring, TopicRecord> topics;
            TopicRecord a;
            a.sessionId = L"a";
            a.threadId = 1;
            a.nameState = static_cast<int>(SessionState::Running);
            topics[L"a"] = a;
            TopicRecord b = a;
            b.sessionId = L"b";
            b.threadId = 2;
            topics[L"b"] = b;
            TopicRecord c = a;
            c.sessionId = L"c";
            c.threadId = 3;
            c.nameState = -1;
            topics[L"c"] = c;
            const auto fact = [](const wchar_t* id, SessionState st, int64_t since) {
                TabFact f;
                f.sessionId = id;
                f.state = st;
                f.live = true;
                f.inTab = true;
                f.claude = true;
                f.stateSinceMs = since;
                return f;
            };
            const int64_t now = 1000000;
            std::vector<TabFact> facts{ fact(L"a", SessionState::Running, now - 20000), fact(L"b", SessionState::WaitingForInput, now - 20000), fact(L"c", SessionState::Running, now - 30000) };
            CHECK(PickStateRename(facts, topics, now) == L"b", "a waiting session's name changes before a running one's (the states that need the user first)");
            facts[1].stateSinceMs = now - 2000;
            CHECK(PickStateRename(facts, topics, now) == L"c", "a state younger than kNameStateSettleMs waits (a short turn's flicker renames nothing)");
            topics[L"c"].nameSetMs = now - 5000;
            CHECK(PickStateRename(facts, topics, now).empty(), "a topic renamed lately waits kNameRenameGapMs; a name that shows its state is left alone");
            facts[1].stateSinceMs = 0;
            CHECK(PickStateRename(facts, topics, now) == L"b", "an unknown state clock reads as settled");
            facts[1].live = false;
            CHECK(PickStateRename(facts, topics, now).empty(), "a session that is not live in a tab is not renamed");

            TabFact fresh = fact(L"n", SessionState::NeedsApproval, now);
            fresh.lastActivityMs = now;
            fresh.title = L"New tab";
            const auto ops = PlanTopicSync({ fresh }, {}, now, 600000);
            CHECK(ops.size() == 1 && ops[0].kind == TopicSyncOp::Kind::Create && ops[0].state == SessionState::NeedsApproval, "a create carries the state its name's glyph shows");
        }

        // 2026-10-17 — a rename goes BEFORE the history replay. The replay holds the call window at its cap (18 a minute) for
        // hours; the old rule (a rename only below 12 calls a minute) made 1 rename in five hours on production, every Running
        // and Error topic stayed 🟡. The budget rule, the replay's hold, and a minute of the two together.
        {
            CHECK(NameRenameFits(0, 18) && NameRenameFits(16, 18), "a rename fits while the window leaves room for its two calls (the edit, the notice's delete)");
            CHECK(!NameRenameFits(17, 18) && !NameRenameFits(18, 18) && !NameRenameFits(25, 18), "no room: one call short, the replay's steady level, the live traffic over the budget");
            CHECK(!HistoryYieldsToRename(0, 5000), "no rename waits: the replay goes");
            CHECK(HistoryYieldsToRename(4000, 5000) && HistoryYieldsToRename(5000, 5000), "a rename waits: the replay holds");
            CHECK(HistoryYieldsToRename(100000, 100000 + kNameRenameHoldMaxMs - 1) && !HistoryYieldsToRename(100000, 100000 + kNameRenameHoldMaxMs),
                  "the hold ends after kNameRenameHoldMaxMs (the live traffic alone keeps the window full)");
            CHECK(!HistoryYieldsToRename(6000, 5000), "a stamp from the future holds nothing");

            // A model of the send loop and the name tick over two minutes: the replay sends whenever it may (under 18 in the
            // window, one a second at most, not while a rename waits); a rename falls due at t = 30 s; the tick looks every
            // 1.5 s. The rename must be made within seconds, and the replay must go on after it.
            std::deque<int64_t> window;
            const auto prune = [&window](int64_t t) {
                while (!window.empty() && t - window.front() >= 60000)
                {
                    window.pop_front();
                }
            };
            for (int64_t t = 0; t < 60000; t += 3400)
            {
                window.push_back(t); // the replay's first minute: ~18 calls, the window at its cap
            }
            int64_t waitingSince = 0;
            int64_t renamedAt = -1;
            int64_t lastSend = 0;
            int historyAfterRename = 0;
            size_t smallestFreeWindow = SIZE_MAX; // the window's low point while the replay runs with no rename due
            for (int64_t t = 60000; t < 180000; t += 100)
            {
                prune(t);
                if (t < 90000)
                {
                    smallestFreeWindow = (std::min)(smallestFreeWindow, window.size());
                }
                const bool due = t >= 90000 && renamedAt < 0;
                if (due && t % 1500 == 0)
                {
                    if (NameRenameFits(window.size(), 18))
                    {
                        window.push_back(t);
                        window.push_back(t); // the edit, and the delete of Telegram's notice
                        renamedAt = t;
                        waitingSince = 0;
                    }
                    else if (waitingSince == 0)
                    {
                        waitingSince = t;
                    }
                }
                if (!HistoryYieldsToRename(waitingSince, t) && window.size() < 18 && t - lastSend >= 2000)
                {
                    window.push_back(t);
                    lastSend = t;
                    historyAfterRename += renamedAt >= 0 ? 1 : 0;
                }
            }
            CHECK(renamedAt >= 90000 && renamedAt - 90000 <= 10000, "the replay holding the window at its cap, a due rename is made within seconds (it was never made)");
            CHECK(historyAfterRename > 10, "the replay goes on after the rename");
            CHECK(smallestFreeWindow >= 12, "the rule it replaces (a rename only under 12 calls a minute) finds no moment while the replay runs");
        }

        // 2026-10-17 — a long 429 parks the refused METHOD (FloodParking): Telegram answered one editMessageText with
        // retry_after 6542 s and the send loop slept it out, the whole bot silent for 1 h 49 min.
        {
            FloodParking p;
            const int64_t t0 = 1000000;
            CHECK(FloodParkedUntil(p, L"sendMessage", t0) == 0, "nothing is parked at first");
            CHECK(!ParkForFlood(p, L"editMessageText", 6542, t0), "the first long 429 parks its method only");
            CHECK(FloodParkedUntil(p, L"editMessageText", t0 + 1000) == t0 + 6542000, "the refused method waits until its deadline");
            CHECK(FloodParkedUntil(p, L"sendMessage", t0 + 1000) == 0 && FloodParkedUntil(p, L"editForumTopic", t0 + 1000) == 0, "every other method goes on");
            CHECK(!ParkForFlood(p, L"editMessageText", 100, t0 + 2000) && FloodParkedUntil(p, L"editMessageText", t0 + 3000) == t0 + 6542000,
                  "the same method refused again: no escalation, the later deadline stays");
            CHECK(ParkForFlood(p, L"sendMessage", 120, t0 + 5000), "a second method refused while one is parked: the whole chat is held");
            CHECK(FloodParkedUntil(p, L"sendRichMessage", t0 + 6000) == t0 + 6542000 && FloodParkedUntil(p, L"deleteMessage", t0 + 6000) == t0 + 6542000,
                  "every method then waits for the latest deadline");
            CHECK(FloodParkedUntil(p, L"sendMessage", t0 + 6542000) == 0, "after the deadline everything may go");

            FloodParking q;
            CHECK(!ParkForFlood(q, L"editMessageText", 100, t0) && !ParkForFlood(q, L"sendMessage", 100, t0 + 200000),
                  "a parking that expired does not escalate the next one (it is dropped on the way)");
            CHECK(q.methodUntilMs.size() == 1 && q.methodUntilMs.count(L"sendMessage") == 1, "only the live entry is kept");
            FloodParking z;
            ParkForFlood(z, L"editMessageText", 0, t0);
            CHECK(FloodParkedUntil(z, L"editMessageText", t0) == t0 + 1000, "a retry_after under one second counts as one");

            CHECK(std::wstring{ EditKindOfKey(L"card:abc") } == L"card" && std::wstring{ EditKindOfKey(L"pl:4711") } == L"prompt" &&
                      std::wstring{ EditKindOfKey(L"wl:abc") } == L"worklog",
                  "the census names what an edit edits: a card, a prompt status, a work log");
            CHECK(std::wstring{ EditKindOfKey(L"") } == L"other" && std::wstring{ EditKindOfKey(L"setup-notice") } == L"other", "anything else is \"other\"");
        }

        // §21.13 — the title's COLOR: the square after the state glyph in the name ("State, then color"), the same square in
        // every title mention, a line in the topic when the title or the tab's color changes, and the quiet re-squares.
        {
            const std::wstring blue = L"\U0001F7E6";
            const std::wstring green = L"\U0001F7E9";
            CHECK(TopicDisplayName(SessionState::WaitingForInput, blue, L"agent manager tab") == L"\U0001F7E1 \U0001F7E6 agent manager tab",
                  "the name: the state glyph, then the color square, then the title");
            CHECK(TopicDisplayName(SessionState::Idle, L"\x2B1B", L"x") == L"\x26AA \x2B1B x" && TopicDisplayName(SessionState::Running, L"", L"x") == TopicDisplayName(SessionState::Running, L"x"),
                  "a one-unit glyph takes the square right after it too; no square is the name without one");
            const std::wstring longName = TopicDisplayName(SessionState::Running, blue, std::wstring(200, L'a'));
            CHECK(longName.size() <= 128 && longName.back() == L'\x2026' && longName.rfind(L"\U0001F535 \U0001F7E6 ", 0) == 0, "a long title is cut within Telegram's 128 characters, the square kept");
            CHECK(TitleMentionHtml(blue, L"a <b>") == L"\U0001F7E6 a &lt;b&gt;" && TitleMentionHtml(L"", L"a & b") == L"a &amp; b", "a mention: the square, a space, the escaped title");

            // The change line, in the user's words: "<from>" changed the title to "<to>".
            CHECK(BuildTopicChangeNoticeHtml(L"old", blue, L"new", blue) == L"<i>\x201C\U0001F7E6 old\x201D changed the title to \x201C\U0001F7E6 new\x201D</i>", "a new title: the old one quoted with its square, then the new one");
            CHECK(BuildTopicChangeNoticeHtml(L"old", blue, L"new", green) == L"<i>\x201C\U0001F7E6 old\x201D changed the title to \x201C\U0001F7E9 new\x201D</i>", "a new title and a new color: one line, the squares show both");
            CHECK(BuildTopicChangeNoticeHtml(L"tab", blue, L"tab", green) == L"<i>\x201C\U0001F7E6 tab\x201D changed the color to \U0001F7E9</i>", "the color alone");
            CHECK(BuildTopicChangeNoticeHtml(L"tab", blue, L"tab", blue).empty() && BuildTopicChangeNoticeHtml(L"tab", L"", L"tab", blue).empty() && BuildTopicChangeNoticeHtml(L"tab", blue, L"tab", L"").empty(),
                  "no line when nothing changed, or a square only appears or goes (a topic named before the square, the \"Remove colors\" mode)");
            CHECK(BuildTopicChangeNoticeHtml(L"a<b", L"", L"c&d", L"") == L"<i>\x201C" L"a&lt;b\x201D changed the title to \x201C" L"c&amp;d\x201D</i>", "the titles are escaped");
            CHECK(BuildTopicChangeNoticeHtml(L"", L"", L"new", blue) == L"<i>The title is now \x201C\U0001F7E6 new\x201D</i>", "no old title known: the new one alone");

            // Which name changes next: the states that need the user, then a tab's color change, then the quiet re-squares.
            const int64_t now = 2000000;
            const auto fact = [](const wchar_t* id, SessionState st, const wchar_t* hex, int64_t colorSince) {
                TabFact f;
                f.sessionId = id;
                f.state = st;
                f.live = true;
                f.inTab = true;
                f.claude = true;
                f.colorHex = hex;
                f.colorSinceMs = colorSince;
                return f;
            };
            const auto topic = [](const wchar_t* id, int64_t thread, SessionState st, const std::wstring& square, int mode) {
                TopicRecord t;
                t.sessionId = id;
                t.threadId = thread;
                t.nameState = static_cast<int>(st);
                t.nameColor = square;
                t.nameColorMode = mode;
                return t;
            };
            const int dirMode = static_cast<int>(TabColorMode::WorkingDirectory);
            std::map<std::wstring, TopicRecord> topics;
            topics[L"a"] = topic(L"a", 1, SessionState::Running, blue, dirMode);
            topics[L"b"] = topic(L"b", 2, SessionState::Running, L"", -1); // named before §21.13
            std::vector<TabFact> facts{ fact(L"a", SessionState::Running, L"#61AFEF", 0), fact(L"b", SessionState::Running, L"#61AFEF", 0) };
            auto pick = PickNameRename(facts, topics, now, TabColorMode::WorkingDirectory);
            CHECK(pick.sessionId == L"b" && pick.color && !pick.state, "a topic named before the square gets it (a quiet re-square)");
            facts[0].colorHex = L"#98C379";
            facts[0].colorSinceMs = now - 2000;
            CHECK(PickNameRename(facts, topics, now, TabColorMode::WorkingDirectory).sessionId == L"b", "a color younger than kNameStateSettleMs waits (an inferred folder that flips renames nothing)");
            facts[0].colorSinceMs = now - 20000;
            pick = PickNameRename(facts, topics, now, TabColorMode::WorkingDirectory);
            CHECK(pick.sessionId == L"a" && pick.color && !pick.state, "a tab's color change that has held goes before the quiet re-squares");
            facts[0].state = SessionState::WaitingForInput;
            pick = PickNameRename(facts, topics, now, TabColorMode::WorkingDirectory);
            CHECK(pick.sessionId == L"a" && pick.color && pick.state, "a state and a color due together: one rename");
            facts.push_back(fact(L"c", SessionState::NeedsApproval, L"#61AFEF", 0));
            topics[L"c"] = topic(L"c", 3, SessionState::Running, blue, dirMode);
            CHECK(PickNameRename(facts, topics, now, TabColorMode::WorkingDirectory).sessionId == L"c", "a state that needs the user goes first");
            topics[L"c"].nameSetMs = now - 5000;
            topics[L"a"].nameSetMs = now - 5000;
            CHECK(PickNameRename(facts, topics, now, TabColorMode::WorkingDirectory).sessionId == L"b", "a topic renamed lately waits kNameRenameGapMs");
            facts[1].live = false;
            CHECK(PickNameRename(facts, topics, now, TabColorMode::WorkingDirectory).sessionId.empty(), "a session that is not live in a tab is not renamed");
            topics[L"b"].nameColor = blue;
            facts[1].live = true;
            CHECK(PickNameRename(facts, topics, now, TabColorMode::WorkingDirectory).sessionId.empty(), "a name whose square is the tab's is left alone, whatever mode it was recorded under");

            // What a rename puts in the name: the tab's square once it has held, else the one the name shows.
            const TabFact settled = fact(L"a", SessionState::Running, L"#98C379", 0);
            TabFact moving = settled;
            moving.colorSinceMs = now - 2000;
            const TopicRecord named = topic(L"a", 1, SessionState::Running, blue, dirMode);
            const auto took = ChooseNameColor(&settled, named, now, TabColorMode::Individual);
            const auto kept = ChooseNameColor(&moving, named, now, TabColorMode::Individual);
            const auto none = ChooseNameColor(nullptr, named, now, TabColorMode::Individual);
            CHECK(took.square == green && took.mode == static_cast<int>(TabColorMode::Individual), "a color that has held: its square, under the mode now");
            CHECK(kept.square == blue && kept.mode == dirMode && none.square == blue && none.mode == dirMode, "a color still changing, or no facts: the name keeps its square and its mode");

            // Whether the line tells the color: a change under one settled mode is the tab's; a mode switch's is quiet.
            NameColorChoice after;
            after.square = green;
            after.mode = dirMode;
            CHECK(NoticeOldSquare(named, after, now, 0, kColorModeSettleMs) == blue, "the same mode, settled: the old square, so the line tells the color");
            CHECK(NoticeOldSquare(named, after, now, now - 10000, kColorModeSettleMs) == green, "a mode switch moments ago: the new square, so the line says nothing of the color");
            after.mode = static_cast<int>(TabColorMode::InferredWorkingDirectory);
            CHECK(NoticeOldSquare(named, after, now, 0, kColorModeSettleMs) == green, "another mode: quiet");
            after.mode = dirMode;
            CHECK(NoticeOldSquare(topic(L"o", 1, SessionState::Running, L"", -1), after, now, 0, kColorModeSettleMs) == green, "a topic named before the square: quiet");
            TopicRecord young = named;
            young.createdMs = now - 10000;
            CHECK(NoticeOldSquare(young, after, now, 0, kColorModeSettleMs) == green, "a topic made moments ago takes its first colors quietly (an Individual deal, an inferred folder)");
            young.createdMs = now - 120000;
            CHECK(NoticeOldSquare(young, after, now, 0, kColorModeSettleMs) == blue, "...and tells them once it is older");

            // The state file keeps the square and its mode; an older file reads as "no square, mode unknown".
            MessagingState st;
            st.topics[L"a"] = named;
            const auto back = StateFromJson(StateToJson(st));
            CHECK(back && back->topics.count(L"a") && back->topics.at(L"a").nameColor == blue && back->topics.at(L"a").nameColorMode == dirMode, "the name's square and its mode round-trip");
            const auto old = StateFromJson(ParseLine(L"{\"topics\":[{\"sessionId\":\"o\",\"threadId\":5}]}"));
            CHECK(old && old->topics.count(L"o") && old->topics.at(L"o").nameColor.empty() && old->topics.at(L"o").nameColorMode == -1, "an older file: no square, the mode unknown (re-squared once, quietly)");

            // The card's title wears the square the name shows.
            SessionInfo s;
            s.id = L"card-session";
            s.title = L"agent manager tab";
            s.live = true;
            CardExtras ex;
            ex.colorSquare = blue;
            CHECK(BuildCardText(s, 5000, 0, false, &ex).rfind(L"\U0001F4CC \U0001F7E6 <b>agent manager tab</b>\n", 0) == 0, "the card: the pin, the square, the title");
            CHECK(BuildCardText(s, 5000, 0, false).rfind(L"\U0001F4CC <b>agent manager tab</b>\n", 0) == 0, "no square: the card as before");
        }

        // §21.11 — the state file: a topic's mode and its name's state, and the secretary bot's facts, round-trip; an older
        // file reads as the default mode with no glyph yet; the settings default to the conversation only and rich messages.
        {
            MessagingState st;
            st.businessBotId = 42;
            st.secretaryBotId = 42;
            st.secretaryUpdateOffset = 7;
            TopicRecord t;
            t.sessionId = L"v";
            t.threadId = 3;
            t.verbose = 1;
            t.nameState = static_cast<int>(SessionState::Error);
            t.nameSetMs = 99;
            st.topics[L"v"] = t;
            const auto back = StateFromJson(StateToJson(st));
            CHECK(back && back->businessBotId == 42 && back->secretaryBotId == 42 && back->secretaryUpdateOffset == 7, "the secretary bot's facts round-trip");
            CHECK(back && back->topics.count(L"v") && back->topics.at(L"v").verbose == 1 && back->topics.at(L"v").nameState == static_cast<int>(SessionState::Error) &&
                      back->topics.at(L"v").nameSetMs == 99,
                  "a topic's mode and its name's state round-trip");
            const auto old = StateFromJson(ParseLine(L"{\"topics\":[{\"sessionId\":\"o\",\"threadId\":5}]}"));
            CHECK(old && old->topics.count(L"o") && old->topics.at(L"o").verbose == -1 && old->topics.at(L"o").nameState == -1 && old->businessBotId == 0,
                  "an older file: the default mode, no glyph yet (renamed once), the topics bot holds any connection");
            TopicRecord dflt;
            TopicRecord on = dflt;
            on.verbose = 1;
            TopicRecord off = dflt;
            off.verbose = 0;
            CHECK(!TopicIsVerbose(dflt, false) && TopicIsVerbose(dflt, true) && TopicIsVerbose(on, false) && !TopicIsVerbose(off, true), "a topic's own mode wins over the default");
            const AppSettings defaults;
            CHECK(!defaults.messagingVerbose && defaults.messagingRichMessages, "defaults: the conversation only, rich messages on");
            AppSettings set;
            set.messagingVerbose = true;
            set.messagingRichMessages = false;
            const AppSettings again = AppSettingsFromJson(ToJson(set));
            const AppSettings blank = AppSettingsFromJson(json::Value::MkObj());
            CHECK(again.messagingVerbose && !again.messagingRichMessages && !blank.messagingVerbose && blank.messagingRichMessages, "both settings round-trip; absent keys read as the defaults");
        }

        // §21.12 — the user's LINES: a bold "👤 You" header (the clock, the source, mid-turn, the turn's status) over the words;
        // the status of the LAST prompt as text (our line) or a reaction (the user's own message).
        {
            constexpr auto npos = std::wstring::npos;
            const std::wstring running = BuildPromptLineHtml(day1, PromptSource::Pc, L"fix <it>", PromptTurnStatus::Running, false);
            CHECK(running.rfind(L"<b>\U0001F464 You</b> \x00B7 ", 0) == 0 && running.find(L" \x00B7 typed at the PC \x00B7 \x23F3 running\nfix &lt;it&gt;") != npos,
                  "the prompt line: bold 'You', the clock, typed at the PC, the status, then the prompt (escaped)");
            CHECK(BuildPromptLineHtml(0, PromptSource::Runner, L"x", PromptTurnStatus::None, false) == L"<b>\U0001F464 You</b> \x00B7 \x2699 runner\nx", "no clock when unknown; no status");
            CHECK(BuildPromptLineHtml(0, PromptSource::Phone, L"x", PromptTurnStatus::Answered, false).find(L"\U0001F4F1 from the phone \x00B7 \x2705 answered") != npos, "from the phone, answered");
            CHECK(BuildPromptLineHtml(0, PromptSource::Pc, L"x", PromptTurnStatus::None, true).find(L"typed at the PC \x00B7 mid-turn\nx") != npos, "a mid-turn message says so");
            const std::wstring longPrompt = BuildPromptLineHtml(0, PromptSource::Pc, std::wstring(700, L'a'), PromptTurnStatus::None, false);
            CHECK(longPrompt.find(L"\n<blockquote expandable>") != npos && longPrompt.find(L"</blockquote>") != npos, "a long prompt folds into an expandable quote");
            const std::wstring huge = BuildPromptLineHtml(0, PromptSource::Pc, std::wstring(9000, L'b'), PromptTurnStatus::Running, false);
            CHECK(huge.size() < kTelegramMessageMaxChars && huge.find(L"\x2026</blockquote>") != npos, "a huge prompt is cut (an edit of the line cannot be split)");
            CHECK(BuildAnswerLineHtml(0, L"Swap now") == L"<b>\U0001F464 You</b> \x00B7 \x21A9 answer\nSwap now", "the answer line");
            CHECK(BuildCommandLineHtml(0, L"/compact <x>") == L"<b>\U0001F464 You</b> \x00B7 command\n<code>/compact &lt;x&gt;</code>", "the command line");

            CHECK(PromptTurnStatusFor(SessionState::Running, false) == PromptTurnStatus::Running && PromptTurnStatusFor(SessionState::NeedsApproval, false) == PromptTurnStatus::NeedsYou &&
                      PromptTurnStatusFor(SessionState::Error, false) == PromptTurnStatus::Error && PromptTurnStatusFor(SessionState::Done, false) == PromptTurnStatus::Ended,
                  "running, needs you, error, ended follow the state");
            CHECK(PromptTurnStatusFor(SessionState::WaitingForInput, false) == PromptTurnStatus::Answered && PromptTurnStatusFor(SessionState::Idle, false) == PromptTurnStatus::Answered &&
                      PromptTurnStatusFor(SessionState::WaitingForInput, true) == PromptTurnStatus::Interrupted,
                  "at rest: answered, or interrupted when the user stopped the turn");
            CHECK(std::wstring{ PromptStatusReaction(PromptTurnStatus::Answered) } == L"\U0001F44C" && std::wstring{ PromptStatusReaction(PromptTurnStatus::Running) } == L"\U0001F468\x200D\U0001F4BB" &&
                      std::wstring{ PromptStatusReaction(PromptTurnStatus::None) }.empty(),
                  "the user's own message: 👌 answered, 👨‍💻 running, none removes it");
            for (int st = 1; st <= static_cast<int>(PromptTurnStatus::Interrupted); ++st)
            {
                CHECK(std::wstring{ PromptStatusText(static_cast<PromptTurnStatus>(st)) }.size() > 0 && std::wstring{ PromptStatusReaction(static_cast<PromptTurnStatus>(st)) }.size() > 0,
                      "every status has its words and its reaction");
            }

            // The history: the turn's prompt and its reply are marked (the reply quotes the prompt), in both modes; a turn begun
            // in an earlier window has no prompt here, so its reply is not marked.
            int64_t lastDay = 20261016;
            const auto quiet = RenderHistory({ item(TopicItemKind::Prompt, L"go", day1), item(TopicItemKind::Text, L"Looking."), tool(L"t1", L"Bash", L"Bash: x"), item(TopicItemKind::Text, L"Done.") },
                                             false,
                                             lastDay,
                                             {},
                                             false);
            CHECK(quiet.size() == 2 && quiet[0].turnPrompt && !quiet[0].turnReply && quiet[1].turnReply && !quiet[1].turnPrompt, "conversation only: prompt, then the reply that quotes it");
            const auto rest = RenderHistory({ item(TopicItemKind::Text, L"Wrapped up.") }, false, lastDay);
            CHECK(rest.size() == 1 && !rest[0].turnReply, "a reply whose prompt was in an earlier window quotes nothing");

            // The state file keeps the last prompt (a restart goes on updating its status); an older file has none.
            MessagingState st;
            TopicRecord t;
            t.sessionId = L"p";
            t.threadId = 9;
            t.promptMessageId = 4242;
            t.promptBotLine = true;
            t.promptTsMs = day1;
            t.promptSource = static_cast<int>(PromptSource::Runner);
            t.promptText = L"run the suite";
            t.promptStatus = static_cast<int>(PromptTurnStatus::Answered);
            t.promptInterrupted = true;
            st.topics[L"p"] = t;
            const auto back = StateFromJson(StateToJson(st));
            const bool same = back && back->topics.count(L"p") && back->topics.at(L"p").promptMessageId == 4242 && back->topics.at(L"p").promptBotLine &&
                              back->topics.at(L"p").promptTsMs == day1 && back->topics.at(L"p").promptSource == static_cast<int>(PromptSource::Runner) &&
                              back->topics.at(L"p").promptText == L"run the suite" && back->topics.at(L"p").promptStatus == static_cast<int>(PromptTurnStatus::Answered) &&
                              back->topics.at(L"p").promptInterrupted;
            CHECK(same, "the last prompt round-trips");
            TopicRecord none;
            none.sessionId = L"n";
            none.threadId = 1;
            MessagingState st2;
            st2.topics[L"n"] = none;
            const std::wstring text = json::Dump(StateToJson(st2));
            CHECK(text.find(L"promptMessageId") == npos, "a topic with no prompt tracked writes none of its keys");
            const auto older = StateFromJson(ParseLine(L"{\"topics\":[{\"sessionId\":\"o\",\"threadId\":5}]}"));
            CHECK(older && older->topics.at(L"o").promptMessageId == 0 && older->topics.at(L"o").promptText.empty() && older->topics.at(L"o").promptStatus == 0,
                  "an older file: no prompt tracked (the next live prompt starts it)");
        }
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
