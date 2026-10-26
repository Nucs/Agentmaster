// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — what Claude Code's own /status dialog shows, read WITHOUT opening it (MESSAGING.md §21.40). The user's request
// (2026-10-26): "When user send in telegram /status, we need to get all the info we can see in claude code status and other tabs
// like usage to be sent to the user."
//
// Claude Code 2.1.310's /status is a dialog with four tabs — Status (version, session name and id, cwd, the account, the model,
// MCP servers, memory files, setting sources), Config (the settings list), Usage (this process's cost and tokens, the plan's
// limits, what contributes to them, usage credits) and Stats (an activity heatmap and all-time totals). Typing /status into a
// session would show it on the PC, write "/status" + "Settings dialog dismissed" into the session's transcript, need an empty
// input box, and work only while claude runs. So Agentmaster reads the same facts OUT OF BAND instead, from three sources:
//   * the session's TRANSCRIPT (ClaudeTranscriptStatusReader, below): the model, effort, permission mode, MCP servers (and the
//     ones that failed), the instruction files, the extra working folders, the session name, the tokens of every response;
//   * Claude Code's own HEADLESS commands, run by Agentmaster as short-lived children (ClaudeStatus.cpp, ClaudeStatusProbe):
//     `claude auth status --text` prints the account lines exactly as the Status tab shows them (0.5 s), and
//     `claude -p /usage --output-format json` prints the Usage tab's plan limits and "What's contributing" as text — a LOCAL
//     command, no model call (measured on 2.1.310: num_turns 0, total_cost_usd 0, local_command "usage", ~6 s);
//   * Agentmaster's registry and claude's presence heartbeat (the service fills those in, MessagingStatus.h).
//
// This header is the PURE half (no process, no file, no Windows call): the version gate of the headless /usage, the parsers of
// the two commands' output, the /usage text's limit lines, the token counts' wording, and the transcript reader. The m5 suite
// pins every rule (tests_claude_status.cpp). The OS half — the child processes, their cache, the transcript file read — is
// declared at the end and lives in ClaudeStatus.cpp.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Json.h"

namespace Agentmaster
{
    // ===== the headless /usage version gate ===========================================================================

    /// <summary>
    /// The oldest Claude Code whose /usage runs NON-INTERACTIVELY (`claude -p /usage`) as a local command: 2.1.310, the version
    /// measured (2026-10-26). An older claude may not know the non-interactive /usage, and would then send the text "/usage" to
    /// the MODEL as a prompt — a paid call for nothing — so the probe never runs it below this version (ClaudeStatusProbe).
    /// </summary>
    inline constexpr uint32_t kHeadlessUsageMinMajor = 2;
    /// <summary>The minor part of the gate (see kHeadlessUsageMinMajor).</summary>
    inline constexpr uint32_t kHeadlessUsageMinMinor = 1;
    /// <summary>The build (Claude Code's patch number) of the gate (see kHeadlessUsageMinMajor).</summary>
    inline constexpr uint32_t kHeadlessUsageMinBuild = 310;

    /// <summary>
    /// Whether a claude.exe of this version may run the headless /usage (see kHeadlessUsageMinMajor). Compares major, minor and
    /// build in that order; the PE file version's 4th part is not Claude Code's and is ignored by the caller.
    /// </summary>
    /// <param name="major">The "2" of 2.1.310.</param>
    /// <param name="minor">The "1" of 2.1.310.</param>
    /// <param name="build">The "310" of 2.1.310.</param>
    /// <returns>True for 2.1.310 and newer.</returns>
    inline bool ClaudeVersionRunsHeadlessUsage(uint32_t major, uint32_t minor, uint32_t build) noexcept
    {
        if (major != kHeadlessUsageMinMajor)
        {
            return major > kHeadlessUsageMinMajor;
        }
        if (minor != kHeadlessUsageMinMinor)
        {
            return minor > kHeadlessUsageMinMinor;
        }
        return build >= kHeadlessUsageMinBuild;
    }

    // ===== "Label: value" lines (claude auth status --text) ===========================================================

    /// <summary>
    /// One line of Claude Code's "Label: value" output, as `claude auth status --text` prints the Status tab's account rows
    /// ("Login method: Claude Max account", "Organization: …", "Email: …", "Anthropic base URL: …"). A line that is no such pair
    /// keeps its text whole in `value` with an empty `label` (a notice such as "Not logged in"), so nothing Claude Code says is lost.
    /// </summary>
    struct ClaudeStatusLine
    {
        std::wstring label; // "Login method" ("" for a line that is no "Label: value" pair)
        std::wstring value; // "Claude Max account" (the whole line when label is "")
    };

    /// <summary>The most lines ParseClaudeStatusLines keeps: the account block is 3–6 lines; anything past this is not the account.</summary>
    inline constexpr size_t kClaudeStatusMaxLines = 24;

