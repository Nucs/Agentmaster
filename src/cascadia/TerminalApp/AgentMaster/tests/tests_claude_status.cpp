// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.40: /status in Telegram shows what Claude Code's own /status dialog shows (the user's request
// of 2026-10-26: "When user send in telegram /status, we need to get all the info we can see in claude code status and other
// tabs like usage to be sent to the user."). This file pins:
//   1. the readers of Claude Code's headless output (ClaudeStatus.h): the /usage version gate, `claude auth status --text`'s lines,
//      the `-p /usage --output-format json` answer (a result, a verbose array, an error, a MODEL turn), the /usage limit lines and
//      the contributions heading, the token and count wording, the MCP tool names — on the texts Claude Code 2.1.310 printed on
//      this PC (2026-10-26);
//   2. the transcript fold (ClaudeTranscriptStatusReader) on 2.1.310's own line shapes: tokens once per response, a synthetic
//      line, the MCP tools fold and its reset at a compact boundary, the failed / pending / needs-auth lists, the instruction
//      files' full list and its update, the newest model / effort / mode / titles / folders; and the raw-byte filter;
//   3. the report's layout (MessagingStatus.h): every section, the fallbacks, the escaping, the bars, the split under Telegram's
//      limit, the fleet report;
//   4. the OS half on this PC (ClaudeStatus.cpp): the transcript FILE read (CRLF, a BOM, a line across a 1 MiB chunk, a torn last
//      line) equals the in-memory fold; the hidden child runner with cmd.exe (output, standard error, exit code, working folder,
//      the scrubbed environment, the timeout, a missing executable); the probe's cache and report slots with cmd.exe as a stand-in
//      (claude is NEVER started here: the live check is the dev bot, MESSAGING.md §21.40 "Check after the build").

