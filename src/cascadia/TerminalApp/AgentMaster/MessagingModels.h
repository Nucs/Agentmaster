// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MessagingModels: the PURE core of the messaging feature (MESSAGING.md — the fleet
// in Telegram). Everything here is a decision or a transformation with no I/O: what a transcript
// line means for a topic (ClassifyTranscriptLine), which topics must exist for which tabs
// (PlanTopicSync), what a typed `/command` is (ParseRemoteCommand), the state file the connector
// keeps (MessagingState), the card's text (BuildCardText), the backlog rule. The service
// (Messaging.cpp) does the I/O around these; the m5 harness pins them (tests_messaging.cpp).
//
// Header-only and WinRT-free, like PendingInput.h / PromptAnchor.h. It includes SessionModels.h
// (SessionInfo, SessionState) and TranscriptStore.h (IsNoiseUserPrompt — a .cpp function the
// harness and the app both link), nothing heavier.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Json.h"
#include "ProcessInspect.h" // NormalizeRecapText (the one away_summary normalizer every recap reader shares)
#include "SessionModels.h"
#include "TelegramFormat.h"
#include "TranscriptStore.h" // IsNoiseUserPrompt, PickDisplayTitle
#include "TranscriptTurns.h" // ParseTranscriptJsonl (SplitHistoryWindow)

namespace Agentmaster::Msg
{
    // ===== what a transcript line means for a topic ===========================================

    // One thing a topic shows (or the service acts on), read out of one transcript line. A line can
    // yield several (an assistant line holding a text block AND a tool_use block).
    enum class TopicItemKind
    {
        None, // the line shows nothing (bookkeeping, a meta line, a tool-result of no interest)
        Prompt, // the user's message that starts a turn (or one typed while it ran: midTurn)
        Text, // an assistant text block: a REMARK when a tool call follows it, the FINAL reply when the turn ends after it
        ToolCall, // an assistant tool_use block (name + a one-line summary of its input)
        ToolResult, // the user line that returns a tool's result (only its error flag + head are kept)
        Question, // an AskUserQuestion tool_use: the question(s) with their options
        Plan, // an ExitPlanMode tool_use: the plan text, waiting for approval
        Compaction, // a system/compact_boundary line
        Interrupt, // the "[Request interrupted by user…]" marker: the turn was stopped
        ApiError, // the synthetic isApiErrorMessage assistant line: the turn died
        Command, // a typed slash command's echo (<command-name>)
        Recap, // Claude Code's idle recap (a system/away_summary line): shown in the sticky card while it is fresh
        Answer, // the user's answer to a question (an AskUserQuestion tool result with `toolUseResult.answers`): the user's message
    };

    struct TopicItem
    {
        TopicItemKind kind{ TopicItemKind::None };
        std::wstring text; // Prompt / Text / ApiError / Compaction note / Command line / Question text / Plan text / ToolResult head
        std::wstring toolName; // ToolCall / ToolResult / Question / Plan
        std::wstring toolUseId; // ToolCall / ToolResult / Question / Plan (the tool_use id, to pair a result with its call)
        std::wstring summary; // ToolCall: the one-line summary ("Bash: npm test")
        std::wstring messageId; // assistant items: message.id (the lines of one API message share it)
        std::wstring stopReason; // assistant items: message.stop_reason ("end_turn", "tool_use", "" when absent)
        bool turnEnds{ false }; // assistant items: the stop reason ends the turn (IsTerminalStopReason's rule)
        bool isError{ false }; // ToolResult: is_error
        bool midTurn{ false }; // Prompt: typed while the turn ran (a queued_command attachment)
        bool multiSelect{ false }; // Question
        std::vector<std::wstring> options; // Question: the option labels (of the FIRST question when several)
        int64_t tsMs{ 0 }; // the line's timestamp, unix ms (0 when absent)
    };

    // Telegram shows a tool call as "Name: summary" in the work log. Summaries are capped here.
    inline constexpr size_t kToolSummaryMaxChars = 120;

    namespace detail
    {
        inline std::wstring Lower(std::wstring s)
        {
            for (auto& c : s)
            {
                c = static_cast<wchar_t>(::towlower(c));
            }
            return s;
        }

        // The last `keep` path segments ("TerminalApp/AgentMaster/Messaging.cpp" for keep=3), with the
        // separators normalized to '/'. A short path is returned whole.
        inline std::wstring ShortPath(std::wstring_view path, size_t keep = 3)
        {
            std::wstring p{ path };
            for (auto& c : p)
            {
                if (c == L'\\')
                {
                    c = L'/';
                }
            }
            while (!p.empty() && p.back() == L'/')
            {
                p.pop_back();
            }
            size_t pos = p.size();
            for (size_t i = 0; i < keep; ++i)
            {
                const size_t slash = pos == 0 ? std::wstring::npos : p.rfind(L'/', pos - 1);
                if (slash == std::wstring::npos)
                {
                    return p;
                }
                pos = slash;
            }
            return L"\x2026" + p.substr(pos);
        }

        // ISO-8601 "2026-10-16T07:29:14.217Z" -> unix ms (UTC); 0 when malformed. Only the shape Claude
        // Code writes (an explicit Z or no zone; both read as UTC).
        inline int64_t IsoToUnixMs(std::wstring_view ts) noexcept
        {
            if (ts.size() < 19)
            {
                return 0;
            }
            const auto num = [&](size_t at, size_t len) -> int {
                int v = 0;
                for (size_t i = 0; i < len; ++i)
                {
                    const wchar_t c = at + i < ts.size() ? ts[at + i] : L'x';
                    if (c < L'0' || c > L'9')
                    {
                        return -1;
                    }
                    v = v * 10 + (c - L'0');
                }
                return v;
            };
            const int Y = num(0, 4), M = num(5, 2), D = num(8, 2), h = num(11, 2), m = num(14, 2), s = num(17, 2);
            if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || m < 0 || s < 0)
            {
                return 0;
            }
            int ms = 0;
            if (ts.size() > 20 && ts[19] == L'.')
            {
                int digits = 0;
                size_t i = 20;
                while (i < ts.size() && ts[i] >= L'0' && ts[i] <= L'9' && digits < 3)
                {
                    ms = ms * 10 + (ts[i] - L'0');
                    ++digits;
                    ++i;
                }
                while (digits < 3)
                {
                    ms *= 10;
                    ++digits;
                }
            }
            // Days since the epoch (a civil-from-days inverse; Howard Hinnant's algorithm).
            const int y = Y - (M <= 2 ? 1 : 0);
            const int era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153u * static_cast<unsigned>(M + (M > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(D) - 1u;
            const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
            const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
            return ((days * 86400 + h * 3600 + m * 60 + s) * 1000) + ms;
        }

        // A content value's TEXT: a string as is; an array of blocks -> its text blocks joined; else "".
        inline std::wstring ContentText(const json::Value* content)
        {
            if (!content)
            {
                return {};
            }
            if (content->type == json::Value::Type::Str)
            {
                return content->str;
            }
            if (content->type != json::Value::Type::Arr)
            {
                return {};
            }
            std::wstring out;
            for (const auto& b : content->arr)
            {
                if (b.type == json::Value::Type::Obj && b.StrAt(L"type") == L"text")
                {
                    if (!out.empty())
                    {
                        out += L'\n';
                    }
                    out += b.StrAt(L"text");
                }
            }
            return out;
        }

        inline bool IsInterruptMarker(std::wstring_view text) noexcept
        {
            return text.rfind(L"[Request interrupted by user", 0) == 0;
        }

        inline bool IsTerminalStop(std::wstring_view reason) noexcept
        {
            return reason == L"end_turn" || reason == L"stop_sequence" || reason == L"max_tokens" || reason == L"refusal";
        }

        // "<command-name>/x</command-name>" + "<command-args>…</command-args>" -> "/x args" ("" when the
        // text is not a command echo).
        inline std::wstring CommandEchoLine(std::wstring_view text)
        {
            const size_t a = text.find(L"<command-name>");
            if (a == std::wstring_view::npos)
            {
                return {};
            }
            const size_t b = text.find(L"</command-name>", a);
            if (b == std::wstring_view::npos)
            {
                return {};
            }
            std::wstring name{ text.substr(a + 14, b - a - 14) };
            const size_t ns = name.find_first_not_of(L" \t\r\n");
            name = ns == std::wstring::npos ? std::wstring{} : name.substr(ns);
            while (!name.empty() && (name.back() == L' ' || name.back() == L'\n' || name.back() == L'\r'))
            {
                name.pop_back();
            }
            if (name.empty())
            {
                return {};
            }
            if (name.front() != L'/')
            {
                name.insert(name.begin(), L'/');
            }
            std::wstring args;
            const size_t c = text.find(L"<command-args>");
            if (c != std::wstring_view::npos)
            {
                const size_t d = text.find(L"</command-args>", c);
                if (d != std::wstring_view::npos)
                {
                    args = std::wstring{ text.substr(c + 14, d - c - 14) };
                    const size_t as = args.find_first_not_of(L" \t\r\n");
                    args = as == std::wstring::npos ? std::wstring{} : args.substr(as);
                    while (!args.empty() && (args.back() == L' ' || args.back() == L'\n' || args.back() == L'\r'))
                    {
                        args.pop_back();
                    }
                }
            }
            return args.empty() ? name : name + L" " + args;
        }
    }

    /// <summary>
    /// One line for a tool call in the work log: the tool's name and the one argument that says what
    /// it did ("Bash: npm test", "Edit: …/AgentMaster/Messaging.cpp", "Grep: TODO in src"). An MCP tool
    /// reads "server · tool: first argument".
    /// </summary>
    /// <param name="name">The tool_use block's name.</param>
    /// <param name="input">The tool_use block's input object (nullptr when absent).</param>
    /// <returns>The summary, capped at kToolSummaryMaxChars.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring ToolCallSummary(std::wstring_view name, const json::Value* input)
    {
        const auto str = [&](std::wstring_view key) -> std::wstring {
            return input ? input->StrAt(key) : std::wstring{};
        };
        std::wstring label{ name };
        std::wstring arg;
        if (name == L"Bash")
        {
            // WHY the command runs comes first (§21.6, the user's "including the message of why the bash is
            // invoked"): Claude Code's Bash input carries a short `description` written for exactly that; the
            // command itself follows in parentheses, cut to fit. A call without a description shows the command.
            const std::wstring description = TgFirstLine(str(L"description"), kToolSummaryMaxChars);
            const std::wstring command = TgFirstLine(str(L"command"), kToolSummaryMaxChars);
            if (description.empty())
            {
                arg = command;
            }
            else
            {
                arg = description;
                // "Bash: " + description + " ($ " + command + ")" within the cap; the command is the part cut.
                const size_t used = label.size() + 2 + description.size() + 4 + 1;
                if (!command.empty() && used + 8 < kToolSummaryMaxChars)
                {
                    const size_t room = kToolSummaryMaxChars - used;
                    arg += L" ($ " + (command.size() > room ? command.substr(0, room - 1) + L"\x2026" : command) + L")";
                }
            }
        }
        else if (name == L"Read" || name == L"Edit" || name == L"Write" || name == L"MultiEdit" || name == L"NotebookEdit")
        {
            std::wstring p = str(L"file_path");
            if (p.empty())
            {
                p = str(L"notebook_path");
            }
            arg = detail::ShortPath(p);
        }
        else if (name == L"Grep")
        {
            arg = str(L"pattern");
            if (const auto p = str(L"path"); !p.empty())
            {
                arg += L" in " + detail::ShortPath(p, 2);
            }
        }
        else if (name == L"Glob")
        {
            arg = str(L"pattern");
        }
        else if (name == L"WebFetch")
        {
            arg = str(L"url");
        }
        else if (name == L"WebSearch")
        {
            arg = str(L"query");
        }
        else if (name == L"Task" || name == L"Agent")
        {
            arg = str(L"description");
            if (arg.empty())
            {
                arg = TgFirstLine(str(L"prompt"), 80);
            }
            if (const auto t = str(L"subagent_type"); !t.empty())
            {
                label += L" (" + t + L")";
            }
        }
        else if (name == L"Skill")
        {
            arg = str(L"skill");
        }
        else if (name == L"TodoWrite")
        {
            size_t n = 0, done = 0;
            if (input)
            {
                if (const auto* todos = input->Find(L"todos"); todos && todos->type == json::Value::Type::Arr)
                {
                    n = todos->arr.size();
                    for (const auto& t : todos->arr)
                    {
                        if (t.type == json::Value::Type::Obj && t.StrAt(L"status") == L"completed")
                        {
                            ++done;
                        }
                    }
                }
            }
            arg = std::to_wstring(done) + L"/" + std::to_wstring(n) + L" done";
        }
        else if (name.rfind(L"mcp__", 0) == 0)
        {
            // mcp__<server>__<tool> -> "server · tool"
            const std::wstring_view rest = name.substr(5);
            const size_t sep = rest.find(L"__");
            if (sep != std::wstring_view::npos)
            {
                label = std::wstring{ rest.substr(0, sep) } + L" \x00B7 " + std::wstring{ rest.substr(sep + 2) };
            }
            if (input && input->type == json::Value::Type::Obj)
            {
                for (const auto& [k, v] : input->members)
                {
                    if (v.type == json::Value::Type::Str && !v.str.empty())
                    {
                        arg = TgFirstLine(v.str, 80);
                        break;
                    }
                }
            }
        }
        else if (input && input->type == json::Value::Type::Obj)
        {
            // An unknown tool: its first string argument.
            for (const auto& [k, v] : input->members)
            {
                if (v.type == json::Value::Type::Str && !v.str.empty())
                {
                    arg = TgFirstLine(v.str, 80);
                    break;
                }
            }
        }
        std::wstring out = arg.empty() ? label : label + L": " + arg;
        if (out.size() > kToolSummaryMaxChars)
        {
            out = out.substr(0, kToolSummaryMaxChars - 1) + L"\x2026";
        }
        return out;
    }

    /// <summary>
    /// Reads one Claude Code transcript line into the items a topic shows. Pure; the service feeds it
    /// every complete line past its cursor.
    /// </summary>
    /// <param name="line">The parsed JSONL line (an object).</param>
    /// <param name="includeForkCopies">True for a HISTORY replay (§21.9): a fork's copies of its source's lines are part of
    /// the conversation the fork carries on, so its topic's history shows them. False (the live mirror): they are skipped.</param>
    /// <returns>The items, in block order; empty when the line shows nothing (bookkeeping lines, meta
    /// lines, sidechain lines, the synthetic resume placeholder, protocol noise).</returns>
    /// <remarks>
    /// The REMARK-vs-FINAL decision for an assistant Text is the SERVICE's: this reads the stop reason
    /// (turnEnds) but a text with a non-terminal stop reason is a remark only if a tool call follows
    /// it; the service holds the text until it knows. A prompt's SOURCE (typed at the PC, sent by the
    /// runner, sent from the topic) is the service's too: it needs the session's queue.
    /// </remarks>
    /// <exception cref="std::bad_alloc">If the items cannot be allocated.</exception>
    inline std::vector<TopicItem> ClassifyTranscriptLine(const json::Value& line, bool includeForkCopies = false)
    {
        std::vector<TopicItem> items;
        if (line.type != json::Value::Type::Obj)
        {
            return items;
        }
        const std::wstring type = line.StrAt(L"type");
        const int64_t ts = detail::IsoToUnixMs(line.StrAt(L"timestamp"));
        if (line.BoolAt(L"isSidechain", false))
        {
            return items; // a subagent's inline lines (older Claude Code): the parent's view skips them
        }
        if (!includeForkCopies && line.Find(L"forkedFrom"))
        {
            // A FORK's copy of its source's history: `claude --resume <src> --fork-session` writes the source's
            // lines into the fork's own transcript, each stamped `forkedFrom: {sessionId, messageUuid}`, ahead of
            // the fork's own first message (measured: 958 copied lines, then the fork's own). The LIVE mirror reads
            // the fork's transcript from its first line once it appears, and its topic's history (§21.9) already
            // showed the source's conversation — the copies must not be posted a second time (§21.6).
            return items;
        }
        if (type == L"user")
        {
            if (line.BoolAt(L"isMeta", false) || line.BoolAt(L"isCompactSummary", false))
            {
                return items;
            }
            const json::Value* msg = line.Find(L"message");
            const json::Value* content = msg ? msg->Find(L"content") : nullptr;
            if (!content)
            {
                return items;
            }
            if (content->type == json::Value::Type::Arr)
            {
                bool anyResult = false;
                for (const auto& b : content->arr)
                {
                    if (b.type == json::Value::Type::Obj && b.StrAt(L"type") == L"tool_result")
                    {
                        anyResult = true;
                        TopicItem it;
                        it.kind = TopicItemKind::ToolResult;
                        it.toolUseId = b.StrAt(L"tool_use_id");
                        it.isError = b.BoolAt(L"is_error", false);
                        it.text = TgFirstLine(detail::ContentText(b.Find(L"content")), 200);
                        it.tsMs = ts;
                        items.push_back(std::move(it));
                    }
                }
                if (anyResult)
                {
                    // The user's ANSWER to an AskUserQuestion (§21.9): Claude Code keeps the answers on the line
                    // (`toolUseResult.answers`: question -> the chosen option's label, or the user's own words; the
                    // `questions` array names each question's header). They are the user's message, not a tool's output:
                    // one answer reads as itself, several as "header: answer" lines.
                    const json::Value* tur = line.Find(L"toolUseResult");
                    const json::Value* answers = (tur && tur->type == json::Value::Type::Obj) ? tur->Find(L"answers") : nullptr;
                    if (items.size() == 1 && answers && answers->type == json::Value::Type::Obj && !answers->members.empty())
                    {
                        const json::Value* questions = tur->Find(L"questions");
                        const auto headerOf = [questions](const std::wstring& question) {
                            if (questions && questions->type == json::Value::Type::Arr)
                            {
                                for (const auto& q : questions->arr)
                                {
                                    if (q.type == json::Value::Type::Obj && q.StrAt(L"question") == question)
                                    {
                                        return q.StrAt(L"header");
                                    }
                                }
                            }
                            return std::wstring{};
                        };
                        std::wstring text;
                        for (const auto& [question, answer] : answers->members)
                        {
                            const std::wstring value = answer.type == json::Value::Type::Str ? answer.str : std::wstring{};
                            if (value.empty())
                            {
                                continue;
                            }
                            if (answers->members.size() > 1)
                            {
                                const std::wstring header = headerOf(question);
                                text += (text.empty() ? L"" : L"\n") + (header.empty() ? std::wstring{} : header + L": ") + value;
                            }
                            else
                            {
                                text = value;
                            }
                        }
                        if (!text.empty())
                        {
                            items[0].kind = TopicItemKind::Answer;
                            items[0].text = std::move(text);
                            items[0].isError = false;
                        }
                    }
                    return items;
                }
            }
            const std::wstring text = detail::ContentText(content);
            if (text.empty())
            {
                return items;
            }
            if (detail::IsInterruptMarker(text))
            {
                TopicItem it;
                it.kind = TopicItemKind::Interrupt;
                it.text = text;
                it.tsMs = ts;
                items.push_back(std::move(it));
                return items;
            }
            if (const auto cmd = detail::CommandEchoLine(text); !cmd.empty())
            {
                TopicItem it;
                it.kind = TopicItemKind::Command;
                it.text = cmd;
                it.tsMs = ts;
                items.push_back(std::move(it));
                return items;
            }
            if (IsNoiseUserPrompt(text))
            {
                return items; // teammate deliveries, task notifications, reminders, bash echoes
            }
            TopicItem it;
            it.kind = TopicItemKind::Prompt;
            it.text = text;
            it.tsMs = ts;
            items.push_back(std::move(it));
            return items;
        }
        if (type == L"assistant")
        {
            const json::Value* msg = line.Find(L"message");
            if (!msg)
            {
                return items;
            }
            const std::wstring stop = msg->StrAt(L"stop_reason");
            const bool ends = detail::IsTerminalStop(stop);
            const std::wstring mid = msg->StrAt(L"id");
            if (line.BoolAt(L"isApiErrorMessage", false))
            {
                TopicItem it;
                it.kind = TopicItemKind::ApiError;
                it.text = detail::ContentText(msg->Find(L"content"));
                if (const int64_t st = line.I64At(L"apiErrorStatus", 0); st > 0)
                {
                    it.text = L"HTTP " + std::to_wstring(st) + L": " + it.text;
                }
                it.messageId = mid;
                it.stopReason = stop;
                it.turnEnds = true;
                it.tsMs = ts;
                items.push_back(std::move(it));
                return items;
            }
            const json::Value* content = msg->Find(L"content");
            if (!content || content->type != json::Value::Type::Arr)
            {
                return items;
            }
            for (const auto& b : content->arr)
            {
                if (b.type != json::Value::Type::Obj)
                {
                    continue;
                }
                const std::wstring bt = b.StrAt(L"type");
                if (bt == L"text")
                {
                    const std::wstring text = b.StrAt(L"text");
                    if (text == L"No response requested.")
                    {
                        continue; // the resume placeholder — not model output
                    }
                    if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos)
                    {
                        continue;
                    }
                    TopicItem it;
                    it.kind = TopicItemKind::Text;
                    it.text = text;
                    it.messageId = mid;
                    it.stopReason = stop;
                    it.turnEnds = ends;
                    it.tsMs = ts;
                    items.push_back(std::move(it));
                }
                else if (bt == L"tool_use")
                {
                    const std::wstring name = b.StrAt(L"name");
                    const json::Value* input = b.Find(L"input");
                    TopicItem it;
                    it.toolName = name;
                    it.toolUseId = b.StrAt(L"id");
                    it.messageId = mid;
                    it.stopReason = stop;
                    it.tsMs = ts;
                    if (name == L"AskUserQuestion")
                    {
                        it.kind = TopicItemKind::Question;
                        if (input)
                        {
                            if (const auto* qs = input->Find(L"questions"); qs && qs->type == json::Value::Type::Arr && !qs->arr.empty())
                            {
                                std::wstring text;
                                for (size_t qi = 0; qi < qs->arr.size(); ++qi)
                                {
                                    const auto& q = qs->arr[qi];
                                    if (q.type != json::Value::Type::Obj)
                                    {
                                        continue;
                                    }
                                    if (!text.empty())
                                    {
                                        text += L"\n";
                                    }
                                    const std::wstring header = q.StrAt(L"header");
                                    text += (header.empty() ? std::wstring{} : header + L": ") + q.StrAt(L"question");
                                    if (qi == 0)
                                    {
                                        it.multiSelect = q.BoolAt(L"multiSelect", false);
                                        if (const auto* opts = q.Find(L"options"); opts && opts->type == json::Value::Type::Arr)
                                        {
                                            for (const auto& o : opts->arr)
                                            {
                                                if (o.type == json::Value::Type::Obj)
                                                {
                                                    it.options.push_back(o.StrAt(L"label"));
                                                }
                                            }
                                        }
                                    }
                                }
                                it.text = text;
                            }
                        }
                        if (it.text.empty())
                        {
                            it.text = L"(a question)";
                        }
                    }
                    else if (name == L"ExitPlanMode")
                    {
                        it.kind = TopicItemKind::Plan;
                        it.text = input ? input->StrAt(L"plan") : std::wstring{};
                    }
                    else
                    {
                        it.kind = TopicItemKind::ToolCall;
                        it.summary = ToolCallSummary(name, input);
                    }
                    items.push_back(std::move(it));
                }
            }
            return items;
        }
        if (type == L"system")
        {
            if (line.StrAt(L"subtype") == L"away_summary")
            {
                // Claude Code's idle RECAP: written a few minutes after a turn when the user is away
                // ("... (disable recaps in /config)", the hint stripped by the shared normalizer). Not a message:
                // the service shows it in the sticky card while no conversation line is newer (§21.6).
                if (std::wstring text = NormalizeRecapText(line.StrAt(L"content")); !text.empty())
                {
                    TopicItem it;
                    it.kind = TopicItemKind::Recap;
                    it.text = std::move(text);
                    it.tsMs = ts;
                    items.push_back(std::move(it));
                }
                return items;
            }
            if (line.StrAt(L"subtype") == L"compact_boundary")
            {
                TopicItem it;
                it.kind = TopicItemKind::Compaction;
                int64_t pre = 0, post = 0;
                std::wstring trigger;
                if (const auto* meta = line.Find(L"compactMetadata"); meta && meta->type == json::Value::Type::Obj)
                {
                    pre = meta->I64At(L"preTokens", 0);
                    post = meta->I64At(L"postTokens", 0);
                    trigger = meta->StrAt(L"trigger");
                }
                it.text = L"compacted";
                if (!trigger.empty())
                {
                    it.text += L" (" + trigger + L")";
                }
                if (pre > 0 || post > 0)
                {
                    const auto k = [](int64_t v) { return std::to_wstring(v / 1000) + L"K"; };
                    it.text += L" " + k(pre) + L" \x2192 " + k(post);
                }
                it.tsMs = ts;
                items.push_back(std::move(it));
            }
            return items;
        }
        if (type == L"attachment")
        {
            // A message typed WHILE the turn ran: Claude Code records it as a queued_command attachment
            // when the running turn absorbs it (CLAUDE.md "A message typed WHILE claude works").
            const json::Value* att = line.Find(L"attachment");
            if (att && att->StrAt(L"type") == L"queued_command")
            {
                const std::wstring mode = att->StrAt(L"commandMode");
                std::wstring originKind;
                if (const auto* origin = att->Find(L"origin"); origin && origin->type == json::Value::Type::Obj)
                {
                    originKind = origin->StrAt(L"kind");
                }
                const bool human = (mode.empty() || mode == L"prompt") && (originKind.empty() || originKind == L"human");
                const std::wstring text = att->StrAt(L"prompt");
                if (human && !text.empty() && !IsNoiseUserPrompt(text))
                {
                    TopicItem it;
                    it.kind = TopicItemKind::Prompt;
                    it.text = text;
                    it.midTurn = true;
                    it.tsMs = detail::IsoToUnixMs(att->StrAt(L"timestamp"));
                    if (it.tsMs == 0)
                    {
                        it.tsMs = ts;
                    }
                    items.push_back(std::move(it));
                }
            }
            return items;
        }
        return items;
    }

    // ===== the recap's freshness =============================================================