    /// <summary>
    /// Splits Claude Code's "Label: value" text into lines. Blank lines go; a line is a pair when it holds ": " after a label of 1
    /// to 48 characters that starts with a letter and holds no ':' itself (so a URL's "http://" in the VALUE never splits the
    /// label); every other line is kept whole as a label-less line. Whitespace around both parts is trimmed, and only the first
    /// kClaudeStatusMaxLines lines are kept.
    /// </summary>
    /// <param name="text">The command's standard output (CRLF or LF).</param>
    /// <returns>The lines in order; empty for empty text.</returns>
    /// <exception cref="std::bad_alloc">If the lines cannot be allocated.</exception>
    inline std::vector<ClaudeStatusLine> ParseClaudeStatusLines(std::wstring_view text)
    {
        std::vector<ClaudeStatusLine> out;
        const auto trim = [](std::wstring_view s) {
            while (!s.empty() && std::iswspace(s.front()))
            {
                s.remove_prefix(1);
            }
            while (!s.empty() && std::iswspace(s.back()))
            {
                s.remove_suffix(1);
            }
            return s;
        };
        size_t start = 0;
        while (start <= text.size() && out.size() < kClaudeStatusMaxLines)
        {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring_view::npos)
            {
                end = text.size();
            }
            const std::wstring_view line = trim(text.substr(start, end - start));
            start = end + 1;
            if (line.empty())
            {
                if (end == text.size())
                {
                    break;
                }
                continue;
            }
            ClaudeStatusLine row;
            const size_t sep = line.find(L": ");
            const std::wstring_view label = sep == std::wstring_view::npos ? std::wstring_view{} : trim(line.substr(0, sep));
            // A label is short, starts with a letter and holds no ':' of its own; anything else ("Not logged in. Run claude
            // auth login", a stray "x: y: z" line) stays whole, so the reader sees exactly what Claude Code printed.
            if (!label.empty() && label.size() <= 48 && std::iswalpha(label.front()) && label.find(L':') == std::wstring_view::npos)
            {
                row.label = std::wstring{ label };
                row.value = std::wstring{ trim(line.substr(sep + 2)) };
            }
            else
            {
                row.value = std::wstring{ line };
            }
            out.push_back(std::move(row));
            if (end == text.size())
            {
                break;
            }
        }
        return out;
    }

    // ===== the headless /usage answer ==================================================================================

    /// <summary>
    /// What `claude -p /usage --output-format json` answered (ParseClaudeUsageAnswer). The command's whole value is its `result`
    /// TEXT — Claude Code's own wording of the Usage tab — so that is what is kept; `modelTurn` is the alarm that the text went
    /// to the model instead of running as the local command (an incompatible Claude Code: the probe then stops asking it).
    /// </summary>
    struct ClaudeUsageAnswer
    {
        bool ok{ false }; // the local command ran and printed its text
        std::wstring text; // the Usage text (ok), or Claude Code's own error message (is_error)
        std::wstring error; // why it is not ok, in a few words ("" when ok)
        bool modelTurn{ false }; // the answer came from a MODEL turn (local_command missing, or turns taken): never ask this claude again
    };

    /// <summary>
    /// Reads the JSON a `claude -p … --output-format json` run printed for "/usage". The answer is one result object
    /// ({"type":"result","is_error":false,"result":"…","local_command":"usage","num_turns":0,…}); a claude whose settings turn
    /// `verbose` on prints an ARRAY of every message instead, so an array's LAST "result" object is read the same way. It is ok
    /// only when the result says it was the local command "usage", took no model turn, is no error and holds text.
    /// </summary>
    /// <param name="stdoutText">The run's standard output (UTF-16).</param>
    /// <returns>The answer; never throws a parse failure (it reads as not ok, with `error` saying so).</returns>
    /// <exception cref="std::bad_alloc">If the JSON or the text cannot be allocated.</exception>
    inline ClaudeUsageAnswer ParseClaudeUsageAnswer(std::wstring_view stdoutText)
    {
        ClaudeUsageAnswer a;
        // The JSON is the output's last non-blank line in the normal case; the whole output parses in both forms, so the whole
        // output is parsed first and only a refusal tries the last line (a warning a future build prints before it).
        std::optional<json::Value> parsed = json::Parse(stdoutText);
        if (!parsed)
        {
            const size_t last = stdoutText.find_last_not_of(L" \t\r\n");
            if (last != std::wstring_view::npos)
            {
                const size_t lineBreak = stdoutText.find_last_of(L'\n', last);
                const size_t from = lineBreak == std::wstring_view::npos ? 0 : lineBreak + 1;
                parsed = json::Parse(stdoutText.substr(from, last + 1 - from));
            }
        }
        if (!parsed)
        {
            a.error = L"its answer was not JSON";
            return a;
        }
        const json::Value* result = nullptr;
        if (parsed->type == json::Value::Type::Arr)
        {
            for (const auto& item : parsed->arr)
            {
                if (item.type == json::Value::Type::Obj && item.StrAt(L"type") == L"result")
                {
                    result = &item; // the last one wins: a verbose stream ends with its result
                }
            }
        }
        else if (parsed->type == json::Value::Type::Obj)
        {
            result = &*parsed;
        }
        if (!result || result->StrAt(L"type") != L"result")
        {
            a.error = L"its answer held no result";
            return a;
        }
        const std::wstring local = result->StrAt(L"local_command");
        const int64_t turns = result->I64At(L"num_turns", 0);
        a.text = result->StrAt(L"result");
        if (local != L"usage" || turns > 0)
        {
            // The words went to the model (or something other than /usage ran): whatever text came back is not the Usage tab.
            a.modelTurn = true;
            a.text.clear();
            a.error = L"this Claude Code did not run /usage as a local command";
            return a;
        }
        if (result->BoolAt(L"is_error", false))
        {
            a.error = L"Claude Code reported an error";
            return a; // `text` keeps Claude Code's own message (an API-key login has no plan usage, for one)
        }
        if (a.text.find_first_not_of(L" \t\r\n") == std::wstring::npos)
        {
            a.text.clear();
            a.error = L"it printed nothing";
            return a;
        }
        a.ok = true;
        return a;
    }

    // ===== the /usage text ===============================================================================================

    /// <summary>
    /// One plan limit of the /usage text: "Current session: 7% used · resets Oct 26, 10:20pm (Asia/Jerusalem)" →
    /// {"Current session", 7, "resets Oct 26, 10:20pm (Asia/Jerusalem)"}.
    /// </summary>
    struct ClaudeUsageLimit
    {
        std::wstring title; // "Current session", "Current week (all models)", "Current week (Fable)"
        int percent{ 0 }; // 0–100 as Claude Code printed it (a value above 100 is kept: an overage)
        std::wstring tail; // what follows " · " ("resets …"), or ""
    };

    /// <summary>
    /// Reads one line of the /usage text as a plan limit: "&lt;title&gt;: &lt;digits&gt;% used", then optionally " · &lt;tail&gt;".
    /// The title is 1–80 characters with no leading blank (an indented line is a contributions row, not a limit). Any other line
    /// is no limit — the caller keeps it as text, so a wording Claude Code changes later still shows, only without its bar.
    /// </summary>
    /// <param name="line">One line, without its line break.</param>
    /// <returns>The limit, or nullopt.</returns>
    /// <exception cref="std::bad_alloc">If the parts cannot be copied.</exception>
    inline std::optional<ClaudeUsageLimit> ParseClaudeUsageLimitLine(std::wstring_view line)
    {
        if (line.empty() || std::iswspace(line.front()))
        {
            return std::nullopt;
        }
        const size_t colon = line.find(L": ");
        if (colon == std::wstring_view::npos || colon == 0 || colon > 80)
        {
            return std::nullopt;
        }
        size_t i = colon + 2;
        int value = 0;
        size_t digits = 0;
        while (i < line.size() && line[i] >= L'0' && line[i] <= L'9' && digits < 4)
        {
            value = value * 10 + static_cast<int>(line[i] - L'0');
            ++i;
            ++digits;
        }
        constexpr std::wstring_view kUsed = L"% used";
        if (digits == 0 || line.substr(i, kUsed.size()) != kUsed)
        {
            return std::nullopt;
        }
        i += kUsed.size();
        ClaudeUsageLimit limit;
        limit.title = std::wstring{ line.substr(0, colon) };
        limit.percent = value;
        constexpr std::wstring_view kSep = L" \x00B7 ";
        if (line.substr(i, kSep.size()) == kSep)
        {
            limit.tail = std::wstring{ line.substr(i + kSep.size()) };
        }
        else if (i != line.size())
        {
            return std::nullopt; // "7% used by …": a sentence, not the limit row
        }
        return limit;
    }

    /// <summary>
    /// Whether a /usage line opens its "What's contributing to your limits usage?" section (the long, approximate breakdown of the
    /// last 24 hours and 7 days that the report folds away). Read by its first words, with a straight or a curly apostrophe.
    /// </summary>
    /// <param name="line">One line, without its line break.</param>
    /// <returns>True for the section's heading.</returns>
    inline bool IsClaudeUsageContributionsHeading(std::wstring_view line) noexcept
    {
        return line.substr(0, 19) == L"What's contributing" || line.substr(0, 19) == L"What\x2019s contributing";
    }

    // ===== token counts ==================================================================================================

    /// <summary>
    /// A token count in Claude Code's own short form (its Stats tab writes "423.7b", "128.4m"): below 1,000 the number itself,
    /// then one decimal and k / m / b ("312.4k", "1.2m", "4.1b"). A negative count reads as 0.
    /// </summary>
    /// <param name="n">The count.</param>
    /// <returns>The short form.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring FormatClaudeTokenCount(int64_t n)
    {
        if (n < 1000)
        {
            return std::to_wstring(n < 0 ? 0 : n);
        }
        const wchar_t* unit = L"k";
        int64_t scale = 1000;
        if (n >= 1000000000)
        {
            unit = L"b";
            scale = 1000000000;
        }
        else if (n >= 1000000)
        {
            unit = L"m";
            scale = 1000000;
        }
        // Tenths, rounded half up, in integers: no floating-point text formatting, so the same count always reads the same.
        int64_t tenths = (n * 10 + scale / 2) / scale;
        if (tenths >= 10000 && scale < 1000000000)
        {
            // 999,950 rounds to "1000.0k": say "1.0m" instead, as the next unit would.
            tenths = (n * 10 + scale * 500) / (scale * 1000);
            unit = scale == 1000 ? L"m" : L"b";
        }
        return std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + unit;
    }

    /// <summary>
    /// A count with thousands separators ("27,341"), as Claude Code writes request counts. A negative count reads as 0.
    /// </summary>
    /// <param name="n">The count.</param>
    /// <returns>The grouped digits.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring GroupClaudeCount(int64_t n)
    {
        std::wstring digits = std::to_wstring(n < 0 ? 0 : n);
        for (size_t i = digits.size(); i > 3; i -= 3)
        {
            digits.insert(i - 3, 1, L',');
        }
        return digits;
    }

    // ===== MCP tool names ================================================================================================

    /// <summary>
    /// The MCP server of a tool name as Claude Code names MCP tools: "mcp__&lt;server&gt;__&lt;tool&gt;" →
    /// "&lt;server&gt;" ("mcp__catalog__catalog_read" → "catalog", "mcp__claude_ai_Gmail__search" → "claude_ai_Gmail"). Claude
    /// Code normalizes a server name so it holds no "__", so the server ends at the first "__" after the prefix.
    /// </summary>
    /// <param name="toolName">A tool name.</param>
    /// <returns>The server, or "" for a tool that is no MCP tool.</returns>
    /// <exception cref="std::bad_alloc">If the name cannot be copied.</exception>
    inline std::wstring McpServerOfToolName(std::wstring_view toolName)
    {
        constexpr std::wstring_view kPrefix = L"mcp__";
        if (toolName.substr(0, kPrefix.size()) != kPrefix)
        {
            return {};
        }
        const std::wstring_view rest = toolName.substr(kPrefix.size());
        const size_t sep = rest.find(L"__");
        if (sep == 0 || sep == std::wstring_view::npos)
        {
            return {};
        }
        return std::wstring{ rest.substr(0, sep) };
    }

    // ===== the transcript's status facts =================================================================================

    /// <summary>One instruction (memory) file Claude Code loaded: the Status tab's memory files, from the transcript.</summary>
    struct ClaudeInstructionFile
    {
        std::wstring path; // "K:\source\Agentmaster\CLAUDE.md"
        std::wstring type; // Claude Code's kind: "User", "Project", "Local", "AutoMem", "Managed", …
    };

    /// <summary>An MCP server that failed to connect, as the transcript's last tools delta reports it.</summary>
    struct ClaudeMcpFailure
    {
        std::wstring name; // "claude-fleet"
        std::wstring error; // "Version negotiation probe timed out after 5000ms" ("" when none was given)
    };

    /// <summary>
    /// What a session's transcript says about its Claude Code: the newest value of each fact, folded line by line
    /// (ClaudeTranscriptStatusReader). Empty fields were never written (an older Claude Code writes fewer of these lines; a fresh
    /// session has none yet): the report leaves their rows out.
    /// </summary>
    struct ClaudeTranscriptStatus
    {
        std::wstring version; // the Claude Code that wrote the newest message line ("2.1.310")
        std::wstring customTitle; // the newest /rename title ({"type":"custom-title"})
        std::wstring aiTitle; // the newest generated title ({"type":"ai-title"})
        std::wstring modelId; // the newest model attachment's id ("claude-opus-5-5[1m]")
        std::wstring modelName; // ...and its name ("Opus 5.5 (1M context)")
        std::wstring requestedModel; // the newest response's requested model ("claude-opus-5-5[1m]"; it follows a /model switch)
        std::wstring responseModel; // the newest response's model as the API named it ("claude-opus-5-5"; never "<synthetic>")
        std::wstring effort; // the newest response's effort ("max")
        std::wstring permissionMode; // the newest permission-mode line ("bypassPermissions")
        std::vector<ClaudeInstructionFile> instructionFiles; // the instruction files in force (full list, then its updates)
        std::vector<std::wstring> mcpServers; // the MCP servers with tools announced, sorted
        std::vector<std::wstring> mcpPending; // still connecting at the newest delta
        std::vector<std::wstring> mcpNeedsAuth; // waiting for an authorization at the newest delta
        std::vector<ClaudeMcpFailure> mcpFailed; // failed at the newest delta
        bool mcpSeen{ false }; // a tools delta was read at all (false: the MCP rows are unknown, not "none")
        std::vector<std::wstring> extraDirectories; // the extra working folders (/add-dir), from the newest environment attachment
        bool isWorktree{ false }; // the newest environment attachment says the folder is a git worktree
        int64_t responses{ 0 }; // distinct API responses (a response is split over several lines; counted once by its id)
        int64_t inputTokens{ 0 }; // their uncached input tokens
        int64_t outputTokens{ 0 }; // their output tokens
        int64_t cacheReadTokens{ 0 }; // their cache-read input tokens
        int64_t cacheWriteTokens{ 0 }; // their cache-creation input tokens
        int64_t contextTokens{ 0 }; // the newest response's size: input + cache read + cache write + output (the context now)
        int64_t compactions{ 0 }; // compact boundaries passed
        int64_t lines{ 0 }; // lines fed
        int64_t parsedLines{ 0 }; // lines that were parsed (the rest were passed by the filter)
        int64_t unparsedLines{ 0 }; // wanted lines that did not parse (a torn last line while claude writes, for one)
    };

    namespace claude_status_detail
    {
        /// <summary>
        /// Whether `hay` holds the ASCII text `needle`, for either character width (the transcript filter runs on the file's
        /// UTF-8 bytes in the app and on UTF-16 lines in the tests). Every needle here is ASCII, so a byte-for-unit comparison
        /// is exact: a UTF-8 multi-byte sequence never holds an ASCII byte.
        /// </summary>
        /// <typeparam name="Ch">char (UTF-8) or wchar_t (UTF-16).</typeparam>
        /// <param name="hay">The text searched.</param>
        /// <param name="needle">The ASCII text looked for.</param>
        /// <param name="from">Where the search starts.</param>
        /// <returns>The position of the first match, or npos.</returns>
        template<class Ch>
        size_t FindAscii(std::basic_string_view<Ch> hay, std::string_view needle, size_t from = 0) noexcept
        {
            if (needle.empty() || hay.size() < needle.size())
            {
                return std::string_view::npos;
            }
            // Both sides as `unsigned`: a UTF-8 lead or continuation byte is a NEGATIVE char, which becomes a value above 0xFF
            // here and so can never equal an ASCII needle byte (and no signed/unsigned comparison is made).
            const unsigned first = static_cast<unsigned char>(needle[0]);
            for (size_t i = from; i + needle.size() <= hay.size(); ++i)
            {
                if (static_cast<unsigned>(hay[i]) != first)
                {
                    continue;
                }
                size_t k = 1;
                while (k < needle.size())
                {
                    const unsigned want = static_cast<unsigned char>(needle[k]);
                    if (static_cast<unsigned>(hay[i + k]) != want)
                    {
                        break;
                    }
                    ++k;
                }
                if (k == needle.size())
                {
                    return i;
                }
            }
            return std::string_view::npos;
        }

        /// <summary>The attachment types the status reader folds (every other attachment is passed without parsing).</summary>
        inline constexpr std::string_view kStatusAttachmentTypes[] = {
            "\"attachment\":{\"type\":\"model\"",
            "\"attachment\":{\"type\":\"instructions\"",
            "\"attachment\":{\"type\":\"deferred_tools_delta\"",
            "\"attachment\":{\"type\":\"environment\"",
        };
    }

    /// <summary>
    /// The CHEAP filter in front of the transcript reader: whether a line can carry a status fact at all, decided on its raw text
    /// without parsing it. A transcript is mostly tool results and message bodies; only these lines are parsed: the response
    /// lines ("type":"assistant" — their tokens, model and effort), the four attachments the status folds, the permission-mode and
    /// title lines, and the compact boundaries (which reset the MCP fold). The raw text can be trusted for this: inside a JSON
    /// string every '"' is escaped, so `"type":"assistant"` can only appear as a real key and value — a false match (the same pair
    /// in a nested object of another line) only costs a parse, which ClaudeTranscriptStatusReader::Feed then ignores by the
    /// line's own top-level type.
    /// </summary>
    /// <typeparam name="Ch">char (the file's UTF-8 bytes) or wchar_t (a decoded line).</typeparam>
    /// <param name="line">One transcript line.</param>
    /// <returns>True when the line must be parsed.</returns>
    template<class Ch>
    bool ClaudeTranscriptStatusLineWanted(std::basic_string_view<Ch> line) noexcept
    {
        // These three lines START with their type in 2.1.310 ({"type":"permission-mode",…}); they are searched anywhere in the
        // line all the same, so a later Claude Code that orders the keys differently still has them read.
        if (claude_status_detail::FindAscii(line, "\"type\":\"permission-mode\"") != std::string_view::npos ||
            claude_status_detail::FindAscii(line, "\"type\":\"custom-title\"") != std::string_view::npos || claude_status_detail::FindAscii(line, "\"type\":\"ai-title\"") != std::string_view::npos)
        {
            return true;
        }
        if (claude_status_detail::FindAscii(line, "\"type\":\"assistant\"") != std::string_view::npos)
        {
            return true;
        }
        for (const std::string_view type : claude_status_detail::kStatusAttachmentTypes)
        {
            if (claude_status_detail::FindAscii(line, type) != std::string_view::npos)
            {
                return true;
            }
        }
        return claude_status_detail::FindAscii(line, "\"subtype\":\"compact_boundary\"") != std::string_view::npos;
    }

    /// <summary>
    /// Folds a Claude Code transcript, line by line in file order, into the session's status facts (ClaudeTranscriptStatus). The
    /// rules follow what Claude Code itself does with these lines (read in the 2.1.310 bundle and checked on real transcripts):
    ///   * a response is written as several lines (one per content block) that repeat ONE usage; its tokens count once, by the
    ///     message id (a line with no id counts on its own);
    ///   * the MCP tools are a fold of the "deferred_tools_delta" attachments (added names in, removed names out), reset at each
    ///     compact boundary — after one, Claude Code announces every tool again; the pending / needs-auth / failed lists are
    ///     the newest delta that carries each;
    ///   * the "instructions" attachment without a "changed" key lists every instruction file (it replaces the list); one WITH it
    ///     updates the files it names;
    ///   * every other fact is the newest line that carries it.
    /// Feed only whole lines (no line break). Not thread-safe: one reader per read.
    /// </summary>
    class ClaudeTranscriptStatusReader
    {
    public:
        /// <summary>
        /// Folds one transcript line. A line the filter passes, or that does not parse, changes nothing but the counters; a torn
        /// last line (claude writing it right now) counts as unparsed and is otherwise ignored.
        /// </summary>
        /// <param name="line">One line, UTF-16, without its line break.</param>
        /// <exception cref="std::bad_alloc">If the parse or the fold cannot allocate.</exception>
        void Feed(std::wstring_view line)
        {
            ++_status.lines;
            if (!ClaudeTranscriptStatusLineWanted(line))
            {
                return;
            }
            const std::optional<json::Value> parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                ++_status.unparsedLines;
                return;
            }
            ++_status.parsedLines;
            const json::Value& v = *parsed;
            const std::wstring type = v.StrAt(L"type");
            if (type == L"assistant")
            {
                _foldResponse(v);
            }
            else if (type == L"attachment")
            {
                if (const json::Value* a = v.Find(L"attachment"); a && a->type == json::Value::Type::Obj)
                {
                    _foldAttachment(*a);
                }
            }
            else if (type == L"permission-mode")
            {
                _setIfPresent(_status.permissionMode, v.StrAt(L"permissionMode"));
            }
            else if (type == L"custom-title")
            {
                _setIfPresent(_status.customTitle, v.StrAt(L"customTitle"));
            }
            else if (type == L"ai-title")
            {
                _setIfPresent(_status.aiTitle, v.StrAt(L"aiTitle"));
            }
            else if (type == L"system" && v.StrAt(L"subtype") == L"compact_boundary")
            {
                // After a compaction Claude Code announces every tool again from nothing (it folds only the messages after the
                // boundary), so the fold restarts with it; a server that never comes back is gone from the session's context.
                ++_status.compactions;
                _mcpTools.clear();
            }
        }

        /// <summary>
        /// Counts a line the caller's own filter passed without decoding it (ReadClaudeTranscriptStatusFile filters the raw UTF-8
        /// bytes, so most lines never become UTF-16): `lines` then still counts every line of the file. Never throws.
        /// </summary>
        void NotePassedLine() noexcept
        {
            ++_status.lines;
        }

        /// <summary>
        /// The facts folded so far. The MCP server list is derived here from the tool fold (sorted, each server once).
        /// </summary>
        /// <returns>A copy of the facts.</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        ClaudeTranscriptStatus Result() const
        {
            ClaudeTranscriptStatus out = _status;
            std::set<std::wstring> servers;
            for (const auto& tool : _mcpTools)
            {
                if (std::wstring server = McpServerOfToolName(tool); !server.empty())
                {
                    servers.insert(std::move(server));
                }
            }
            out.mcpServers.assign(servers.begin(), servers.end());
            return out;
        }

    private:
        /// <summary>Overwrites `field` with `value` unless the value is empty (a line that names nothing keeps the older value).</summary>
        /// <param name="field">The fact.</param>
        /// <param name="value">The newer value.</param>
        static void _setIfPresent(std::wstring& field, std::wstring value)
        {
            if (!value.empty())
            {
                field = std::move(value);
            }
        }

        /// <summary>The strings of a JSON array member (non-strings skipped); empty when the member is missing or no array.</summary>
        /// <param name="obj">The object.</param>
        /// <param name="key">The member.</param>
        /// <returns>The strings.</returns>
        static std::vector<std::wstring> _strings(const json::Value& obj, std::wstring_view key)
        {
            std::vector<std::wstring> out;
            if (const json::Value* arr = obj.Find(key); arr && arr->type == json::Value::Type::Arr)
            {
                for (const auto& item : arr->arr)
                {
                    if (item.type == json::Value::Type::Str)
                    {
                        out.push_back(item.str);
                    }
                    else if (item.type == json::Value::Type::Obj && !item.StrAt(L"name").empty())
                    {
                        out.push_back(item.StrAt(L"name")); // a server named by an object ({"name": …})
                    }
                }
            }
            return out;
        }

        /// <summary>Whether the object carries `key` as an ARRAY (Claude Code sets a list only when the delta carries one).</summary>
        /// <param name="obj">The object.</param>
        /// <param name="key">The member.</param>
        /// <returns>True for an array member.</returns>
        static bool _hasArray(const json::Value& obj, std::wstring_view key) noexcept
        {
            const json::Value* m = obj.Find(key);
            return m && m->type == json::Value::Type::Arr;
        }

        /// <summary>
        /// Folds one response line: the version, the requested model and effort, the API's model, and — once per message id —
        /// the tokens; the context is the newest line's usage sum. A SYNTHETIC line (Claude Code's own placeholder for an API
        /// error or a cut-off request, model "&lt;synthetic&gt;") is no response of the API: it gives the version only.
        /// </summary>
        /// <param name="v">The parsed line.</param>
        void _foldResponse(const json::Value& v)
        {
            _setIfPresent(_status.version, v.StrAt(L"version"));
            const json::Value* message = v.Find(L"message");
            if (!message || message->type != json::Value::Type::Obj)
            {
                return;
            }
            const std::wstring model = message->StrAt(L"model");
            if (model == L"<synthetic>")
            {
                return; // no model answered, nothing was billed: neither a response nor a context size
            }
            _setIfPresent(_status.requestedModel, v.StrAt(L"requestedModel"));
            _setIfPresent(_status.effort, v.StrAt(L"effort"));
            if (!model.empty())
            {
                _status.responseModel = model;
            }
            const json::Value* usage = message->Find(L"usage");
            if (!usage || usage->type != json::Value::Type::Obj)
            {
                return;
            }
            const int64_t in = usage->I64At(L"input_tokens", 0);
            const int64_t out = usage->I64At(L"output_tokens", 0);
            const int64_t read = usage->I64At(L"cache_read_input_tokens", 0);
            const int64_t write = usage->I64At(L"cache_creation_input_tokens", 0);
            if (in + out + read + write > 0)
            {
                // The newest response's size is the context NOW (the same sum SessionScanner keeps as contextTokens). A usage of
                // all zeros (a response a gateway reported without counts) is no size, so it never resets the context to 0.
                _status.contextTokens = in + read + write + out;
            }
            const std::wstring id = message->StrAt(L"id");
            if (!id.empty() && !_responseIds.insert(id).second)
            {
                return; // another content block of a response already counted
            }
            ++_status.responses;
            _status.inputTokens += in;
            _status.outputTokens += out;
            _status.cacheReadTokens += read;
            _status.cacheWriteTokens += write;
        }

        /// <summary>
        /// Folds one of the four attachments the status reads (the model, the instruction files, the tools delta, the environment);
        /// every other attachment type is ignored.
        /// </summary>
        /// <param name="a">The attachment object.</param>
        void _foldAttachment(const json::Value& a)
        {
            const std::wstring kind = a.StrAt(L"type");
            if (kind == L"model")
            {
                if (const json::Value* identity = a.Find(L"identity"); identity && identity->type == json::Value::Type::Obj)
                {
                    _setIfPresent(_status.modelId, identity->StrAt(L"modelId"));
                    _setIfPresent(_status.modelName, identity->StrAt(L"marketingName"));
                }
            }
            else if (kind == L"instructions")
            {
                _foldInstructions(a);
            }
            else if (kind == L"deferred_tools_delta")
            {
                _status.mcpSeen = true;
                for (auto& name : _strings(a, L"addedNames"))
                {
                    if (!McpServerOfToolName(name).empty())
                    {
                        _mcpTools.insert(std::move(name));
                    }
                }
                for (const auto& name : _strings(a, L"removedNames"))
                {
                    _mcpTools.erase(name);
                }
                if (_hasArray(a, L"pendingMcpServers"))
                {
                    _status.mcpPending = _strings(a, L"pendingMcpServers");
                }
                if (_hasArray(a, L"needsAuthMcpServers"))
                {
                    _status.mcpNeedsAuth = _strings(a, L"needsAuthMcpServers");
                }
                if (_hasArray(a, L"failedMcpServers"))
                {
                    _status.mcpFailed.clear();
                    for (const auto& item : a.Find(L"failedMcpServers")->arr)
                    {
                        ClaudeMcpFailure f;
                        if (item.type == json::Value::Type::Obj)
                        {
                            f.name = item.StrAt(L"name");
                            f.error = item.StrAt(L"error");
                        }
                        else if (item.type == json::Value::Type::Str)
                        {
                            f.name = item.str;
                        }
                        if (!f.name.empty())
                        {
                            _status.mcpFailed.push_back(std::move(f));
                        }
                    }
                }
            }
            else if (kind == L"environment")
            {
                if (const json::Value* snap = a.Find(L"snapshot"); snap && snap->type == json::Value::Type::Obj)
                {
                    _status.extraDirectories = _strings(*snap, L"additionalWorkingDirectories");
                    _status.isWorktree = snap->BoolAt(L"isWorktree", false);
                }
            }
        }

        /// <summary>
        /// Folds an "instructions" attachment: without a "changed" key it is the whole list (it replaces the fold); with one it
        /// names only the files that changed (each replaces its path's entry, or joins the list).
        /// </summary>
        /// <param name="a">The attachment object.</param>
        void _foldInstructions(const json::Value& a)
        {
            const json::Value* files = a.Find(L"files");
            if (!files || files->type != json::Value::Type::Arr)
            {
                return;
            }
            std::vector<ClaudeInstructionFile> named;
            for (const auto& f : files->arr)
            {
                if (f.type == json::Value::Type::Obj && !f.StrAt(L"path").empty())
                {
                    named.push_back(ClaudeInstructionFile{ f.StrAt(L"path"), f.StrAt(L"type") });
                }
            }
            if (!a.Find(L"changed"))
            {
                _status.instructionFiles = std::move(named);
                return;
            }
            for (auto& f : named)
            {
                bool replaced = false;
                for (auto& existing : _status.instructionFiles)
                {
                    if (existing.path == f.path)
                    {
                        existing = f;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                {
                    _status.instructionFiles.push_back(std::move(f));
                }
            }
        }

        ClaudeTranscriptStatus _status; // the facts so far (the MCP servers are derived in Result)
        std::set<std::wstring> _mcpTools; // the MCP tools announced and not removed since the last compact boundary
        std::unordered_set<std::wstring> _responseIds; // the message ids whose tokens were counted
    };

    // ===== the OS half (ClaudeStatus.cpp) ================================================================================

    /// <summary>
    /// What reading the account lines produced (ClaudeStatusProbe::Account): `claude auth status --text`, Claude Code's own words
    /// for the Status tab's account rows.
    /// </summary>
    struct ClaudeAccountRead
    {
        bool ok{ false }; // the command ran and printed lines
        std::vector<ClaudeStatusLine> lines; // the lines (ok)
        std::wstring error; // why there are none ("" when ok)
        int64_t readAtMs{ 0 }; // when the lines were read (Unix ms; 0 = never)
        bool cached{ false }; // answered from the cache, not a new run
    };

    /// <summary>
    /// What reading the plan usage produced (ClaudeStatusProbe::Usage): `claude -p /usage`, Claude Code's Usage tab as text.
    /// </summary>
    struct ClaudeUsageRead
    {
        bool ok{ false }; // the text is the Usage tab's
        std::wstring text; // the Usage text (ok), or Claude Code's own error message
        std::wstring error; // why there is no usage ("" when ok)
        int64_t readAtMs{ 0 }; // when it was read (Unix ms; 0 = never)
        bool cached{ false }; // answered from the cache, not a new run
    };

    /// <summary>
    /// The output of one short-lived child process (RunClaudeCapture): whether it started and ended, its exit code, and its two
    /// output streams as text.
    /// </summary>
    struct ClaudeCaptureResult
    {
        bool started{ false }; // CreateProcessW succeeded
        bool timedOut{ false }; // it ran past its time and was ended (with everything it started)
        uint32_t exitCode{ 0 }; // its exit code (only when it ended by itself)
        std::wstring out; // its standard output, decoded from UTF-8 (cut at kClaudeCaptureMaxBytes)
        std::wstring err; // its standard error, likewise
        std::wstring error; // why it did not start or run ("" otherwise)
        int64_t tookMs{ 0 }; // from the start to the end (or to the kill)
    };

    /// <summary>The most bytes RunClaudeCapture keeps of each stream: the answers are a few KB; more is a runaway, and is cut.</summary>
    inline constexpr size_t kClaudeCaptureMaxBytes = 1024 * 1024;

    /// <summary>
    /// Runs `exe` with `args` as a hidden child and collects its output (ClaudeStatus.cpp): the environment is Agentmaster's own
    /// minus every variable of a parent Claude Code session or terminal tab (BuildHarnessEnvironment's list) and with
    /// AM_HARNESS=claude-status — so the Fleet Observer and the CLI skip it — plus DISABLE_AUTOUPDATER=1 (a child must never
    /// update the user's claude.exe) and CLAUDE_CODE_DISABLE_AUTO_MEMORY=1 (without it a headless run leaves an empty
    /// projects\&lt;folder&gt;\memory\ behind); standard input is NUL (an open, silent stdin costs a headless claude 3 s); the
    /// child runs in a kill-on-close job, so a timeout — or Agentmaster ending — ends it with everything it started.
    /// </summary>
    /// <param name="exe">The executable's full path (the engine's claude.exe).</param>
    /// <param name="args">The arguments, already quoted for a command line.</param>
    /// <param name="workDir">The child's working folder ("" = Agentmaster's own).</param>
    /// <param name="timeoutMs">How long it may run before it is ended.</param>
    /// <returns>What happened; never throws (a failure is in `error`).</returns>
    ClaudeCaptureResult RunClaudeCapture(const std::wstring& exe, const std::wstring& args, const std::wstring& workDir, uint32_t timeoutMs) noexcept;

    /// <summary>
    /// Reads a transcript FILE into its status facts: the file is read in chunks while claude may append to it (shared read),
    /// split into lines, each line filtered on its raw UTF-8 bytes (ClaudeTranscriptStatusLineWanted) and only a wanted line
    /// decoded and folded (ClaudeTranscriptStatusReader) — so a 100 MB transcript of tool output costs a read, not a parse.
    /// </summary>
    /// <param name="path">The transcript's path.</param>
    /// <param name="out">Receives the facts.</param>
    /// <param name="error">Receives why the file could not be read ("" on success).</param>
    /// <returns>True when the file was read (to its end at the moment of the read).</returns>
    bool ReadClaudeTranscriptStatusFile(const std::wstring& path, ClaudeTranscriptStatus& out, std::wstring& error) noexcept;

    /// <summary>
    /// The process-wide reader of Claude Code's ACCOUNT and PLAN USAGE (both are the account's, not a session's, so one cache
    /// serves every topic and every window). Each read runs Claude Code's own headless command (RunClaudeCapture) at most once at
    /// a time — a second caller waits for the run in flight instead of starting another — and keeps its answer for a while:
    /// the account kClaudeAccountCacheMs, the usage kClaudeUsageCacheMs, so a burst of /status costs one run. The usage is also
    /// never asked of a claude older than the headless /usage gate (ClaudeVersionRunsHeadlessUsage), nor ever again of a claude
    /// whose answer was a model turn (its path, size and write time are remembered for the process's life).
    /// Thread-safe; every method blocks while its command runs, so call it from a worker thread, never a UI or poll thread.
    /// </summary>
    class ClaudeStatusProbe
    {
    public:
        /// <summary>The account lines (auth status). Any thread but a UI one; blocks up to kClaudeAccountTimeoutMs.</summary>
        /// <param name="claudeExe">The claude.exe to run ("" = Claude Code is not found: an answer that says so).</param>
        /// <returns>The lines, or why there are none.</returns>
        /// <exception cref="std::bad_alloc">If the answer cannot be copied.</exception>
        ClaudeAccountRead Account(const std::wstring& claudeExe);

        /// <summary>The plan usage (the headless /usage). Any thread but a UI one; blocks up to kClaudeUsageTimeoutMs.</summary>
        /// <param name="claudeExe">The claude.exe to run ("" = Claude Code is not found: an answer that says so).</param>
        /// <returns>The usage text, or why there is none.</returns>
        /// <exception cref="std::bad_alloc">If the answer cannot be copied.</exception>
        ClaudeUsageRead Usage(const std::wstring& claudeExe);

        /// <summary>
        /// Claims one of kClaudeStatusMaxReports report slots (the Telegram service's report workers): false when that many are
        /// running, and nothing is claimed. Pair every true with EndReport.
        /// </summary>
        /// <returns>True when a slot was claimed.</returns>
        bool TryBeginReport() noexcept;

        /// <summary>Gives back a slot TryBeginReport claimed. Never throws.</summary>
        void EndReport() noexcept;

    private:
        /// <summary>One cached answer of one command, and the single-flight state of its runs.</summary>
        template<class Read>
        struct Slot
        {
            bool running{ false }; // a run is in flight (the other callers wait for it)
            uint64_t serial{ 0 }; // runs finished so far (a waiter knows its run ended when this moves)
            Read last; // its last answer (readAtMs == 0: none yet)
            int64_t lastTick{ 0 }; // GetTickCount64 when `last` was taken (the cache age, immune to clock changes)
        };

        /// <summary>
        /// The single flight + cache shared by Account and Usage (ClaudeStatus.cpp; instantiated only there): a GOOD answer younger
        /// than `cacheMs` is returned as is (marked cached); a run already in flight is waited for (at most `runMs` + 15 s) and its
        /// answer returned, good or bad; otherwise `run` runs OUTSIDE the lock and its answer is stored and returned. A failed answer
        /// is handed to the callers that waited for it but never reused by a later call, so a timeout is retried at the next ask.
        /// </summary>
        /// <typeparam name="Read">ClaudeAccountRead or ClaudeUsageRead (ok, error, readAtMs, cached).</typeparam>
        /// <typeparam name="Run">A callable returning a Read; it may lock `_mtx` itself (it runs unlocked).</typeparam>
        /// <param name="slot">The command's slot.</param>
        /// <param name="cacheMs">How long a good answer is reused.</param>
        /// <param name="runMs">The command's own time limit (bounds a waiter's wait).</param>
        /// <param name="run">Runs the command once.</param>
        /// <returns>The answer.</returns>
        /// <exception cref="std::bad_alloc">If an answer cannot be copied (a failed run is contained and reads as an error).</exception>
        /// <exception cref="std::system_error">From the lock (not expected).</exception>
        template<class Read, class Run>
        Read _read(Slot<Read>& slot, int64_t cacheMs, uint32_t runMs, Run&& run);

        std::mutex _mtx; // guards both slots and the latch
        std::condition_variable _done; // a run ended (its waiters read the slot again)
        Slot<ClaudeAccountRead> _account;
        Slot<ClaudeUsageRead> _usage;
        std::wstring _usageRefusedExe; // "<path>|<size>|<write time>" of a claude whose /usage was a model turn ("" = none)
        int _reports{ 0 }; // report slots in use
    };

    /// <summary>How long the account lines are reused before auth status runs again (10 minutes: the account changes at a login).</summary>
    inline constexpr int64_t kClaudeAccountCacheMs = 10 * 60 * 1000;
    /// <summary>How long the plan usage is reused before /usage runs again (60 s: a burst of /status costs one ~6 s run).</summary>
    inline constexpr int64_t kClaudeUsageCacheMs = 60 * 1000;
    /// <summary>How long auth status may run (it takes ~0.5 s; a stuck one must not hold a report).</summary>
    inline constexpr uint32_t kClaudeAccountTimeoutMs = 20 * 1000;
    /// <summary>How long the headless /usage may run (~6 s measured; its "What's contributing" scans this PC's recent sessions).</summary>
    inline constexpr uint32_t kClaudeUsageTimeoutMs = 60 * 1000;
    /// <summary>How many status reports may be read at once (each holds a thread and may read a large transcript).</summary>
    inline constexpr int kClaudeStatusMaxReports = 2;

    /// <summary>
    /// The process's ONE probe (created on first use, never destroyed: a report worker may still hold it when the app quits).
    /// </summary>
    /// <returns>The probe.</returns>
    /// <exception cref="std::bad_alloc">On the first call only, if the probe cannot be allocated (a later call then tries again).</exception>
    ClaudeStatusProbe& SharedClaudeStatusProbe();
}
