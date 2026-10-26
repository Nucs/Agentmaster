// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — /status in Telegram shows what Claude Code's own /status dialog shows (MESSAGING.md §21.40). The user's request
// (2026-10-26): "When user send in telegram /status, we need to get all the info we can see in claude code status and other tabs
// like usage to be sent to the user."
//
// What the bot sends. In a session's topic, /status keeps its card copy (§21.24) and then sends the REPORT below as a reply to
// the command: the dialog's Status tab for this session, its Usage tab's plan limits and contributions, and this conversation's
// tokens. In the 🔎 Search topic and in the chat's main view, /status keeps the bot's own line and then sends the ACCOUNT and
// PLAN USAGE alone: they are the account's, not a session's.
//
//   📊 Claude Code status · 🟦 Agentmaster
//
//   Status
//   Version: 2.1.310
//   Session name: Telegram status from Claude Code tabs
//   Session ID: f15a6e0a-b72d-4c4e-9e64-6072ddffd9f0
//   cwd: K:\source\Agentmaster
//   Model: Opus 5.5 (1M context) (claude-opus-5-5[1m])
//   Effort: max
//   Permission mode: bypassPermissions
//   Context: 312.4k tokens
//   MCP servers: 7 connected, 1 failed  (their names, then each failure's reason)
//   Memory: 3 files  (each path and kind)
//   Claude: busy · pid 50500 · running 2h13m
//
//   Account                                (claude auth status --text, verbatim)
//   Login method: Claude Max account …
//
//   Plan usage                             (claude -p /usage, verbatim, a bar beside each limit)
//   Current session: ▰▱▱▱▱▱▱▱▱▱ 7% used · resets …
//   ▸ What's contributing to your limits usage?   (an expandable quote)
//
//   This conversation
//   Usage: 1.2m input, 345.6k output, 98.7m cache read, 2.1m cache write · 1,234 responses
//
// Where each fact comes from is ClaudeStatus.h's business; this header only LAYS OUT what the service gathered (pure: no
// process, no file, no Telegram call), so the m5 suite pins the layout, the fallbacks and the size limits
// (tests_claude_status.cpp). The service's half is Messaging.Status.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ClaudeStatus.h" // ClaudeTranscriptStatus, ClaudeAccountRead, ClaudeUsageRead, the /usage line readers, the token wording
#include "SessionModels.h" // SessionInfo
#include "TelegramFormat.h" // TgEscapeHtml, TgShortSpan, TgClockText

namespace Agentmaster::Msg
{
    /// <summary>
    /// What claude's presence HEARTBEAT (~/.claude/sessions/&lt;pid&gt;.json) says about the session's process — the service
    /// reads it and keeps it only when the row names this session (a pid Windows reused for another claude never matches).
    /// </summary>
    struct ClaudeStatusProcess
    {
        bool running{ false }; // a live heartbeat of this session was read
        uint32_t pid{ 0 }; // its process id
        std::wstring version; // the version that process runs ("2.1.310")
        std::wstring status; // claude's own word: busy, idle, waiting, shell
        std::wstring waitingFor; // with "waiting": what for ("input needed")
        int64_t startedAtMs{ 0 }; // when the process started (Unix ms; 0 = unknown)
    };

    /// <summary>Everything one report is laid out from (BuildClaudeStatusReport). The service fills it on its worker thread.</summary>
    struct ClaudeStatusReportInput
    {
        /// <summary>The session the report is about; null for the FLEET report (the account and the plan usage only).</summary>
        const SessionInfo* session{ nullptr };
        /// <summary>The session's title as a title TOKEN (TgTitleToken; the send layer renders it as a chip or a square); "" for none.</summary>
        std::wstring titleToken;
        /// <summary>The heartbeat's facts (running false when there is none).</summary>
        ClaudeStatusProcess process;
        /// <summary>The transcript was read: `transcript` holds its facts (false: no transcript yet, or it could not be read).</summary>
        bool transcriptRead{ false };
        /// <summary>The transcript's facts (ClaudeTranscriptStatusReader).</summary>
        ClaudeTranscriptStatus transcript;
        /// <summary>
        /// A line about the transcript the report shows under its Status section: whose transcript was read (a fork's SOURCE, for a
        /// fork that has no message of its own yet), or why none was. "" = nothing to say.
        /// </summary>
        std::wstring transcriptNote;
        /// <summary>Whether the Account section is shown (false for a session that is not Claude Code's).</summary>
        bool accountWanted{ true };
        /// <summary>The account lines, or why there are none.</summary>
        ClaudeAccountRead account;
        /// <summary>Whether the Plan usage section is shown.</summary>
        bool usageWanted{ true };
        /// <summary>The plan usage, or why there is none.</summary>
        ClaudeUsageRead usage;
        /// <summary>Now (Unix ms): the footer's clock, the process's running span, a cached answer's age.</summary>
        int64_t nowMs{ 0 };
    };