    /// <summary>
    /// Whether an item is a CONVERSATION message for the recap's freshness rule (§21.6): something the user or
    /// the assistant said or did — a prompt, an assistant text or tool call, a question, a plan, an API error, a
    /// typed command, an interrupt. A tool's result follows its call (already counted), a compaction marker and the
    /// recap itself describe the conversation without moving it on, so none of them makes a recap stale.
    /// </summary>
    /// <param name="kind">The item's kind.</param>
    /// <returns>True when a recap older than this item no longer says where the conversation stands.</returns>
    inline bool IsConversationItem(TopicItemKind kind) noexcept
    {
        switch (kind)
        {
        case TopicItemKind::Prompt:
        case TopicItemKind::Text:
        case TopicItemKind::ToolCall:
        case TopicItemKind::Question:
        case TopicItemKind::Plan:
        case TopicItemKind::ApiError:
        case TopicItemKind::Command:
        case TopicItemKind::Interrupt:
        case TopicItemKind::Answer:
            return true;
        default:
            return false;
        }
    }

    /// <summary>
    /// Folds a batch of transcript items, in file order, into a topic's recap facts (TopicRecord::recapText /
    /// recapAtMs / lastMessageAtMs): a Recap item (Claude Code's away_summary) replaces an older recap; a
    /// conversation item (IsConversationItem) moves the newest-message time on. Shared by the topic's seed (a
    /// read of the transcript's tail when the topic is made) and the mirror (every batch it reads), so the two
    /// can never disagree about which recap is current.
    /// </summary>
    /// <param name="items">The items, in the order the transcript holds them.</param>
    /// <param name="recapText">In/out: the current recap ("" = none).</param>
    /// <param name="recapAtMs">In/out: its transcript time.</param>
    /// <param name="lastMessageAtMs">In/out: the newest conversation item's transcript time.</param>
    /// <remarks>
    /// An item without a timestamp (Claude Code stamps every line; only a hand-made line lacks one) counts as
    /// written after everything folded so far — file order is the transcript's time order. A recap OLDER than
    /// the one held is ignored, so a re-read of an old window can never bring back a recap a newer one replaced.
    /// </remarks>
    /// <exception cref="std::bad_alloc">If the recap text cannot be copied.</exception>
    inline void FoldRecapFacts(const std::vector<TopicItem>& items, std::wstring& recapText, int64_t& recapAtMs, int64_t& lastMessageAtMs)
    {
        for (const auto& it : items)
        {
            const bool recap = it.kind == TopicItemKind::Recap;
            if (!recap && !IsConversationItem(it.kind))
            {
                continue;
            }
            // (std::max): a TU that includes <windows.h> without NOMINMAX before this header has a `max` macro.
            const int64_t at = it.tsMs > 0 ? it.tsMs : (std::max)(recapAtMs, lastMessageAtMs) + 1;
            if (recap)
            {
                if (!it.text.empty() && at >= recapAtMs)
                {
                    recapText = it.text;
                    recapAtMs = at;
                }
            }
            else if (at > lastMessageAtMs)
            {
                lastMessageAtMs = at;
            }
        }
    }

    /// <summary>
    /// The card's recap rule (§21.6, the user's "add the recap if we have a fresh one in respect to its time of
    /// creation to other messages"): the recap is shown only while it is NEWER than every conversation message the
    /// topic knows — it then says where things stand and repeats nothing the thread shows; the next message makes
    /// it stale.
    /// </summary>
    /// <param name="recapText">TopicRecord::recapText.</param>
    /// <param name="recapAtMs">TopicRecord::recapAtMs.</param>
    /// <param name="lastMessageAtMs">TopicRecord::lastMessageAtMs.</param>
    /// <returns>True when the card shows the recap.</returns>
    inline bool RecapIsFresh(std::wstring_view recapText, int64_t recapAtMs, int64_t lastMessageAtMs) noexcept
    {
        return !recapText.empty() && recapAtMs > 0 && recapAtMs > lastMessageAtMs;
    }

    // ===== the user's lines (§21.12) =========================================================

    /// <summary>
    /// Where a prompt came from (§21.12): the user's line in a topic names it — typed at the PC, sent from the phone (a
    /// message of this topic, QueuedPrompt::remoteSource), or sent by the Prompt Queue Runner (origin Autorun).
    /// </summary>
    enum class PromptSource : int
    {
        Pc = 0,
        Phone = 1,
        Runner = 2,
    };

    /// <summary>
    /// The status a turn's PROMPT shows (§21.12, the user's "a status on the prompt itself"): while the turn runs, what it
    /// stopped at, or that it was answered — and None once a newer prompt was sent (only the last prompt carries one).
    /// </summary>
    enum class PromptTurnStatus : int
    {
        None = 0,
        Running = 1,
        NeedsYou = 2,
        Answered = 3,
        Error = 4,
        Ended = 5,
        Interrupted = 6,
    };

    /// <summary>The source's words in the user's line: "typed at the PC", "📱 from the phone", "⚙ runner".</summary>
    /// <param name="s">The source.</param>
    /// <returns>The words (a static string).</returns>
    inline const wchar_t* PromptSourceText(PromptSource s) noexcept
    {
        switch (s)
        {
        case PromptSource::Phone:
            return L"\U0001F4F1 from the phone";
        case PromptSource::Runner:
            return L"\x2699 runner";
        case PromptSource::Pc:
        default:
            return L"typed at the PC";
        }
    }

    /// <summary>A status as the prompt line shows it: "⏳ running", "🟠 needs you", "✅ answered", "🔴 error", "⏹ ended", "■ interrupted"; "" for None.</summary>
    /// <param name="s">The status.</param>
    /// <returns>The words (a static string).</returns>
    inline const wchar_t* PromptStatusText(PromptTurnStatus s) noexcept
    {
        switch (s)
        {
        case PromptTurnStatus::Running:
            return L"\x23F3 running";
        case PromptTurnStatus::NeedsYou:
            return L"\U0001F7E0 needs you";
        case PromptTurnStatus::Answered:
            return L"\x2705 answered";
        case PromptTurnStatus::Error:
            return L"\U0001F534 error";
        case PromptTurnStatus::Ended:
            return L"\x23F9 ended";
        case PromptTurnStatus::Interrupted:
            return L"\x25A0 interrupted";
        case PromptTurnStatus::None:
        default:
            return L"";
        }
    }

    /// <summary>
    /// A status as a REACTION on the user's own message (a prompt sent from the phone, or as the user through the secretary
    /// bot — a bot cannot edit the user's words, and Telegram allows only its fixed reaction set): 👨‍💻 running, 🤔 needs you,
    /// 👌 answered, 💔 error, 😴 ended, 🙈 interrupted; "" removes the reaction (None).
    /// </summary>
    /// <param name="s">The status.</param>
    /// <returns>The reaction emoji (a static string; "" = none).</returns>
    inline const wchar_t* PromptStatusReaction(PromptTurnStatus s) noexcept
    {
        switch (s)
        {
        case PromptTurnStatus::Running:
            return L"\U0001F468\x200D\U0001F4BB";
        case PromptTurnStatus::NeedsYou:
            return L"\U0001F914";
        case PromptTurnStatus::Answered:
            return L"\U0001F44C";
        case PromptTurnStatus::Error:
            return L"\U0001F494";
        case PromptTurnStatus::Ended:
            return L"\U0001F634";
        case PromptTurnStatus::Interrupted:
            return L"\U0001F648";
        case PromptTurnStatus::None:
        default:
            return L"";
        }
    }

    /// <summary>
    /// The status the LAST prompt shows for its session's state (§21.12): Running → running, Needs approval → needs you,
    /// Error → error, Done (claude exited) → ended, and at rest (Waiting for you, Idle) → answered, or interrupted when the
    /// user stopped the turn.
    /// </summary>
    /// <param name="state">The session's state.</param>
    /// <param name="interrupted">The turn ended on an interrupt (Esc, /interrupt).</param>
    /// <returns>The status to show.</returns>
    inline PromptTurnStatus PromptTurnStatusFor(SessionState state, bool interrupted) noexcept
    {
        switch (state)
        {
        case SessionState::Running:
            return PromptTurnStatus::Running;
        case SessionState::NeedsApproval:
            return PromptTurnStatus::NeedsYou;
        case SessionState::Error:
            return PromptTurnStatus::Error;
        case SessionState::Done:
            return PromptTurnStatus::Ended;
        case SessionState::WaitingForInput:
        case SessionState::Idle:
        default:
            return interrupted ? PromptTurnStatus::Interrupted : PromptTurnStatus::Answered;
        }
    }

    // The user's words in their line: plain when short, an expandable quote past these (Telegram folds it), cut at
    // kUserLineMaxChars (an edit cannot be split: the whole line stays under Telegram's 4,096 characters).
    inline constexpr size_t kUserLinePlainMaxChars = 600;
    inline constexpr size_t kUserLinePlainMaxLines = 8;
    inline constexpr size_t kUserLineMaxChars = 3000;

    namespace detail
    {
        // "<b>👤 You</b> · 14:02 · <what>" — the clock left out when unknown (0).
        inline std::wstring UserLineHeader(int64_t tsMs, std::wstring_view what)
        {
            std::wstring h = L"<b>\U0001F464 You</b>";
            if (tsMs > 0)
            {
                h += L" \x00B7 " + TgClockText(tsMs, tsMs);
            }
            if (!what.empty())
            {
                h += L" \x00B7 ";
                h.append(what);
            }
            return h;
        }

        // §21.17: the header and, when given, the session's TITLE ROW under it — the user's "the title at 2nd row (below "You *
        // time * typed at pc") which is clickable jumping to the thread". `titleRow` is a title token (TgTitleToken with the
        // topic's thread), so the send layer renders it as a link chip (rich) or a linked mention (classic). In the chat's "All"
        // view, where every topic's messages run together, it names the thread a "👤 You" line belongs to.
        //   tsMs     - the line's clock (0 = none), as UserLineHeader.
        //   what     - the header's "where from" part, as UserLineHeader.
        //   titleRow - the title token ("" = no row: the pre-§21.17 line, byte for byte).
        //   returns  - the header, then "\n" and the row when there is one.
        //   throws   - std::bad_alloc.
        inline std::wstring UserLineHead(int64_t tsMs, std::wstring_view what, std::wstring_view titleRow)
        {
            std::wstring h = UserLineHeader(tsMs, what);
            if (!titleRow.empty())
            {
                h += L"\n";
                h.append(titleRow);
            }
            return h;
        }