#include "m5_tests.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../ClaudeStatus.h"
#include "../MessagingStatus.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    // What `claude auth status --text` printed on this PC (Claude Code 2.1.310, 2026-10-26).
    constexpr wchar_t kAuthText[] = L"Login method: Claude Max account\r\nOrganization: elibelash@gmail.com's Organization\r\nEmail: elibelash@gmail.com\r\nAnthropic base URL: http://127.0.0.1:18764\r\n";

    // What `claude -p /usage` printed on this PC (the result text of its JSON answer, 2026-10-26).
    constexpr wchar_t kUsageText[] =
        L"You are currently using your subscription to power your Claude Code usage\n"
        L"\n"
        L"Current session: 7% used \x00B7 resets Oct 26, 10:20pm (Asia/Jerusalem)\n"
        L"Current week (all models): 98% used \x00B7 resets Oct 27, 8pm (Asia/Jerusalem)\n"
        L"Current week (Fable): 0% used \x00B7 resets Oct 27, 8pm (Asia/Jerusalem)\n"
        L"\n"
        L"What's contributing to your limits usage?\n"
        L"Approximate \x00B7 sessions on this machine only \x00B7 shares overlap\n"
        L"\n"
        L"Last 24h \x00B7 4415 requests \x00B7 12 sessions\n"
        L"  100%  Save max effort for hard tasks\n"
        L"  99%  Start fresh when you switch tasks \x00B7 run /clear\n"
        L"  Top MCP servers: catalog 2%, claude-fleet 1%\n";

    // A transcript in Claude Code 2.1.310's own line shapes (each line as the CLI writes it, its bodies cut short).
    std::vector<std::wstring> SampleTranscript()
    {
        return {
            LR"J({"parentUuid":null,"isSidechain":false,"attachment":{"type":"model","identity":{"modelId":"claude-opus-5-5[1m]","marketingName":"Opus 5.5 (1M context)","knowledgeCutoff":"June 2026"},"text":"You are powered by the model named Opus 5.5 (1M context)."},"type":"attachment","uuid":"u1","timestamp":"2026-10-26T10:00:00.000Z","version":"2.1.310","sessionId":"s1"})J",
            LR"J({"parentUuid":"u1","isSidechain":false,"attachment":{"type":"instructions","files":[{"path":"C:\\Users\\ELI\\.claude\\CLAUDE.md","type":"User","content":"x"},{"path":"K:\\source\\Agentmaster\\CLAUDE.md","type":"Project","content":"y"},{"path":"C:\\Users\\ELI\\.claude\\projects\\K--source-Agentmaster\\memory\\MEMORY.md","type":"AutoMem","content":"z"}]},"type":"attachment","uuid":"u2","version":"2.1.310"})J",
            LR"J({"parentUuid":"u2","isSidechain":false,"attachment":{"type":"deferred_tools_delta","addedNames":["CronCreate","mcp__catalog__catalog_read","mcp__catalog__catalog_edit","mcp__claude-fleet__list_desktops","mcp__claude_ai_Gmail__search_threads"],"addedLines":[],"removedNames":[],"wireHiddenNames":[],"readdedNames":[],"pendingMcpServers":[],"failedMcpServers":[]},"type":"attachment","uuid":"u3"})J",
            LR"J({"parentUuid":"u3","isSidechain":false,"attachment":{"type":"environment","snapshot":{"workingDirectory":"K:\\source\\Agentmaster","isWorktree":false,"isGitRepo":true,"additionalWorkingDirectories":["Q:\\tmp\\x"],"platform":"win32"}},"type":"attachment","uuid":"u4"})J",
            // A user line whose tool result QUOTES the pair (escaped): the filter must pass it unparsed.
            LR"J({"parentUuid":"u4","isSidechain":false,"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":"grep found \"type\":\"assistant\" in the file"}]},"uuid":"u5","version":"2.1.310"})J",
            // One response in two lines (one per content block), the SAME id and usage: counted once.
            LR"J({"parentUuid":"u5","isSidechain":false,"message":{"model":"claude-opus-5-5","id":"msg_A","type":"message","role":"assistant","content":[{"type":"thinking","thinking":"..."}],"usage":{"input_tokens":2,"cache_creation_input_tokens":91240,"cache_read_input_tokens":13745,"output_tokens":1678}},"requestedModel":"claude-opus-5-5[1m]","type":"assistant","uuid":"u6","effort":"max","version":"2.1.310"})J",
            LR"J({"parentUuid":"u6","isSidechain":false,"message":{"model":"claude-opus-5-5","id":"msg_A","type":"message","role":"assistant","content":[{"type":"tool_use","id":"t2","name":"Bash","input":{}}],"usage":{"input_tokens":2,"cache_creation_input_tokens":91240,"cache_read_input_tokens":13745,"output_tokens":1678}},"requestedModel":"claude-opus-5-5[1m]","type":"assistant","uuid":"u7","effort":"max","version":"2.1.310"})J",
            LR"J({"parentUuid":"u7","isSidechain":false,"message":{"model":"claude-opus-5-5","id":"msg_B","type":"message","role":"assistant","content":[{"type":"text","text":"done"}],"usage":{"input_tokens":3,"cache_creation_input_tokens":2524,"cache_read_input_tokens":104985,"output_tokens":69}},"requestedModel":"claude-opus-5-5[1m]","type":"assistant","uuid":"u8","effort":"max","version":"2.1.310"})J",
            // Claude Code's own placeholder for an API error: no response, no model, no context.
            LR"J({"parentUuid":"u8","isSidechain":false,"message":{"model":"<synthetic>","id":"syn-1","type":"message","role":"assistant","content":[{"type":"text","text":"API Error"}],"usage":{"input_tokens":0,"output_tokens":0}},"type":"assistant","uuid":"u9","isApiErrorMessage":true,"version":"2.1.310"})J",
            LR"J({"type":"permission-mode","permissionMode":"bypassPermissions","sessionId":"s1"})J",
            LR"J({"type":"custom-title","customTitle":"Telegram status","sessionId":"s1"})J",
            LR"J({"type":"ai-title","aiTitle":"Generated title","sessionId":"s1"})J",
            // A resume where claude-fleet failed: its tools removed, the failure listed.
            LR"J({"parentUuid":"u9","isSidechain":false,"attachment":{"type":"deferred_tools_delta","addedNames":[],"addedLines":[],"removedNames":["mcp__claude-fleet__list_desktops"],"wireHiddenNames":[],"readdedNames":[],"pendingMcpServers":[],"failedMcpServers":[{"name":"claude-fleet","errorCode":"CONNECT_TIMEOUT","error":"Version negotiation probe timed out after 5000ms"}]},"type":"attachment","uuid":"u10"})J",
            // An update of the instruction files: only the changed ones (one replaced, one new).
            LR"J({"parentUuid":"u10","isSidechain":false,"attachment":{"type":"instructions","files":[{"path":"C:\\Users\\ELI\\.claude\\projects\\K--source-Agentmaster\\memory\\MEMORY.md","type":"AutoMem","content":"new"},{"path":"K:\\source\\Agentmaster\\docs\\NEW.md","type":"Project","content":"n"}],"changed":["MEMORY.md"],"reason":"edit"},"type":"attachment","uuid":"u11"})J",
            // A compaction: every tool is announced again from nothing after it.
            LR"J({"parentUuid":null,"logicalParentUuid":"u11","isSidechain":false,"type":"system","subtype":"compact_boundary","content":"Conversation compacted","compactMetadata":{"trigger":"auto","preTokens":180000},"uuid":"u12"})J",
            LR"J({"parentUuid":"u12","isSidechain":false,"attachment":{"type":"deferred_tools_delta","addedNames":["mcp__catalog__catalog_read","mcp__claude_ai_Gmail__search_threads","mcp__tampermonkey__tampermonkey_list"],"addedLines":[],"removedNames":[],"wireHiddenNames":[],"readdedNames":[]},"type":"attachment","uuid":"u13"})J",
            LR"J({"parentUuid":"u13","isSidechain":false,"attachment":{"type":"deferred_tools_delta","addedNames":[],"removedNames":[],"pendingMcpServers":["slowserver"],"needsAuthMcpServers":["claude.ai Calendar"]},"type":"attachment","uuid":"u14"})J",
            // A /model switch: the response asks for another model; the model attachment above is now stale.
            LR"J({"parentUuid":"u14","isSidechain":false,"message":{"model":"claude-sonnet-5-5","id":"msg_C","type":"message","role":"assistant","content":[{"type":"text","text":"ok"}],"usage":{"input_tokens":5,"cache_creation_input_tokens":1000,"cache_read_input_tokens":50000,"output_tokens":200}},"requestedModel":"claude-sonnet-5-5","type":"assistant","uuid":"u15","effort":"high","version":"2.1.310"})J",
            // The last line, still being written.
            LR"J({"parentUuid":"u15","isSidechain":false,"type":"assistant","message":{"model":"claude-sonnet-5-5")J",
        };
    }

    /// <summary>Folds lines into a reader and returns its facts.</summary>
    ClaudeTranscriptStatus Fold(const std::vector<std::wstring>& lines)
    {
        ClaudeTranscriptStatusReader reader;
        for (const auto& line : lines)
        {
            reader.Feed(line);
        }
        return reader.Result();
    }

    /// <summary>UTF-16 → UTF-8 for the file test.</summary>
    std::string ToUtf8(const std::wstring& s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    /// <summary>Whether `hay` holds `needle`.</summary>
    bool Has(const std::wstring& hay, std::wstring_view needle)
    {
        return hay.find(needle) != std::wstring::npos;
    }

    /// <summary>cmd.exe's full path (the child runner's stand-in for claude.exe).</summary>
    std::wstring CmdExe()
    {
        wchar_t dir[MAX_PATH]{};
        const UINT n = ::GetSystemDirectoryW(dir, MAX_PATH);
        return std::wstring(dir, n) + L"\\cmd.exe";
    }

    /// <summary>A report input for the sample session, fully filled (each test changes what it looks at).</summary>
    ClaudeStatusReportInput SampleReport(const SessionInfo& s, int64_t now)
    {
        ClaudeStatusReportInput in;
        in.session = &s;
        in.titleToken = TgTitleToken(TgTitleForm::Plain, L"", L"Agentmaster", 0);
        in.process.running = true;
        in.process.pid = 50500;
        in.process.version = L"2.1.310";
        in.process.status = L"busy";
        in.process.startedAtMs = now - (2 * 3600 + 13 * 60) * 1000LL;
        in.transcriptRead = true;
        in.transcript = Fold(SampleTranscript());
        in.account.ok = true;
        in.account.lines = ParseClaudeStatusLines(kAuthText);
        in.usage.ok = true;
        in.usage.text = kUsageText;
        in.usage.readAtMs = now;
        in.nowMs = now;
        return in;
    }
}