    /// <summary>
    /// The longest message the report builder makes (HTML characters, tags included): under Telegram's 4,096 with room, and the
    /// HTML is never shorter than the text Telegram counts, so a part this long always fits in one message. A report that would
    /// be longer goes as two messages, split between whole sections (never inside a quote, which a split would break).
    /// </summary>
    inline constexpr size_t kClaudeStatusMessageMaxChars = 3800;
    /// <summary>The most characters of the /usage "What's contributing" section shown (its quote is cut with "…" past it).</summary>
    inline constexpr size_t kClaudeStatusContributionsMaxChars = 2400;
    /// <summary>The most MCP server names listed (a count says how many more).</summary>
    inline constexpr size_t kClaudeStatusMaxMcpNames = 24;
    /// <summary>The most instruction files listed (a count says how many more).</summary>
    inline constexpr size_t kClaudeStatusMaxMemoryFiles = 8;

    namespace status_detail
    {
        /// <summary>One "Label: value" row: the label as written, then the value (HTML the caller escaped or built).</summary>
        /// <param name="label">The label (plain text; escaped here).</param>
        /// <param name="valueHtml">The value as HTML.</param>
        /// <returns>"Label: value\n".</returns>
        /// <exception cref="std::bad_alloc">If the row cannot be allocated.</exception>
        inline std::wstring StatusRowHtml(std::wstring_view label, std::wstring_view valueHtml)
        {
            return TgEscapeHtml(label) + L": " + std::wstring{ valueHtml } + L"\n";
        }

        /// <summary>A value in a code span: an id or a path, which the phone then copies with one tap.</summary>
        /// <param name="text">The plain value.</param>
        /// <returns>&lt;code&gt;value&lt;/code&gt;.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring CodeHtml(std::wstring_view text)
        {
            return L"<code>" + TgEscapeHtml(text) + L"</code>";
        }

        /// <summary>
        /// A limit's bar: ten cells, ▰ filled and ▱ empty, one cell per 10 % rounded half up — but at least one filled cell for
        /// any use above 0 % (a 3 % session still shows that it started) and every cell at 100 % or more.
        /// </summary>
        /// <param name="percent">The percent used (anything above 100 fills the bar).</param>
        /// <returns>The ten cells.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring UsageBar(int percent)
        {
            int filled = percent <= 0 ? 0 : (percent >= 100 ? 10 : (percent + 5) / 10);
            if (percent > 0 && filled == 0)
            {
                filled = 1;
            }
            std::wstring bar;
            bar.reserve(10);
            for (int i = 0; i < 10; ++i)
            {
                bar += i < filled ? L'\x25B0' : L'\x25B1';
            }
            return bar;
        }

        /// <summary>
        /// Cuts a text to at most `maxChars` UTF-16 units with an ellipsis, never splitting a surrogate pair (a lone half makes the
        /// text invalid UTF-16, which Telegram refuses) and preferring the last line break before the cut.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <param name="maxChars">The cap.</param>
        /// <returns>The text, or its cut form ending in "\n…" or "…".</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring CutText(std::wstring_view text, size_t maxChars)
        {
            if (text.size() <= maxChars || maxChars < 2)
            {
                return std::wstring{ text };
            }
            size_t cut = maxChars - 1;
            if (cut > 0 && text[cut - 1] >= 0xD800 && text[cut - 1] <= 0xDBFF)
            {
                --cut; // keep a surrogate pair whole
            }
            const size_t lineBreak = text.substr(0, cut).find_last_of(L'\n');
            if (lineBreak != std::wstring_view::npos && lineBreak > cut / 2)
            {
                return std::wstring{ text.substr(0, lineBreak) } + L"\n\x2026";
            }
            return std::wstring{ text.substr(0, cut) } + L"\x2026";
        }