        // The user's words below the header: escaped as they are when short, else in an expandable quote; cut at
        // kUserLineMaxChars (never between the halves of a surrogate pair).
        inline std::wstring UserLineBody(std::wstring_view text)
        {
            std::wstring t{ text };
            while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' '))
            {
                t.pop_back();
            }
            if (t.size() > kUserLineMaxChars)
            {
                size_t cut = kUserLineMaxChars;
                if (t[cut - 1] >= 0xD800 && t[cut - 1] <= 0xDBFF)
                {
                    --cut;
                }
                t = t.substr(0, cut) + L"\x2026";
            }
            const size_t lines = 1 + static_cast<size_t>(std::count(t.begin(), t.end(), L'\n'));
            if (t.size() <= kUserLinePlainMaxChars && lines <= kUserLinePlainMaxLines)
            {
                return TgEscapeHtml(t);
            }
            return L"<blockquote expandable>" + TgEscapeHtml(t) + L"</blockquote>";
        }
    }

    /// <summary>
    /// The user's PROMPT as the bot posts it in a topic (§21.12, the user's design): a bold "👤 You" header with the clock,
    /// where it came from ("typed at the PC", "📱 from the phone", "⚙ runner"), "mid-turn" for a message typed while the turn
    /// ran, and the turn's status when it has one — then the prompt itself:
    /// "&lt;b&gt;👤 You&lt;/b&gt; · 14:02 · typed at the PC · ⏳ running\n&lt;the prompt&gt;". The status changes are edits of
    /// this same line, so the builder renders every state from the same facts.
    /// </summary>
    /// <param name="tsMs">The prompt's transcript time (unix ms; 0 = no clock).</param>
    /// <param name="source">Where it came from.</param>
    /// <param name="text">The prompt as typed.</param>
    /// <param name="status">The turn's status (None = no status: an older prompt, or a history line).</param>
    /// <param name="midTurn">Typed while the turn ran (no status of its own: the turn's prompt carries it).</param>
    /// <param name="titleRow">§21.17: the session's title token for the second row (UserLineHead); "" (the default) = no row.
    /// A status edit of the line must pass the same row, or the edit drops it.</param>
    /// <returns>Telegram HTML (with a title token when `titleRow` is given: send it through the title-aware send paths).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildPromptLineHtml(int64_t tsMs, PromptSource source, std::wstring_view text, PromptTurnStatus status, bool midTurn, std::wstring_view titleRow = {})
    {
        std::wstring what = PromptSourceText(source);
        if (midTurn)
        {
            what += L" \x00B7 mid-turn";
        }
        if (status != PromptTurnStatus::None)
        {
            what += L" \x00B7 ";
            what += PromptStatusText(status);
        }
        return detail::UserLineHead(tsMs, what, titleRow) + L"\n" + detail::UserLineBody(text);
    }

    /// <summary>The user's ANSWER to a question (§21.9 / §21.12): "&lt;b&gt;👤 You&lt;/b&gt; · 14:02 · ↩ answer\n&lt;the answer&gt;".</summary>
    /// <param name="tsMs">The answer's transcript time (0 = no clock).</param>
    /// <param name="answer">The answer as given.</param>
    /// <param name="titleRow">§21.17: the session's title token for the second row; "" (the default) = no row.</param>
    /// <returns>Telegram HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildAnswerLineHtml(int64_t tsMs, std::wstring_view answer, std::wstring_view titleRow = {})
    {
        return detail::UserLineHead(tsMs, L"\x21A9 answer", titleRow) + L"\n" + detail::UserLineBody(answer);
    }

    /// <summary>The user's slash COMMAND (§21.12): "&lt;b&gt;👤 You&lt;/b&gt; · 14:02 · command\n&lt;code&gt;/compact …&lt;/code&gt;".</summary>
    /// <param name="tsMs">The command's transcript time (0 = no clock).</param>
    /// <param name="command">The command line as echoed ("/compact keep the plan"; cut at 300).</param>
    /// <param name="titleRow">§21.17: the session's title token for the second row; "" (the default) = no row.</param>
    /// <returns>Telegram HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildCommandLineHtml(int64_t tsMs, std::wstring_view command, std::wstring_view titleRow = {})
    {
        return detail::UserLineHead(tsMs, L"command", titleRow) + L"\n<code>" + TgEscapeHtml(command.size() > 300 ? command.substr(0, 300) : command) + L"</code>";
    }

    /// <summary>
    /// The TITLE ROW of a session's "👤 You" lines (§21.17): its title as a title token that names its topic, in the tab's color
    /// — one call for the live mirror, the status edits and the history replay, so the three draw the same row.
    /// </summary>
    /// <param name="title">The session's title ("" = the first 8 characters of `sessionId`).</param>
    /// <param name="sessionId">The session id (the fallback name).</param>
    /// <param name="colorSquare">The square the topic's name shows (TopicRecord::nameColor; "" = none).</param>
    /// <param name="threadId">The topic's thread (0 = no link: the row names the session without one).</param>
    /// <returns>The token (TgTitleForm::Plain, the title on one line — TgTitleLine: a two-line tab title shows both lines,
    /// joined by a space — and cut to 120 characters).</returns>
    /// <exception cref="std::bad_alloc">If the token cannot be allocated.</exception>
    inline std::wstring UserLineTitleRow(std::wstring_view title, std::wstring_view sessionId, std::wstring_view colorSquare, int64_t threadId)
    {
        const std::wstring name = title.empty() ? std::wstring{ sessionId.substr(0, 8) } : TgTitleLine(title, 120);
        return TgTitleToken(TgTitleForm::Plain, colorSquare, name, threadId);
    }

    // ===== the history replay (§21.9) ========================================================

    // A history replay packs a turn's narrative — the assistant's remarks and the tool calls they led to — into
    // messages of at most this many characters of Telegram HTML (Telegram's limit is 4096 after the tags).
    inline constexpr size_t kHistoryPackChars = 3500;

    /// <summary>
    /// One message of a topic's HISTORY replay (§21.9), in order. A user's turn (`fromUser`) is sent AS THE USER through
    /// the Telegram Business connection when the secretary probe found that this works (MessagingState::businessMode == 1),
    /// so the topic reads like the live conversation it was; otherwise the bot posts `html`, a ⌨ line with the clock.
    /// </summary>
    struct HistoryPost
    {
        bool fromUser{ false };
        std::wstring userText; // fromUser: the user's words as typed (plain text)
        std::wstring html; // the bot's message (Telegram HTML); for fromUser, the line the bot posts in the user's place
        /// <summary>
        /// §21.11: the same message as Markdown for a RICH message (Bot API 10.1) — Claude's own text as written, our own
        /// lines escaped (TgMdEscapeText), a tool group as a collapsible &lt;details&gt; block. Empty = `html` only (a day
        /// line, a user's line). The sender prefers it and falls back to `html` when Telegram refuses the rich send.
        /// </summary>
        std::wstring markdown;
        /// <summary>
        /// §21.12: the TURN's prompt (its first user line — not a message typed while it ran) and the turn's final REPLY. The
        /// sender sends the reply as a reply to the prompt's message (Telegram shows a quote of the prompt on the answer),
        /// once the prompt's own send answered with its message id.
        /// </summary>
        bool turnPrompt{ false };
        bool turnReply{ false };
    };

    /// <summary>
    /// Renders transcript items (ClassifyTranscriptLine in history mode) into a topic's history (§21.9, the user's "a
    /// thread is a reflection of the entire session"). Per turn: a day line when the day changes; the user's prompt (a
    /// HistoryPost from the user); then, VERBOSE, the assistant's narrative — each remark ("why the bash is invoked")
    /// followed by the tool calls it led to in one folded group, compactions, interrupts, API errors, questions, plans —
    /// packed into messages of at most kHistoryPackChars, and the turn's final reply as its own message. NOT VERBOSE
    /// (§21.11, the default — the user's "only user's prompt and the assistant's turn's last message"): the user's posts,
    /// the questions and plans that asked the user something, an interrupt or an API error that ended a turn, and each
    /// turn's final reply — user → assistant → user → assistant. Both modes keep the user's own posts: a message typed
    /// while the turn ran, an answer to a question, a slash command. The packing is what makes a whole session fit the
    /// chat's budget: 266 turns of 49 sessions came to 1,175 messages (measured 2026-10-16), against 6,581 in the live
    /// layout (one message per remark and per work-log segment).
    /// </summary>
    /// <param name="items">The items in transcript order. They start at a turn's prompt — or, after a reader window
    /// that split one long turn, inside a turn (those items render as narrative).</param>
    /// <param name="lastTurnContinues">The last turn goes on past these items (the reader's window ended inside it):
    /// its last text is narrative, not its final reply — the reply comes with the window where the turn ends.</param>
    /// <param name="lastDayKey">In/out: TgDayKey of the last turn rendered; a turn on another day gets a day line first.</param>
    /// <param name="promptSource">Optional: where a prompt came from (§21.12: "typed at the PC", "📱 from the phone", "⚙ runner" in
    /// the user's line); empty = typed at the PC.</param>
    /// <param name="verbose">True: the narrative too (remarks, tool calls, compactions); false: the conversation only.</param>
    /// <param name="titleRow">§21.17: the session's title token (UserLineTitleRow) for the second row of every "👤 You" line the
    /// bot posts (a prompt, an answer, a command); "" (the default) = no row. A post sent AS THE USER carries only the user's
    /// words, so the row is in its `html` (the bot's fallback) only.</param>
    /// <returns>The posts, in the order they are to be sent.</returns>
    /// <exception cref="std::bad_alloc">If the posts cannot be allocated.</exception>
    inline std::vector<HistoryPost> RenderHistory(const std::vector<TopicItem>& items, bool lastTurnContinues, int64_t& lastDayKey, const std::function<PromptSource(const std::wstring&)>& promptSource = {}, bool verbose = true, std::wstring_view titleRow = {})
    {
        std::vector<HistoryPost> out;
        const auto isTurnStart = [](const TopicItem& it) { return it.kind == TopicItemKind::Prompt && !it.midTurn; };
        const auto emitBot = [&](std::wstring html, std::wstring markdown) {
            if (!html.empty())
            {
                HistoryPost p;
                p.html = std::move(html);
                p.markdown = std::move(markdown);
                out.push_back(std::move(p));
            }
        };
        const auto userLine = [&](const TopicItem& it, bool midTurn) {
            // §21.12: "👤 You · 14:02 · <source>" over the prompt; a history line carries no status (only the live LAST
            // prompt does).
            HistoryPost p;
            p.fromUser = true;
            p.userText = it.text;
            p.turnPrompt = !midTurn;
            const PromptSource source = promptSource ? promptSource(it.text) : PromptSource::Pc;
            p.html = BuildPromptLineHtml(it.tsMs, source, it.text, PromptTurnStatus::None, midTurn, titleRow);
            return p;
        };
        // Our own blocks, in both forms (the HTML the fallback sends, the Markdown a rich message carries).
        const auto questionBlock = [](const TopicItem& it) {
            std::wstring html = L"\x2753 <b>" + TgEscapeHtml(it.text) + L"</b>";
            // The bold run must start and end next to a character (Markdown's flanking rule), so the question is trimmed.
            std::wstring_view q = it.text;
            while (!q.empty() && std::iswspace(q.front()))
            {
                q.remove_prefix(1);
            }
            while (!q.empty() && std::iswspace(q.back()))
            {
                q.remove_suffix(1);
            }
            std::wstring md = q.empty() ? std::wstring{ L"\x2753" } : L"\x2753 **" + TgMdEscapeText(q) + L"**";
            for (size_t o = 0; o < it.options.size(); ++o)
            {
                html += L"\n" + std::to_wstring(o + 1) + L". " + TgEscapeHtml(it.options[o]);
                md += (o == 0 ? L"\n\n" : L"\n") + std::to_wstring(o + 1) + L". " + TgMdEscapeText(it.options[o]);
            }
            return std::pair<std::wstring, std::wstring>{ std::move(html), std::move(md) };
        };
        const auto planBlock = [](const TopicItem& it) {
            return std::pair<std::wstring, std::wstring>{ L"\U0001F4CB <b>Plan</b>\n" + TgMarkdownToHtml(it.text.size() > 3000 ? it.text.substr(0, 3000) + L"\x2026" : it.text),
                                                          L"\U0001F4CB **Plan**\n\n" + (it.text.size() > 20000 ? it.text.substr(0, 20000) + L"\x2026" : it.text) };
        };
        const auto interruptBlock = []() { return std::pair<std::wstring, std::wstring>{ L"\x25A0 <i>interrupted</i>", L"\x25A0 *interrupted*" }; };
        const auto apiErrorBlock = [](const TopicItem& it) {
            const std::wstring text = it.text.size() > 2000 ? it.text.substr(0, 2000) : it.text;
            return std::pair<std::wstring, std::wstring>{ L"\x26A0 <b>API error</b>\n" + TgEscapeHtml(text), L"\x26A0 **API error**  \n" + TgMdEscapeText(text) };
        };
        std::unordered_map<std::wstring, std::wstring> toolNames; // tool_use id -> name, to label a failed result
        size_t i = 0;
        while (i < items.size())
        {
            const size_t start = i;
            const bool hasPrompt = isTurnStart(items[start]);
            size_t end = hasPrompt ? start + 1 : start;
            while (end < items.size() && !isTurnStart(items[end]))
            {
                ++end;
            }
            const bool continues = lastTurnContinues && end == items.size();
            // The user's turn.
            if (hasPrompt)
            {
                const TopicItem& p = items[start];
                if (const int64_t day = TgDayKey(p.tsMs); day != 0 && day != lastDayKey)
                {
                    emitBot(L"<i>\x2014 " + TgEscapeHtml(TgDayText(p.tsMs)) + L" \x2014</i>", std::wstring{});
                    lastDayKey = day;
                }
                out.push_back(userLine(p, false));
            }
            // The final reply: the turn's last text after which no tool call, question, plan or mid-turn message came
            // (only for a turn that ends inside these items).
            size_t replyIdx = static_cast<size_t>(-1);
            if (!continues)
            {
                for (size_t k = end; k > (hasPrompt ? start + 1 : start);)
                {
                    --k;
                    const TopicItemKind kind = items[k].kind;
                    if (kind == TopicItemKind::Text)
                    {
                        replyIdx = k;
                        break;
                    }
                    if (kind == TopicItemKind::ToolResult || kind == TopicItemKind::Recap || kind == TopicItemKind::Compaction)
                    {
                        continue; // bookkeeping after the reply
                    }
                    break; // the turn ended on a tool call, a question, a plan, an error, an interrupt, a command
                }
            }
            // The narrative, packed — in both forms side by side.
            std::wstring cur; // the message being packed (HTML)
            std::wstring curMd; // the same message (Markdown)
            std::wstring group; // the open folded group of tool lines (plain text)
            std::vector<std::wstring> groupTools; // the group's tool names, in order (its summary)
            const auto closeGroup = [&]() {
                if (group.empty())
                {
                    return;
                }
                std::wstring g = L"<blockquote expandable>" + TgEscapeHtml(group) + L"</blockquote>";
                // The Markdown: a collapsible block whose summary names the tools ("🔧 Bash ×3, Edit"), the lines inside
                // a fenced block (nothing in them is read as formatting) with a fence longer than any backtick run.
                std::wstring summary;
                {
                    std::vector<std::pair<std::wstring, size_t>> counts;
                    for (const auto& name : groupTools)
                    {
                        auto c = std::find_if(counts.begin(), counts.end(), [&name](const auto& e) { return e.first == name; });
                        if (c == counts.end())
                        {
                            counts.emplace_back(name, 1);
                        }
                        else
                        {
                            ++c->second;
                        }
                    }
                    for (const auto& [name, n] : counts)
                    {
                        summary += (summary.empty() ? L"" : L", ") + TgEscapeHtml(name.empty() ? std::wstring{ L"tool" } : name) + (n > 1 ? L" \x00D7" + std::to_wstring(n) : std::wstring{});
                    }
                    if (summary.empty())
                    {
                        summary = L"failed tool calls";
                    }
                }
                size_t longest = 0;
                for (size_t a = 0; a < group.size();)
                {
                    if (group[a] == L'`')
                    {
                        size_t n = 0;
                        while (a + n < group.size() && group[a + n] == L'`')
                        {
                            ++n;
                        }
                        longest = (std::max)(longest, n);
                        a += n;
                    }
                    else
                    {
                        ++a;
                    }
                }
                const std::wstring fence((std::max)(size_t{ 3 }, longest + 1), L'`');
                std::wstring gMd = L"<details><summary>\U0001F527 " + summary + L"</summary>\n\n" + fence + L"text\n" + group + L"\n" + fence + L"\n\n</details>";
                group.clear();
                groupTools.clear();
                if (!cur.empty() && cur.size() + 1 + g.size() > kHistoryPackChars)
                {
                    emitBot(std::move(cur), std::move(curMd));
                    cur.clear();
                    curMd.clear();
                }
                if (!cur.empty())
                {
                    cur += L"\n";
                    curMd += L"\n\n";
                }
                cur += g;
                curMd += gMd;
            };
            const auto addLine = [&](const std::wstring& line, const std::wstring& tool) {
                // A group that would no longer fit beside what the message holds is closed, and the message sent.
                if (!group.empty() && cur.size() + group.size() + line.size() + 48 > kHistoryPackChars)
                {
                    closeGroup();
                    emitBot(std::move(cur), std::move(curMd));
                    cur.clear();
                    curMd.clear();
                }
                if (!group.empty())
                {
                    group += L"\n";
                }
                group += line;
                if (!tool.empty())
                {
                    groupTools.push_back(tool);
                }
            };
            const auto addBlock = [&](const std::wstring& html, const std::wstring& md) {
                closeGroup();
                if (!cur.empty() && cur.size() + 2 + html.size() > kHistoryPackChars)
                {
                    emitBot(std::move(cur), std::move(curMd));
                    cur.clear();
                    curMd.clear();
                }
                if (!cur.empty())
                {
                    cur += L"\n\n";
                    curMd += L"\n\n";
                }
                cur += html; // a block longer than the limit is a message of its own; the sender splits it
                curMd += md;
            };
            const auto flush = [&]() {
                closeGroup();
                emitBot(std::move(cur), std::move(curMd));
                cur.clear();
                curMd.clear();
            };
            for (size_t k = hasPrompt ? start + 1 : start; k < end; ++k)
            {
                const TopicItem& it = items[k];
                if (k == replyIdx)
                {
                    flush();
                    emitBot(TgMarkdownToHtml(it.text), it.text);
                    out.back().turnReply = hasPrompt; // §21.12: the reply quotes its prompt (a turn begun in an earlier window has none here)
                    continue;
                }
                switch (it.kind)
                {
                case TopicItemKind::Text:
                    if (verbose)
                    {
                        addBlock(TgMarkdownToHtml(it.text), it.text);
                    }
                    break;
                case TopicItemKind::ToolCall:
                    toolNames[it.toolUseId] = it.toolName;
                    if (verbose)
                    {
                        addLine(L"\x2022 " + it.summary, it.toolName);
                    }
                    break;
                case TopicItemKind::ToolResult:
                    if (verbose && it.isError)
                    {
                        const auto n = toolNames.find(it.toolUseId);
                        addLine(L"\x2717 " + (n == toolNames.end() ? std::wstring{} : n->second + L": ") + it.text, std::wstring{});
                    }
                    break;
                case TopicItemKind::Question:
                {
                    // A question asked the user something: in both modes (its answer, a user post, follows).
                    auto [html, md] = questionBlock(it);
                    if (verbose)
                    {
                        addBlock(html, md);
                    }
                    else
                    {
                        flush();
                        emitBot(std::move(html), std::move(md));
                    }
                    break;
                }
                case TopicItemKind::Plan:
                {
                    auto [html, md] = planBlock(it);
                    if (verbose)
                    {
                        addBlock(html, md);
                    }
                    else
                    {
                        flush();
                        emitBot(std::move(html), std::move(md));
                    }
                    break;
                }
                case TopicItemKind::Compaction:
                    if (verbose)
                    {
                        addBlock(L"<i>\x2014 " + TgEscapeHtml(it.text) + L" \x2014</i>", L"*\x2014 " + TgMdEscapeText(it.text) + L" \x2014*");
                    }
                    break;
                case TopicItemKind::Interrupt:
                {
                    // The turn ended here (the user pressed Esc): in both modes.
                    auto [html, md] = interruptBlock();
                    if (verbose)
                    {
                        addBlock(html, md);
                    }
                    else
                    {
                        flush();
                        emitBot(std::move(html), std::move(md));
                    }
                    break;
                }
                case TopicItemKind::ApiError:
                {
                    // The turn died here: in both modes.
                    auto [html, md] = apiErrorBlock(it);
                    if (verbose)
                    {
                        addBlock(html, md);
                    }
                    else
                    {
                        flush();
                        emitBot(std::move(html), std::move(md));
                    }
                    break;
                }
                case TopicItemKind::Command:
                {
                    // The user's slash command is the user's turn too: sent as the user, or the bot's code line.
                    flush();
                    HistoryPost p;
                    p.fromUser = true;
                    p.userText = it.text;
                    p.html = BuildCommandLineHtml(it.tsMs, it.text, titleRow);
                    out.push_back(std::move(p));
                    break;
                }
                case TopicItemKind::Prompt:
                    // A message typed while the turn ran (a queued_command): the user's, between the assistant's posts.
                    flush();
                    out.push_back(userLine(it, true));
                    break;
                case TopicItemKind::Answer:
                {
                    // The user's answer to the question above: the user's post, as it was in the conversation.
                    flush();
                    HistoryPost p;
                    p.fromUser = true;
                    p.userText = it.text;
                    p.html = BuildAnswerLineHtml(it.tsMs, it.text, titleRow);
                    out.push_back(std::move(p));
                    break;
                }
                default:
                    break; // a Recap: the card shows the current one
                }
            }
            flush();
            i = end;
        }
        return out;
    }

    /// <summary>
    /// The topics to RE-POST — each topic's sticky card sent again — so that Telegram's topic list (newest last message
    /// on top) reads in the desired MOST RECENT order (§21.9). Re-posting moves a topic to the top, so the shortest fix
    /// re-posts the desired order's top part and leaves the rest where it is: the longest tail of the desired order that
    /// is already in the same relative order in the list stays, everything above it is re-posted, lowest-ranked first.
    /// </summary>
    /// <param name="currentTopFirst">The list as it stands, top first (by each topic's last message).</param>
    /// <param name="desiredTopFirst">The desired order, top first (TopicRanksAbove). Ids missing from `currentTopFirst`
    /// are ignored.</param>
    /// <returns>The ids to re-post, in the order to post them (lowest-ranked first); empty when the list is in order.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::vector<std::wstring> PlanTopicResort(const std::vector<std::wstring>& currentTopFirst, const std::vector<std::wstring>& desiredTopFirst)
    {
        std::unordered_map<std::wstring, size_t> pos;
        for (size_t p = 0; p < currentTopFirst.size(); ++p)
        {
            pos.emplace(currentTopFirst[p], p);
        }
        std::vector<std::wstring> desired;
        for (const auto& id : desiredTopFirst)
        {
            if (pos.count(id))
            {
                desired.push_back(id);
            }
        }
        if (desired.size() < 2)
        {
            return {};
        }
        // The kept tail: walk up from the bottom while each one sits above the one below it in the current list.
        size_t keep = desired.size() - 1;
        while (keep > 0 && pos[desired[keep - 1]] < pos[desired[keep]])
        {
            --keep;
        }
        std::vector<std::wstring> repost;
        for (size_t k = keep; k > 0; --k)
        {
            repost.push_back(desired[k - 1]); // lowest-ranked of the re-posted part first: the top one is posted last
        }
        return repost;
    }

    /// <summary>
    /// One window of a HISTORY replay's reader (§21.9, SplitHistoryWindow): the items of the turns it holds and where the
    /// next window starts.
    /// </summary>
    struct HistoryWindow
    {
        /// <summary>In transcript order (ClassifyTranscriptLine in history mode: a fork's copied lines included).</summary>
        std::vector<TopicItem> items;
        /// <summary>The file offset of the line each item came from (parallel to `items`): where a turn starts, so a
        /// turn's end — the next turn's first line — can be committed alone (PlanHistoryPosts).</summary>
        std::vector<int64_t> itemAt;
        /// <summary>Where the window's use ended — the next window's start; -1 = nothing usable was read (the file shrank
        /// or is gone, or `needMore`).</summary>
        int64_t consumedTo{ -1 };
        /// <summary>The window ended inside one turn longer than it: RenderHistory's `lastTurnContinues`.</summary>
        bool turnContinues{ false };
        /// <summary>The window holds no complete line: the reader must read a bigger one.</summary>
        bool needMore{ false };
    };

    /// <summary>
    /// Splits one window of transcript bytes for the HISTORY replay (§21.9): every complete line is classified in history
    /// mode (ClassifyTranscriptLine with the fork's copies — a fork's topic shows the conversation it was forked from too).
    /// Unless the window reaches the replay's end, its use stops at the start of the LAST turn begun inside it, so every turn
    /// is rendered whole, with its final reply as its own message — except one turn longer than the window, which is cut
    /// (`turnContinues`; its reply is found in the window where it ends). A turn that ends exactly at a window's edge with
    /// no turn after it in the window reads as cut too: its reply is then shown as narrative (the next line, which would
    /// tell, is not read).
    /// </summary>
    /// <param name="bytes">The bytes [pos, pos + bytes.size()) of the transcript.</param>
    /// <param name="pos">The file offset of `bytes[0]`, a line start.</param>
    /// <param name="reachedEnd">The bytes end where the replay ends (a line end): every line of them is used.</param>
    /// <returns>The window's items and where it ended.</returns>
    /// <exception cref="std::bad_alloc">If the items cannot be allocated.</exception>
    inline HistoryWindow SplitHistoryWindow(std::string_view bytes, int64_t pos, bool reachedEnd)
    {
        HistoryWindow w;
        const size_t lastNl = bytes.rfind('\n');
        if (!reachedEnd && lastNl == std::string_view::npos)
        {
            w.needMore = true; // not one complete line yet (a pasted image can make one line megabytes long)
            return w;
        }
        const size_t usable = reachedEnd ? bytes.size() : lastNl + 1;
        // Each line's offset is needed for the cut, so the lines are parsed one at a time.
        std::vector<std::pair<int64_t, std::vector<TopicItem>>> lines;
        size_t p = 0;
        while (p < usable)
        {
            const size_t nl = bytes.find('\n', p);
            const size_t lineEnd = (nl == std::string_view::npos || nl >= usable) ? usable : nl;
            if (lineEnd > p)
            {
                auto parsed = ParseTranscriptJsonl(std::string{ bytes.substr(p, lineEnd - p) });
                if (!parsed.empty())
                {
                    auto its = ClassifyTranscriptLine(parsed.front(), true);
                    if (!its.empty())
                    {
                        lines.emplace_back(pos + static_cast<int64_t>(p), std::move(its));
                    }
                }
            }
            p = lineEnd + 1;
        }
        int64_t cut = -1;
        if (!reachedEnd)
        {
            for (auto it = lines.rbegin(); it != lines.rend(); ++it)
            {
                const bool turnStart = std::any_of(it->second.begin(), it->second.end(), [](const TopicItem& x) { return x.kind == TopicItemKind::Prompt && !x.midTurn; });
                if (turnStart)
                {
                    if (it->first > pos)
                    {
                        cut = it->first; // the last turn begun here is read whole by the next window
                    }
                    break;
                }
            }
        }
        for (auto& [at, its] : lines)
        {
            if (cut >= 0 && at >= cut)
            {
                break;
            }
            for (auto& x : its)
            {
                w.items.push_back(std::move(x));
                w.itemAt.push_back(at);
            }
        }
        if (reachedEnd)
        {
            w.consumedTo = pos + static_cast<int64_t>(bytes.size());
        }
        else if (cut >= 0)
        {
            w.consumedTo = cut;
        }
        else
        {
            w.consumedTo = pos + static_cast<int64_t>(usable); // one turn longer than the window: it goes on next time
            w.turnContinues = true;
        }
        return w;
    }

    /// <summary>One post of a history window (PlanHistoryPosts) and what its answer commits.</summary>
    struct HistoryPlanPost
    {
        HistoryPost post;
        /// <summary>The last post of a turn: the offset where the NEXT turn starts (or the window's end) — once it is
        /// answered, the turn is in the topic and the replay may go on from there. 0 on every other post.</summary>
        int64_t commitTo{ 0 };
        /// <summary>TgDayKey of the last turn rendered up to and including this post (committed with `commitTo`).</summary>
        int64_t dayAfter{ 0 };
    };

    /// <summary>
    /// A history window's posts (§21.9), rendered TURN BY TURN (RenderHistory per turn: its packing never crosses a turn)
    /// so that each turn's last post carries the turn's end: the replay commits a turn as soon as its last post is
    /// answered, and a stop midway re-sends at most the turn that was going out — not the whole window (the 00:07 restart
    /// of the first live run would have repeated ~20 messages of one topic).
    /// </summary>
    /// <param name="w">The window (SplitHistoryWindow), with `itemAt`.</param>
    /// <param name="lastDayKey">In/out: TgDayKey of the last turn rendered before the window.</param>
    /// <param name="promptSource">Optional: where a prompt came from (RenderHistory's user line, §21.12).</param>
    /// <param name="verbose">RenderHistory's mode (§21.11): true = the narrative too, false = the conversation only.</param>
    /// <param name="titleRow">§21.17: RenderHistory's title row of the "👤 You" lines ("" = none).</param>
    /// <returns>The posts in order; a turn that renders nothing hands its end to the next turn's last post.</returns>
    /// <exception cref="std::bad_alloc">If the posts cannot be allocated.</exception>
    inline std::vector<HistoryPlanPost> PlanHistoryPosts(const HistoryWindow& w, int64_t& lastDayKey, const std::function<PromptSource(const std::wstring&)>& promptSource = {}, bool verbose = true, std::wstring_view titleRow = {})
    {
        std::vector<HistoryPlanPost> out;
        const auto isTurnStart = [](const TopicItem& it) { return it.kind == TopicItemKind::Prompt && !it.midTurn; };
        size_t begin = 0;
        while (begin < w.items.size())
        {
            size_t end = begin + 1;
            while (end < w.items.size() && !isTurnStart(w.items[end]))
            {
                ++end;
            }
            const bool lastGroup = end == w.items.size();
            const std::vector<TopicItem> group(w.items.begin() + static_cast<std::ptrdiff_t>(begin), w.items.begin() + static_cast<std::ptrdiff_t>(end));
            auto rendered = RenderHistory(group, lastGroup && w.turnContinues, lastDayKey, promptSource, verbose, titleRow);
            // Where this turn ends: the next turn's first line, or the window's end. Without itemAt (a caller that did not
            // fill it) only the window's end is known.
            const int64_t commitTo = lastGroup ? w.consumedTo : (end < w.itemAt.size() ? w.itemAt[end] : 0);
            for (size_t k = 0; k < rendered.size(); ++k)
            {
                HistoryPlanPost p;
                p.post = std::move(rendered[k]);
                if (k + 1 == rendered.size())
                {
                    p.commitTo = commitTo;
                    p.dayAfter = lastDayKey;
                }
                out.push_back(std::move(p));
            }
            begin = end;
        }
        return out;
    }

    // ===== the connector's state file ========================================================

    // One session's topic in the owner's chat: what the Bot API cannot tell us back (it lists no
    // topics), so this record is the only link between a session and its topic.
    struct TopicRecord
    {
        std::wstring sessionId;
        int64_t threadId{ 0 }; // Telegram's message_thread_id of the topic
        int64_t cardMessageId{ 0 }; // the pinned card message (0 until sent)
        int64_t workLogMessageId{ 0 }; // the current turn's work-log message (0 when the turn has none yet)
        std::wstring title; // the topic's name as last set (the tab title)
        int64_t idleSinceMs{ 0 }; // when the session went Idle (0 = not idle): the grace clock
        int64_t cursor{ 0 }; // the transcript byte offset the mirror has consumed
        std::wstring transcriptPath; // the transcript the cursor refers to ("" until resolved)
        int64_t createdMs{ 0 }; // when the topic was created
        /// <summary>
        /// The STICKY CARD's clocks (§21.5): `lastPostedMs` is when anything but the card last landed in the topic
        /// (the bot's prompts, remarks, replies and answers at enqueue time; the user's own messages when read),
        /// `cardPostedMs` when the current card message was sent. A card older than the newest post is deleted
        /// and sent again at the bottom, so the buttons and the state are always the last message.
        /// </summary>
        int64_t lastPostedMs{ 0 };
        int64_t cardPostedMs{ 0 };
        /// <summary>
        /// The RECAP (§21.6): Claude Code's OWN idle recap — the `{"type":"system","subtype":"away_summary"}` line
        /// it writes a few minutes after a turn when the user is away — normalized (NormalizeRecapText), with its
        /// transcript time (`recapAtMs`); and `lastMessageAtMs`, the transcript time of the newest conversation
        /// line the topic knows (a prompt, an assistant text or tool call, a question, a plan, an API error, a
        /// command). The card shows the recap only while it is FRESH — newer than every message (`recapAtMs >
        /// lastMessageAtMs`), the user's "add the recap if we have a fresh one in respect to its time of creation
        /// to other messages"; the next message makes it stale. Both seeded from the transcript's tail when the
        /// topic is created, then kept by the mirror. (§21.5's first version synthesized a "last prompt + last
        /// reply" block, which only repeated the reply right above the card; its keys are no longer read.)
        /// </summary>
        std::wstring recapText;
        int64_t recapAtMs{ 0 };
        int64_t lastMessageAtMs{ 0 };
        /// <summary>
        /// FNV-1a 32 of the card text last sent or edited (0 = none yet). A restart re-renders every card, and a
        /// text the topic already shows is not sent again: before it, every start re-edited every card once
        /// (117 edits in the first ten minutes of the 20:30 start, 2026-10-16) on the one chat's ~1 call/s budget.
        /// Since §21.15 the hash is taken over the card's CLASSIC render (its title tokens as the §21.13 square and title), so it
        /// does not depend on the form the card is shown in, and a card that did not change keeps its hash across the upgrade.
        /// </summary>
        uint32_t cardTextHash{ 0 };
        /// <summary>
        /// The FORM the topic's card is shown in (§21.15): true = a rich message whose title is a chip, false = a classic HTML
        /// message (every card posted before the chips, and every card while rich messages are off). Taken from Telegram's own
        /// answer to the post or edit (the Message carries `rich_message` or `text`), so a rich send that fell back to HTML
        /// records false. A card whose text is unchanged but whose form differs from the wanted one is re-edited slowly — one card
        /// at a time (MessagingService::_takeCardFormatSlot) — never in a burst.
        /// </summary>
        bool cardRich{ false };
        /// <summary>
        /// Whether the topic's card shows its title as a LINK to the topic (§21.17): true once a post or edit went out while the
        /// bot's username was known (a URL chip in a rich card, an &lt;a&gt; in a classic one); false for every card posted before
        /// the links. Like `cardRich` it is a FORM: the card hash ignores it (TgRenderTitleTokensClassic without the username), and
        /// a card whose text is unchanged but whose links are missing is re-edited by the slow form upgrade, one at a time
        /// (MessagingService::_takeCardFormatSlot) — so the upgrade never edits a whole chat's cards in a burst.
        /// </summary>
        bool cardLinked{ false };
        /// <summary>
        /// The HISTORY replay's last turn prompt (§21.17): the message id of the bot's "👤 You" line of the newest turn the replay
        /// has sent so far (0 = none yet). Each replayed turn's line replies to the one before it, and the first live turn after the
        /// replay replies to this one — the user's "should be a reply to previous user message. this way we can jump between user
        /// turns". Persisted, so a replay a restart interrupted goes on with the chain. A line sent AS THE USER (secretary mode) is
        /// not a reply and does not move it.
        /// </summary>
        int64_t historyPromptMessageId{ 0 };
        /// <summary>
        /// The tab (WT_SESSION) that hosted the topic's session at the last sync. Topics are PER TAB (§21.6, the
        /// user's "if we show per tab"): when the conversation in a tab changes (/clear, /resume, a handover in
        /// place) the old session closes while a new one takes the SAME tab, and the topic follows the tab to it
        /// instead of being deleted and created again. A token is unique per connection, so a stale one (after
        /// an app restart every tab has a new token) can never match.
        /// </summary>
        std::wstring tabToken;
        /// <summary>
        /// The topic was told that the session is in Error; cleared when the session runs again. Without it a
        /// session at rest in Error was announced at EVERY app start (the registry loads it as Idle, the scanner
        /// re-asserts Error — an edge each time; measured 2026-10-16, 19:30 and 19:45).
        /// </summary>
        bool errorAnnounced{ false };
        /// <summary>
        /// The HISTORY replay (§21.9): a topic made for a session that already has a conversation replays it first —
        /// `backfillPath` (the session's transcript, or a fresh fork's source), bytes [backfillPos, backfillEnd), oldest
        /// first. `backfillPos` moves on only when a batch's messages were all sent (a stop midway re-sends at most one
        /// batch, never loses one); `backfillDayKey` is the day of the last turn replayed (TgDayKey). While backfillPos
        /// &lt; backfillEnd the live mirror of the topic waits (its cursor starts at backfillEnd, so nothing is lost) and
        /// the sticky card is not re-posted. Empty path / equal positions = nothing to replay.
        /// </summary>
        std::wstring backfillPath;
        int64_t backfillPos{ 0 };
        int64_t backfillEnd{ 0 };
        int64_t backfillDayKey{ 0 };
        /// <summary>
        /// The topic's own VERBOSE mode (§21.11): -1 follows the default for all topics (AppSettings::messagingVerbose),
        /// 1 verbose (the remarks, the work log of tool calls, compactions — the whole session as it runs), 0 the
        /// conversation only (the user's posts and each turn's final reply, plus the questions, plans, interrupts and API
        /// errors that ended or paused a turn). Set by /verbose in the topic and by recreating the topic in a mode; a
        /// recreation of ALL topics clears it (the chosen mode becomes the default instead). TopicIsVerbose resolves it.
        /// </summary>
        int verbose{ -1 };
        /// <summary>
        /// The STATE GLYPH the topic's name shows (§21.11, the user's "an emoji representing the status … similarly to
        /// how tabs present"): the SessionState as an int, -1 = none yet (a topic made before the glyph existed, or one
        /// whose rename has not run). `nameSetMs` is when the name was last set (the state renames' per-topic gap).
        /// </summary>
        int nameState{ -1 };
        int64_t nameSetMs{ 0 };
        /// <summary>
        /// The COLOR SQUARE the topic's name shows after the state glyph (§21.13, the user's "can the title have a color
        /// background in all places we print title, that matches"): TgTitleColorSquare of the tab color when the name was last
        /// set ("" = none: the "Remove colors" mode, or a topic named before §21.13). `nameColorMode` is the TabColorMode that
        /// color was resolved under (-1 = unknown: a topic named before §21.13). A square that differs under the SAME mode is a
        /// change of the tab's color, and the topic gets a line saying so (BuildTopicChangeNoticeHtml); a topic named under
        /// another mode, or before §21.13, gets the new square QUIETLY (a mode switch would otherwise post a line in every
        /// topic). The card's title shows the same square (CardExtras::colorSquare), so the card and the list always agree.
        /// </summary>
        std::wstring nameColor;
        int nameColorMode{ -1 };
        /// <summary>
        /// Whether the topic's ICON shows its session's state (§21.22, the user's "remove the status circle in the title when we
        /// have status circle in topic icon"): true = the icon is StateTopicIcon(nameState) and the NAME carries no state glyph
        /// (TopicDisplayName's stateIcon form, `🟦 title`); false = the name starts with the glyph (§21.11, `🟡 🟦 title`) and the
        /// icon is Telegram's default, which draws the name's first character — the state a second time. Recorded with every
        /// rename and create (what the call asked for); persisted as `stateIcon`. Absent in an older file = false: the topic is
        /// renamed once into the icon form, quietly (PickNameRename's form rule). After Telegram refused an icon in a run
        /// (TopicIconRefused), the topics that show one go back to the glyph form for the rest of that run.
        /// </summary>
        bool stateIcon{ false };
        /// <summary>
        /// The LAST PROMPT the user sent in this topic's session (§21.12) — the one whose line carries the turn's status and
        /// whose message the turn's final reply answers. `promptMessageId`: the bot's prompt line, or the user's own message
        /// (a prompt from the phone, or one sent as the user by the secretary bot); 0 = not known yet (the send has not
        /// answered). `promptBotLine`: true = our message, whose status is an EDIT of the line (BuildPromptLineHtml from
        /// promptTsMs / promptSource / promptText); false = the user's message, whose status is a REACTION.
        /// `promptStatus`: the PromptTurnStatus the message shows now (None = none). `promptInterrupted`: the turn ended
        /// on an interrupt. Set by the live mirror when a new turn's prompt arrives (the previous prompt's status is
        /// removed then); persisted, so a restart keeps updating it. Empty for a topic whose live mirror has seen no prompt.
        /// </summary>
        int64_t promptMessageId{ 0 };
        bool promptBotLine{ false };
        int64_t promptTsMs{ 0 };
        int promptSource{ 0 };
        std::wstring promptText;
        int promptStatus{ 0 };
        bool promptInterrupted{ false };
    };

    /// <summary>Whether a topic shows the whole session (verbose) or the conversation only (§21.11).</summary>
    /// <param name="t">The topic.</param>
    /// <param name="defaultVerbose">The default for all topics (AppSettings::messagingVerbose; off by default).</param>
    /// <returns>True when the topic is verbose.</returns>
    inline bool TopicIsVerbose(const TopicRecord& t, bool defaultVerbose) noexcept
    {
        return t.verbose < 0 ? defaultVerbose : t.verbose == 1;
    }

    /// <summary>Whether a topic's history replay (§21.9) still has to run (or finish).</summary>
    /// <param name="t">The topic.</param>
    /// <returns>True while part of the history is not sent yet.</returns>
    inline bool TopicBackfillPending(const TopicRecord& t) noexcept
    {
        return !t.backfillPath.empty() && t.backfillPos < t.backfillEnd;
    }

    /// <summary>
    /// The topic LAYOUT this build makes topics under (§21.8). A state file whose topics were made under an older
    /// layout gets them deleted and made again ONCE — the RECREATION — when the service starts. Layout 1 (§21.8): the
    /// topics in MOST RECENT order (before it they were created in session-id order, in the formats §21.4–§21.6
    /// replaced). Layout 2 (§21.9): every topic carries its session's whole HISTORY (the user's "a thread is a
    /// reflection of the entire session"; layout 1 topics showed the session only from their creation on). Raise it
    /// only for a change that existing topics cannot follow.
    /// </summary>
    inline constexpr int kTopicLayoutVersion = 2;

    struct MessagingState
    {
        int64_t updateOffset{ 0 }; // Telegram's getUpdates offset: the next update id to ask for
        int64_t searchThreadId{ 0 }; // the pinned "Search" topic (0 until created)
        int64_t setupNoticeMs{ 0 }; // when the "turn Threaded Mode on" notice was last sent (0 = never)
        /// <summary>
        /// The bot this state belongs to (getMe's user id; 0 = written before the key existed, or never connected).
        /// Everything else in the file is PER BOT — the update offset (ids are per bot: a stale higher offset
        /// would confirm, i.e. drop, every update of a new bot below it), the Search topic and the topic map
        /// (thread ids of the old bot's chat) — so a different id at connect time resets all of it. The username
        /// cannot serve as the key: @BotFather renames a bot without changing its id.
        /// </summary>
        int64_t botId{ 0 };
        std::wstring botUsername; // from getMe, for the status line
        /// <summary>
        /// The Telegram Business connection — what the user calls "Secretary mode": the bot added under
        /// Settings → Telegram Business → Chatbots (§21.5). `businessConnectionId` arrives in a
        /// `business_connection` update (never by any other call; an update sent before the bot listened for it
        /// is gone, so the user re-toggles the entry); `businessEnabled` / `businessCanReply` are that update's
        /// `is_enabled` and `rights.can_reply`. `businessMode` is the SECRETARY PROBE's verdict: 0 not tried yet,
        /// 1 a send on the user's behalf into this chat's topics works (prompts typed at the PC then appear as
        /// the user's own messages), -1 Telegram refused it (`businessNote` holds the description; prompts keep
        /// the ⌨ line). Telegram documents business sends for "private chats that had incoming messages in the
        /// last 24 hours"; whether the user's chat with the bot itself counts is what the probe finds out.
        /// </summary>
        std::wstring businessConnectionId;
        bool businessEnabled{ false };
        bool businessCanReply{ false };
        int businessMode{ 0 };
        std::wstring businessNote;
        /// <summary>
        /// Which bot holds the business connection (§21.11): 0 or the topics bot's id = the topics bot itself; the
        /// SECRETARY bot's id = the second bot the user connected under Chat Automation, whose client then sends as the
        /// user (Telegram documents a business bot sending to the user's chats with OTHER bots). `secretaryBotId` and
        /// `secretaryUpdateOffset` are the secretary bot's getMe id and getUpdates offset (update ids are per bot: a new
        /// secretary bot starts from 0).
        /// </summary>
        int64_t businessBotId{ 0 };
        int64_t secretaryBotId{ 0 };
        int64_t secretaryUpdateOffset{ 0 };
        // The layout the session topics were made under (kTopicLayoutVersion; 0 = before §21.8, or a file without the key).
        int topicLayout{ 0 };
        // The RECREATION's old topics (thread ids) that still have to be deleted in Telegram (§21.8). Persisted, so a
        // stop midway leaves no orphan behind: the next start finishes the deletes. While it is not empty, no new topic
        // is made — the new list is built after the old one is gone.
        std::vector<int64_t> orphanThreads;
        std::map<std::wstring, TopicRecord> topics; // session id -> its topic
        /// <summary>
        /// The DIRECTORY (§21.16): the one pinned message in the chat's main view that lists the dedicated topics and the
        /// tags with their tabs. `directoryMessageId` is its message (0 = none yet: the service posts one — the first start of
        /// a build with the directory, or after the old one was found deleted). `directoryHash` is the content hash of what it
        /// shows (DirectoryRender::hash), `directoryRich` whether it is a rich message: an unchanged content in the wanted form
        /// is never edited, also across a restart. `directoryPinned`: Telegram took the pin of THIS message (a refused pin is
        /// tried again once per run). Per bot, like everything here: a different bot resets them.
        /// </summary>
        int64_t directoryMessageId{ 0 };
        uint32_t directoryHash{ 0 };
        bool directoryRich{ false };
        bool directoryPinned{ false };
        /// <summary>
        /// §21.25: the NEW-THREAD WIZARDS and their launches — every thread the user opened that has no session yet (its
        /// wizard's step and answers, the folder lists and model / window choices it showed, the messages it holds) and every
        /// launch waiting for its session to come up, as MessagingNewThreadWizard.h NewThreadsToJson writes them. Kept OPAQUE
        /// here (this header does not know the wizard's types): the service converts it at its start (NewThreadsFromJson) and
        /// writes it back at every save, and a file without the key (written before §21.25) reads as no wizard. Per bot like
        /// everything here: the identity gate's reset clears it with the topics (thread ids name threads of one bot's chat).
        /// A Null value = none (not written to the file).
        /// </summary>
        json::Value newThreads;
    };

    /// <summary>
    /// Whether the state's session topics must be made again (§21.8): they were made under an older layout. A state with
    /// no session topic never needs it (the service just records the current layout).
    /// </summary>
    /// <param name="s">The state.</param>
    /// <returns>True when the RECREATION is due.</returns>
    inline bool NeedsTopicRecreation(const MessagingState& s) noexcept
    {
        return s.topicLayout < kTopicLayoutVersion && !s.topics.empty();
    }

    /// <summary>Serializes the connector's state file (messaging\telegram\state.json).</summary>
    /// <param name="s">The state.</param>
    /// <returns>The JSON object.</returns>
    /// <exception cref="std::bad_alloc">If the document cannot be allocated.</exception>
    inline json::Value StateToJson(const MessagingState& s)
    {
        auto o = json::Value::MkObj();
        o.Set(L"version", json::Value::MkNum(1));
        o.Set(L"updateOffset", json::Value::MkNum(static_cast<double>(s.updateOffset)));
        o.Set(L"searchThreadId", json::Value::MkNum(static_cast<double>(s.searchThreadId)));
        o.Set(L"setupNoticeMs", json::Value::MkNum(static_cast<double>(s.setupNoticeMs)));
        o.Set(L"botId", json::Value::MkNum(static_cast<double>(s.botId)));
        o.Set(L"botUsername", json::Value::MkStr(s.botUsername));
        o.Set(L"businessConnectionId", json::Value::MkStr(s.businessConnectionId));
        o.Set(L"businessEnabled", json::Value::MkBool(s.businessEnabled));
        o.Set(L"businessCanReply", json::Value::MkBool(s.businessCanReply));
        o.Set(L"businessMode", json::Value::MkNum(static_cast<double>(s.businessMode)));
        o.Set(L"businessNote", json::Value::MkStr(s.businessNote));
        o.Set(L"businessBotId", json::Value::MkNum(static_cast<double>(s.businessBotId)));
        o.Set(L"secretaryBotId", json::Value::MkNum(static_cast<double>(s.secretaryBotId)));
        o.Set(L"secretaryUpdateOffset", json::Value::MkNum(static_cast<double>(s.secretaryUpdateOffset)));
        o.Set(L"topicLayout", json::Value::MkNum(static_cast<double>(s.topicLayout)));
        // §21.16: the directory message (0 = none) and what it shows.
        o.Set(L"directoryMessageId", json::Value::MkNum(static_cast<double>(s.directoryMessageId)));
        o.Set(L"directoryHash", json::Value::MkNum(static_cast<double>(s.directoryHash)));
        o.Set(L"directoryRich", json::Value::MkBool(s.directoryRich));
        o.Set(L"directoryPinned", json::Value::MkBool(s.directoryPinned));
        auto orphans = json::Value::MkArr();
        for (const int64_t thread : s.orphanThreads)
        {
            orphans.Push(json::Value::MkNum(static_cast<double>(thread)));
        }
        o.Set(L"orphanThreads", std::move(orphans));
        auto topics = json::Value::MkArr();
        for (const auto& [sid, t] : s.topics)
        {
            auto to = json::Value::MkObj();
            to.Set(L"sessionId", json::Value::MkStr(sid));
            to.Set(L"threadId", json::Value::MkNum(static_cast<double>(t.threadId)));
            to.Set(L"cardMessageId", json::Value::MkNum(static_cast<double>(t.cardMessageId)));
            to.Set(L"workLogMessageId", json::Value::MkNum(static_cast<double>(t.workLogMessageId)));
            to.Set(L"title", json::Value::MkStr(t.title));
            to.Set(L"idleSinceMs", json::Value::MkNum(static_cast<double>(t.idleSinceMs)));
            to.Set(L"cursor", json::Value::MkNum(static_cast<double>(t.cursor)));
            to.Set(L"transcriptPath", json::Value::MkStr(t.transcriptPath));
            to.Set(L"createdMs", json::Value::MkNum(static_cast<double>(t.createdMs)));
            to.Set(L"lastPostedMs", json::Value::MkNum(static_cast<double>(t.lastPostedMs)));
            to.Set(L"cardPostedMs", json::Value::MkNum(static_cast<double>(t.cardPostedMs)));
            to.Set(L"recapText", json::Value::MkStr(t.recapText));
            to.Set(L"recapAtMs", json::Value::MkNum(static_cast<double>(t.recapAtMs)));
            to.Set(L"lastMessageAtMs", json::Value::MkNum(static_cast<double>(t.lastMessageAtMs)));
            to.Set(L"cardTextHash", json::Value::MkNum(static_cast<double>(t.cardTextHash)));
            to.Set(L"cardRich", json::Value::MkBool(t.cardRich)); // §21.15: the card's form (a rich card with a title chip)
            to.Set(L"cardLinked", json::Value::MkBool(t.cardLinked)); // §21.17: the card's title links to its topic
            if (t.historyPromptMessageId > 0)
            {
                // §21.17: the history replay's last "👤 You" line (omitted while the replay has sent none).
                to.Set(L"historyPromptMessageId", json::Value::MkNum(static_cast<double>(t.historyPromptMessageId)));
            }
            to.Set(L"tabToken", json::Value::MkStr(t.tabToken));
            to.Set(L"errorAnnounced", json::Value::MkBool(t.errorAnnounced));
            to.Set(L"backfillPath", json::Value::MkStr(t.backfillPath));
            to.Set(L"backfillPos", json::Value::MkNum(static_cast<double>(t.backfillPos)));
            to.Set(L"backfillEnd", json::Value::MkNum(static_cast<double>(t.backfillEnd)));
            to.Set(L"backfillDayKey", json::Value::MkNum(static_cast<double>(t.backfillDayKey)));
            to.Set(L"verbose", json::Value::MkNum(static_cast<double>(t.verbose)));
            to.Set(L"nameState", json::Value::MkNum(static_cast<double>(t.nameState)));
            to.Set(L"nameSetMs", json::Value::MkNum(static_cast<double>(t.nameSetMs)));
            // §21.13: the name's color square and the mode it was resolved under (-1 = the name predates the square).
            to.Set(L"nameColor", json::Value::MkStr(t.nameColor));
            to.Set(L"nameColorMode", json::Value::MkNum(static_cast<double>(t.nameColorMode)));
            // §21.22: the state is the topic's icon, and the name carries no glyph.
            to.Set(L"stateIcon", json::Value::MkBool(t.stateIcon));
            if (t.promptMessageId != 0 || !t.promptText.empty())
            {
                // §21.12: the last prompt's line (omitted when the live mirror has seen none).
                to.Set(L"promptMessageId", json::Value::MkNum(static_cast<double>(t.promptMessageId)));
                to.Set(L"promptBotLine", json::Value::MkBool(t.promptBotLine));
                to.Set(L"promptTsMs", json::Value::MkNum(static_cast<double>(t.promptTsMs)));
                to.Set(L"promptSource", json::Value::MkNum(static_cast<double>(t.promptSource)));
                to.Set(L"promptText", json::Value::MkStr(t.promptText));
                to.Set(L"promptStatus", json::Value::MkNum(static_cast<double>(t.promptStatus)));
                to.Set(L"promptInterrupted", json::Value::MkBool(t.promptInterrupted));
            }
            topics.Push(std::move(to));
        }
        o.Set(L"topics", std::move(topics));
        if (s.newThreads.type == json::Value::Type::Obj)
        {
            // §21.25: the wizards and their launches, as the service wrote them (opaque here; omitted when there are none).
            o.Set(L"newThreads", s.newThreads);
        }
        return o;
    }

    /// <summary>Reads the connector's state file; tolerant of missing members (a fresh file).</summary>
    /// <param name="v">The parsed document.</param>
    /// <returns>The state; nullopt when the document is not an object.</returns>
    /// <exception cref="std::bad_alloc">If the state cannot be allocated.</exception>
    inline std::optional<MessagingState> StateFromJson(const json::Value& v)
    {
        if (v.type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        MessagingState s;
        s.updateOffset = v.I64At(L"updateOffset", 0);
        s.searchThreadId = v.I64At(L"searchThreadId", 0);
        s.setupNoticeMs = v.I64At(L"setupNoticeMs", 0);
        s.botId = v.I64At(L"botId", 0); // absent in a file written before the key: 0 = "take the first bot seen"
        s.botUsername = v.StrAt(L"botUsername");
        s.businessConnectionId = v.StrAt(L"businessConnectionId");
        s.businessEnabled = v.BoolAt(L"businessEnabled", false);
        s.businessCanReply = v.BoolAt(L"businessCanReply", false);
        s.businessMode = static_cast<int>(v.I64At(L"businessMode", 0));
        s.businessNote = v.StrAt(L"businessNote");
        s.businessBotId = v.I64At(L"businessBotId", 0); // absent = the topics bot (the only one before §21.11)
        s.secretaryBotId = v.I64At(L"secretaryBotId", 0);
        s.secretaryUpdateOffset = v.I64At(L"secretaryUpdateOffset", 0);
        s.topicLayout = static_cast<int>(v.I64At(L"topicLayout", 0)); // absent = topics made before §21.8
        // §21.16 (absent in an older file: no directory message yet — the service posts and pins one).
        s.directoryMessageId = (std::max)(int64_t{ 0 }, v.I64At(L"directoryMessageId", 0)); // (std::max): the <windows.h> macro
        s.directoryHash = static_cast<uint32_t>(v.I64At(L"directoryHash", 0));
        s.directoryRich = v.BoolAt(L"directoryRich", false);
        s.directoryPinned = v.BoolAt(L"directoryPinned", false);
        if (const auto* orphans = v.Find(L"orphanThreads"); orphans && orphans->type == json::Value::Type::Arr)
        {
            for (const auto& o : orphans->arr)
            {
                if (o.type == json::Value::Type::Num && o.num > 0)
                {
                    s.orphanThreads.push_back(static_cast<int64_t>(o.num));
                }
            }
        }
        if (const auto* topics = v.Find(L"topics"); topics && topics->type == json::Value::Type::Arr)
        {
            for (const auto& to : topics->arr)
            {
                if (to.type != json::Value::Type::Obj)
                {
                    continue;
                }
                TopicRecord t;
                t.sessionId = to.StrAt(L"sessionId");
                if (t.sessionId.empty())
                {
                    continue;
                }
                t.threadId = to.I64At(L"threadId", 0);
                t.cardMessageId = to.I64At(L"cardMessageId", 0);
                t.workLogMessageId = to.I64At(L"workLogMessageId", 0);
                t.title = to.StrAt(L"title");
                t.idleSinceMs = to.I64At(L"idleSinceMs", 0);
                t.cursor = to.I64At(L"cursor", 0);
                t.transcriptPath = to.StrAt(L"transcriptPath");
                t.createdMs = to.I64At(L"createdMs", 0);
                t.lastPostedMs = to.I64At(L"lastPostedMs", 0);
                t.cardPostedMs = to.I64At(L"cardPostedMs", 0);
                t.recapText = to.StrAt(L"recapText"); // absent (a 0.0.1.15 file: recapPrompt / recapReply) = no recap
                t.recapAtMs = to.StrAt(L"recapText").empty() ? 0 : to.I64At(L"recapAtMs", 0);
                t.lastMessageAtMs = to.I64At(L"lastMessageAtMs", 0);
                t.cardTextHash = static_cast<uint32_t>(to.I64At(L"cardTextHash", 0));
                // §21.15 (absent in an older file: every card posted before the chips is a classic message).
                t.cardRich = to.BoolAt(L"cardRich", false);
                // §21.17 (absent in an older file: every card posted before the links — the slow form upgrade links it once).
                t.cardLinked = to.BoolAt(L"cardLinked", false);
                t.historyPromptMessageId = (std::max)(int64_t{ 0 }, to.I64At(L"historyPromptMessageId", 0)); // (std::max): the <windows.h> macro
                t.tabToken = to.StrAt(L"tabToken");
                t.errorAnnounced = to.BoolAt(L"errorAnnounced", false);
                t.backfillPath = to.StrAt(L"backfillPath"); // absent (an older file) = no history replay owed
                t.backfillPos = to.I64At(L"backfillPos", 0);
                t.backfillEnd = to.I64At(L"backfillEnd", 0);
                t.backfillDayKey = to.I64At(L"backfillDayKey", 0);
                // §21.11 (absent in an older file: the default mode, and a name with no glyph yet — renamed once).
                t.verbose = std::clamp(static_cast<int>(to.I64At(L"verbose", -1)), -1, 1);
                t.nameState = std::clamp(static_cast<int>(to.I64At(L"nameState", -1)), -1, static_cast<int>(SessionState::Done));
                t.nameSetMs = to.I64At(L"nameSetMs", 0);
                // §21.13 (absent in an older file: a name with no square yet and an unknown mode — re-squared once, quietly).
                t.nameColor = to.StrAt(L"nameColor");
                t.nameColorMode = std::clamp(static_cast<int>(to.I64At(L"nameColorMode", -1)), -1, static_cast<int>(TabColorMode::NoColor));
                // §21.22 (absent in an older file: the glyph form — renamed once into the icon form, quietly).
                t.stateIcon = to.BoolAt(L"stateIcon", false);
                // §21.12 (absent in an older file: no prompt tracked yet — the next live prompt starts it).
                t.promptMessageId = to.I64At(L"promptMessageId", 0);
                t.promptBotLine = to.BoolAt(L"promptBotLine", false);
                t.promptTsMs = to.I64At(L"promptTsMs", 0);
                t.promptSource = std::clamp(static_cast<int>(to.I64At(L"promptSource", 0)), 0, static_cast<int>(PromptSource::Runner));
                t.promptText = to.StrAt(L"promptText");
                t.promptStatus = std::clamp(static_cast<int>(to.I64At(L"promptStatus", 0)), 0, static_cast<int>(PromptTurnStatus::Interrupted));
                t.promptInterrupted = to.BoolAt(L"promptInterrupted", false);
                s.topics[t.sessionId] = std::move(t);
            }
        }
        if (const auto* newThreads = v.Find(L"newThreads"); newThreads && newThreads->type == json::Value::Type::Obj)
        {
            // §21.25 (absent in an older file: no wizard to resume). Copied as it is: the service reads it at its start.
            s.newThreads = *newThreads;
        }
        return s;
    }

    // ===== topics = tabs: the sync decider ===================================================

    // What the sync needs to know about one session (read from the registry's live set).
    struct TabFact
    {
        std::wstring sessionId;
        std::wstring title;
        SessionState state{ SessionState::Idle };
        bool live{ false }; // has an open tab this run (SessionInfo::live)
        bool inTab{ false }; // a ConPTY hosts it (SessionInfo::tabToken non-empty)
        bool claude{ true }; // AgentKind::Claude (D23: Codex is never listed)
        bool pinned{ false }; // never idle (SessionInfo::pinned)
        // The tab color: ResolveSessionColorHex under the tab-color mode, as the tab, the board and the side panel resolve it
        // ("" = none: the "Remove colors" mode, or a caller that did not resolve it). Since §21.13 the sync fills it for every
        // session: the topic's icon color at creation, and the color square of its name, its card and every title mention.
        std::wstring colorHex;
        // When colorHex last changed as the sync saw it (§21.13; the service's own clock, not persisted): how long the color the
        // topic name would show has held (PickNameRename). 0 = unknown (its first sight in this run): treated as settled.
        int64_t colorSinceMs{ 0 };
        // SessionInfo::lastActivityUnixMs (persisted): the newest hook / activity — and, for a NEW session or a
        // fork, its creation instant (the launch seam stamps it). 0 = never.
        int64_t lastActivityMs{ 0 };
        std::wstring tabToken; // the hosting tab's WT_SESSION (SessionInfo::tabToken; "" = no tab)
        // The conversation this one continues (SessionInfo::continuedFromId: a /clear predecessor, a /handover origin):
        // a /handover-here successor takes its origin's tab on a NEW connection (a new tabToken), so this is how its
        // topic follows the tab. "" = none.
        std::wstring continuedFromId;
        // MostRecentSortKeyMs(session): the side panel's MOST RECENT key, which orders the topic list (§21.8).
        int64_t mostRecentMs{ 0 };
        // SessionInfo::stateChangedAtUnixMs: when the state (or live / started) last changed — how long the state the topic
        // name would show has held (PickStateRename). 0 = unknown: treated as settled.
        int64_t stateSinceMs{ 0 };
    };

    /// <summary>
    /// The one rule behind "topics = tabs" (MESSAGING.md §5.1, D3, revised in §21.6): a topic exists for a
    /// Claude Code session that has a tab, unless the tab is IDLE — and "idle" means Idle AND untouched for the
    /// idle grace. A NEW tab (a fresh session, a fork, a resumed or just-activated one: its last activity is
    /// moments old) is listed at once, before it has any message — the user's "a new session/tab with no new
    /// messages should appear on telegram". Before §21.6 a new tab, which reads Idle until its first prompt, got
    /// its topic only at that prompt, and the prompt itself never reached the thread. A pinned session is listed
    /// whatever its state (§5.3). Restored tabs at an app start keep their old last activity, so an idle one
    /// still gets no topic.
    /// </summary>
    /// <param name="f">The session's facts.</param>
    /// <param name="nowMs">The clock (Unix ms).</param>
    /// <param name="idleGraceMs">How long an untouched Idle tab stays listed (D25); &lt;= 0 = an Idle tab is never listed.</param>
    /// <returns>True when the session should have a topic.</returns>
    inline bool QualifiesForTopic(const TabFact& f, int64_t nowMs, int64_t idleGraceMs) noexcept
    {
        if (!(f.live && f.inTab && f.claude))
        {
            return false;
        }
        if (f.pinned || f.state != SessionState::Idle)
        {
            return true;
        }
        return idleGraceMs > 0 && f.lastActivityMs > 0 && nowMs - f.lastActivityMs < idleGraceMs;
    }

    /// <summary>
    /// When an Idle topic leaves the list (D25): one idle grace after the LATER of the moment the topic first saw
    /// the session Idle and the session's last activity — so a session that was just used (a resumed or activated
    /// tab, a new one) keeps its topic for a whole grace, and one that decayed to Idle long after its last turn
    /// still gets the full grace from the decay on.
    /// </summary>
    /// <param name="idleSinceMs">TopicRecord::idleSinceMs (0 = the topic has not seen it Idle yet).</param>
    /// <param name="lastActivityMs">SessionInfo::lastActivityUnixMs (0 = never).</param>
    /// <param name="idleGraceMs">The idle grace.</param>
    /// <returns>The Unix ms when the topic leaves; 0 when the topic has not seen the session Idle.</returns>
    inline int64_t IdleTopicLeavesAtMs(int64_t idleSinceMs, int64_t lastActivityMs, int64_t idleGraceMs) noexcept
    {
        if (idleSinceMs == 0)
        {
            return 0;
        }
        return (std::max)(idleSinceMs, lastActivityMs) + (std::max)(int64_t{ 0 }, idleGraceMs); // (std::max): the <windows.h> macro
    }

    // What the sync must do to make the topics equal the qualifying tabs.
    struct TopicSyncOp
    {
        enum class Kind
        {
            Create, // the session qualifies and has no topic
            Delete, // the topic's session is closed / left its tab / is not Claude / stayed Idle past the grace
            Rename, // the tab's title changed
            IdleCard, // the session just went Idle: show the Idle card, start the grace clock
            UnidleCard, // the session left Idle within the grace: back to the normal card
            Rekey, // the topic's session closed and ANOTHER session took its tab: the topic follows the tab (§21.6)
        };
        Kind kind{ Kind::Create };
        std::wstring sessionId; // Rekey: the session that now holds the tab (the topic's new key)
        std::wstring fromSessionId; // Rekey: the closed session the topic belonged to
        std::wstring reason; // Delete: "closed" | "idle" | "not a claude tab"
        std::wstring title; // Create / Rename / Rekey: the title to use
        std::wstring colorHex; // Create: the tab color
        int64_t mostRecentMs{ 0 }; // Create: the session's MOST RECENT key (the creates come oldest first, §21.8)
        SessionState state{ SessionState::Idle }; // Create / Rename / Rekey: the state the name's glyph shows (§21.11)
    };

    namespace detail
    {
        // A WT_SESSION in a canonical form for map keys: lower-case (two sources may spell a GUID in different
        // cases — SessionRegistry compares them with TabTokenEq, a case-insensitive compare).
        inline std::wstring TabKey(std::wstring_view token)
        {
            std::wstring k{ token };
            for (auto& c : k)
            {
                c = static_cast<wchar_t>(std::towlower(c));
            }
            return k;
        }

        // Ordinal, case-insensitive "a < b" over UPPER-cased code units — the side panel's title tiebreak
        // (CompareStringOrdinal with ignoreCase, which upper-cases), mirrored without <windows.h>. Identical for
        // ASCII titles; the C runtime's upper-casing of other letters can differ from the system table's.
        inline bool TitleLessCi(std::wstring_view a, std::wstring_view b) noexcept
        {
            const size_t n = a.size() < b.size() ? a.size() : b.size();
            for (size_t i = 0; i < n; ++i)
            {
                const wint_t ca = std::towupper(a[i]);
                const wint_t cb = std::towupper(b[i]);
                if (ca != cb)
                {
                    return ca < cb;
                }
            }
            return a.size() < b.size();
        }
    }

    /// <summary>
    /// The TOPIC LIST's order (§21.8, the user's "ordered by MOST RECENT like the sidemenu does"): the side panel's
    /// MOST RECENT order — `a` ranks ABOVE `b` when its key (MostRecentSortKeyMs, the session's newest turn boundary)
    /// is larger, equal keys by the title, case-insensitive. Telegram lists a chat's topics by their LAST message,
    /// newest on top, and a bot cannot reorder them; so the service places a topic by WHEN it posts in it: it creates
    /// topics — and posts into several topics in one pass — in the REVERSE of this order (the lowest-ranked first), so
    /// the highest-ranked lands on top.
    /// </summary>
    /// <param name="aKey">a's MOST RECENT key.</param>
    /// <param name="aTitle">a's title.</param>
    /// <param name="bKey">b's MOST RECENT key.</param>
    /// <param name="bTitle">b's title.</param>
    /// <returns>True when a is listed above b.</returns>
    inline bool TopicRanksAbove(int64_t aKey, std::wstring_view aTitle, int64_t bKey, std::wstring_view bTitle) noexcept
    {
        if (aKey != bKey)
        {
            return aKey > bKey;
        }
        return detail::TitleLessCi(aTitle, bTitle);
    }

    /// <summary>
    /// Plans the topic edits that make the topic set equal the listed tabs (M8, §21.6). Pure; the service applies
    /// the ops and updates the records (IdleCard sets idleSinceMs, UnidleCard clears it, Rekey moves the record).
    /// </summary>
    /// <param name="facts">Every LIVE session's facts (a session absent from the list is closed).</param>
    /// <param name="topics">The topics that exist (the state file).</param>
    /// <param name="nowMs">The clock.</param>
    /// <param name="idleGraceMs">How long an Idle, untouched tab's topic stays before it is deleted (D25).</param>
    /// <returns>The ops, deterministic: per topic in session-id order a Rekey or a Delete or its card / title
    /// changes, then the creates in the REVERSE of the topic list's order (TopicRanksAbove — the lowest-ranked first),
    /// so that the newest topic, created last, lands on top (§21.8).</returns>
    /// <remarks>
    /// PER TAB (§21.6): a tab has at most one topic. When a topic's session is CLOSED and another live Claude
    /// session without a topic now holds the SAME tab (TopicRecord::tabToken == TabFact::tabToken — a /clear, a
    /// /resume in the tab), or CONTINUES the closed one (TabFact::continuedFromId — a /handover-here successor, which
    /// takes the tab on a new connection), the topic is REKEYED to that session instead of deleted, and that session
    /// gets no topic of its own (the tab match first). And while a topic's session still holds a tab, no other session
    /// in that tab gets a new topic (the moment between a conversation switch and the old record closing): the next
    /// sync, with the old session closed, carries the topic over.
    /// </remarks>
    /// <exception cref="std::bad_alloc">If the ops cannot be allocated.</exception>
    inline std::vector<TopicSyncOp> PlanTopicSync(const std::vector<TabFact>& facts,
                                                  const std::map<std::wstring, TopicRecord>& topics,
                                                  int64_t nowMs,
                                                  int64_t idleGraceMs)
    {
        std::vector<TopicSyncOp> ops;
        std::map<std::wstring, const TabFact*> byId;
        for (const auto& f : facts)
        {
            byId[f.sessionId] = &f;
        }
        // The tabs the topics know, and the live tabs held by a session with no topic (the carry-over targets;
        // the first in session-id order when — only in a moment of re-binding — two claim one tab).
        std::map<std::wstring, std::wstring> heirByTab; // tab key -> session id
        std::map<std::wstring, std::wstring> heirByOrigin; // the session it continues -> session id
        for (const auto& [sid, f] : byId)
        {
            if (f->live && f->inTab && f->claude && topics.find(sid) == topics.end())
            {
                if (!f->tabToken.empty())
                {
                    heirByTab.emplace(detail::TabKey(f->tabToken), sid);
                }
                if (!f->continuedFromId.empty())
                {
                    heirByOrigin.emplace(f->continuedFromId, sid);
                }
            }
        }
        std::map<std::wstring, bool> tabsWithTopic; // tab key -> true: a live session in that tab has a topic
        std::map<std::wstring, bool> rekeyedTo; // session id -> true: it received a topic this pass
        for (const auto& [sid, t] : topics)
        {
            const auto it = byId.find(sid);
            const TabFact* f = it == byId.end() ? nullptr : it->second;
            if (!f || !f->live || !f->inTab || !f->claude)
            {
                if (!f || !f->live)
                {
                    // The heir: the session now in the topic's tab, else the one that continues the closed session.
                    std::wstring heirId;
                    if (!t.tabToken.empty())
                    {
                        if (const auto h = heirByTab.find(detail::TabKey(t.tabToken)); h != heirByTab.end() && !rekeyedTo.count(h->second))
                        {
                            heirId = h->second;
                        }
                    }
                    if (heirId.empty())
                    {
                        if (const auto h = heirByOrigin.find(sid); h != heirByOrigin.end() && !rekeyedTo.count(h->second))
                        {
                            heirId = h->second;
                        }
                    }
                    if (!heirId.empty())
                    {
                        const TabFact* heir = byId.at(heirId);
                        TopicSyncOp op;
                        op.kind = TopicSyncOp::Kind::Rekey;
                        op.sessionId = heir->sessionId;
                        op.fromSessionId = sid;
                        op.title = heir->title.empty() ? heir->sessionId.substr(0, 8) : heir->title;
                        op.colorHex = heir->colorHex;
                        op.state = heir->state;
                        rekeyedTo[heir->sessionId] = true;
                        ops.push_back(std::move(op));
                        continue;
                    }
                }
                TopicSyncOp op;
                op.kind = TopicSyncOp::Kind::Delete;
                op.sessionId = sid;
                op.reason = !f || !f->live ? L"closed" : (!f->claude ? L"not a claude tab" : L"left its tab");
                ops.push_back(std::move(op));
                continue;
            }
            if (!f->tabToken.empty())
            {
                tabsWithTopic[detail::TabKey(f->tabToken)] = true;
            }
            const bool idle = !f->pinned && f->state == SessionState::Idle;
            if (idle)
            {
                // Idle: the grace clock decides, counted from the later of "first seen Idle" and the last activity.
                if (t.idleSinceMs == 0)
                {
                    TopicSyncOp op;
                    op.kind = TopicSyncOp::Kind::IdleCard;
                    op.sessionId = sid;
                    ops.push_back(std::move(op));
                }
                else if (idleGraceMs <= 0 || nowMs >= IdleTopicLeavesAtMs(t.idleSinceMs, f->lastActivityMs, idleGraceMs))
                {
                    TopicSyncOp op;
                    op.kind = TopicSyncOp::Kind::Delete;
                    op.sessionId = sid;
                    op.reason = L"idle";
                    ops.push_back(std::move(op));
                    continue;
                }
            }
            else if (t.idleSinceMs != 0)
            {
                TopicSyncOp op;
                op.kind = TopicSyncOp::Kind::UnidleCard;
                op.sessionId = sid;
                ops.push_back(std::move(op));
            }
            if (!f->title.empty() && f->title != t.title)
            {
                TopicSyncOp op;
                op.kind = TopicSyncOp::Kind::Rename;
                op.sessionId = sid;
                op.title = f->title;
                op.state = f->state;
                ops.push_back(std::move(op));
            }
        }
        // Creates, in the REVERSE of the topic list's order (§21.8): Telegram puts the topic that got a message last on
        // top, and a new topic's first messages are its creation and its card, so the topic that ranks highest must be
        // made last. The service takes them in this order across ticks (it makes a few per tick), so a list made from
        // nothing — the first start, a recreation — comes out in MOST RECENT order.
        std::vector<TopicSyncOp> creates;
        for (const auto& [sid, f] : byId)
        {
            if (topics.find(sid) != topics.end() || rekeyedTo.count(sid) || !QualifiesForTopic(*f, nowMs, idleGraceMs))
            {
                continue;
            }
            if (!f->tabToken.empty() && tabsWithTopic.count(detail::TabKey(f->tabToken)))
            {
                continue; // per tab: this tab's topic belongs to a session that still holds it (see remarks)
            }
            TopicSyncOp op;
            op.kind = TopicSyncOp::Kind::Create;
            op.sessionId = sid;
            op.title = f->title.empty() ? sid.substr(0, 8) : f->title;
            op.colorHex = f->colorHex;
            op.mostRecentMs = f->mostRecentMs;
            op.state = f->state;
            creates.push_back(std::move(op));
        }
        std::stable_sort(creates.begin(), creates.end(), [](const TopicSyncOp& x, const TopicSyncOp& y) {
            return TopicRanksAbove(y.mostRecentMs, y.title, x.mostRecentMs, x.title); // x first when y ranks above it
        });
        for (auto& op : creates)
        {
            ops.push_back(std::move(op));
        }
        return ops;
    }

    // ===== the topic name's state glyph (§21.11) ===============================================

    /// <summary>
    /// The glyph a topic's NAME starts with for its session's state (§21.11, the user's "an emoji representing the status
    /// of the tab/thread similarly to how tabs present"): the tab strip's status dot as an emoji — Running 🔵, Waiting for
    /// you 🟡, Needs approval 🟠, Error 🔴, Done (claude exited) 🟢, Idle ⚪ (the tab's dot is gray; Telegram has no gray
    /// circle).
    /// </summary>
    /// <param name="s">The state.</param>
    /// <returns>The glyph (a static string).</returns>
    inline const wchar_t* StateGlyph(SessionState s) noexcept
    {
        switch (s)
        {
        case SessionState::Running:
            return L"\U0001F535";
        case SessionState::WaitingForInput:
            return L"\U0001F7E1";
        case SessionState::NeedsApproval:
            return L"\U0001F7E0";
        case SessionState::Error:
            return L"\U0001F534";
        case SessionState::Done:
            return L"\U0001F7E2";
        case SessionState::Idle:
        default:
            return L"\x26AA";
        }
    }

    /// <summary>
    /// A state's topic ICON (§21.22): one of Telegram's forum-topic icons — the 112 custom emoji that getForumTopicIconStickers
    /// lists and any bot may set without Premium (read 2026-10-18; both bots got the same ids). Why the state moved into the
    /// icon: Telegram draws a topic that has no icon of its own as a bubble in its creation color with the NAME'S FIRST
    /// CHARACTER inside (Telegram iOS and Web take the first grapheme), so a name that starts with the state glyph showed the
    /// state twice, in the icon and in the name — the user's "two color circles" (2026-10-18). The icon is set apart from the
    /// name, and editForumTopic can change it at any time (a topic's `icon_color` is fixed at creation), so the state goes
    /// there and the name keeps the color square and the title (TopicDisplayName's stateIcon form). None of the 112 icons is a
    /// colored circle (the circles are custom emoji only a Premium owner's bot may use), so the states read as shapes:
    /// Running ⚡️, Waiting for you 💬, Needs approval ❓, Error ‼️, Done (claude exited) 🏁, Idle ☕️ — six distinct icons, and
    /// none of the marks of the prompt line's status (§21.12.3: ✅ answered, ⏹ ended), so "done" never reads as "answered".
    /// </summary>
    struct TopicStateIcon
    {
        const wchar_t* emoji; // what the icon shows (the logs, the docs)
        const wchar_t* customEmojiId; // Telegram's custom_emoji_id, for createForumTopic / editForumTopic `icon_custom_emoji_id`
    };

    /// <summary>The topic icon that shows a session's state (§21.22).</summary>
    /// <param name="s">The state.</param>
    /// <returns>The icon (static strings).</returns>
    inline TopicStateIcon StateTopicIcon(SessionState s) noexcept
    {
        switch (s)
        {
        case SessionState::Running:
            return { L"\x26A1\xFE0F", L"5312016608254762256" };
        case SessionState::WaitingForInput:
            return { L"\U0001F4AC", L"5417915203100613993" };
        case SessionState::NeedsApproval:
            return { L"\x2753", L"5377316857231450742" };
        case SessionState::Error:
            return { L"\x203C\xFE0F", L"5377498341074542641" };
        case SessionState::Done:
            return { L"\U0001F3C1", L"5408906741125490282" };
        case SessionState::Idle:
        default:
            return { L"\x2615\xFE0F", L"5350392020785437399" };
        }
    }

    /// <summary>
    /// Whether Telegram refused a topic call because of its ICON (§21.22): a 400 whose description names the icon, the custom
    /// emoji, or the document behind it ("icon", "emoji", "document_invalid", any case). The service then puts the state glyph
    /// back into the topic names for the rest of the run, so a topic never loses its state. Every other answer — a 429, "message
    /// thread not found", "topic not modified" — is not an icon refusal. No allocation: a refusal is read on the tick thread.
    /// </summary>
    /// <param name="httpStatus">The call's HTTP status.</param>
    /// <param name="description">Telegram's description of the refusal.</param>
    /// <returns>True for an icon refusal.</returns>
    inline bool TopicIconRefused(int httpStatus, std::wstring_view description) noexcept
    {
        if (httpStatus != 400)
        {
            return false;
        }
        const auto containsCi = [description](std::wstring_view needle) {
            if (needle.size() > description.size())
            {
                return false;
            }
            for (size_t at = 0; at + needle.size() <= description.size(); ++at)
            {
                size_t i = 0;
                for (; i < needle.size(); ++i)
                {
                    wchar_t c = description[at + i];
                    if (c >= L'A' && c <= L'Z')
                    {
                        c = static_cast<wchar_t>(c - L'A' + L'a'); // ASCII folding: Telegram's descriptions are ASCII
                    }
                    if (c != needle[i])
                    {
                        break;
                    }
                }
                if (i == needle.size())
                {
                    return true;
                }
            }
            return false;
        };
        return containsCi(L"icon") || containsCi(L"emoji") || containsCi(L"document_invalid");
    }

    // The most UTF-16 units of a title in a topic's name (Telegram allows 128 characters; the glyph and a space go first).
    inline constexpr size_t kTopicTitleMaxChars = 120;

    /// <summary>
    /// A topic's name (§21.11): the state glyph, a space, the title — a title past kTopicTitleMaxChars ends in "…" (never cut
    /// between the halves of a surrogate pair). A title with line breaks is put on ONE line first (TgOneLineTitle, 2026-10-18):
    /// a topic name is one line, and the name sent before carried the tab's raw CR (`🔵 🟪 BetterClipboard\ron-launch`). Every
    /// overload builds on this one, so every topic name follows the rule.
    /// </summary>
    /// <param name="s">The session's state.</param>
    /// <param name="title">The tab title, as typed ("" = the caller's fallback, e.g. the short session id).</param>
    /// <returns>The name for createForumTopic / editForumTopic.</returns>
    /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
    inline std::wstring TopicDisplayName(SessionState s, std::wstring_view title)
    {
        std::wstring t = TgOneLineTitle(title);
        if (t.size() > kTopicTitleMaxChars)
        {
            size_t cut = kTopicTitleMaxChars - 1;
            if (cut > 0 && t[cut - 1] >= 0xD800 && t[cut - 1] <= 0xDBFF)
            {
                --cut;
            }
            t = t.substr(0, cut) + L"\x2026";
        }
        return std::wstring{ StateGlyph(s) } + L" " + t;
    }

    /// <summary>
    /// A topic's name with the tab's COLOR (§21.13, the user's "State, then color"): the state glyph, a space, the color
    /// square, a space, the title — `🟡 🟦 agent manager tab`. Telegram has no colored text or background in a topic name or a
    /// message (HTML `span style` / `mark` are refused, a rich message's `==mark==` is a highlight of the reader's own color,
    /// custom emoji need a Premium owner — measured 2026-10-17), so a square emoji in the tab's color family is the one marker
    /// that shows everywhere and follows a change. An empty square is the 2-argument name exactly (the "Remove colors" mode).
    /// The title is cut like the 2-argument name: the glyph, the square and two spaces take at most 6 of Telegram's 128
    /// characters.
    /// </summary>
    /// <param name="s">The session's state.</param>
    /// <param name="colorSquare">TgTitleColorSquare of the tab color ("" = no square).</param>
    /// <param name="title">The tab title ("" = the caller's fallback, e.g. the short session id).</param>
    /// <returns>The name for createForumTopic / editForumTopic.</returns>
    /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
    inline std::wstring TopicDisplayName(SessionState s, std::wstring_view colorSquare, std::wstring_view title)
    {
        std::wstring name = TopicDisplayName(s, title);
        if (!colorSquare.empty())
        {
            // After the glyph and its space: the 2-argument name is "<glyph> <title>", so the square goes at that offset (the
            // glyph is one or two UTF-16 units, StateGlyph's table).
            const size_t at = std::wstring_view{ StateGlyph(s) }.size() + 1;
            name.insert(at, std::wstring{ colorSquare } + L" ");
        }
        return name;
    }

    /// <summary>
    /// A topic's name in either FORM (§21.22): with `stateIcon` the topic's ICON shows the state (StateTopicIcon), so the name
    /// is the color square, a space, the title — `🟦 agent manager tab` (no square: the title alone); without it, the
    /// 3-argument name exactly (`🟡 🟦 agent manager tab`, the glyph form a topic falls back to when Telegram refuses an icon).
    /// The title is cut like the 3-argument name, so the icon form never runs past Telegram's 128 characters either.
    /// </summary>
    /// <param name="s">The session's state (the glyph form's first character; the icon form leaves it to the icon).</param>
    /// <param name="colorSquare">TgTitleColorSquare of the tab color ("" = no square).</param>
    /// <param name="title">The tab title ("" = the caller's fallback, e.g. the short session id).</param>
    /// <param name="stateIcon">True when the topic's icon shows the state (the name then carries no glyph).</param>
    /// <returns>The name for createForumTopic / editForumTopic.</returns>
    /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
    inline std::wstring TopicDisplayName(SessionState s, std::wstring_view colorSquare, std::wstring_view title, bool stateIcon)
    {
        std::wstring name = TopicDisplayName(s, colorSquare, title);
        if (stateIcon)
        {
            // The 3-argument name starts "<glyph> " (StateGlyph's one or two UTF-16 units and a space): drop exactly that.
            name.erase(0, std::wstring_view{ StateGlyph(s) }.size() + 1);
        }
        return name;
    }

    /// <summary>
    /// A title MENTION with the tab's color (§21.13): the color square, a space, the escaped title — what a message that names
    /// a session shows in its classic (HTML) form, so a title reads the same color in every place. No square = the escaped
    /// title alone. Since §21.15 the builders write a title TOKEN instead (TgTitleToken, TgTitleForm::Plain), which renders to
    /// exactly this text in a classic message and to a CHIP in a rich one (TgRenderTitleTokensClassic / TgRichHtmlFromClassic).
    /// </summary>
    /// <param name="colorSquare">TgTitleColorSquare of the tab color ("" = none).</param>
    /// <param name="title">The title as is: put on one line (TgOneLineTitle, as a token's title is) and escaped here.</param>
    /// <returns>Telegram HTML (no tags: the caller wraps it in &lt;b&gt; where it wants).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring TitleMentionHtml(std::wstring_view colorSquare, std::wstring_view title)
    {
        const std::wstring escaped = TgEscapeHtml(TgOneLineTitle(title));
        return colorSquare.empty() ? escaped : std::wstring{ colorSquare } + L" " + escaped;
    }

    // The most UTF-16 units of a title the change line names (BuildTopicChangeNoticeHtml), the ellipsis included: the Search
    // answer's pick menu shows a title the same way (BuildSearchPickMenuHtml).
    inline constexpr size_t kTopicChangeTitleMaxChars = 200;

    /// <summary>
    /// Whether a topic's change line has a TITLE change to tell (2026-10-18): the two titles as the line PRINTS them — on one
    /// line and cut (TgTitleLine) — differ, and the new one is not blank. The line used to compare the titles as typed while it
    /// printed only their first lines, so a second line typed into a tab title was told as "“🟪 BetterClipboard” changed the
    /// title to “🟪 BetterClipboard”". Now a change the line cannot show tells nothing — a space retyped as a line break, a
    /// blank at an end, a difference past the cut — while the topic's NAME still follows the tab and a color change still gets
    /// its own line. The service logs by the same answer, so "told: its title changed" means a title line went out.
    /// </summary>
    /// <param name="oldTitle">The title the topic's name showed, as typed ("" = unknown).</param>
    /// <param name="newTitle">The title the topic's name shows now, as typed.</param>
    /// <returns>True when the line names a new title.</returns>
    /// <exception cref="std::bad_alloc">If a printed title cannot be allocated.</exception>
    inline bool TopicChangeShowsTitle(std::wstring_view oldTitle, std::wstring_view newTitle)
    {
        const std::wstring shownNew = TgTitleLine(newTitle, kTopicChangeTitleMaxChars);
        return !shownNew.empty() && TgTitleLine(oldTitle, kTopicChangeTitleMaxChars) != shownNew;
    }

    /// <summary>
    /// The line a topic gets when its tab's TITLE or COLOR changed (§21.13, the user's "send a message like whatsapp does for
    /// group chat name changed", in their words: "&lt;from&gt;" changed the title to "&lt;to&gt;"). It quotes the title the
    /// topic showed, with its color square, and says what it became: `“🟦 old” changed the title to “🟦 new”` (a color that
    /// changed at the same time shows in the two squares), or `“🟦 title” changed the color to 🟩` when only the color
    /// changed. Italic: a line about the topic, not a message of the conversation. Telegram's own "topic renamed" notice is
    /// deleted by the service — it names no old title and shows no color.
    /// The titles are title TOKENS (§21.15): the classic render is the text above, byte for byte; a rich message shows each
    /// title as a CHIP in its color (the quotes give way to the button), and the color line shows the new color as a chip too
    /// — `[🟦 tab] changed the color to [🟩 tab]` — where the classic line has the bare square.
    /// A title is PRINTED on one line and cut (TgTitleLine, kTopicChangeTitleMaxChars), and the line tells a title change only
    /// when those printed titles differ (TopicChangeShowsTitle, 2026-10-18): a tab title that got a second line reads
    /// `“🟪 BetterClipboard” changed the title to “🟪 BetterClipboard on-launch”`, and never again "… to “🟪 BetterClipboard”".
    /// </summary>
    /// <param name="oldTitle">The title the topic's name showed, as typed ("" or blank = unknown: the line names the new title
    /// only).</param>
    /// <param name="oldSquare">The color square the name showed ("" = none).</param>
    /// <param name="newTitle">The title the topic's name shows now, as typed.</param>
    /// <param name="newSquare">The color square the name shows now ("" = none).</param>
    /// <param name="threadId">§21.17: the topic the line is posted in — every title of the line links to it (0, the default = no
    /// links).</param>
    /// <returns>Telegram HTML with title tokens; "" when the printed title did not change and the color did not change from one
    /// square to another (a square that only appears or goes — a topic named before §21.13, the "Remove colors" mode — is not a
    /// change worth a line).</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring BuildTopicChangeNoticeHtml(std::wstring_view oldTitle, std::wstring_view oldSquare, std::wstring_view newTitle, std::wstring_view newSquare, int64_t threadId = 0)
    {
        // A mention names a title as the line prints it: on one line, cut like the Search pick menu's titles (an ellipsis past
        // 200 units). Classic, it reads “🟦 title” (TgTitleForm::Quoted).
        const std::wstring shownOld = TgTitleLine(oldTitle, kTopicChangeTitleMaxChars);
        const std::wstring shownNew = TgTitleLine(newTitle, kTopicChangeTitleMaxChars);
        const auto mention = [threadId](std::wstring_view square, std::wstring_view shown) {
            return TgTitleToken(TgTitleForm::Quoted, square, shown, threadId);
        };
        // The decision is taken over the PRINTED titles (one rule with the service's log line): titles that differ only where
        // the line cannot show it would otherwise be told as "“X” changed the title to “X”".
        if (TopicChangeShowsTitle(oldTitle, newTitle))
        {
            if (shownOld.empty())
            {
                return L"<i>The title is now " + mention(newSquare, shownNew) + L"</i>";
            }
            return L"<i>" + mention(oldSquare, shownOld) + L" changed the title to " + mention(newSquare, shownNew) + L"</i>";
        }
        if (!oldSquare.empty() && !newSquare.empty() && oldSquare != newSquare)
        {
            // The new color: the bare square in a classic line, the title as a chip of the new color in a rich one
            // (TgTitleForm::Square).
            const std::wstring& shown = shownNew.empty() ? shownOld : shownNew;
            return L"<i>" + mention(oldSquare, shown) + L" changed the color to " + TgTitleToken(TgTitleForm::Square, newSquare, shown, threadId) + L"</i>";
        }
        return {};
    }

    // A state the topic name follows must have held this long (a Running ↔ Waiting flicker of a short turn renames nothing),
    // and two renames of one topic are this far apart at least. Each rename costs two calls in the chat (the edit, and the
    // delete of the "topic renamed" notice Telegram posts), so the names follow the fleet with a lag rather than spend the
    // chat's budget (the service renames only while the last minute's calls leave room for both: NameRenameFits).
    inline constexpr int64_t kNameStateSettleMs = 8 * 1000;
    inline constexpr int64_t kNameRenameGapMs = 30 * 1000;

    /// <summary>
    /// What one topic RENAME costs in the chat's call window: the editForumTopic call, and the deleteMessage of the "topic
    /// renamed" notice Telegram posts into the topic (the poll thread removes it when it arrives).
    /// </summary>
    inline constexpr size_t kNameRenameCallCost = 2;

    /// <summary>
    /// How long the HISTORY REPLAY holds its next message for a rename that is due but finds no room, at most (2026-10-17).
    /// Why the replay holds at all: it keeps the chat's call window at its cap for hours (§21.9 — ~18 calls a minute while
    /// ~2,000 turns of ~411 topics replay), and a rename that waited for "room" behind it never went. Production renamed 1 of
    /// ~411 topics in five hours, so a session that ran or failed kept its topic at 🟡 all afternoon (the user's report: "rename
    /// topic when the topic has a running session (blue circle) instead of the yellow one or as red when error"). The window
    /// frees a call every few seconds, so the hold normally lasts seconds; the bound only matters when the LIVE traffic alone
    /// keeps the window full (it never checks the window), and then the replay goes on after this long.
    /// </summary>
    inline constexpr int64_t kNameRenameHoldMaxMs = 90 * 1000;

    /// <summary>
    /// Whether a due rename fits the chat's call window now: the calls of the last minute plus the rename's own
    /// (kNameRenameCallCost) stay within the budget the history replay is held to. Renames go BEFORE the replay (the replay
    /// waits for them, HistoryYieldsToRename) and after the live traffic (which goes first by itself).
    /// </summary>
    /// <param name="callsInLastMinute">The call window's size: calls into the chat in the last 60 s, any method.</param>
    /// <param name="budgetPerMinute">The window's budget (the service's kHistoryCallsPerMinute).</param>
    /// <returns>True when the rename may be made now.</returns>
    inline constexpr bool NameRenameFits(size_t callsInLastMinute, size_t budgetPerMinute) noexcept
    {
        return callsInLastMinute + kNameRenameCallCost <= budgetPerMinute;
    }

    /// <summary>
    /// Whether the history replay holds its next message for a waiting rename: a rename has been due without room since
    /// `renameWaitingSinceMs`, less than kNameRenameHoldMaxMs ago. The service sets the stamp when a due rename finds the window
    /// full and clears it when the rename was made or none is due, so the replay never holds for a rename that is gone.
    /// </summary>
    /// <param name="renameWaitingSinceMs">When the due rename first found no room (GetTickCount64 ms; 0 = no rename waits).</param>
    /// <param name="nowMs">The same clock, now.</param>
    /// <returns>True while the replay must leave the next free call to the rename.</returns>
    inline constexpr bool HistoryYieldsToRename(int64_t renameWaitingSinceMs, int64_t nowMs) noexcept
    {
        return renameWaitingSinceMs > 0 && nowMs >= renameWaitingSinceMs && nowMs - renameWaitingSinceMs < kNameRenameHoldMaxMs;
    }

    /// <summary>
    /// A 429 whose retry_after is longer than this PARKS the refused method instead of holding every call of the bot
    /// (FloodParking). A shorter wait is still slept through by the thread that met it, which keeps the outbox's order.
    /// </summary>
    inline constexpr int kFloodParkAfterSec = 60;

    /// <summary>
    /// The LONG-FLOOD PARKING (2026-10-17): which methods Telegram's flood control holds, and until when. Telegram answers a call
    /// over the chat's budget with 429 and a retry_after, and the send loop used to sleep that whole time — every call, every
    /// topic — while its log line said "live messages go on". One editMessageText answered with retry_after 6542 s (production,
    /// 2026-10-17 17:00:59) and the bot posted nothing for an hour and 49 minutes. Now a long wait parks only the refused
    /// METHOD (its queued items go after the deadline; every other method goes on), and a long 429 on ANOTHER method while one
    /// is parked means the whole chat is held: every method parks until the later deadline (ParkForFlood). Clocks are
    /// GetTickCount64 ms. The service keeps one under its mutex; the rules are pure so the m5 suite pins them.
    /// </summary>
    struct FloodParking
    {
        std::map<std::wstring, int64_t> methodUntilMs; // a parked method -> its deadline
        int64_t allUntilMs{ 0 }; // every method is parked until then (0 = none)
    };

    /// <summary>The deadline that holds `method` now: the chat-wide one, else the method's own; 0 when it may be called.</summary>
    /// <param name="p">The parking.</param>
    /// <param name="method">The Bot API method about to be called ("sendDocument" for a document upload).</param>
    /// <param name="nowMs">The clock (GetTickCount64 ms).</param>
    /// <returns>The later deadline that holds the method, or 0.</returns>
    inline int64_t FloodParkedUntil(const FloodParking& p, const std::wstring& method, int64_t nowMs)
    {
        if (p.allUntilMs > nowMs)
        {
            return p.allUntilMs;
        }
        if (const auto it = p.methodUntilMs.find(method); it != p.methodUntilMs.end() && it->second > nowMs)
        {
            return it->second;
        }
        return 0;
    }

    /// <summary>
    /// Records a long 429 (retry_after above kFloodParkAfterSec): parks `method` until now + retry_after, or — when another
    /// method is parked still — every method until the latest of the deadlines (Telegram holds the whole chat, not one method:
    /// calling on would only collect more refusals). Expired entries are dropped on the way.
    /// </summary>
    /// <param name="p">The parking to update.</param>
    /// <param name="method">The method Telegram refused.</param>
    /// <param name="retryAfterSec">Telegram's retry_after (seconds; values below 1 count as 1).</param>
    /// <param name="nowMs">The clock (GetTickCount64 ms).</param>
    /// <returns>True when the parking now holds every method.</returns>
    /// <exception cref="std::bad_alloc">If the map cannot grow.</exception>
    inline bool ParkForFlood(FloodParking& p, const std::wstring& method, int retryAfterSec, int64_t nowMs)
    {
        const int64_t until = nowMs + static_cast<int64_t>((std::max)(1, retryAfterSec)) * 1000; // (std::max): the <windows.h> macro
        int64_t otherUntil = 0;
        for (auto it = p.methodUntilMs.begin(); it != p.methodUntilMs.end();)
        {
            if (it->second <= nowMs)
            {
                it = p.methodUntilMs.erase(it); // expired: it holds nothing any more
                continue;
            }
            if (it->first != method)
            {
                otherUntil = (std::max)(otherUntil, it->second);
            }
            ++it;
        }
        if (otherUntil > 0 || p.allUntilMs > nowMs)
        {
            p.allUntilMs = (std::max)({ p.allUntilMs, until, otherUntil });
            return true;
        }
        auto& mine = p.methodUntilMs[method];
        mine = (std::max)(mine, until);
        return false;
    }

    /// <summary>
    /// What an editMessageText edits, from its outbox key, for the traffic census's `edits(…)` part: "card" (a topic card,
    /// key "card:&lt;session&gt;"), "prompt" (a §21.12 prompt status, "pl:&lt;message&gt;"), "worklog" (a verbose topic's
    /// work log, "wl:&lt;session&gt;"), "directory" (the §21.16 pinned directory, "dir:edit"), else "other" (an edit with no
    /// key, or a key of a kind added later).
    /// </summary>
    /// <param name="key">The outbox item's key ("" = none).</param>
    /// <returns>The kind's census name.</returns>
    inline const wchar_t* EditKindOfKey(std::wstring_view key) noexcept
    {
        if (key.starts_with(L"card:"))
        {
            return L"card";
        }
        if (key.starts_with(L"pl:"))
        {
            return L"prompt";
        }
        if (key.starts_with(L"wl:"))
        {
            return L"worklog";
        }
        if (key.starts_with(L"dir:"))
        {
            return L"directory";
        }
        return L"other";
    }
    // §21.13: a switch of the tab-color mode re-colors every tab, and the Individual mode deals new colors moments after it.
    // A color change this soon after the service saw the mode change — or this soon after the topic was made (a new tab's
    // first colors) — is not the user's recolor of a tab, so its topic changes square QUIETLY (NoticeOldSquare), and only a
    // mode that has held this long is stamped on a topic whose square it does not change (the service's silent
    // TopicRecord::nameColorMode update).
    inline constexpr int64_t kColorModeSettleMs = 60 * 1000;

    /// <summary>
    /// The topic whose NAME should change next to show its session's state (§21.11): one whose name shows another state (or
    /// none yet), whose session is live in a tab, whose state has held for kNameStateSettleMs, and whose last rename is
    /// kNameRenameGapMs old. The states that need the user go first (Needs approval, Error, Waiting for you, then the rest),
    /// then the state that has held the longest. A title change is not this function's: PlanTopicSync's Rename sets the title
    /// and the glyph together. Since §21.13 the service picks with PickNameRename, which applies this rule first and the
    /// color square's after it; this state-only form stays the rule's reference (the m5 suite pins it).
    /// </summary>
    /// <param name="facts">The live sessions (the sync's facts, with stateSinceMs).</param>
    /// <param name="topics">The topic map.</param>
    /// <param name="nowMs">The clock (Unix ms).</param>
    /// <returns>The session id of the topic to rename; "" when none is due.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring PickStateRename(const std::vector<TabFact>& facts, const std::map<std::wstring, TopicRecord>& topics, int64_t nowMs)
    {
        const auto urgency = [](SessionState s) {
            switch (s)
            {
            case SessionState::NeedsApproval:
                return 0;
            case SessionState::Error:
                return 1;
            case SessionState::WaitingForInput:
                return 2;
            default:
                return 3;
            }
        };
        const TabFact* best = nullptr;
        for (const auto& f : facts)
        {
            if (!f.live || !f.inTab || !f.claude)
            {
                continue;
            }
            const auto it = topics.find(f.sessionId);
            if (it == topics.end() || it->second.threadId <= 0)
            {
                continue;
            }
            const TopicRecord& t = it->second;
            if (t.nameState == static_cast<int>(f.state))
            {
                continue;
            }
            if (f.stateSinceMs > 0 && nowMs - f.stateSinceMs < kNameStateSettleMs)
            {
                continue; // not settled yet
            }
            if (t.nameSetMs > 0 && nowMs - t.nameSetMs < kNameRenameGapMs)
            {
                continue; // renamed lately
            }
            if (!best || urgency(f.state) < urgency(best->state) || (urgency(f.state) == urgency(best->state) && f.stateSinceMs < best->stateSinceMs))
            {
                best = &f;
            }
        }
        return best ? best->sessionId : std::wstring{};
    }

    /// <summary>The topic PickNameRename chose, and what of its name is out of date.</summary>
    struct NameRenamePick
    {
        std::wstring sessionId; // "" = no rename is due
        bool state{ false }; // the topic shows another state than the session's (its icon, or its name's glyph)
        bool color{ false }; // the name's square differs from the tab's color square (§21.13)
        bool form{ false }; // the topic shows its state the other way than wanted: the glyph in the name vs the icon (§21.22)
    };

    /// <summary>
    /// The topic whose NAME should change next (§21.11 + §21.13): PickStateRename's rule for the state glyph, and the same for
    /// the COLOR SQUARE — a name whose square differs from its tab's (TgTitleColorSquare of TabFact::colorHex, a changed tab
    /// color, a topic named before §21.13, a switch of the tab-color mode) is due once that color has held kNameStateSettleMs
    /// (TabFact::colorSinceMs; an inferred folder that flips back and forth renames nothing). Every rename sets the whole name,
    /// so one call fixes both. The order: the states that need the user first (as PickStateRename), then a real color change
    /// of a tab (the topic's square was resolved under `colorMode`: its topic gets a line about it), then the quiet re-squares
    /// (a topic named before §21.13 or under another mode), each the change that has held the longest first. A title change is
    /// not this function's: PlanTopicSync's Rename sets the title, the glyph and the square at once.
    /// Since §21.22 the state may be the topic's ICON instead of the name's glyph (TopicRecord::stateIcon): a topic that shows
    /// its state the other way than `stateIcons` asks — a glyph-form name made before §21.22, or an icon after Telegram refused
    /// one in this run — is due too, as a QUIET change of the same rank as a quiet re-square, once its state has held
    /// kNameStateSettleMs (so the call carries a settled state). The same call fixes a state, a square and the form at once.
    /// </summary>
    /// <param name="facts">The live sessions (the sync's facts, with stateSinceMs, colorHex and colorSinceMs).</param>
    /// <param name="topics">The topic map.</param>
    /// <param name="nowMs">The clock (Unix ms).</param>
    /// <param name="colorMode">The tab-color mode the facts' colors were resolved under.</param>
    /// <param name="stateIcons">True when the topics' icons should show the states (the service's state icons, §21.22: on until
    /// Telegram refuses an icon in a run); false = the glyph form, the rule before §21.22 (the default, which the m5 suite's
    /// §21.11–§21.13 checks pin).</param>
    /// <returns>The topic to rename and why; an empty session id when none is due.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline NameRenamePick PickNameRename(const std::vector<TabFact>& facts, const std::map<std::wstring, TopicRecord>& topics, int64_t nowMs, TabColorMode colorMode, bool stateIcons = false)
    {
        // 0-3: a state the name must show (PickStateRename's urgency); 4: the tab's color changed; 5: a quiet re-square.
        const auto urgency = [](SessionState s) {
            switch (s)
            {
            case SessionState::NeedsApproval:
                return 0;
            case SessionState::Error:
                return 1;
            case SessionState::WaitingForInput:
                return 2;
            default:
                return 3;
            }
        };
        NameRenamePick pick;
        int bestRank = 0;
        int64_t bestSince = 0;
        for (const auto& f : facts)
        {
            if (!f.live || !f.inTab || !f.claude)
            {
                continue;
            }
            const auto it = topics.find(f.sessionId);
            if (it == topics.end() || it->second.threadId <= 0)
            {
                continue;
            }
            const TopicRecord& t = it->second;
            if (t.nameSetMs > 0 && nowMs - t.nameSetMs < kNameRenameGapMs)
            {
                continue; // renamed lately
            }
            const bool stateSettled = f.stateSinceMs <= 0 || nowMs - f.stateSinceMs >= kNameStateSettleMs;
            const bool stateDue = t.nameState != static_cast<int>(f.state) && stateSettled;
            const bool colorDue = t.nameColor != TgTitleColorSquare(f.colorHex) && (f.colorSinceMs <= 0 || nowMs - f.colorSinceMs >= kNameStateSettleMs);
            // §21.22: the state shown the other way than wanted (glyph vs icon) — quiet, and only with a settled state.
            const bool formDue = t.stateIcon != stateIcons && stateSettled;
            if (!stateDue && !colorDue && !formDue)
            {
                continue; // up to date, or a change that has not held yet
            }
            // A form change alone ranks with the quiet re-squares (5): every state and every real color change goes first.
            const int rank = stateDue ? urgency(f.state) : (colorDue && t.nameColorMode == static_cast<int>(colorMode) ? 4 : 5);
            const int64_t since = stateDue ? f.stateSinceMs : (colorDue ? f.colorSinceMs : f.stateSinceMs);
            if (pick.sessionId.empty() || rank < bestRank || (rank == bestRank && since < bestSince))
            {
                pick.sessionId = f.sessionId;
                pick.state = stateDue;
                pick.color = colorDue;
                pick.form = formDue;
                bestRank = rank;
                bestSince = since;
            }
        }
        return pick;
    }

    /// <summary>The color square a rename puts in a topic's name, and the tab-color mode it stands for (§21.13).</summary>
    struct NameColorChoice
    {
        std::wstring square; // TgTitleColorSquare of the color ("" = none)
        int mode{ -1 }; // the TabColorMode the square was resolved under (-1 = unknown), for TopicRecord::nameColorMode
    };

    /// <summary>
    /// The color square a rename puts in a topic's NAME (§21.13): the tab's color once it has held kNameStateSettleMs (or its
    /// clock is unknown), else the square the name shows already, with the mode it was set under — so a rename for another
    /// reason (the state glyph, the user's new title) never carries a color that is still changing, and the change is made,
    /// and told, once it has held (PickNameRename). With no facts for the session (it left the sync's list), the name keeps
    /// its square too.
    /// </summary>
    /// <param name="f">The session's facts (null = not in the sync's list).</param>
    /// <param name="t">The topic as it is (nameColor, nameColorMode).</param>
    /// <param name="nowMs">The clock (Unix ms).</param>
    /// <param name="colorMode">The tab-color mode the facts' colors were resolved under.</param>
    /// <returns>The square and its mode.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline NameColorChoice ChooseNameColor(const TabFact* f, const TopicRecord& t, int64_t nowMs, TabColorMode colorMode)
    {
        NameColorChoice c;
        if (f && (f->colorSinceMs <= 0 || nowMs - f->colorSinceMs >= kNameStateSettleMs))
        {
            c.square = TgTitleColorSquare(f->colorHex);
            c.mode = static_cast<int>(colorMode);
        }
        else
        {
            c.square = t.nameColor;
            c.mode = t.nameColorMode;
        }
        return c;
    }

    /// <summary>
    /// The square the CHANGE LINE quotes as the one the topic showed before a rename (§21.13): the name's own square when its
    /// change to `after` is a change of the TAB's color — both squares resolved under the same tab-color mode, a mode that has
    /// not changed for `settleMs` (a mode switch re-colors, and an Individual switch deals new colors moments later), on a
    /// topic older than `settleMs` (a new tab's Individual color is dealt at its first paint, and a home-folder launch's
    /// inferred folder can be found moments after its topic was made) — else the new square itself, which makes
    /// BuildTopicChangeNoticeHtml say nothing about the color (a topic named before §21.13, a switch of the mode, the
    /// "Remove colors" mode, a topic just made: the square changes quietly; a title change is still told).
    /// </summary>
    /// <param name="before">The topic as it was named (nameColor, nameColorMode, createdMs).</param>
    /// <param name="after">ChooseNameColor's choice for the rename.</param>
    /// <param name="nowMs">The clock (Unix ms).</param>
    /// <param name="modeChangedMs">When the service saw the tab-color mode change last (0 = not in this run).</param>
    /// <param name="settleMs">How long after a mode change, or a topic's creation, a color change is not the user's recolor of a
    /// tab (kColorModeSettleMs).</param>
    /// <returns>The square for BuildTopicChangeNoticeHtml's `oldSquare`.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring NoticeOldSquare(const TopicRecord& before, const NameColorChoice& after, int64_t nowMs, int64_t modeChangedMs, int64_t settleMs)
    {
        const bool modeStable = modeChangedMs <= 0 || nowMs - modeChangedMs >= settleMs;
        const bool topicSettled = before.createdMs <= 0 || nowMs - before.createdMs >= settleMs;
        if (modeStable && topicSettled && before.nameColorMode >= 0 && before.nameColorMode == after.mode && !before.nameColor.empty())
        {
            return before.nameColor;
        }
        return after.square;
    }

    // ===== what a message in a topic is ======================================================

    enum class RemoteKind
    {
        Text, // a prompt (queued)
        AgentmasterCommand, // one of the tab's actions / the session's own commands (§7.3)
        ClaudeCommand, // a Claude Code command that leaves a record: queued like a prompt (§7.2)
        RefusedNoRecord, // a Claude Code command that leaves no record in the session (the user's rule)
        RefusedLeavesTab, // a Claude Code command that would take the conversation out of its tab
    };

    struct RemoteCommand
    {
        RemoteKind kind{ RemoteKind::Text };
        std::wstring name; // the command word, lowercase, without the '/' (AgentmasterCommand / ClaudeCommand / refused)
        std::wstring args; // the rest of the first line + any further lines, trimmed
        std::wstring text; // the whole message text (for Text and ClaudeCommand: what is queued)
    };

    // Agentmaster's own commands in a session's topic and in the Search topic (§7.3 + §5.4). Names never
    // collide with Claude Code's (its /rename, /fork, /color, /stop, /branch exist).
    inline constexpr std::wstring_view kAgentmasterCommands[] = {
        L"start", L"help", L"queue", L"sendnow", L"unqueue", L"runner", L"pin", L"unpin", L"rename", L"markunread",
        L"moveidle", L"wake", L"favorite", L"unfavorite", L"tags", L"tag", L"untag", L"sessionid", L"path", L"gitbranch",
        L"draft", L"summary", L"transcript", L"screen", L"restartsession", L"deactivate", L"activate", L"forksession",
        L"newsession", L"interrupt", L"takedraft", L"idle", L"board", L"pauseall", L"resumeall", L"cc", L"status", L"card",
        L"reactivate", L"recreate", L"verbose",
        L"screenimage", // §21.24: the terminal's screen as a picture (the card's 🖼 Screen Image; /screen is the text)
        L"search-tab", L"search_tab", // §21.19: jump to the 🔎 Search topic from any topic (kSearchTabCommands)
        L"search" // §21.19: /search <words> asks the Search topic's agent from any topic; a bare /search jumps (kSearchCommand)
    };

    // Claude Code commands that leave NO record in the session (§7.2, provisional until P0-19): run at
    // the PC. Reopening the session would show nothing of them, so they are not sent from a topic.
    inline constexpr std::wstring_view kClaudeNoRecordCommands[] = {
        L"reload-plugins", L"reload-skills", L"btw", L"config", L"status", L"help", L"hooks", L"permissions", L"memory",
        L"mcp", L"tasks", L"workflows", L"diff", L"rewind", L"release-notes", L"skills", L"theme", L"tui", L"keybindings",
        L"scroll-speed", L"terminal-setup", L"voice", L"export", L"copy", L"heapdump", L"ide", L"chrome", L"login",
        L"logout", L"upgrade", L"usage-credits", L"privacy-settings", L"bug", L"feedback", L"install-github-app",
        L"install-slack-app", L"mobile", L"radio", L"stickers", L"passes", L"powerup", L"artifacts", L"doctor", L"cost",
        L"context", L"vim", L"statusline", L"agents", L"plugin", L"plugins", L"remote-control", L"resume-list"
    };

    // Claude Code commands that take the conversation out of its tab, move it, or end it (§7.2).
    inline constexpr std::wstring_view kClaudeLeavesTabCommands[] = {
        L"resume", L"branch", L"fork", L"background", L"teleport", L"desktop", L"stop", L"exit", L"quit", L"cd"
    };

    namespace detail
    {
        inline bool InList(std::wstring_view name, const std::wstring_view* begin, const std::wstring_view* end) noexcept
        {
            for (auto p = begin; p != end; ++p)
            {
                if (*p == name)
                {
                    return true;
                }
            }
            return false;
        }

        inline std::wstring Trim(std::wstring_view s)
        {
            const size_t a = s.find_first_not_of(L" \t\r\n");
            if (a == std::wstring_view::npos)
            {
                return {};
            }
            const size_t b = s.find_last_not_of(L" \t\r\n");
            return std::wstring{ s.substr(a, b - a + 1) };
        }
    }

    /// <summary>
    /// Decides what a message typed in a topic is: a prompt, one of Agentmaster's `/commands`, a
    /// Claude Code command to queue, or a refused one (§7.2, §7.3). `/cc <command…>` forces the rest
    /// through to Claude Code even when its name is one of Agentmaster's (the refusal tables still apply).
    /// A Telegram `@botname` suffix on the command word is dropped.
    /// </summary>
    /// <param name="text">The message text.</param>
    /// <returns>The classification with the lowercase command name and its arguments.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline RemoteCommand ParseRemoteCommand(std::wstring_view text)
    {
        RemoteCommand r;
        r.text = detail::Trim(text);
        if (r.text.empty() || r.text.front() != L'/')
        {
            return r;
        }
        // The command word: up to whitespace; strip a "@bot" suffix.
        const size_t ws = r.text.find_first_of(L" \t\r\n");
        std::wstring word = r.text.substr(1, ws == std::wstring::npos ? std::wstring::npos : ws - 1);
        if (const size_t at = word.find(L'@'); at != std::wstring::npos)
        {
            word = word.substr(0, at);
        }
        word = detail::Lower(word);
        std::wstring args = ws == std::wstring::npos ? std::wstring{} : detail::Trim(r.text.substr(ws));
        if (word.empty())
        {
            return r; // a lone "/" is text
        }
        const auto classifyClaude = [&](std::wstring name, std::wstring a, std::wstring whole) {
            r.name = std::move(name);
            r.args = std::move(a);
            r.text = std::move(whole);
            if (detail::InList(r.name, std::begin(kClaudeNoRecordCommands), std::end(kClaudeNoRecordCommands)))
            {
                r.kind = RemoteKind::RefusedNoRecord;
            }
            else if (detail::InList(r.name, std::begin(kClaudeLeavesTabCommands), std::end(kClaudeLeavesTabCommands)))
            {
                r.kind = RemoteKind::RefusedLeavesTab;
            }
            else if (r.name == L"model" && r.args.empty())
            {
                r.kind = RemoteKind::RefusedNoRecord; // /model with no argument opens the picker: a TUI-only panel
            }
            else
            {
                r.kind = RemoteKind::ClaudeCommand;
            }
        };
        if (word == L"cc")
        {
            // Pass-through: the rest is the Claude Code command (or plain text when it has no '/').
            if (args.empty() || args.front() != L'/')
            {
                r.kind = RemoteKind::Text;
                r.text = args;
                return r;
            }
            const size_t ws2 = args.find_first_of(L" \t\r\n");
            std::wstring w2 = detail::Lower(args.substr(1, ws2 == std::wstring::npos ? std::wstring::npos : ws2 - 1));
            std::wstring a2 = ws2 == std::wstring::npos ? std::wstring{} : detail::Trim(args.substr(ws2));
            classifyClaude(std::move(w2), std::move(a2), args);
            return r;
        }
        if (detail::InList(word, std::begin(kAgentmasterCommands), std::end(kAgentmasterCommands)))
        {
            r.kind = RemoteKind::AgentmasterCommand;
            r.name = std::move(word);
            r.args = std::move(args);
            return r;
        }
        classifyClaude(std::move(word), std::move(args), r.text);
        return r;
    }

    /// <summary>
    /// §21.19, the words of the command that JUMPS to the 🔎 Search topic from any topic (the user's request of 2026-10-18:
    /// "Add /search-tab command which jumps to the search tab"). Two spellings of one command:
    ///   "search-tab" — the user's name for it, typed as it is;
    ///   "search_tab" — the one Telegram's "/" menu can list (setMyCommands takes only a-z, 0-9 and "_"), and the one the
    ///                  help texts print: a client makes a command LINK of "/search-tab" only up to the hyphen, so a tap on
    ///                  a printed "/search-tab" would send "/search".
    /// "/search" itself is the second command (kSearchCommand, the user's follow-up of the same night: "also add /search
    /// which sends a search command to search tab"): "/search <words>" asks the Search topic's agent from any topic, and a
    /// BARE "/search" — that link's tap, too — is this jump. It is not in this list, because its words are a request.
    /// Every name is also in kAgentmasterCommands (a session's topic never sends it to Claude Code) and in
    /// kSearchTopicFleetCommands (the Search topic runs it); the m5 suite checks both lists against this one.
    /// </summary>
    inline constexpr std::wstring_view kSearchTabCommands[] = { L"search-tab", L"search_tab" };

    /// <summary>
    /// Whether a message is the /search-tab command (kSearchTabCommands), read the way every command is read
    /// (ParseRemoteCommand: any case, a "@bot" suffix dropped, words after the command ignored). `/cc /search-tab` is NOT:
    /// "/cc" forces its command through to Claude Code, and that choice is the user's. The service checks it FIRST for a
    /// message in any thread — before a session topic's own handling, which would note the message as the topic's newest
    /// post and re-post its sticky card a few seconds later, putting THAT topic back on top of the list the jump just
    /// reordered (MessagingService::_jumpToSearchTopic).
    /// </summary>
    /// <param name="text">The message text.</param>
    /// <returns>True for /search-tab and /search_tab.</returns>
    /// <exception cref="std::bad_alloc">If ParseRemoteCommand cannot allocate its reading.</exception>
    inline bool IsSearchTabCommand(std::wstring_view text)
    {
        const RemoteCommand cmd = ParseRemoteCommand(text);
        return cmd.kind == RemoteKind::AgentmasterCommand && detail::InList(cmd.name, std::begin(kSearchTabCommands), std::end(kSearchTabCommands));
    }

    /// <summary>
    /// §21.19, the command that SENDS a search to the 🔎 Search topic from any topic (the user's request of 2026-10-18: "also
    /// add /search which sends a search command to search tab"): "/search the session where I fixed the tab drag crash". Its
    /// words are posted in the Search topic as the user's line and asked of the Search agent there, and the answer comes in
    /// the Search topic, as for words typed there (MessagingService::_relaySearchRequest). A bare "/search" has nothing to
    /// ask, and is the /search-tab jump instead. In the Search topic itself "/search <words>" is simply a request
    /// (ClassifySearchTopicMessage).
    /// </summary>
    inline constexpr std::wstring_view kSearchCommand = L"search";

    /// <summary>
    /// Reads a /search message (kSearchCommand) the way every command is read (ParseRemoteCommand: any case, a "@bot" suffix
    /// dropped): the words after the command — the rest of its line and every further line, trimmed — or "" for a bare
    /// "/search". `/cc /search x` is not one: "/cc" forces its command through to Claude Code, the user's choice. A longer word
    /// that starts with it ("/searching", "/search-tab") is not one either: the command word is read up to whitespace.
    /// </summary>
    /// <param name="text">The message text.</param>
    /// <returns>The words (possibly ""), or nullopt when the message is not /search.</returns>
    /// <exception cref="std::bad_alloc">If ParseRemoteCommand cannot allocate its reading.</exception>
    inline std::optional<std::wstring> SearchCommandWords(std::wstring_view text)
    {
        RemoteCommand cmd = ParseRemoteCommand(text);
        if (cmd.kind != RemoteKind::AgentmasterCommand || cmd.name != kSearchCommand)
        {
            return std::nullopt;
        }
        return std::move(cmd.args);
    }

    /// <summary>
    /// §21.19: a /search typed in ANOTHER thread, as the bot posts it in the 🔎 Search topic right before the agent's
    /// "Searching…" and answer, so the Search topic reads like a request typed there: the "👤 You" header (§21.12) with the
    /// clock and where it was asked, a second row naming the session's topic it came from — a title token that links back to
    /// that topic (§21.17), "" for the chat's main view or a thread no session owns — then the words (UserLineBody: plain when
    /// short, an expandable quote past it, cut at kUserLineMaxChars).
    /// "&lt;b&gt;👤 You&lt;/b&gt; · 14:02 · 🔎 search, from a topic\n🟦 Agentmaster\nthe tab drag crash". The header writes
    /// "search", never "/search": a command in a bot's message is a tappable link, and a tap would send a bare /search.
    /// </summary>
    /// <param name="tsMs">The command message's time (unix ms; 0 = no clock).</param>
    /// <param name="words">The search words (SearchCommandWords).</param>
    /// <param name="fromRow">The title token of the session's topic it came from (UserLineTitleRow); "" = none.</param>
    /// <param name="fromMainView">It was typed in the chat's main view ("All"), not in a topic.</param>
    /// <returns>Telegram HTML, with the row as a title token (the send layer renders it).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildRelayedSearchLineHtml(int64_t tsMs, std::wstring_view words, std::wstring_view fromRow, bool fromMainView)
    {
        const std::wstring_view what = fromMainView ? std::wstring_view{ L"\U0001F50E search, from the main view" } : std::wstring_view{ L"\U0001F50E search, from a topic" };
        return detail::UserLineHead(tsMs, what, fromRow) + L"\n" + detail::UserLineBody(words);
    }

    // ===== the Search topic (§5.4; the user's rules of 2026-10-16 night, §21.7) ==============

    // The Search topic's conversation is NOT a session (the user's rule, §21.7): it has no transcript, so it is never a
    // search result, and nothing in this topic can wake, resume, fork or pin it. It carries on by itself after a restart
    // (the agent harness keeps its record), and the user can only CLEAR it. What a message typed in the Search topic is:
    enum class SearchTopicVerb
    {
        Request, // plain words: a request for the search agent (a follow-up refines the previous answer)
        Clear, // /clear or /reset: throw the search conversation away (the Sessions page's crossed circle); the next request starts fresh
        FleetCommand, // a command the Search topic RUNS (kSearchTopicFleetCommands): /help, /status, /idle, /board, /newsession, …
        SessionCommand, // a SESSION topic's command (/wake, /pin, /forksession, /transcript, /cc, …): refused here,
                        // because the Search conversation is not a session (the refusal says what it can do instead)
        ClaudeCommand, // any other /command, i.e. Claude Code's: refused here, it belongs in a session's topic
    };

    // The Agentmaster commands the Search topic RUNS (MessagingService::_runSearchCommand). Every other name of
    // kAgentmasterCommands is a session's command and is refused in the Search topic (SearchTopicVerb::SessionCommand);
    // a session's topic still forwards /idle, /board, /pauseall and /resumeall here (they work in any topic).
    inline constexpr std::wstring_view kSearchTopicFleetCommands[] = {
        L"start", L"help", L"status", L"idle", L"board", L"newsession", L"pauseall", L"resumeall",
        L"recreate", // §21.8: make the session topics again in MOST RECENT order (asks first)
        L"search-tab", L"search_tab", // §21.19: in the Search topic itself it only puts the topic first again
        L"search" // §21.19: a bare /search is that jump too; "/search <words>" here is a REQUEST (ClassifySearchTopicMessage)
    };

    // A message of the Search topic, classified.
    struct SearchTopicMessage
    {
        SearchTopicVerb verb{ SearchTopicVerb::Request };
        // ParseRemoteCommand's reading of the same text: the request's words are `cmd.text`; a command's word and
        // arguments are `cmd.name` / `cmd.args`. For `/cc …` the name is set to "cc" (the word that was typed), so the
        // refusal names it — ParseRemoteCommand itself reads the forwarded command there.
        RemoteCommand cmd;
    };

    /// <summary>
    /// Decides what a message typed in the 🔎 Search topic is (§5.4, §21.7: the Search conversation is never woken,
    /// resumed or forked; the user can only clear it). ParseRemoteCommand reads the text first; then two words mean
    /// something else HERE: `/clear` and `/reset` (Claude Code's own "start this conversation over", queued as prompts
    /// in a session's topic, where they clear THAT session) clear the SEARCH conversation. They are read from the raw
    /// text's first word, so a forced `/cc /clear` stays what `/cc` is: a session's command, which has no session to go
    /// to in this topic. Everything Claude Code's (recorded, no-record and leaves-the-tab alike) is refused here.
    /// </summary>
    /// <param name="text">The message text.</param>
    /// <returns>The verb, with ParseRemoteCommand's reading.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline SearchTopicMessage ClassifySearchTopicMessage(std::wstring_view text)
    {
        SearchTopicMessage m;
        m.cmd = ParseRemoteCommand(text);
        // The raw first word, read the way ParseRemoteCommand reads one ("/Clear@MyBot" -> "clear"); empty for plain words.
        const std::wstring whole = detail::Trim(text);
        std::wstring word;
        if (!whole.empty() && whole.front() == L'/')
        {
            const size_t ws = whole.find_first_of(L" \t\r\n");
            word = whole.substr(1, ws == std::wstring::npos ? std::wstring::npos : ws - 1);
            if (const size_t at = word.find(L'@'); at != std::wstring::npos)
            {
                word = word.substr(0, at);
            }
            word = detail::Lower(word);
        }
        if (word == L"clear" || word == L"reset")
        {
            m.verb = SearchTopicVerb::Clear;
            return m;
        }
        if (word == L"cc")
        {
            // ParseRemoteCommand turns "/cc /x" into the command x and "/cc words" into plain words: neither may become
            // a search request or a refusal that names the wrong command.
            m.verb = SearchTopicVerb::SessionCommand;
            m.cmd.name = L"cc";
            return m;
        }
        // §21.19: "/search <words>" is the command that sends a search to this topic from any other one; typed HERE, its words
        // are a request like plain words (what the agent is asked is the words, not the command). A bare "/search" stays a
        // fleet command: the jump that puts this topic first (MessagingService::_runSearchCommand).
        if (m.cmd.kind == RemoteKind::AgentmasterCommand && m.cmd.name == kSearchCommand && !m.cmd.args.empty())
        {
            m.verb = SearchTopicVerb::Request;
            m.cmd.text = m.cmd.args;
            return m;
        }
        switch (m.cmd.kind)
        {
        case RemoteKind::Text:
            m.verb = SearchTopicVerb::Request;
            break;
        case RemoteKind::AgentmasterCommand:
            m.verb = detail::InList(m.cmd.name, std::begin(kSearchTopicFleetCommands), std::end(kSearchTopicFleetCommands)) ? SearchTopicVerb::FleetCommand : SearchTopicVerb::SessionCommand;
            break;
        default:
            m.verb = SearchTopicVerb::ClaudeCommand; // there is no Claude Code session in this topic to queue it for
            break;
        }
        return m;
    }

    // A Search answer lists its sessions as numbered rows with ONE PICK button per row (the user's rule, §21.7): a pick
    // opens a second message, the ACTION MENU of that session (▶ Resume, ▶ Resume 📌, ⑂ Fork — no fork-and-pin — and
    // ✕ Cancel). A pick's callback data is "p:<row number>:<session id>": STATELESS on purpose, unlike the single-use
    // action tokens (M5). A pick does nothing but open the menu, so it may be tapped again, and an answer's picks still
    // work after the bot or the app restarted, like the conversation they belong to. Telegram caps callback data at 64
    // bytes; a Claude session id (36 characters) makes 41 at most.
    inline constexpr std::wstring_view kSearchPickPrefix = L"p:";
    // The callback data of the "⊘ Clear" button every Search answer carries: stateless like a pick; it clears the
    // conversation that exists when it is pressed, whichever answer carried it.
    inline constexpr std::wstring_view kSearchClearCallbackData = L"s:clear";
    // How many sessions one answer lists with a pick button (a model that shows more gets a "N more" line), and how
    // many pick buttons share one row of the keyboard.
    inline constexpr size_t kSearchAnswerMaxRows = 10;
    inline constexpr size_t kSearchPickButtonsPerRow = 5;

    // A pick, read back from its button.
    struct SearchPick
    {
        size_t number{ 0 }; // the row number the answer showed (1-based; only the menu's title uses it)
        std::wstring sessionId; // the session the row listed
    };

    namespace detail
    {
        /// <summary>
        /// Whether a pick button may carry `id`: 8 to 40 hex digits and dashes (a Claude session id is a UUID). The bound
        /// keeps the callback data under Telegram's 64 bytes; the charset keeps a forged callback (any client can send
        /// any bytes) from carrying anything else into the menu, a token or a launch.
        /// </summary>
        /// <param name="id">The id to check.</param>
        /// <returns>True when it can be carried.</returns>
        inline bool IsPickableSessionId(std::wstring_view id) noexcept
        {
            if (id.size() < 8 || id.size() > 40)
            {
                return false;
            }
            for (const wchar_t c : id)
            {
                const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
                if (!hex && c != L'-')
                {
                    return false;
                }
            }
            return true;
        }
    }

    /// <summary>Builds a pick button's callback data ("p:&lt;number&gt;:&lt;session id&gt;", see kSearchPickPrefix).</summary>
    /// <param name="number">The row number the answer shows (1 … 99).</param>
    /// <param name="sessionId">The session the row lists.</param>
    /// <returns>The data, or "" when the number or the id cannot be carried (that row then gets no button).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring SearchPickCallbackData(size_t number, std::wstring_view sessionId)
    {
        if (number < 1 || number > 99 || !detail::IsPickableSessionId(sessionId))
        {
            return {};
        }
        return std::wstring{ kSearchPickPrefix } + std::to_wstring(number) + L":" + std::wstring{ sessionId };
    }

    /// <summary>
    /// Reads a pick button's callback data back. The data comes from Telegram, so all of it is checked: the prefix, a
    /// row number of 1 … 99 written in one or two digits, and a pickable session id.
    /// </summary>
    /// <param name="data">The callback query's data.</param>
    /// <returns>The pick, or nullopt for anything else (another button's data, or a malformed one).</returns>
    /// <exception cref="std::bad_alloc">If the id cannot be copied.</exception>
    inline std::optional<SearchPick> ParseSearchPickCallbackData(std::wstring_view data)
    {
        if (data.substr(0, kSearchPickPrefix.size()) != kSearchPickPrefix)
        {
            return std::nullopt;
        }
        const std::wstring_view rest = data.substr(kSearchPickPrefix.size());
        const size_t colon = rest.find(L':');
        if (colon == std::wstring_view::npos || colon == 0 || colon > 2)
        {
            return std::nullopt;
        }
        size_t number = 0;
        for (size_t i = 0; i < colon; ++i)
        {
            const wchar_t c = rest[i];
            if (c < L'0' || c > L'9')
            {
                return std::nullopt;
            }
            number = number * 10 + static_cast<size_t>(c - L'0');
        }
        const std::wstring_view id = rest.substr(colon + 1);
        if (number < 1 || !detail::IsPickableSessionId(id))
        {
            return std::nullopt;
        }
        return SearchPick{ number, std::wstring{ id } };
    }

    /// <summary>
    /// Lays out an answer's pick buttons: the row numbers 1 … count (at most kSearchAnswerMaxRows),
    /// kSearchPickButtonsPerRow to a row of the keyboard, in order.
    /// </summary>
    /// <param name="count">How many sessions the answer lists.</param>
    /// <returns>The keyboard's rows of row numbers; empty for 0.</returns>
    /// <exception cref="std::bad_alloc">If the rows cannot be allocated.</exception>
    inline std::vector<std::vector<size_t>> SearchPickKeyboardLayout(size_t count)
    {
        std::vector<std::vector<size_t>> rows;
        const size_t n = (std::min)(count, kSearchAnswerMaxRows);
        for (size_t i = 1; i <= n; ++i)
        {
            if ((i - 1) % kSearchPickButtonsPerRow == 0)
            {
                rows.emplace_back();
            }
            rows.back().push_back(i);
        }
        return rows;
    }

    // Where a session that a Search answer listed stands NOW. It is read when the answer is posted (the row's word) and
    // again at every tap (the menu's line, and what its Resume does): never cached, because a session changes state
    // between an answer and a tap (M5).
    enum class SearchPickState
    {
        Closed, // not open in any tab (archived, never managed, or a live record without a tab): Resume opens it into a background tab
        IdleTab, // open in a tab but Idle and not pinned, so it has no topic: Resume wakes it (Waiting for you) and its topic appears
        Listed, // open in a tab and not Idle (or pinned, which never idles): it has its own topic already, Resume has nothing to open
    };

    /// <summary>
    /// Classifies where a picked session stands, the split the answer's words have always used: a live session with a
    /// tab is IdleTab when its state is Idle and it is not pinned (§5.3), else Listed; anything else is Closed.
    /// </summary>
    /// <param name="known">Whether the registry knows the session (a session never opened in Agentmaster is not known).</param>
    /// <param name="live">SessionInfo::live.</param>
    /// <param name="hasTab">Whether the record names a tab (a non-empty SessionInfo::tabToken).</param>
    /// <param name="state">SessionInfo::state.</param>
    /// <param name="pinned">SessionInfo::pinned.</param>
    /// <returns>The state.</returns>
    inline SearchPickState ClassifySearchPickState(bool known, bool live, bool hasTab, SessionState state, bool pinned) noexcept
    {
        if (!known || !live || !hasTab)
        {
            return SearchPickState::Closed;
        }
        return state == SessionState::Idle && !pinned ? SearchPickState::IdleTab : SearchPickState::Listed;
    }

    /// <summary>The word an answer's row shows for a state ("closed", "idle tab", "has a topic").</summary>
    /// <param name="state">The state.</param>
    /// <returns>A static string; never null.</returns>
    inline const wchar_t* SearchPickStateWord(SearchPickState state) noexcept
    {
        switch (state)
        {
        case SearchPickState::IdleTab:
            return L"idle tab";
        case SearchPickState::Listed:
            return L"has a topic";
        case SearchPickState::Closed:
        default:
            return L"closed";
        }
    }

    /// <summary>The action menu's line for a state: where the session is and what its Resume does there.</summary>
    /// <param name="state">The state.</param>
    /// <returns>A static string; never null.</returns>
    inline const wchar_t* SearchPickStateLine(SearchPickState state) noexcept
    {
        switch (state)
        {
        case SearchPickState::IdleTab:
            return L"Open in a tab, idle: Resume brings its topic back.";
        case SearchPickState::Listed:
            return L"Open, with its own topic: Resume has nothing to open.";
        case SearchPickState::Closed:
        default:
            return L"Closed: Resume opens it in a background tab, and its topic appears.";
        }
    }

    // ===== the Search answer as EVIDENCE CARDS (§21.10, the user's choice "C1" of 2026-10-17) =============
    //
    // The answer is the model's one-sentence VERDICT, then one CARD per shown session: its number (the label of its
    // pick button), a glyph for where it stands, its title; a line with its folder, the day it was last active and
    // how many prompts it had; and ONE QUOTE — the model's `why`, the evidence it saw when it read the session
    // (SESSIONS.md §9.12). A pick (the number) opens the session's menu: the whole folder and branch, when it ran,
    // the same quote, where it stands now, then Resume / Resume 📌 / Fork / Cancel. Both texts are built here, pure,
    // so the m5 suite pins them (TestMessagingSearch).

    /// <summary>One session as the Search answer shows it (a card), and as its pick menu does.</summary>
    struct SearchCard
    {
        size_t number{ 0 }; // the 1-based row number: the card's label and its pick button's
        std::wstring sessionId;
        std::wstring title; // "" = shown as the short id
        std::wstring dir; // the working folder ("" = unknown)
        std::wstring branch; // the git branch ("" = unknown)
        int64_t createdMs{ 0 }; // when the conversation started (0 = unknown)
        int64_t lastActivityMs{ 0 }; // its newest message (0 = unknown)
        int prompts{ 0 }; // prompts the user typed in it
        std::wstring why; // the model's one line of evidence ("" = it gave none)
        SearchPickState state{ SearchPickState::Closed };
        // §21.13: the session's tab color as a square (TgTitleColorSquare), before its title like in every place that names a
        // session; an on-disk session with no tab wears its folder's color, like the Sessions page's chip. "" = none.
        std::wstring colorSquare;
        // §21.17: the session's topic (0 = none: a closed session, an idle tab) — its title on the card and in the pick menu is a
        // link to that topic.
        int64_t threadId{ 0 };
    };

    /// <summary>
    /// The glyph a card and the pick menu show for where the session stands: 📦 closed, 💤 an idle tab, 💬 a topic of
    /// its own. The menu's line (SearchPickStateLine) says in words what each means for Resume.
    /// </summary>
    /// <param name="state">The state.</param>
    /// <returns>A static string; never null.</returns>
    inline const wchar_t* SearchPickStateGlyph(SearchPickState state) noexcept
    {
        switch (state)
        {
        case SearchPickState::IdleTab:
            return L"\U0001F4A4";
        case SearchPickState::Listed:
            return L"\U0001F4AC";
        case SearchPickState::Closed:
        default:
            return L"\U0001F4E6";
        }
    }

    namespace detail
    {
        /// <summary>The last segment of a folder path ("Agentmaster" for "K:\source\Agentmaster\"), the text itself when it has no separator.</summary>
        /// <param name="dir">The folder.</param>
        /// <returns>The segment.</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        inline std::wstring FolderLeaf(std::wstring_view dir)
        {
            while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
            {
                dir.remove_suffix(1);
            }
            const size_t cut = dir.find_last_of(L"\\/");
            return std::wstring{ cut == std::wstring_view::npos ? dir : dir.substr(cut + 1) };
        }

        /// <summary>A Unix ms instant as local broken-down time.</summary>
        /// <param name="unixMs">The instant.</param>
        /// <param name="out">Receives the local time.</param>
        /// <returns>False when the C runtime cannot convert it (it then writes nothing).</returns>
        inline bool LocalTmOf(int64_t unixMs, std::tm& out) noexcept
        {
            const time_t t = static_cast<time_t>(unixMs / 1000);
            return localtime_s(&out, &t) == 0;
        }

        /// <summary>"1 prompt", "14 prompts"; empty for none (a card says nothing rather than "0 prompts").</summary>
        /// <param name="prompts">The count.</param>
        /// <returns>The words.</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        inline std::wstring PromptCountText(int prompts)
        {
            return prompts <= 0 ? std::wstring{} : std::to_wstring(prompts) + (prompts == 1 ? L" prompt" : L" prompts");
        }
    }

    /// <summary>
    /// The DAY a card names: "today", "yesterday", "11 Oct" in this year, "11 Oct 2025" before it — local time, day
    /// first like TgClockText. "Yesterday" is the calendar day before today's (mktime normalizes the day, so a day of
    /// 23 or 25 hours is still one day).
    /// </summary>
    /// <param name="unixMs">The instant; 0 or less = unknown.</param>
    /// <param name="nowMs">The clock.</param>
    /// <returns>The words; empty when the instant is unknown or cannot be converted.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring SearchDayText(int64_t unixMs, int64_t nowMs)
    {
        std::tm when{};
        std::tm now{};
        if (unixMs <= 0 || !detail::LocalTmOf(unixMs, when) || !detail::LocalTmOf(nowMs, now))
        {
            return {};
        }
        if (when.tm_year == now.tm_year && when.tm_yday == now.tm_yday)
        {
            return L"today";
        }
        std::tm previous = now;
        previous.tm_mday -= 1;
        previous.tm_isdst = -1;
        if (std::mktime(&previous) != static_cast<time_t>(-1) && when.tm_year == previous.tm_year && when.tm_yday == previous.tm_yday)
        {
            return L"yesterday";
        }
        static constexpr const wchar_t* kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        const int mon = when.tm_mon >= 0 && when.tm_mon < 12 ? when.tm_mon : 0;
        std::wstring text = std::to_wstring(when.tm_mday) + L" " + kMonths[mon];
        if (when.tm_year != now.tm_year)
        {
            text += L" " + std::to_wstring(when.tm_year + 1900);
        }
        return text;
    }

    /// <summary>
    /// When a session ran, for its pick menu: "14:02 → 16:40" today, "11 Oct 14:02 → 16:40" within one day,
    /// "10 Oct 09:00 → 11 Oct 16:40" across days (TgClockText's forms); one time when only one is known or both are
    /// the same.
    /// </summary>
    /// <param name="createdMs">When the conversation started; 0 = unknown.</param>
    /// <param name="lastActivityMs">Its newest message; 0 = unknown.</param>
    /// <param name="nowMs">The clock.</param>
    /// <returns>The words; empty when neither instant is known.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring SearchSpanText(int64_t createdMs, int64_t lastActivityMs, int64_t nowMs)
    {
        if (createdMs <= 0 && lastActivityMs <= 0)
        {
            return {};
        }
        if (createdMs <= 0)
        {
            return L"active " + TgClockText(lastActivityMs, nowMs);
        }
        std::wstring text = TgClockText(createdMs, nowMs);
        if (lastActivityMs <= createdMs)
        {
            return text;
        }
        const std::wstring end = TgClockText(lastActivityMs, nowMs);
        std::tm a{};
        std::tm b{};
        const bool sameDay = detail::LocalTmOf(createdMs, a) && detail::LocalTmOf(lastActivityMs, b) && a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
        // One day: the end's clock alone (TgClockText ends with "HH:MM" in every form).
        const size_t space = end.rfind(L' ');
        return text + L" \x2192 " + (sameDay && space != std::wstring::npos ? end.substr(space + 1) : end);
    }

    /// <summary>
    /// The Search answer (§21.10): the model's verdict, then one card per session — "<b>1</b> 📦 <b>title</b>", the line
    /// "folder · day · N prompts", and the model's why as one quote — then how many shown sessions were not listed. Each title
    /// is a title token (§21.15): its classic render is the text above, a rich answer shows a chip.
    /// </summary>
    /// <param name="verdict">The model's one sentence, plain text (escaped here); empty = none.</param>
    /// <param name="cards">The listed sessions, in order (the caller cuts them to kSearchAnswerMaxRows).</param>
    /// <param name="notListed">Shown sessions past the cut (they follow as lines, BuildSearchMoreLinesHtml).</param>
    /// <param name="nowMs">The clock (a parameter, so the days a test reads are its own).</param>
    /// <returns>Telegram HTML with title tokens, balanced; "Nothing found." when there is nothing at all.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring BuildSearchAnswerHtml(std::wstring_view verdict, const std::vector<SearchCard>& cards, size_t notListed, int64_t nowMs)
    {
        std::wstring html = TgEscapeHtml(verdict);
        for (const auto& card : cards)
        {
            if (!html.empty())
            {
                html += L"\n\n";
            }
            // The title on one line (TgTitleLine: a two-line tab title shows both lines, joined by a space), 100 units at most.
            const std::wstring title = card.title.empty() ? card.sessionId.substr(0, (std::min)(card.sessionId.size(), size_t{ 8 })) : TgTitleLine(card.title, 100);
            // The glyph for where it stands, then the tab's color square (§21.13: "State, then color", like the topic names);
            // §21.15: the square and the title as a title token — "🟦 <b>title</b>" classic, a chip in a rich answer.
            html += L"<b>" + std::to_wstring(card.number) + L"</b> " + SearchPickStateGlyph(card.state) + L" " + TgTitleToken(TgTitleForm::Bold, card.colorSquare, title, card.threadId);
            std::wstring meta;
            const auto add = [&meta](const std::wstring& part) {
                if (!part.empty())
                {
                    meta += (meta.empty() ? L"" : L" \x00B7 ") + part;
                }
            };
            add(detail::FolderLeaf(card.dir));
            add(SearchDayText(card.lastActivityMs, nowMs));
            add(detail::PromptCountText(card.prompts));
            if (!meta.empty())
            {
                html += L"\n" + TgEscapeHtml(meta);
            }
            if (!card.why.empty())
            {
                // The quote opens right after a line break and closes before the next one (TgMarkdownToHtml's rule): a
                // break inside the tag would show as an empty last line of the quote.
                html += L"\n<blockquote>" + TgEscapeHtml(card.why) + L"</blockquote>";
            }
        }
        if (notListed > 0)
        {
            // The rest follows in the next message(s), one line each (BuildSearchMoreLinesHtml): every shown session reaches
            // the phone (2026-10-18; it used to say "N more not listed: ask to narrow it down").
            html += (html.empty() ? L"" : L"\n\n") + std::wstring{ L"<i>(" } + std::to_wstring(notListed) + L" more below, one line each.)</i>";
        }
        if (html.empty())
        {
            html = L"Nothing found.";
        }
        return html;
    }

    // The most characters of Telegram HTML one message of an answer's rest carries (Telegram's limit is 4096 after the tags).
    inline constexpr size_t kSearchMoreLinesPackChars = 3500;

    /// <summary>
    /// The REST of a Search answer (§21.10, the user's rule of 2026-10-18 "searches must never miss"): the shown sessions past
    /// the cards (kSearchAnswerMaxRows), one line each — "<b>11</b> 📦 🟦 title · folder · day" — packed into as few messages
    /// as fit kSearchMoreLinesPackChars each. An answer of 104 sessions used to end in "94 more not listed"; now the phone
    /// gets every session the agent showed, in its order. The lines carry no pick button (the answer's keyboard holds the
    /// first rows): the user names a number in a reply ("resume 37"), which the conversation knows.
    /// </summary>
    /// <param name="cards">The sessions past the cards, in order, with their row numbers (their `why` is not shown).</param>
    /// <param name="nowMs">The clock (the day words a test reads are its own).</param>
    /// <returns>The messages' HTML, in order; empty for no cards. Plain HTML (no title token): a line is short and a rich
    /// message of a hundred chips would be refused.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::vector<std::wstring> BuildSearchMoreLinesHtml(const std::vector<SearchCard>& cards, int64_t nowMs)
    {
        std::vector<std::wstring> messages;
        std::wstring current;
        for (const auto& card : cards)
        {
            // The title on one line (TgTitleLine), 80 units at most: a line break would split the session's line in two.
            const std::wstring title = card.title.empty() ? card.sessionId.substr(0, (std::min)(card.sessionId.size(), size_t{ 8 })) : TgTitleLine(card.title, 80);
            std::wstring line = L"<b>" + std::to_wstring(card.number) + L"</b> " + SearchPickStateGlyph(card.state) + L" ";
            if (!card.colorSquare.empty())
            {
                line += card.colorSquare + L" ";
            }
            line += TgEscapeHtml(title);
            std::wstring meta;
            const auto add = [&meta](const std::wstring& part) {
                if (!part.empty())
                {
                    meta += L" \x00B7 " + part;
                }
            };
            add(detail::FolderLeaf(card.dir));
            add(SearchDayText(card.lastActivityMs, nowMs));
            line += TgEscapeHtml(meta);
            if (!current.empty() && current.size() + 1 + line.size() > kSearchMoreLinesPackChars)
            {
                messages.push_back(std::move(current));
                current.clear();
            }
            current += (current.empty() ? L"" : L"\n") + line;
        }
        if (!current.empty())
        {
            messages.push_back(std::move(current));
        }
        return messages;
    }

    /// <summary>
    /// A picked session's action menu (§21.10): "<b>1 · title</b>", the whole folder and the branch, when it ran and how
    /// many prompts it had, the model's why as one quote, and where it stands now (glyph + SearchPickStateLine). The
    /// buttons (Resume / Resume 📌 / Fork / Cancel) are the caller's. The title is a title token (§21.15): "🟦 title" inside the
    /// bold head in a classic menu, a chip after "1 ·" in a rich one.
    /// </summary>
    /// <param name="card">The session, as the answer listed it (or as the registry knows it).</param>
    /// <param name="nowMs">The clock.</param>
    /// <returns>Telegram HTML with a title token, balanced.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring BuildSearchPickMenuHtml(const SearchCard& card, int64_t nowMs)
    {
        // The title on one line (TgTitleLine), 200 units at most.
        const std::wstring title = card.title.empty() ? card.sessionId.substr(0, (std::min)(card.sessionId.size(), size_t{ 8 })) : TgTitleLine(card.title, 200);
        // §21.13: the color square first; §21.15: square and title as a title token (TitleMentionHtml's text when classic).
        std::wstring html = L"<b>" + std::to_wstring(card.number) + L" \x00B7 " + TgTitleToken(TgTitleForm::Plain, card.colorSquare, title, card.threadId) + L"</b>";
        std::wstring place = card.dir;
        if (!card.branch.empty())
        {
            place += (place.empty() ? L"" : L" \x00B7 ") + card.branch;
        }
        if (!place.empty())
        {
            html += L"\n" + TgEscapeHtml(place);
        }
        std::wstring when = SearchSpanText(card.createdMs, card.lastActivityMs, nowMs);
        if (const std::wstring prompts = detail::PromptCountText(card.prompts); !prompts.empty())
        {
            when += (when.empty() ? L"" : L" \x00B7 ") + prompts;
        }
        if (!when.empty())
        {
            html += L"\n" + TgEscapeHtml(when);
        }
        if (!card.why.empty())
        {
            html += L"\n<blockquote>" + TgEscapeHtml(card.why) + L"</blockquote>";
        }
        html += L"\n<i>" + std::wstring{ SearchPickStateGlyph(card.state) } + L" " + SearchPickStateLine(card.state) + L"</i>";
        return html;
    }

    // ===== the backlog rule (D2) ==============================================================

    // An update older than this when it is processed was sent while Agentmaster was closed (Telegram
    // keeps a bot's updates 24 h): it is asked about before it acts.
    inline constexpr int64_t kBacklogAgeMs = 10 * 60 * 1000;

    /// <summary>The backlog rule: is an update too old to act on without asking?</summary>
    /// <param name="messageDateMs">The message's own `date` (unix ms).</param>
    /// <param name="nowMs">The clock.</param>
    /// <returns>True past kBacklogAgeMs; a date of 0 (unknown) is never backlog.</returns>
    inline bool IsBacklogUpdate(int64_t messageDateMs, int64_t nowMs) noexcept
    {
        return messageDateMs > 0 && nowMs - messageDateMs >= kBacklogAgeMs;
    }

    // ===== the outbox ======================================================================

    // The order the outbox serves (§6.6): lower first.
    enum class SendPriority : int
    {
        Final = 0, // final messages, questions, plans, answers to the user's commands
        New = 1, // new prompts, new work-log messages, topic creation
        WorkLogEdit = 2,
        CardEdit = 3,
        Backfill = 4, // backfill, files, reactions, typing
        /// <summary>
        /// A topic's HISTORY replay (§21.9): last of all, and at most one message per kBackfillGapMs (Messaging.cpp), so
        /// the live conversation of every session always goes first and keeps half of the chat's ~1 call/s budget.
        /// </summary>
        History = 5,
    };

    // ===== the card ========================================================================

    /// <summary>A state's glyph + word for the card and the Search results.</summary>
    /// <param name="s">The state.</param>
    /// <returns>"🔵 Running", "🟡 Waiting for you", "🟠 Needs approval", "🔴 Error", "✅ Done", "💤 Idle".</returns>
    inline const wchar_t* StateLabel(SessionState s) noexcept
    {
        switch (s)
        {
        case SessionState::Running:
            return L"\U0001F535 Running";
        case SessionState::WaitingForInput:
            return L"\U0001F7E1 Waiting for you";
        case SessionState::NeedsApproval:
            return L"\U0001F7E0 Needs approval";
        case SessionState::Error:
            return L"\U0001F534 Error";
        case SessionState::Done:
            return L"✅ Done";
        case SessionState::Idle:
        default:
            return L"\U0001F4A4 Idle";
        }
    }

    /// <summary>
    /// What the card shows beyond the session record (§21.6): Claude Code's own RECAP (its away_summary) — shown
    /// only when the service found it FRESH, newer than every conversation message (TopicRecord::recapAtMs &gt;
    /// lastMessageAtMs).
    /// </summary>
    struct CardExtras
    {
        bool recapFresh{ false };
        std::wstring recapText; // NormalizeRecapText of the away_summary content
        int64_t recapAtMs{ 0 }; // the away_summary line's transcript time (a clock time in the card)
        // §21.9: the topic's history replay is not done yet. The card says so; when the last part is in, the card is
        // re-posted below it without the line (or, for a history with nothing to show, edited once).
        bool historyPending{ false };
        // §21.13: the color square before the card's title — the square the topic's NAME shows (TopicRecord::nameColor), so
        // the card and the topic list always agree, and a color change edits the card in the same step as the name (the
        // rename refreshes the card). "" = none. §21.15: in a rich card it also picks the title chip's look (TgTitleChipStyle).
        std::wstring colorSquare;
        // §21.17: the topic the card sits in — its title links to it (0 = no link). In the chat's "All" view, where the card is
        // the topic's newest message, the link is the way into the thread.
        int64_t threadId{ 0 };
    };

    /// <summary>
    /// The moment the card's "since" names, from PERSISTED stamps only: the turn's start while it runs (or waits
    /// for an approval), else the newer turn boundary (a turn that ended, or one that was cut off), else the
    /// session's last activity (a new session: its creation). SessionInfo::stateChangedAtUnixMs — what the card
    /// read before §21.6 — is stamped when the registry LOADS a record, so after every app restart every card's
    /// "since" changed and every card was edited again; it is only the last fallback here.
    /// </summary>
    /// <param name="s">The session.</param>
    /// <returns>Unix ms, or 0 when nothing is known.</returns>
    inline int64_t CardStateSinceMs(const SessionInfo& s) noexcept
    {
        int64_t since = 0;
        if (s.state == SessionState::Running || s.state == SessionState::NeedsApproval)
        {
            since = s.turnStartUnixMs;
        }
        else
        {
            since = (std::max)(s.turnStartUnixMs, s.turnEndUnixMs); // (std::max): the <windows.h> macro
        }
        if (since == 0)
        {
            since = s.lastActivityUnixMs;
        }
        if (since == 0)
        {
            since = s.stateChangedAtUnixMs;
        }
        return since;
    }

    /// <summary>
    /// The card text's identity for TopicRecord::cardTextHash: FNV-1a 32 over the UTF-16 code units, never 0
    /// (0 means "no card text recorded"). A collision only costs one skipped edit of a card whose next change
    /// re-edits it.
    /// </summary>
    /// <param name="text">The card's HTML.</param>
    /// <returns>The hash, 1..2^32-1.</returns>
    inline uint32_t CardTextHash(std::wstring_view text) noexcept
    {
        uint32_t h = 2166136261u;
        for (const wchar_t c : text)
        {
            h ^= static_cast<uint32_t>(c);
            h *= 16777619u;
        }
        return h == 0 ? 1u : h;
    }

    /// <summary>
    /// Whether ▶ Reactivate has something to do for this session (`/reactivate`, and the card's first button — named
    /// 🔧 Resume Session for a session in Error since §21.24): its tab is parked (restored, deactivated, cut off with the
    /// app — its claude is not running), or its claude ran into an Error (a cut-off or failed turn), or claude exited
    /// (Done). A running or waiting session has nothing to reactivate.
    /// </summary>
    /// <param name="s">The session.</param>
    /// <returns>True when Reactivate applies.</returns>
    inline bool SessionIsReactivatable(const SessionInfo& s) noexcept
    {
        return s.live && ((!s.started && !s.external) || s.state == SessionState::Error || s.state == SessionState::Done);
    }

    // ===== the card's buttons: two pages (§21.24) ==============================================

    /// <summary>
    /// The two pages of buttons a sticky card can show (MESSAGING.md §21.24). MAIN is what every card shows by default: ⚙ Session
    /// and 🔄 Restart Session. ⚙ Session turns the card's keyboard into the SESSION page: ⑂ Fork and 🆕 New Session in the folder
    /// first (the user's placement of 2026-10-18: "after ⚙ Session"), then every button the card had before (the runner, the
    /// queue, the pin, the transcript, the screen as text), the screen as a picture, and ◀ Back.
    /// </summary>
    /// <remarks>
    /// The page belongs to the card the button sits on: the service remembers the sticky card's page per topic in memory
    /// (a re-post keeps it; a card that starts to RING - a session that now needs the user - goes back to Main, where the
    /// state's own action is, and a page opened after that is kept), and a typed /status copy starts on Main and flips by
    /// itself. Nothing persists: after a restart every card is on Main again.
    /// </remarks>
    enum class CardPage
    {
        Main, // ⚙ Session · 🔄 Restart Session
        Session, // ⑂ Fork · 🆕 New Session · the runner · Queue · Pin · Transcript · 🖥 Screen Text · 🖼 Screen Image · ◀ Back
    };

    /// <summary>What the card's buttons depend on, read from the session at render time (CardButtonFactsFor).</summary>
    struct CardButtonFacts
    {
        bool pinned{ false }; // 📌 Unpin rather than 📌 Pin
        bool runnerAuto{ false }; // ⏸ Pause runner (the runner is Auto) rather than ▶ Runner auto (it is Paused)
        bool reactivatable{ false }; // ▶ Reactivate first on both pages: a parked tab, or a claude that exited (SessionIsReactivatable)
        bool error{ false }; // 🔧 Resume Session first instead: the session is in Error (§21.24 - the user's "in case of error")
    };

    /// <summary>Reads the card's button facts from a session: its pin, its runner's mode, and whether its state has an action of its own.</summary>
    /// <param name="s">The session (a lean record is enough: no queue is read).</param>
    /// <returns>The facts.</returns>
    inline CardButtonFacts CardButtonFactsFor(const SessionInfo& s) noexcept
    {
        CardButtonFacts f;
        f.pinned = s.pinned;
        f.runnerAuto = s.autorunner.mode == AutorunnerMode::Full;
        f.error = s.live && s.state == SessionState::Error;
        f.reactivatable = SessionIsReactivatable(s);
        return f;
    }

    // The card's callback verbs. A card button's data is "c:<verb>:<session id>" - STABLE data, no token: the verb acts on the
    // session's CURRENT state when it is tapped, so a button on an old card does what it does now (M5's stale-button rule is
    // about answers and queue items, which carry tokens). The longest ("c:newsession:", "c:reactivate:", "c:transcript:") with
    // a 36-character id is 49 bytes of Telegram's 64. The verbs of the cards before §21.24 stay understood (runner, queue, pin,
    // transcript, screen, reactivate, sendnow), so a card that still shows the old keyboard keeps working until its next edit.
    /// <summary>⚙ Session: shows the Session page on the card the button sits on.</summary>
    inline constexpr std::wstring_view kCardVerbSessionPage = L"panel";
    /// <summary>◀ Back: shows the Main page again.</summary>
    inline constexpr std::wstring_view kCardVerbMainPage = L"main";
    /// <summary>⑂ Fork: the topic's /forksession (a fork in a new background tab, held in Waiting for you once it is live).</summary>
    inline constexpr std::wstring_view kCardVerbFork = L"fork";
    /// <summary>🆕 New Session: the topic's /newsession (a fresh session in the same folder, a new background tab).</summary>
    inline constexpr std::wstring_view kCardVerbNewSession = L"newsession";
    /// <summary>🔄 Restart Session: the topic's /restartsession (it asks first while a turn runs: the turn would be cut off).</summary>
    inline constexpr std::wstring_view kCardVerbRestart = L"restart";
    /// <summary>🔧 Resume Session: what ▶ Reactivate does, for a session in Error (a parked tab is started, a claude that is up is restarted).</summary>
    inline constexpr std::wstring_view kCardVerbResume = L"resume";
    /// <summary>🖼 Screen Image: the topic's /screenimage (the terminal's screen as a picture).</summary>
    inline constexpr std::wstring_view kCardVerbScreenImage = L"screenimg";

    /// <summary>The callback data of a card button: "c:&lt;verb&gt;:&lt;session id&gt;".</summary>
    /// <param name="verb">The verb (kCardVerb*, or one of the older verbs).</param>
    /// <param name="sessionId">The card's session.</param>
    /// <returns>The data.</returns>
    /// <exception cref="std::bad_alloc">If the data cannot be allocated.</exception>
    inline std::wstring CardCallbackData(std::wstring_view verb, std::wstring_view sessionId)
    {
        std::wstring data = L"c:";
        data += verb;
        data += L':';
        data += sessionId;
        return data;
    }

    /// <summary>
    /// The sticky card's buttons on one page, as rows of (label, callback data) - the service turns them into Telegram's
    /// inline keyboard. Both pages START with the state's own action when it has one, because that is what a card in that
    /// state is for: 🔧 Resume Session for a session in Error, else ▶ Reactivate for a parked tab or a claude that exited.
    /// Main then holds ⚙ Session · 🔄 Restart Session; Session holds ⑂ Fork · 🆕 New Session, the runner · 📋 Queue,
    /// 📌 Pin · 📄 Transcript, 🖥 Screen Text · 🖼 Screen Image, and ◀ Back.
    /// </summary>
    /// <param name="sessionId">The card's session (it goes into every button's data).</param>
    /// <param name="facts">The facts the labels and the first row depend on (CardButtonFactsFor).</param>
    /// <param name="page">The page.</param>
    /// <returns>The rows, top to bottom; two buttons a row at most, so a phone shows every label whole.</returns>
    /// <exception cref="std::bad_alloc">If the rows cannot be allocated.</exception>
    inline std::vector<std::vector<std::pair<std::wstring, std::wstring>>> BuildCardButtonRows(std::wstring_view sessionId, const CardButtonFacts& facts, CardPage page)
    {
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
        if (facts.error)
        {
            rows.push_back({ { L"\U0001F527 Resume Session", CardCallbackData(kCardVerbResume, sessionId) } });
        }
        else if (facts.reactivatable)
        {
            rows.push_back({ { L"\x25B6 Reactivate", CardCallbackData(L"reactivate", sessionId) } });
        }
        if (page == CardPage::Main)
        {
            // The cog carries U+FE0F so every app draws it as the emoji (U+2699 alone may be drawn as a text glyph).
            rows.push_back({ { L"\x2699\xFE0F Session", CardCallbackData(kCardVerbSessionPage, sessionId) },
                             { L"\U0001F504 Restart Session", CardCallbackData(kCardVerbRestart, sessionId) } });
            return rows;
        }
        // The user's placement (2026-10-18): ⑂ Fork and 🆕 New Session come right after ⚙ Session is tapped, at the top of its page.
        rows.push_back({ { L"\x2442 Fork", CardCallbackData(kCardVerbFork, sessionId) },
                         { L"\U0001F195 New Session", CardCallbackData(kCardVerbNewSession, sessionId) } });
        rows.push_back({ { facts.runnerAuto ? L"\x23F8 Pause runner" : L"\x25B6 Runner auto", CardCallbackData(L"runner", sessionId) },
                         { L"\U0001F4CB Queue", CardCallbackData(L"queue", sessionId) } });
        rows.push_back({ { facts.pinned ? L"\U0001F4CC Unpin" : L"\U0001F4CC Pin", CardCallbackData(L"pin", sessionId) },
                         { L"\U0001F4C4 Transcript", CardCallbackData(L"transcript", sessionId) } });
        rows.push_back({ { L"\U0001F5A5 Screen Text", CardCallbackData(L"screen", sessionId) },
                         { L"\U0001F5BC Screen Image", CardCallbackData(kCardVerbScreenImage, sessionId) } });
        rows.push_back({ { L"\x25C0 Back", CardCallbackData(kCardVerbMainPage, sessionId) } });
        return rows;
    }

    /// <summary>
    /// The topic's STICKY card (MESSAGING.md §6.1, §21.5, §21.6), as Telegram HTML: the title, the state and since
    /// when (a clock time from persisted stamps, CardStateSinceMs), a line naming the state's own action — 🔧 Resume Session
    /// for a session in Error (§21.24), ▶ Reactivate for a parked tab or a claude that exited —, folder · branch · model ·
    /// context, the runner and the queue unless they are the
    /// common default (an Auto runner with nothing queued and no global pause: no line — §21.18), the unsent
    /// draft, and Claude Code's own recap while it is fresh. Pure, so the service compares two renders and edits
    /// only on a change — which is why nothing in it may be a relative span (see TgClockText): every change is an
    /// API call on the one chat. The title is a title TOKEN (§21.15, TgTitleForm::Bold): the classic render reads
    /// "📌 🟦 <b>title</b>" exactly as before (the hash the service compares is taken over that render), a rich message shows
    /// the title as a chip.
    /// </summary>
    /// <param name="s">The session (a full record: the queue is read).</param>
    /// <param name="nowMs">The clock.</param>
    /// <param name="idleLeavesAtMs">When an Idle topic leaves the list (0 = not idle).</param>
    /// <param name="globalPause">The runner's process-wide pause.</param>
    /// <param name="extras">The recap and its freshness, the history line, the title's color square (CardExtras); null = none of them.</param>
    /// <returns>The HTML text, with the title as a title token.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildCardText(const SessionInfo& s, int64_t nowMs, int64_t idleLeavesAtMs, bool globalPause, const CardExtras* extras = nullptr)
    {
        std::wstring out;
        // §21.13: the title with the tab's color square before it (the square the topic's name shows; none without extras);
        // §21.15: as a title token — "🟦 <b>title</b>" in a classic card, a chip in a rich one.
        out += L"\U0001F4CC ";
        out += TgTitleToken(TgTitleForm::Bold, extras ? std::wstring_view{ extras->colorSquare } : std::wstring_view{}, s.title.empty() ? s.id.substr(0, 8) : s.title, extras ? extras->threadId : 0);
        out += L"\n";
        // State line.
        out += StateLabel(s.state);
        // CLOCK TIMES, never spans: the service edits the card whenever this text changes, so a "3m" that
        // became "4m" re-edited every card every minute — 47 topics' cards made ~47 edits a minute and
        // Telegram answered with a 429 whose retry_after was 21 minutes (2026-10-16). "since 19:04"
        // changes once, at the next midnight — and it comes from persisted stamps, so a restart changes nothing.
        if (const int64_t since = CardStateSinceMs(s); since > 0 && nowMs >= since)
        {
            out += L" since " + TgClockText(since, nowMs);
        }
        if (s.pinned)
        {
            out += L" \U0001F4CC";
        }
        // The user is not supposed to care whether a tab is "activated" (§21.5): a message here starts a parked
        // tab by itself, and ▶ Reactivate (the card's button, /reactivate) does it on demand — also for a claude
        // that exited, which a restart of the same conversation recovers. §21.24: a session in ERROR names the card's
        // 🔧 Resume Session instead (the same action: a parked tab is started in place, a claude that is up is restarted),
        // and Error is read FIRST: a tab restored after a crash with its turn cut off is parked AND in Error, and resuming
        // the turn is what it needs, not a bare "not running".
        if (s.live && s.state == SessionState::Error)
        {
            out += (!s.started && !s.external) ? L"\n\U0001F527 Resume Session starts it in its tab (a cut-off turn goes on by itself)" :
                                                 L"\n\U0001F527 Resume Session restarts it (the same conversation goes on)";
        }
        else if (s.live && !s.started && !s.external)
        {
            out += L"\n\U0001F4A4 not running \x2014 a message here or \x25B6 Reactivate resumes it";
        }
        else if (s.live && s.state == SessionState::Done)
        {
            out += L"\n\x23F9 claude exited \x2014 \x25B6 Reactivate starts it again (the same conversation)";
        }
        if (idleLeavesAtMs > 0)
        {
            out += L"\n\U0001F4A4 leaves the list at " + TgClockText(idleLeavesAtMs, nowMs) + L" (/pin keeps it, /wake brings it back)";
        }
        if (extras && extras->historyPending)
        {
            // No percentage: every change of this text is an edit on the chat's budget (§21.4).
            out += L"\n⏳ the session's history is being added below; the live conversation follows it";
        }
        out += L"\n";
        // Place line: folder · branch · model · ctx.
        {
            std::wstring dir = s.inferredWorkingDir.empty() ? s.workingDir : s.inferredWorkingDir;
            std::wstring place = TgEscapeHtml(detail::ShortPath(dir, 2));
            if (!s.branch.empty())
            {
                place += L" \x00B7 " + TgEscapeHtml(s.branch);
            }
            const std::wstring model = s.currentModel.empty() ? s.model : s.currentModel;
            if (!model.empty())
            {
                place += L" \x00B7 " + TgEscapeHtml(model);
            }
            if (!s.effort.empty())
            {
                place += L" \x00B7 " + TgEscapeHtml(s.effort);
            }
            if (s.contextTokens > 0)
            {
                place += L" \x00B7 ctx " + std::to_wstring(s.contextTokens / 1000) + L"K";
            }
            // No line break after it: each line below brings its own, so a line that is left out (the runner's, below)
            // leaves no empty line in the card and no trailing break in the text the hash covers.
            out += place;
        }
        // Runner + queue — only when they say something. "Runner Auto · queue 0" is the common default (every new session
        // gets Auto from the cog, and most queues are empty), so on most cards the line only repeated what the user knows
        // (the user's request of 2026-10-18: "is the common default and should be hidden (not plotted)", §21.18). Any part
        // that is not the default brings the whole line back as it was — a Paused runner, the global pause, or a queued
        // prompt (with the next one's first line) — so a card that shows the line shows the same text as before, and only
        // the cards in the default lose it (one edit each, once, as their hash changes).
        {
            size_t pending = 0;
            std::wstring next;
            for (const auto& q : s.queue)
            {
                if (q.status == PromptStatus::Pending)
                {
                    if (pending == 0)
                    {
                        next = TgFirstLine(q.text, 60);
                    }
                    ++pending;
                }
            }
            const bool runnerIsDefault = s.autorunner.mode == AutorunnerMode::Full && !globalPause && pending == 0;
            if (!runnerIsDefault)
            {
                out += L"\nRunner " + std::wstring{ s.autorunner.mode == AutorunnerMode::Full ? L"Auto" : L"Paused" };
                if (globalPause)
                {
                    out += L" (Off globally)";
                }
                out += L" \x00B7 queue " + std::to_wstring(pending);
                if (!next.empty())
                {
                    out += L" \x2014 next: \x201C" + TgEscapeHtml(next) + L"\x201D";
                }
            }
        }
        if (!s.pendingInput.empty())
        {
            out += L"\n✎ unsent at the PC: \x201C" + TgEscapeHtml(TgFirstLine(s.pendingInput, 80)) + L"\x201D";
        }
        if (s.state == SessionState::Error && !s.errorMessage.empty())
        {
            out += L"\n⚠ " + TgEscapeHtml(TgFirstLine(s.errorMessage, 160));
        }
        // Claude Code's own recap (its away_summary), while it is fresh: newer than every message of the
        // conversation, so it says where things stand and repeats nothing the thread shows. Capped so the card
        // stays a status message (a recap is a few sentences; the cap only guards a runaway one).
        if (extras && extras->recapFresh && !extras->recapText.empty())
        {
            out += L"\n\n\U0001F4DD <b>Recap</b>";
            if (extras->recapAtMs > 0)
            {
                out += L" \x00B7 " + TgClockText(extras->recapAtMs, nowMs);
            }
            const std::wstring text = extras->recapText.size() > 1200 ? extras->recapText.substr(0, 1199) + L"\x2026" : extras->recapText;
            out += L"\n<i>" + TgEscapeHtml(text) + L"</i>";
        }
        return out;
    }
}
