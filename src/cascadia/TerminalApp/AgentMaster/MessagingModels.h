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
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Json.h"
#include "SessionModels.h"
#include "TelegramFormat.h"
#include "TranscriptStore.h" // IsNoiseUserPrompt, PickDisplayTitle

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
            arg = TgFirstLine(str(L"command"), kToolSummaryMaxChars);
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
    /// <returns>The items, in block order; empty when the line shows nothing (bookkeeping lines, meta
    /// lines, sidechain lines, the synthetic resume placeholder, protocol noise).</returns>
    /// <remarks>
    /// The REMARK-vs-FINAL decision for an assistant Text is the SERVICE's: this reads the stop reason
    /// (turnEnds) but a text with a non-terminal stop reason is a remark only if a tool call follows
    /// it; the service holds the text until it knows. A prompt's SOURCE (typed at the PC, sent by the
    /// runner, sent from the topic) is the service's too: it needs the session's queue.
    /// </remarks>
    /// <exception cref="std::bad_alloc">If the items cannot be allocated.</exception>
    inline std::vector<TopicItem> ClassifyTranscriptLine(const json::Value& line)
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
        /// The RECAP (§21.5): the last prompt's first line and the last reply's head, with the transcript moment
        /// they are from (`recapAtMs`, shown as a clock time) and when the service learned them (`recapSeenMs`).
        /// Seeded from the transcript tail when the topic is created — a topic mirrors from its creation on,
        /// so without it the first card would sit over nothing — and kept by the mirror. The card shows it only
        /// while it is newer than the newest post in the topic (`recapSeenMs > lastPostedMs`): once the reply
        /// itself is right above the card, repeating it would be noise.
        /// </summary>
        std::wstring recapPrompt;
        std::wstring recapReply;
        int64_t recapAtMs{ 0 };
        int64_t recapSeenMs{ 0 };
        /// <summary>
        /// The topic was told that the session is in Error; cleared when the session runs again. Without it a
        /// session at rest in Error was announced at EVERY app start (the registry loads it as Idle, the scanner
        /// re-asserts Error — an edge each time; measured 2026-10-16, 19:30 and 19:45).
        /// </summary>
        bool errorAnnounced{ false };
    };

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
        std::map<std::wstring, TopicRecord> topics; // session id -> its topic
    };

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
            to.Set(L"recapPrompt", json::Value::MkStr(t.recapPrompt));
            to.Set(L"recapReply", json::Value::MkStr(t.recapReply));
            to.Set(L"recapAtMs", json::Value::MkNum(static_cast<double>(t.recapAtMs)));
            to.Set(L"recapSeenMs", json::Value::MkNum(static_cast<double>(t.recapSeenMs)));
            to.Set(L"errorAnnounced", json::Value::MkBool(t.errorAnnounced));
            topics.Push(std::move(to));
        }
        o.Set(L"topics", std::move(topics));
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
                t.recapPrompt = to.StrAt(L"recapPrompt");
                t.recapReply = to.StrAt(L"recapReply");
                t.recapAtMs = to.I64At(L"recapAtMs", 0);
                t.recapSeenMs = to.I64At(L"recapSeenMs", 0);
                t.errorAnnounced = to.BoolAt(L"errorAnnounced", false);
                s.topics[t.sessionId] = std::move(t);
            }
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
        std::wstring colorHex; // the tab color ("" = unknown)
    };

    /// <summary>
    /// The one rule behind "topics = tabs" (MESSAGING.md §5.1, D3): a topic exists for a Claude Code
    /// session that has a tab, unless its state is Idle. A pinned session counts as listed whatever
    /// its state (§5.3).
    /// </summary>
    /// <param name="f">The session's facts.</param>
    /// <returns>True when the session should have a topic.</returns>
    inline bool QualifiesForTopic(const TabFact& f) noexcept
    {
        return f.live && f.inTab && f.claude && (f.pinned || f.state != SessionState::Idle);
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
        };
        Kind kind{ Kind::Create };
        std::wstring sessionId;
        std::wstring reason; // Delete: "closed" | "idle" | "not a claude tab"
        std::wstring title; // Create / Rename: the title to use
        std::wstring colorHex; // Create: the tab color
    };

    /// <summary>
    /// Plans the topic edits that make the topic set equal the qualifying tabs (M8). Pure; the
    /// service applies the ops and updates the records (IdleCard sets idleSinceMs, UnidleCard clears it).
    /// </summary>
    /// <param name="facts">Every LIVE session's facts (a session absent from the list is closed).</param>
    /// <param name="topics">The topics that exist (the state file).</param>
    /// <param name="nowMs">The clock.</param>
    /// <param name="idleGraceMs">How long an Idle session's topic stays before it is deleted (D25).</param>
    /// <returns>The ops, deterministic: deletes, then creates, then renames and card changes, each in
    /// session-id order.</returns>
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
        // Deletes first (topics is sorted by session id).
        for (const auto& [sid, t] : topics)
        {
            const auto it = byId.find(sid);
            const TabFact* f = it == byId.end() ? nullptr : it->second;
            if (!f || !f->live || !f->inTab || !f->claude)
            {
                TopicSyncOp op;
                op.kind = TopicSyncOp::Kind::Delete;
                op.sessionId = sid;
                op.reason = !f || !f->live ? L"closed" : (!f->claude ? L"not a claude tab" : L"left its tab");
                ops.push_back(std::move(op));
                continue;
            }
            if (!QualifiesForTopic(*f))
            {
                // Idle: the grace clock decides.
                if (t.idleSinceMs == 0)
                {
                    TopicSyncOp op;
                    op.kind = TopicSyncOp::Kind::IdleCard;
                    op.sessionId = sid;
                    ops.push_back(std::move(op));
                }
                else if (idleGraceMs <= 0 || nowMs - t.idleSinceMs >= idleGraceMs)
                {
                    TopicSyncOp op;
                    op.kind = TopicSyncOp::Kind::Delete;
                    op.sessionId = sid;
                    op.reason = L"idle";
                    ops.push_back(std::move(op));
                }
                continue;
            }
            if (t.idleSinceMs != 0)
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
                ops.push_back(std::move(op));
            }
        }
        // Creates, in session-id order (byId is a map).
        for (const auto& [sid, f] : byId)
        {
            if (topics.find(sid) != topics.end() || !QualifiesForTopic(*f))
            {
                continue;
            }
            TopicSyncOp op;
            op.kind = TopicSyncOp::Kind::Create;
            op.sessionId = sid;
            op.title = f->title.empty() ? sid.substr(0, 8) : f->title;
            op.colorHex = f->colorHex;
            ops.push_back(std::move(op));
        }
        return ops;
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
        L"reactivate"
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
    /// What the card shows beyond the session record (§21.5): the RECAP — the last prompt's first line and the
    /// last reply's head, with their transcript moment — shown only when `recap` is set (the service sets it
    /// while nothing newer than the recap was posted in the topic, so the card never repeats the message right
    /// above it).
    /// </summary>
    struct CardExtras
    {
        bool recap{ false };
        std::wstring recapPrompt; // plain text, the first line of the last prompt
        std::wstring recapReply; // plain text, the head of the last reply
        int64_t recapAtMs{ 0 }; // the transcript moment of the newer of the two (a clock time in the card)
    };

    /// <summary>
    /// The topic's STICKY card (MESSAGING.md §6.1, §21.5), as Telegram HTML: the title, the state and since
    /// when (a clock time), a "not running / Reactivate" line for a parked tab or a session in Error, folder ·
    /// branch · model · context, the runner and the queue, the unsent draft, and the recap when it is fresh.
    /// Pure, so the service compares two renders and edits only on a change — which is why nothing in it may
    /// be a relative span (see TgClockText): every change is an API call on the one chat.
    /// </summary>
    /// <param name="s">The session (a full record: the queue is read).</param>
    /// <param name="nowMs">The clock.</param>
    /// <param name="idleLeavesAtMs">When an Idle topic leaves the list (0 = not idle).</param>
    /// <param name="globalPause">The runner's process-wide pause.</param>
    /// <param name="extras">The recap and its freshness (CardExtras); null = no recap block.</param>
    /// <returns>The HTML text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildCardText(const SessionInfo& s, int64_t nowMs, int64_t idleLeavesAtMs, bool globalPause, const CardExtras* extras = nullptr)
    {
        std::wstring out;
        out += L"\U0001F4CC <b>" + TgEscapeHtml(s.title.empty() ? s.id.substr(0, 8) : s.title) + L"</b>\n";
        // State line.
        out += StateLabel(s.state);
        // CLOCK TIMES, never spans: the service edits the card whenever this text changes, so a "3m" that
        // became "4m" re-edited every card every minute — 47 topics' cards made ~47 edits a minute and
        // Telegram answered with a 429 whose retry_after was 21 minutes (2026-10-16). "since 19:04"
        // changes once, at the next midnight.
        if (s.stateChangedAtUnixMs > 0 && nowMs > s.stateChangedAtUnixMs)
        {
            out += L" since " + TgClockText(s.stateChangedAtUnixMs, nowMs);
        }
        if (s.pinned)
        {
            out += L" \U0001F4CC";
        }
        // The user is not supposed to care whether a tab is "activated" (§21.5): a message here starts a parked
        // tab by itself, and ▶ Reactivate (the card's button, /reactivate) does it on demand — also for a
        // session in Error (a cut-off or failed turn), which a restart of the same conversation recovers.
        if (s.live && !s.started && !s.external)
        {
            out += L"\n\U0001F4A4 not running \x2014 a message here or \x25B6 Reactivate resumes it";
        }
        else if (s.live && s.state == SessionState::Error)
        {
            out += L"\n\x25B6 Reactivate restarts it (the same conversation goes on)";
        }
        if (idleLeavesAtMs > 0)
        {
            out += L"\n\U0001F4A4 leaves the list at " + TgClockText(idleLeavesAtMs, nowMs) + L" (/pin keeps it, /wake brings it back)";
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
            out += place + L"\n";
        }
        // Runner + queue.
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
            out += L"Runner " + std::wstring{ s.autorunner.mode == AutorunnerMode::Full ? L"Auto" : L"Paused" };
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
        if (!s.pendingInput.empty())
        {
            out += L"\n✎ unsent at the PC: \x201C" + TgEscapeHtml(TgFirstLine(s.pendingInput, 80)) + L"\x201D";
        }
        if (s.state == SessionState::Error && !s.errorMessage.empty())
        {
            out += L"\n⚠ " + TgEscapeHtml(TgFirstLine(s.errorMessage, 160));
        }
        // The recap: where the conversation stands, for a card that sits over nothing newer (a topic that was
        // just created mirrors from now on; a restart may have skipped nothing but the card is still the only
        // thing on screen). Hidden the moment the reply itself is posted above the card.
        if (extras && extras->recap && (!extras->recapPrompt.empty() || !extras->recapReply.empty()))
        {
            out += L"\n\n<b>Recap</b>";
            if (extras->recapAtMs > 0)
            {
                out += L" \x00B7 " + TgClockText(extras->recapAtMs, nowMs);
            }
            if (!extras->recapPrompt.empty())
            {
                out += L"\n\U0001F464 " + TgEscapeHtml(extras->recapPrompt);
            }
            if (!extras->recapReply.empty())
            {
                out += L"\n\U0001F916 " + TgEscapeHtml(extras->recapReply);
            }
        }
        return out;
    }
}