        /// <summary>The lines of a text (LF or CRLF), the line breaks dropped, blank lines kept.</summary>
        /// <param name="text">The text.</param>
        /// <returns>The lines; one empty line for empty text.</returns>
        /// <exception cref="std::bad_alloc">If the lines cannot be allocated.</exception>
        inline std::vector<std::wstring> SplitLines(std::wstring_view text)
        {
            std::vector<std::wstring> lines;
            size_t start = 0;
            for (;;)
            {
                const size_t end = text.find(L'\n', start);
                std::wstring_view line = text.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
                if (!line.empty() && line.back() == L'\r')
                {
                    line.remove_suffix(1);
                }
                lines.emplace_back(line);
                if (end == std::wstring_view::npos)
                {
                    return lines;
                }
                start = end + 1;
            }
        }

        /// <summary>
        /// How claude's heartbeat word reads in the report: Claude Code's own word, with what a "waiting" claude waits for.
        /// </summary>
        /// <param name="p">The heartbeat's facts.</param>
        /// <returns>"busy", "waiting (input needed)", … ("" when the heartbeat had no word).</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring HeartbeatWord(const ClaudeStatusProcess& p)
        {
            if (p.status == L"waiting" && !p.waitingFor.empty())
            {
                return L"waiting (" + p.waitingFor + L")";
            }
            return p.status;
        }
    }

    /// <summary>
    /// The Plan usage section of a report from Claude Code's /usage text: every line kept in its order and escaped, each LIMIT line
    /// ("Current session: 7% used · resets …") with its bar after the colon, blank lines kept single; the "What's contributing to
    /// your limits usage?" section and everything after it goes into `contributionsHtml` instead — an expandable quote, cut at
    /// kClaudeStatusContributionsMaxChars — because it is long and approximate. A line this build cannot read as a limit (a
    /// wording Claude Code changes later) still shows, only without a bar.
    /// </summary>
    /// <param name="usageText">The /usage text (ClaudeUsageRead::text).</param>
    /// <param name="contributionsHtml">Receives the expandable quote, or "" when the text has no such section.</param>
    /// <returns>The section's lines as HTML (each ending in a line break); "" for an empty text.</returns>
    /// <exception cref="std::bad_alloc">If the HTML cannot be allocated.</exception>
    inline std::wstring ClaudeUsageSectionHtml(std::wstring_view usageText, std::wstring& contributionsHtml)
    {
        contributionsHtml.clear();
        std::wstring html;
        std::wstring contributions;
        bool inContributions = false;
        bool lastBlank = true; // no blank line at the section's start
        for (const auto& line : status_detail::SplitLines(usageText))
        {
            if (!inContributions && IsClaudeUsageContributionsHeading(line))
            {
                inContributions = true;
            }
            if (inContributions)
            {
                contributions += line;
                contributions += L'\n';
                continue;
            }
            if (line.find_first_not_of(L" \t") == std::wstring::npos)
            {
                if (!lastBlank)
                {
                    html += L'\n';
                    lastBlank = true;
                }
                continue;
            }
            lastBlank = false;
            if (const auto limit = ParseClaudeUsageLimitLine(line))
            {
                html += TgEscapeHtml(limit->title) + L": " + status_detail::UsageBar(limit->percent) + L" " + std::to_wstring(limit->percent) + L"% used";
                if (!limit->tail.empty())
                {
                    html += L" \x00B7 " + TgEscapeHtml(limit->tail);
                }
                html += L'\n';
            }
            else
            {
                html += TgEscapeHtml(line) + L'\n';
            }
        }
        while (!html.empty() && html.back() == L'\n' && html.size() >= 2 && html[html.size() - 2] == L'\n')
        {
            html.pop_back(); // no blank line at the end
        }
        const size_t last = contributions.find_last_not_of(L" \t\r\n");
        if (last != std::wstring::npos)
        {
            contributionsHtml = L"<blockquote expandable>" + TgEscapeHtml(status_detail::CutText(std::wstring_view{ contributions }.substr(0, last + 1), kClaudeStatusContributionsMaxChars)) +
                                L"</blockquote>";
        }
        return html;
    }