void TestClaudeStatus()
{
    wprintf(L"== TestClaudeStatus (MESSAGING.md 21.40: /status shows what Claude Code's own /status shows - readers, fold, layout, file, child) ==\n");

    // ---- 1. the readers ----
    {
        CHECK(ClaudeVersionRunsHeadlessUsage(2, 1, 310) && ClaudeVersionRunsHeadlessUsage(2, 1, 311) && ClaudeVersionRunsHeadlessUsage(2, 2, 0) && ClaudeVersionRunsHeadlessUsage(3, 0, 0),
              "the headless /usage runs from 2.1.310 on (the measured version) and on every later one");
        CHECK(!ClaudeVersionRunsHeadlessUsage(2, 1, 309) && !ClaudeVersionRunsHeadlessUsage(2, 0, 999) && !ClaudeVersionRunsHeadlessUsage(1, 9, 999),
              "never below 2.1.310: an older claude could send \"/usage\" to the model");

        const auto lines = ParseClaudeStatusLines(kAuthText);
        CHECK(lines.size() == 4 && lines[0].label == L"Login method" && lines[0].value == L"Claude Max account" && lines[1].label == L"Organization" &&
                  lines[1].value == L"elibelash@gmail.com's Organization" && lines[3].label == L"Anthropic base URL" && lines[3].value == L"http://127.0.0.1:18764",
              "auth status's four lines read as Claude Code printed them (CRLF dropped; a URL's \"://\" never splits the label)");
        const auto odd = ParseClaudeStatusLines(L"\n\nNot logged in. Run claude auth login\n  x: y: z  \nA label far too long to be any label Claude Code prints here: v\nEmail:   a@b.c  \n");
        CHECK(odd.size() == 4 && odd[0].label.empty() && odd[0].value == L"Not logged in. Run claude auth login" && odd[1].label == L"x" && odd[1].value == L"y: z" &&
                  odd[2].label.empty() && odd[3].label == L"Email" && odd[3].value == L"a@b.c",
              "a line that is no pair stays whole, blank lines go, the value keeps a later \": \", parts are trimmed");
        std::wstring many;
        for (int i = 0; i < 40; ++i)
        {
            many += L"Line" + std::to_wstring(i) + L": v\n";
        }
        CHECK(ParseClaudeStatusLines(many).size() == kClaudeStatusMaxLines && ParseClaudeStatusLines(L"").empty(), "at most kClaudeStatusMaxLines lines; none for empty text");

        const std::wstring json = LR"J({"type":"result","subtype":"success","is_error":false,"duration_api_ms":0,"num_turns":0,"result":"Current session: 7% used","session_id":"x","total_cost_usd":0,"local_command":"usage"})J";
        auto a = ParseClaudeUsageAnswer(json);
        CHECK(a.ok && !a.modelTurn && a.text == L"Current session: 7% used" && a.error.empty(), "the -p /usage JSON answer: ok, its result text");
        a = ParseClaudeUsageAnswer(L"[{\"type\":\"system\",\"subtype\":\"init\"}," + json + L"]");
        CHECK(a.ok && a.text == L"Current session: 7% used", "a verbose array: its last result object is read");
        a = ParseClaudeUsageAnswer(L"warning: something first\n" + json + L"\n");
        CHECK(a.ok, "a line printed before the JSON: the last line is read");
        a = ParseClaudeUsageAnswer(LR"J({"type":"result","is_error":true,"num_turns":0,"result":"Usage is not available for API key logins","local_command":"usage"})J");
        CHECK(!a.ok && !a.modelTurn && a.text == L"Usage is not available for API key logins" && !a.error.empty(), "is_error: not ok, Claude Code's own message kept");
        a = ParseClaudeUsageAnswer(LR"J({"type":"result","is_error":false,"num_turns":1,"result":"The /usage command shows ..."})J");
        CHECK(!a.ok && a.modelTurn && a.text.empty(), "a MODEL turn (no local_command, a turn taken): the alarm that latches the probe, no text kept");
        a = ParseClaudeUsageAnswer(LR"J({"type":"result","is_error":false,"num_turns":0,"result":"x","local_command":"cost"})J");
        CHECK(!a.ok && a.modelTurn, "another local command is not /usage either");
        a = ParseClaudeUsageAnswer(L"Microsoft Windows [Version 10.0]\r\n");
        CHECK(!a.ok && !a.modelTurn && a.error == L"its answer was not JSON", "text that is no JSON: not ok, no latch");
        a = ParseClaudeUsageAnswer(LR"J({"type":"result","is_error":false,"num_turns":0,"result":"  \n","local_command":"usage"})J");
        CHECK(!a.ok && a.error == L"it printed nothing", "a blank result: not ok");

        auto limit = ParseClaudeUsageLimitLine(L"Current session: 7% used \x00B7 resets Oct 26, 10:20pm (Asia/Jerusalem)");
        CHECK(limit && limit->title == L"Current session" && limit->percent == 7 && limit->tail == L"resets Oct 26, 10:20pm (Asia/Jerusalem)", "a limit line: title, percent, tail");
        limit = ParseClaudeUsageLimitLine(L"Current week (all models): 98% used \x00B7 resets Oct 27, 8pm (Asia/Jerusalem)");
        CHECK(limit && limit->title == L"Current week (all models)" && limit->percent == 98, "a title holding parentheses");
        limit = ParseClaudeUsageLimitLine(L"Current week (Fable): 105% used");
        CHECK(limit && limit->percent == 105 && limit->tail.empty(), "no tail; an overage kept as printed");
        CHECK(!ParseClaudeUsageLimitLine(L"  100%  Save max effort for hard tasks") && !ParseClaudeUsageLimitLine(L"Last 24h \x00B7 4415 requests \x00B7 12 sessions") &&
                  !ParseClaudeUsageLimitLine(L"Something: 5% used by many") && !ParseClaudeUsageLimitLine(L"Top MCP servers: catalog 2%") && !ParseClaudeUsageLimitLine(L""),
              "a contributions row, a heading, a sentence and an empty line are no limits");
        CHECK(IsClaudeUsageContributionsHeading(L"What's contributing to your limits usage?") && IsClaudeUsageContributionsHeading(L"What\x2019s contributing to your limits usage?") &&
                  !IsClaudeUsageContributionsHeading(L"Current session: 7% used"),
              "the contributions heading, straight or curly apostrophe");

        CHECK(FormatClaudeTokenCount(0) == L"0" && FormatClaudeTokenCount(999) == L"999" && FormatClaudeTokenCount(1000) == L"1.0k" && FormatClaudeTokenCount(1050) == L"1.1k" &&
                  FormatClaudeTokenCount(312400) == L"312.4k" && FormatClaudeTokenCount(999949) == L"999.9k" && FormatClaudeTokenCount(999950) == L"1.0m" &&
                  FormatClaudeTokenCount(1234567) == L"1.2m" && FormatClaudeTokenCount(128400000) == L"128.4m" && FormatClaudeTokenCount(423700000000) == L"423.7b" &&
                  FormatClaudeTokenCount(999999999) == L"1.0b" && FormatClaudeTokenCount(-5) == L"0",
              "token counts read like Claude Code's own (312.4k, 128.4m, 423.7b), a carry rounding into the next unit");
        CHECK(GroupClaudeCount(0) == L"0" && GroupClaudeCount(999) == L"999" && GroupClaudeCount(1000) == L"1,000" && GroupClaudeCount(27341) == L"27,341" &&
                  GroupClaudeCount(1234567) == L"1,234,567" && GroupClaudeCount(-3) == L"0",
              "counts with thousands separators");
        CHECK(McpServerOfToolName(L"mcp__catalog__catalog_read") == L"catalog" && McpServerOfToolName(L"mcp__claude_ai_Gmail__search_threads") == L"claude_ai_Gmail" &&
                  McpServerOfToolName(L"mcp__claude-fleet__list_desktops") == L"claude-fleet" && McpServerOfToolName(L"mcp__x").empty() && McpServerOfToolName(L"Bash").empty() &&
                  McpServerOfToolName(L"mcp____tool").empty(),
              "an MCP tool's server: up to the first \"__\" after the prefix; no server for anything else");
    }

    // ---- 2. the transcript fold ----
    const ClaudeTranscriptStatus t = Fold(SampleTranscript());
    {
        CHECK(t.responses == 3 && t.inputTokens == 10 && t.outputTokens == 1947 && t.cacheReadTokens == 168730 && t.cacheWriteTokens == 94764,
              "tokens: each response once by its id (msg_A's two lines once), the synthetic placeholder not at all");
        CHECK(t.contextTokens == 51205, "the context is the newest response's size (input + cache read + cache write + output)");
        CHECK(t.modelId == L"claude-opus-5-5[1m]" && t.modelName == L"Opus 5.5 (1M context)" && t.requestedModel == L"claude-sonnet-5-5" && t.responseModel == L"claude-sonnet-5-5" &&
                  t.effort == L"high",
              "the model attachment kept; the newest response's requested model, API model and effort (a /model switch)");
        CHECK(t.permissionMode == L"bypassPermissions" && t.customTitle == L"Telegram status" && t.aiTitle == L"Generated title" && t.version == L"2.1.310",
              "the permission mode, both titles and the version that wrote the newest response");
        CHECK(t.instructionFiles.size() == 4 && t.instructionFiles[0].type == L"User" && t.instructionFiles[1].path == L"K:\\source\\Agentmaster\\CLAUDE.md" &&
                  t.instructionFiles[2].type == L"AutoMem" && t.instructionFiles[3].path == L"K:\\source\\Agentmaster\\docs\\NEW.md",
              "instruction files: the full list, then an update replaces its file in place and adds the new one");
        // (Named lists: a brace list's commas inside CHECK( ) would split the macro's arguments.)
        const std::vector<std::wstring> wantServers{ L"catalog", L"claude_ai_Gmail", L"tampermonkey" };
        const std::vector<std::wstring> wantPending{ L"slowserver" };
        const std::vector<std::wstring> wantNeedsAuth{ L"claude.ai Calendar" };
        const std::vector<std::wstring> wantDirs{ L"Q:\\tmp\\x" };
        CHECK(t.mcpSeen && t.mcpServers == wantServers, "MCP servers: the fold restarted at the compact boundary (CronCreate is no MCP tool; claude-fleet never came back)");
        CHECK(t.mcpFailed.size() == 1 && t.mcpFailed[0].name == L"claude-fleet" && t.mcpFailed[0].error == L"Version negotiation probe timed out after 5000ms",
              "a failure stays until a delta carries the list again (Claude Code's own rule)");
        CHECK(t.mcpPending == wantPending && t.mcpNeedsAuth == wantNeedsAuth, "pending and needs-auth: the newest delta that carries each");
        CHECK(t.extraDirectories == wantDirs && !t.isWorktree && t.compactions == 1, "the extra folders, the worktree flag, one compaction");
        CHECK(t.lines == 19 && t.unparsedLines == 1 && t.parsedLines == 17, "19 lines: the quoting user line passed unparsed, the torn last line counted as unparsed");

        // The filter on raw UTF-8 bytes: an ASCII needle is found next to multi-byte text, never inside it.
        const std::string hebrew = ToUtf8(L"{\"message\":{\"content\":\"\x05E9\x05DC\x05D5\x05DD\"},\"type\":\"assistant\"}");
        CHECK(ClaudeTranscriptStatusLineWanted(std::string_view{ hebrew }) && !ClaudeTranscriptStatusLineWanted(std::string_view{ ToUtf8(L"{\"type\":\"user\",\"c\":\"\x05E9\x05DC\x05D5\x05DD\"}") }),
              "the byte filter finds a response next to Hebrew and passes a user line");
        CHECK(!ClaudeTranscriptStatusLineWanted(std::wstring_view{ SampleTranscript()[4] }) && ClaudeTranscriptStatusLineWanted(std::wstring_view{ SampleTranscript()[9] }),
              "an ESCAPED pair inside a string is no match; a permission-mode line is");
        ClaudeTranscriptStatusReader nested;
        nested.Feed(LR"J({"type":"user","toolUseResult":{"type":"assistant","message":{"id":"x","model":"m","usage":{"input_tokens":5}}}})J");
        const auto n = nested.Result();
        CHECK(n.parsedLines == 1 && n.responses == 0 && n.responseModel.empty(), "a nested raw pair costs a parse, and the line's own type decides");
    }

    // ---- 3. the report's layout ----
    {
        const int64_t now = NowMsTest();
        SessionInfo s = MakeSession(L"f15a6e0a-b72d-4c4e-9e64-6072ddffd9f0", SessionState::Running);
        s.title = L"Agentmaster";
        s.liveCwd = L"K:\\source\\Agentmaster";
        s.started = true;
        s.contextTokens = 312400;
        s.pid = 50500;
        s.effort = L"low"; // the observer's launch value: the transcript's newer effort wins
        s.currentModel = L"claude-opus-5-5";
        ClaudeStatusReportInput in = SampleReport(s, now);
        auto messages = BuildClaudeStatusReport(in);
        CHECK(messages.size() == 1 && messages[0].size() <= kClaudeStatusMessageMaxChars, "a usual report is one message within the limit");
        const std::wstring& r = messages[0];
        CHECK(Has(r, L"\U0001F4CA <b>Claude Code status</b> \x00B7 ") && Has(r, in.titleToken), "the header names the session by its title token");
        CHECK(Has(r, L"<b>Status</b>\nVersion: 2.1.310\nSession name: Telegram status\nSession ID: <code>f15a6e0a-b72d-4c4e-9e64-6072ddffd9f0</code>\ncwd: <code>K:\\source\\Agentmaster</code>\n"),
              "Status: the running version, Claude Code's own session name (the /rename title wins), the id and the cwd, in the dialog's order");
        CHECK(Has(r, L"Model: <code>claude-sonnet-5-5</code>\nEffort: high\nPermission mode: bypassPermissions\nContext: 312.4k tokens\n"),
              "after a /model switch the requested id alone (the attachment's name is stale); the transcript's effort over the launch value; the context");
        CHECK(Has(r, L"MCP servers: 3 connected, 1 need auth, 1 pending, 1 failed\n  catalog, claude_ai_Gmail, tampermonkey\n") &&
                  Has(r, L"claude.ai Calendar: needs authorization") && Has(r, L"slowserver: still connecting") &&
                  Has(r, L"claude-fleet: Version negotiation probe timed out after 5000ms"),
              "MCP servers worded like the dialog, the names, then each problem with Claude Code's reason");
        CHECK(Has(r, L"Memory: 4 files\n  <code>C:\\Users\\ELI\\.claude\\CLAUDE.md</code> (User)\n") && Has(r, L"Additional directories: <code>Q:\\tmp\\x</code>\n"),
              "the instruction files with their kind; the extra folders");
        CHECK(Has(r, L"Claude: busy \x00B7 pid 50500 \x00B7 running 2h13m\n"), "the heartbeat's word, the pid, how long it runs");
        CHECK(Has(r, L"<b>Account</b>\nLogin method: Claude Max account\nOrganization: elibelash@gmail.com's Organization\nEmail: elibelash@gmail.com\nAnthropic base URL: http://127.0.0.1:18764\n"),
              "Account: auth status's lines as Claude Code printed them");
        CHECK(Has(r, L"<b>Plan usage</b>\nYou are currently using your subscription to power your Claude Code usage\n\nCurrent session: \x25B0\x25B1\x25B1\x25B1\x25B1\x25B1\x25B1\x25B1\x25B1\x25B1 7% used \x00B7 resets Oct 26, 10:20pm (Asia/Jerusalem)\n") &&
                  Has(r, L"Current week (all models): \x25B0\x25B0\x25B0\x25B0\x25B0\x25B0\x25B0\x25B0\x25B0\x25B0 98% used") &&
                  Has(r, L"Current week (Fable): \x25B1\x25B1\x25B1\x25B1\x25B1\x25B1\x25B1\x25B1\x25B1\x25B1 0% used"),
              "Plan usage: /usage's own lines, a bar beside each limit (7% shows one cell, 98% all ten, 0% none)");
        CHECK(Has(r, L"<blockquote expandable>What's contributing to your limits usage?\nApproximate") && Has(r, L"  Top MCP servers: catalog 2%, claude-fleet 1%</blockquote>"),
              "the contributions section folded into an expandable quote, its rows' indentation kept");
        CHECK(Has(r, L"<b>This conversation</b> <i>(its transcript)</i>\nUsage: 10 input, 1.9k output, 168.7k cache read, 94.8k cache write\nResponses: 3 \x00B7 1 compaction\n"),
              "This conversation: the transcript's tokens worded like the Usage tab's row, the responses and compactions");
        CHECK(Has(r, L"<i>Read ") && r.find(L"<b>Status</b>") < r.find(L"<b>Account</b>") && r.find(L"<b>Account</b>") < r.find(L"<b>Plan usage</b>") &&
                  r.find(L"<b>Plan usage</b>") < r.find(L"<blockquote expandable>") && r.find(L"<blockquote expandable>") < r.find(L"<b>This conversation</b>"),
              "the sections in order, the footer last");

        // The model attachment's name shows when the response asks for the same model.
        in.transcript.requestedModel = L"claude-opus-5-5[1m]";
        CHECK(Has(BuildClaudeStatusReport(in)[0], L"Model: Opus 5.5 (1M context) (<code>claude-opus-5-5[1m]</code>)\n"), "the model's name with its id when they agree");
        in.transcript.requestedModel = L"claude-sonnet-5-5";

        // Escaping, a cached usage, a parked tab, an exited claude, no transcript.
        in.transcript.customTitle = L"a <b>bold</b> & co";
        in.usage.cached = true;
        in.usage.readAtMs = now - 40000;
        std::wstring e = BuildClaudeStatusReport(in)[0];
        CHECK(Has(e, L"Session name: a &lt;b&gt;bold&lt;/b&gt; &amp; co\n") && Has(e, L"<b>Plan usage</b> <i>(read 40s ago)</i>"), "a name is escaped; a reused usage says its age");
        in.process = ClaudeStatusProcess{};
        s.started = false;
        e = BuildClaudeStatusReport(in)[0];
        CHECK(Has(e, L"Claude: not running \x2014 the tab is parked") && Has(e, L"Version: 2.1.310 (its last run)"), "a parked tab: no process, the transcript's version marked as its last run");
        s.started = true;
        s.state = SessionState::Done;
        CHECK(Has(BuildClaudeStatusReport(in)[0], L"Claude: exited \x2014 /reactivate"), "an exited claude");
        s.state = SessionState::Running;
        in.transcriptRead = false;
        in.transcriptNote = L"No transcript yet: the session has no message, so the rows its transcript gives are not shown.";
        e = BuildClaudeStatusReport(in)[0];
        // ("\nMCP servers: " at a line's start: the usage's contributions quote has a "Top MCP servers:" row of its own.)
        CHECK(Has(e, L"Model: <code>claude-opus-5-5</code>\nEffort: low\n") && Has(e, L"<i>No transcript yet") && !Has(e, L"Memory:") && !Has(e, L"\nMCP servers: ") &&
                  !Has(e, L"This conversation"),
              "no transcript: the registry's model and effort, the note, and no row the transcript gives");
        in = SampleReport(s, now);

        // Failures say why; Claude Code's own message is quoted.
        in.account = ClaudeAccountRead{};
        in.account.error = L"Claude Code was not found on this PC";
        in.usage = ClaudeUsageRead{};
        in.usage.error = L"Claude Code reported an error";
        in.usage.text = L"Usage is not available for API key logins";
        e = BuildClaudeStatusReport(in)[0];
        CHECK(Has(e, L"<b>Account</b>\n\x26A0\xFE0F Claude Code was not found on this PC\n") &&
                  Has(e, L"<b>Plan usage</b>\n\x26A0\xFE0F Claude Code reported an error\n<blockquote>Usage is not available for API key logins</blockquote>"),
              "a failed read says why; an error's own message is quoted");

        // The fleet report: the account and the usage alone.
        ClaudeStatusReportInput fleet;
        fleet.account.ok = true;
        fleet.account.lines = ParseClaudeStatusLines(kAuthText);
        fleet.usage.ok = true;
        fleet.usage.text = kUsageText;
        fleet.nowMs = now;
        const auto f = BuildClaudeStatusReport(fleet);
        CHECK(f.size() == 1 && Has(f[0], L"\U0001F4CA <b>Claude Code</b> \x00B7 account and plan usage\n") && !Has(f[0], L"<b>Status</b>") && Has(f[0], L"<b>Account</b>") &&
                  Has(f[0], L"<b>Plan usage</b>") && !Has(f[0], L"This conversation") && !Has(f[0], L"the session from its transcript"),
              "the fleet report: the account and the plan usage, no session section");

        // Too long for one message: split between whole sections, the quote whole, each part within the limit.
        in = SampleReport(s, now);
        std::wstring longUsage = kUsageText;
        for (int i = 0; i < 80; ++i)
        {
            longUsage += L"  " + std::to_wstring(i) + L"%  A contributions row long enough to fill the quote of the report\n";
        }
        in.usage.text = longUsage;
        for (int i = 0; i < 30; ++i)
        {
            in.transcript.mcpServers.push_back(L"server-with-a-long-name-" + std::to_wstring(i));
        }
        messages = BuildClaudeStatusReport(in);
        bool fits = true;
        for (const auto& m : messages)
        {
            fits = fits && m.size() <= kClaudeStatusMessageMaxChars;
        }
        CHECK(messages.size() == 2 && fits, "a report too long for one message goes as two, each within the limit");
        CHECK(messages.size() == 2 && Has(messages[0], L"<b>Plan usage</b>") && !Has(messages[0], L"<blockquote") && messages[1].rfind(L"<blockquote expandable>", 0) == 0 &&
                  Has(messages[1], L"</blockquote>") && Has(messages[1], L"<b>This conversation</b>") && Has(messages[1], L"<i>Read "),
              "the split falls before the contributions quote, which is whole in the second message, with the rest after it");
        CHECK(messages.size() == 2 && Has(messages[1], L"\n\x2026</blockquote>") && Has(messages[0], L" +" + std::to_wstring(30 + 3 - kClaudeStatusMaxMcpNames) + L" more"),
              "the quote is cut with an ellipsis; the MCP names stop at kClaudeStatusMaxMcpNames with a count");
    }

    // ---- 4. the OS half on this PC ----
    {
        // The FILE read: CRLF, a BOM, a filler line sized so the next (wanted) line STRADDLES the 1 MiB chunk boundary of the
        // reader (kTranscriptChunkBytes), and a torn last line.
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / L"am-m5-claude-status";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path file = dir / L"transcript.jsonl";
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "\xEF\xBB\xBF";
            const auto lines = SampleTranscript();
            for (size_t i = 0; i < lines.size(); ++i)
            {
                if (i == 5)
                {
                    // Line 5 (a response) then starts 100 bytes before 1 MiB and ends after it.
                    const std::string head = "{\"type\":\"user\",\"filler\":\"";
                    const std::string tail = "\"}\r\n";
                    const int64_t at = static_cast<int64_t>(out.tellp());
                    const int64_t fill = (1024 * 1024 - 100) - at - static_cast<int64_t>(head.size() + tail.size());
                    out << head << std::string(static_cast<size_t>(fill > 0 ? fill : 0), 'x') << tail;
                }
                out << ToUtf8(lines[i]);
                if (i + 1 < lines.size())
                {
                    out << "\r\n";
                }
            }
        }
        ClaudeTranscriptStatus fromFile;
        std::wstring error;
        const bool read = ReadClaudeTranscriptStatusFile(file.wstring(), fromFile, error);
        CHECK(read && error.empty(), "the transcript file is read");
        CHECK(fromFile.responses == t.responses && fromFile.inputTokens == t.inputTokens && fromFile.cacheReadTokens == t.cacheReadTokens && fromFile.contextTokens == t.contextTokens &&
                  fromFile.mcpServers == t.mcpServers && fromFile.instructionFiles.size() == t.instructionFiles.size() && fromFile.modelName == t.modelName &&
                  fromFile.permissionMode == t.permissionMode && fromFile.customTitle == t.customTitle && fromFile.mcpFailed.size() == 1 && fromFile.compactions == 1,
              "the file read folds exactly what the in-memory fold does (CRLF dropped, the BOM skipped, a line across two chunks joined)");
        CHECK(fromFile.lines == t.lines + 1 && fromFile.unparsedLines == 1 && fromFile.parsedLines == t.parsedLines, "every line counted (the filler passed unparsed), the torn line too");
        ClaudeTranscriptStatus missing;
        CHECK(!ReadClaudeTranscriptStatusFile((dir / L"none.jsonl").wstring(), missing, error) && !error.empty(), "a missing file: false, with the system's reason");
        std::filesystem::remove_all(dir, ec);

        // The hidden child runner, with cmd.exe as the child (never claude).
        const std::wstring cmd = CmdExe();
        auto c = RunClaudeCapture(cmd, L"/d /c echo hello", L"", 15000);
        CHECK(c.started && !c.timedOut && c.exitCode == 0 && c.out.rfind(L"hello", 0) == 0, "a child's standard output is captured");
        c = RunClaudeCapture(cmd, L"/d /c exit /b 3", L"", 15000);
        CHECK(c.started && c.exitCode == 3, "its exit code");
        c = RunClaudeCapture(cmd, L"/d /c echo oops 1>&2", L"", 15000);
        CHECK(c.started && Has(c.err, L"oops") && c.out.empty(), "standard error apart from standard output");
        const std::wstring tempDir = std::filesystem::temp_directory_path().wstring();
        c = RunClaudeCapture(cmd, L"/d /c cd", tempDir, 15000);
        std::wstring cwdSeen = c.out;
        while (!cwdSeen.empty() && (cwdSeen.back() == L'\n' || cwdSeen.back() == L'\r'))
        {
            cwdSeen.pop_back();
        }
        std::wstring tempTrimmed = tempDir;
        while (tempTrimmed.size() > 3 && (tempTrimmed.back() == L'\\' || tempTrimmed.back() == L'/'))
        {
            tempTrimmed.pop_back();
        }
        CHECK(c.started && ::CompareStringOrdinal(cwdSeen.c_str(), -1, tempTrimmed.c_str(), -1, TRUE) == CSTR_EQUAL, "the child's working folder");
        // The environment: a parent session's variables removed, the harness mark and the two switches set.
        wchar_t previous[256]{};
        const DWORD hadPrevious = ::GetEnvironmentVariableW(L"AM_SESSION", previous, 256);
        ::SetEnvironmentVariableW(L"AM_SESSION", L"m5-probe-session");
        c = RunClaudeCapture(cmd, L"/d /c set", L"", 15000);
        ::SetEnvironmentVariableW(L"AM_SESSION", hadPrevious > 0 && hadPrevious < 256 ? previous : nullptr);
        CHECK(c.started && Has(c.out, L"AM_HARNESS=claude-status") && Has(c.out, L"DISABLE_AUTOUPDATER=1") && Has(c.out, L"CLAUDE_CODE_DISABLE_AUTO_MEMORY=1") &&
                  !Has(c.out, L"AM_SESSION=m5-probe-session"),
              "the child's environment: AM_SESSION removed, AM_HARNESS + DISABLE_AUTOUPDATER + CLAUDE_CODE_DISABLE_AUTO_MEMORY set");
        // The timeout ends the child AND what it started (ping, through the job), and the call returns.
        const ULONGLONG before = ::GetTickCount64();
        c = RunClaudeCapture(cmd, L"/d /c ping -n 30 127.0.0.1 >nul", L"", 800);
        CHECK(c.started && c.timedOut && ::GetTickCount64() - before < 10000, "a child past its time is ended with its own children, and the call returns at once");
        c = RunClaudeCapture(L"C:\\nonexistent-folder\\nope.exe", L"", L"", 5000);
        CHECK(!c.started && !c.error.empty(), "a missing executable: not started, the system's reason");

        // The probe: no claude, the cache (cmd.exe stands in: its banner is "lines"), the report slots.
        ClaudeStatusProbe probe;
        CHECK(probe.Account(L"").error == L"Claude Code was not found on this PC" && probe.Usage(L"").error == L"Claude Code was not found on this PC",
              "no claude.exe: both reads say so, no child");
        const ClaudeAccountRead first = probe.Account(cmd);
        const ClaudeAccountRead second = probe.Account(cmd);
        CHECK(first.ok && !first.cached && first.readAtMs > 0 && second.ok && second.cached && second.lines.size() == first.lines.size(), "a good answer is reused from the cache");
        const ClaudeUsageRead usage = probe.Usage(cmd);
        CHECK(!usage.ok && usage.error == L"its answer was not JSON", "an answer that is not Claude Code's JSON fails (and is not cached)");
        CHECK(probe.TryBeginReport() && probe.TryBeginReport() && !probe.TryBeginReport(), "at most kClaudeStatusMaxReports reports at once");
        probe.EndReport();
        CHECK(probe.TryBeginReport(), "a slot given back is free again");
        probe.EndReport();
        probe.EndReport();
    }
}