    /// <summary>
    /// The Status section of a session's report: the rows of Claude Code's Status tab this build can know out of band, each from
    /// its best source — the heartbeat (a live process's version), the transcript (the session name, the model, the effort, the
    /// permission mode, the MCP servers, the instruction files, the extra folders), and Agentmaster's own record when the
    /// transcript has nothing (the observed model, effort and permission mode, the context). A row with no value is left out;
    /// the MCP row is left out when no tools delta was ever read (unknown, not "none").
    /// </summary>
    /// <param name="in">The report's input (its session must be set).</param>
    /// <returns>The section as HTML, headed "Status".</returns>
    /// <exception cref="std::bad_alloc">If the HTML cannot be allocated.</exception>
    inline std::wstring ClaudeStatusSectionHtml(const ClaudeStatusReportInput& in)
    {
        const SessionInfo& s = *in.session;
        const ClaudeTranscriptStatus& t = in.transcript;
        const bool facts = in.transcriptRead;
        std::wstring html = L"<b>Status</b>\n";
        // Version: the RUNNING process's (the dialog shows its own); else the version that last wrote the transcript.
        if (!in.process.version.empty())
        {
            html += status_detail::StatusRowHtml(L"Version", TgEscapeHtml(in.process.version));
        }
        else if (facts && !t.version.empty())
        {
            html += status_detail::StatusRowHtml(L"Version", TgEscapeHtml(t.version) + L" (its last run)");
        }
        // Session name: Claude Code's own (a /rename title, else the title it generated) — the dialog's "Session name".
        if (facts && (!t.customTitle.empty() || !t.aiTitle.empty()))
        {
            html += status_detail::StatusRowHtml(L"Session name", TgEscapeHtml(TgTitleLine(t.customTitle.empty() ? t.aiTitle : t.customTitle, 200)));
        }
        html += status_detail::StatusRowHtml(L"Session ID", status_detail::CodeHtml(s.id));
        const std::wstring& cwd = s.liveCwd.empty() ? s.workingDir : s.liveCwd;
        if (!cwd.empty())
        {
            html += status_detail::StatusRowHtml(L"cwd", status_detail::CodeHtml(cwd));
        }
        // Model: the transcript's model attachment (Claude Code's own name for it) with the id the newest response asked for —
        // a /model switch shows in the response first; else Agentmaster's observed model.
        {
            const std::wstring id = facts && !t.requestedModel.empty() ? t.requestedModel : (facts ? t.modelId : std::wstring{});
            std::wstring model;
            if (facts && !t.modelName.empty() && (id.empty() || id == t.modelId))
            {
                model = TgEscapeHtml(t.modelName) + (id.empty() ? std::wstring{} : L" (" + status_detail::CodeHtml(id) + L")");
            }
            else if (!id.empty())
            {
                model = status_detail::CodeHtml(id);
            }
            else if (!s.currentModel.empty() || !s.model.empty())
            {
                model = status_detail::CodeHtml(s.currentModel.empty() ? s.model : s.currentModel);
            }
            if (!model.empty())
            {
                html += status_detail::StatusRowHtml(L"Model", model);
            }
        }
        if (const std::wstring& effort = facts && !t.effort.empty() ? t.effort : s.effort; !effort.empty())
        {
            html += status_detail::StatusRowHtml(L"Effort", TgEscapeHtml(effort));
        }
        if (const std::wstring& mode = facts && !t.permissionMode.empty() ? t.permissionMode : s.permissionMode; !mode.empty())
        {
            html += status_detail::StatusRowHtml(L"Permission mode", TgEscapeHtml(mode));
        }
        if (const int64_t context = s.contextTokens > 0 ? s.contextTokens : (facts ? t.contextTokens : 0); context > 0)
        {
            html += status_detail::StatusRowHtml(L"Context", FormatClaudeTokenCount(context) + L" tokens");
        }
        // MCP servers, worded like the dialog's row ("N connected, N need auth, N pending, N failed"), then the names, then each
        // failure with Claude Code's own reason.
        if (facts && t.mcpSeen)
        {
            std::wstring counts;
            const auto part = [&counts](size_t n, const wchar_t* word) {
                if (n > 0)
                {
                    counts += (counts.empty() ? L"" : L", ") + std::to_wstring(n) + L" " + word;
                }
            };
            part(t.mcpServers.size(), L"connected");
            part(t.mcpNeedsAuth.size(), L"need auth");
            part(t.mcpPending.size(), L"pending");
            part(t.mcpFailed.size(), L"failed");
            html += status_detail::StatusRowHtml(L"MCP servers", counts.empty() ? std::wstring{ L"none" } : counts);
            if (!t.mcpServers.empty())
            {
                std::wstring names;
                for (size_t i = 0; i < t.mcpServers.size() && i < kClaudeStatusMaxMcpNames; ++i)
                {
                    names += (i == 0 ? L"" : L", ") + TgEscapeHtml(t.mcpServers[i]);
                }
                if (t.mcpServers.size() > kClaudeStatusMaxMcpNames)
                {
                    names += L" +" + std::to_wstring(t.mcpServers.size() - kClaudeStatusMaxMcpNames) + L" more";
                }
                html += L"  " + names + L"\n";
            }
            for (const auto& name : t.mcpNeedsAuth)
            {
                html += L"  \x26A0\xFE0F " + TgEscapeHtml(name) + L": needs authorization (/mcp at the PC)\n";
            }
            for (const auto& name : t.mcpPending)
            {
                html += L"  \x23F3 " + TgEscapeHtml(name) + L": still connecting\n";
            }
            for (const auto& f : t.mcpFailed)
            {
                html += L"  \x2716\xFE0F " + TgEscapeHtml(f.name) + (f.error.empty() ? std::wstring{} : L": " + TgEscapeHtml(status_detail::CutText(f.error, 200))) + L"\n";
            }
        }
        // Memory: the instruction files Claude Code loaded, each with its kind (User, Project, Local, AutoMem, …).
        if (facts && !t.instructionFiles.empty())
        {
            const size_t n = t.instructionFiles.size();
            html += status_detail::StatusRowHtml(L"Memory", std::to_wstring(n) + (n == 1 ? L" file" : L" files"));
            for (size_t i = 0; i < n && i < kClaudeStatusMaxMemoryFiles; ++i)
            {
                const auto& f = t.instructionFiles[i];
                html += L"  " + status_detail::CodeHtml(f.path) + (f.type.empty() ? std::wstring{} : L" (" + TgEscapeHtml(f.type) + L")") + L"\n";
            }
            if (n > kClaudeStatusMaxMemoryFiles)
            {
                html += L"  +" + std::to_wstring(n - kClaudeStatusMaxMemoryFiles) + L" more\n";
            }
        }
        if (facts && !t.extraDirectories.empty())
        {
            std::wstring dirs;
            for (size_t i = 0; i < t.extraDirectories.size() && i < kClaudeStatusMaxMemoryFiles; ++i)
            {
                dirs += (i == 0 ? L"" : L", ") + status_detail::CodeHtml(t.extraDirectories[i]);
            }
            html += status_detail::StatusRowHtml(L"Additional directories", dirs);
        }
        if (facts && t.isWorktree)
        {
            html += status_detail::StatusRowHtml(L"Git worktree", L"yes");
        }
        // The process: what claude's heartbeat says, or why there is no claude to ask.
        if (in.process.running)
        {
            std::wstring line;
            if (const std::wstring word = status_detail::HeartbeatWord(in.process); !word.empty())
            {
                line = TgEscapeHtml(word) + L" \x00B7 ";
            }
            line += L"pid " + std::to_wstring(in.process.pid);
            if (in.process.startedAtMs > 0 && in.nowMs > in.process.startedAtMs)
            {
                line += L" \x00B7 running " + TgShortSpan(in.nowMs - in.process.startedAtMs);
            }
            html += status_detail::StatusRowHtml(L"Claude", line);
        }
        else if (!s.started && !s.external)
        {
            html += status_detail::StatusRowHtml(L"Claude", L"not running \x2014 the tab is parked; a message here or /activate starts it");
        }
        else if (s.state == SessionState::Done)
        {
            html += status_detail::StatusRowHtml(L"Claude", L"exited \x2014 /reactivate starts it again");
        }
        else if (s.pid != 0)
        {
            html += status_detail::StatusRowHtml(L"Claude", L"pid " + std::to_wstring(s.pid) + L" (no heartbeat read)");
        }
        if (!in.transcriptNote.empty())
        {
            html += L"<i>" + TgEscapeHtml(in.transcriptNote) + L"</i>\n";
        }
        return html;
    }

    /// <summary>
    /// The Account section: Claude Code's own lines (claude auth status --text) as "Label: value" rows, or why there are none.
    /// </summary>
    /// <param name="account">The account read.</param>
    /// <returns>The section as HTML, headed "Account".</returns>
    /// <exception cref="std::bad_alloc">If the HTML cannot be allocated.</exception>
    inline std::wstring ClaudeAccountSectionHtml(const ClaudeAccountRead& account)
    {
        std::wstring html = L"<b>Account</b>\n";
        if (!account.ok)
        {
            return html + L"\x26A0\xFE0F " + TgEscapeHtml(account.error.empty() ? std::wstring{ L"the account could not be read" } : account.error) + L"\n";
        }
        for (const auto& line : account.lines)
        {
            html += line.label.empty() ? TgEscapeHtml(line.value) + L"\n" : status_detail::StatusRowHtml(line.label, TgEscapeHtml(line.value));
        }
        return html;
    }

    /// <summary>
    /// The Plan usage section: Claude Code's /usage text (ClaudeUsageSectionHtml), the age of a reused answer, or why there is
    /// none — with Claude Code's own message when it gave one (an account with no plan usage, for one).
    /// </summary>
    /// <param name="usage">The usage read.</param>
    /// <param name="nowMs">Now (Unix ms), for a reused answer's age.</param>
    /// <param name="contributionsHtml">Receives the expandable "What's contributing" quote ("" when there is none).</param>
    /// <returns>The section as HTML, headed "Plan usage".</returns>
    /// <exception cref="std::bad_alloc">If the HTML cannot be allocated.</exception>
    inline std::wstring ClaudeUsageSectionBlockHtml(const ClaudeUsageRead& usage, int64_t nowMs, std::wstring& contributionsHtml)
    {
        contributionsHtml.clear();
        std::wstring html = L"<b>Plan usage</b>";
        if (usage.ok && usage.cached && usage.readAtMs > 0 && nowMs > usage.readAtMs)
        {
            html += L" <i>(read " + TgShortSpan(nowMs - usage.readAtMs) + L" ago)</i>";
        }
        html += L"\n";
        if (!usage.ok)
        {
            html += L"\x26A0\xFE0F " + TgEscapeHtml(usage.error.empty() ? std::wstring{ L"the usage could not be read" } : usage.error) + L"\n";
            if (usage.text.find_first_not_of(L" \t\r\n") != std::wstring::npos)
            {
                html += L"<blockquote>" + TgEscapeHtml(status_detail::CutText(usage.text, 600)) + L"</blockquote>\n";
            }
            return html;
        }
        return html + ClaudeUsageSectionHtml(usage.text, contributionsHtml);
    }

    /// <summary>
    /// The "This conversation" section: the tokens of every response in the session's transcript (each response once), worded
    /// like the Usage tab's own "Usage:" row, with the number of responses and of compactions. "" when the transcript holds no
    /// response yet.
    /// </summary>
    /// <param name="t">The transcript's facts.</param>
    /// <returns>The section as HTML, or "".</returns>
    /// <exception cref="std::bad_alloc">If the HTML cannot be allocated.</exception>
    inline std::wstring ClaudeConversationSectionHtml(const ClaudeTranscriptStatus& t)
    {
        if (t.responses == 0)
        {
            return {};
        }
        std::wstring html = L"<b>This conversation</b> <i>(its transcript)</i>\n";
        html += status_detail::StatusRowHtml(L"Usage", FormatClaudeTokenCount(t.inputTokens) + L" input, " + FormatClaudeTokenCount(t.outputTokens) + L" output, " +
                                                    FormatClaudeTokenCount(t.cacheReadTokens) + L" cache read, " + FormatClaudeTokenCount(t.cacheWriteTokens) + L" cache write");
        html += status_detail::StatusRowHtml(L"Responses", GroupClaudeCount(t.responses) + (t.compactions > 0 ? L" \x00B7 " + std::to_wstring(t.compactions) + (t.compactions == 1 ? L" compaction" : L" compactions") : std::wstring{}));
        return html;
    }

    /// <summary>
    /// Lays a whole report out as Telegram HTML messages (MESSAGING.md §21.40): the header (with the session's title token, or
    /// the fleet's heading), the Status section (a session only), Account, Plan usage, the contributions quote, This conversation
    /// (a session only), and a footer that names the sources and the time. One message when it fits
    /// kClaudeStatusMessageMaxChars; else two, split before the contributions quote (or, when the first half alone is still too
    /// long, after the Status section) — never inside a section, so every quote and tag stays whole.
    /// </summary>
    /// <param name="in">What was gathered.</param>
    /// <returns>One or two messages of Telegram HTML (title tokens included, which the send layer renders).</returns>
    /// <exception cref="std::bad_alloc">If the HTML cannot be allocated.</exception>
    inline std::vector<std::wstring> BuildClaudeStatusReport(const ClaudeStatusReportInput& in)
    {
        std::vector<std::wstring> sections; // each ends with "\n"; joined with a blank line between
        if (in.session)
        {
            sections.push_back(L"\U0001F4CA <b>Claude Code status</b>" + (in.titleToken.empty() ? std::wstring{} : L" \x00B7 " + in.titleToken) + L"\n");
            sections.push_back(ClaudeStatusSectionHtml(in));
        }
        else
        {
            sections.push_back(L"\U0001F4CA <b>Claude Code</b> \x00B7 account and plan usage\n");
        }
        if (in.accountWanted)
        {
            sections.push_back(ClaudeAccountSectionHtml(in.account));
        }
        std::wstring contributions;
        if (in.usageWanted)
        {
            sections.push_back(ClaudeUsageSectionBlockHtml(in.usage, in.nowMs, contributions));
        }
        // Everything after this index may move to a second message.
        const size_t splitAt = sections.size();
        if (!contributions.empty())
        {
            sections.push_back(contributions + L"\n");
        }
        if (in.session && in.transcriptRead)
        {
            if (std::wstring conversation = ClaudeConversationSectionHtml(in.transcript); !conversation.empty())
            {
                sections.push_back(std::move(conversation));
            }
        }
        std::wstring footer = L"<i>Read " + TgClockText(in.nowMs, in.nowMs) + L" \x00B7 " +
                              (in.session ? L"the session from its transcript and claude's heartbeat; " : L"") +
                              L"the account and the plan usage as Claude Code prints them (claude auth status, /usage)</i>\n";
        sections.push_back(std::move(footer));

        const auto join = [](const std::vector<std::wstring>& parts, size_t from, size_t to) {
            std::wstring out;
            for (size_t i = from; i < to && i < parts.size(); ++i)
            {
                if (!out.empty())
                {
                    out += L'\n'; // the blank line between two sections
                }
                out += parts[i];
            }
            while (!out.empty() && out.back() == L'\n')
            {
                out.pop_back();
            }
            return out;
        };
        std::vector<std::wstring> messages;
        std::wstring whole = join(sections, 0, sections.size());
        if (whole.size() <= kClaudeStatusMessageMaxChars)
        {
            messages.push_back(std::move(whole));
            return messages;
        }
        // Too long for one message: split between whole sections. The first half ends with the plan usage (the facts the user
        // asked for come first); when even that is too long, the Status section goes alone and the rest follows.
        size_t cut = splitAt;
        if (join(sections, 0, cut).size() > kClaudeStatusMessageMaxChars && in.session && cut > 2)
        {
            cut = 2; // the header and the Status section
        }
        messages.push_back(join(sections, 0, cut));
        std::wstring rest = join(sections, cut, sections.size());
        if (!rest.empty())
        {
            messages.push_back(std::move(rest));
        }
        return messages;
    }
}
