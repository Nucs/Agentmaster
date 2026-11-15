// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SIDE QUESTIONS (Claude Code's /btw, asked out of the terminal): the pure half (MESSAGING.md §21.56).
//
// What Claude Code's own /btw is (2.1.322, read from its bundle and measured on this PC, 2026-11-15):
//   * an IMMEDIATE local command (it runs while a turn runs) that answers ONE question about the conversation in a side
//     request: the main thread's LAST request again (its system prompt, tools, model and messages — so the API serves the
//     whole conversation from the prompt cache), then the earlier btw exchanges of this session as user / assistant
//     message pairs, then one user message: a fixed <system-reminder> (kClaudeSideQuestionReminder) and the question;
//   * every tool denied, one turn, its own tail never cached, nothing written to the transcript;
//   * its exchanges (the "btw thread") live only in the running claude's memory (the last 20); the CLI writes no file.
//
// Agentmaster asks the same question two ways (SideQuestion.cpp routes):
//   1. IN THE SESSION, through the agentmaster-link mod (SessionLink.h): the mod calls Claude Code's `$.model.fork` — the
//      very snapshot /btw forks (the main thread's last request), tools denied, nothing cached or saved — with ONE prompt,
//      which this header builds (BuildSideQuestionForkPrompt): the earlier exchanges folded in, the reminder, the question.
//      Measured: 1.0 s on a small session; 8.3 s and 548,147 tokens read from cache (899 written) on a 548k conversation.
//   2. HEADLESS, when the mod cannot (a parked tab, a claude that ran no turn since it started — `nothing-to-fork` —, a
//      launch without the mod): a hidden `claude -p --resume <id> --fork-session --no-session-persistence` in stream-json
//      mode, sent the SDK's own `side_question` control request — Claude Code's /btw runner itself, the history as real
//      message pairs. Measured: 6 s and 334,769 tokens read from cache (2,590 written) on a live 335k conversation.
//
// Agentmaster keeps the btw thread itself, one file per session (<profile>\btw\<session id>.json, SideQuestionThread*),
// so it survives a restart and `/btw clear` can start it over.
//
// Header-only and pure (no Win32, no filesystem): the m5 harness tests it (tests_side_question.cpp).

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Json.h"
#include "SessionModels.h" // SessionInfo, SessionState, AgentKind (the cache-warmth rule)

namespace Agentmaster
{
    // ===== the thread ====================================================================================================

    /// <summary>
    /// How many exchanges a session's btw thread keeps — Claude Code's own cap (`btwHistory.append` keeps `.slice(-20)`).
    /// Each new exchange beyond it drops the oldest, so the context sent with a question stays bounded.
    /// </summary>
    inline constexpr size_t kSideQuestionThreadMax = 20;

    /// <summary>How an answer was produced: in the session's own claude (the link mod) or by a headless fork.</summary>
    enum class SideQuestionVia
    {
        None, // no answer was produced
        Link, // the agentmaster-link mod's $.model.fork inside the running claude
        Headless, // a hidden `claude -p --resume … --fork-session` and its side_question control request
    };

    /// <summary>The word a log line, the thread file and the reply's footer use for a SideQuestionVia.</summary>
    /// <param name="via">The way.</param>
    /// <returns>"link", "headless" or "none" (a static string).</returns>
    inline const wchar_t* SideQuestionViaWord(SideQuestionVia via) noexcept
    {
        switch (via)
        {
        case SideQuestionVia::Link:
            return L"link";
        case SideQuestionVia::Headless:
            return L"headless";
        default:
            return L"none";
        }
    }

    /// <summary>Reads SideQuestionViaWord's word back; anything else is None.</summary>
    /// <param name="word">The word.</param>
    /// <returns>The way.</returns>
    inline SideQuestionVia ParseSideQuestionVia(std::wstring_view word) noexcept
    {
        if (word == L"link")
        {
            return SideQuestionVia::Link;
        }
        if (word == L"headless")
        {
            return SideQuestionVia::Headless;
        }
        return SideQuestionVia::None;
    }

    /// <summary>
    /// One question and its answer in a session's btw thread. The question and the response are what the next question
    /// sends back as context (as /btw sends its history); the rest is for the reply's header, the log and the user.
    /// </summary>
    struct SideQuestionExchange
    {
        std::wstring question; // the question as the user asked it (the trigger words removed)
        std::wstring response; // Claude's answer, as it came (Markdown)
        std::wstring fallbackNotice; // the notice of a refusal fallback (another model answered); "" = none
        int64_t askedAtMs{ 0 }; // when the question was asked (Unix ms)
        int64_t answeredAtMs{ 0 }; // when the answer arrived (Unix ms)
        SideQuestionVia via{ SideQuestionVia::None }; // how it was answered
        int64_t cacheReadTokens{ -1 }; // the answer's cache-read input tokens (-1 = not reported: the headless way reports none)
        int64_t cacheWriteTokens{ -1 }; // the answer's cache-write input tokens (-1 = not reported)
    };

    /// <summary>A session's btw thread, oldest exchange first, at most kSideQuestionThreadMax.</summary>
    struct SideQuestionThread
    {
        std::wstring sessionId; // the conversation it belongs to ("" in a thread read from a file that named none)
        std::vector<SideQuestionExchange> exchanges; // oldest first
    };

    /// <summary>
    /// Appends one exchange, dropping the oldest past kSideQuestionThreadMax (Claude Code's own rule).
    /// </summary>
    /// <param name="thread">The thread (changed in place).</param>
    /// <param name="exchange">The new exchange.</param>
    /// <exception cref="std::bad_alloc">If the vector cannot grow (the thread is then unchanged).</exception>
    inline void AppendSideQuestionExchange(SideQuestionThread& thread, SideQuestionExchange exchange)
    {
        thread.exchanges.push_back(std::move(exchange));
        if (thread.exchanges.size() > kSideQuestionThreadMax)
        {
            thread.exchanges.erase(thread.exchanges.begin(), thread.exchanges.begin() + static_cast<std::ptrdiff_t>(thread.exchanges.size() - kSideQuestionThreadMax));
        }
    }

    /// <summary>
    /// The thread file's document (<profile>\btw\&lt;session&gt;.json): {"version":1,"sessionId":…,"exchanges":[…]}. The token
    /// counts are written only when reported (a headless answer reports none), so -1 never reaches the file.
    /// </summary>
    /// <param name="thread">The thread.</param>
    /// <returns>The document (an object).</returns>
    /// <exception cref="std::bad_alloc">If the document cannot be allocated.</exception>
    inline json::Value SideQuestionThreadDocument(const SideQuestionThread& thread)
    {
        auto root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
        root.Set(L"sessionId", json::Value::MkStr(thread.sessionId));
        auto arr = json::Value::MkArr();
        for (const auto& x : thread.exchanges)
        {
            auto o = json::Value::MkObj();
            o.Set(L"question", json::Value::MkStr(x.question));
            o.Set(L"response", json::Value::MkStr(x.response));
            if (!x.fallbackNotice.empty())
            {
                o.Set(L"fallbackNotice", json::Value::MkStr(x.fallbackNotice));
            }
            o.Set(L"askedAtMs", json::Value::MkNum(static_cast<double>(x.askedAtMs)));
            o.Set(L"answeredAtMs", json::Value::MkNum(static_cast<double>(x.answeredAtMs)));
            o.Set(L"via", json::Value::MkStr(SideQuestionViaWord(x.via)));
            if (x.cacheReadTokens >= 0)
            {
                o.Set(L"cacheReadTokens", json::Value::MkNum(static_cast<double>(x.cacheReadTokens)));
            }
            if (x.cacheWriteTokens >= 0)
            {
                o.Set(L"cacheWriteTokens", json::Value::MkNum(static_cast<double>(x.cacheWriteTokens)));
            }
            arr.Push(std::move(o));
        }
        root.Set(L"exchanges", std::move(arr));
        return root;
    }

    /// <summary>
    /// The thread a parsed thread file holds. An exchange without a question or a response is skipped (it would send an
    /// empty turn as context); past kSideQuestionThreadMax only the newest are kept (a hand-edited file). Anything that is
    /// not an object with an "exchanges" array reads as an empty thread.
    /// </summary>
    /// <param name="doc">The parsed document.</param>
    /// <returns>The thread, oldest first.</returns>
    /// <exception cref="std::bad_alloc">If the thread cannot be allocated.</exception>
    inline SideQuestionThread SideQuestionThreadFromDocument(const json::Value& doc)
    {
        SideQuestionThread t;
        if (doc.type != json::Value::Type::Obj)
        {
            return t;
        }
        t.sessionId = doc.StrAt(L"sessionId");
        if (const auto* arr = doc.Find(L"exchanges"); arr && arr->type == json::Value::Type::Arr)
        {
            for (const auto& v : arr->arr)
            {
                if (v.type != json::Value::Type::Obj)
                {
                    continue;
                }
                SideQuestionExchange x;
                x.question = v.StrAt(L"question");
                x.response = v.StrAt(L"response");
                if (x.question.empty() || x.response.empty())
                {
                    continue;
                }
                x.fallbackNotice = v.StrAt(L"fallbackNotice");
                x.askedAtMs = v.I64At(L"askedAtMs", 0);
                x.answeredAtMs = v.I64At(L"answeredAtMs", 0);
                x.via = ParseSideQuestionVia(v.StrAt(L"via"));
                x.cacheReadTokens = v.Find(L"cacheReadTokens") ? v.I64At(L"cacheReadTokens", -1) : -1;
                x.cacheWriteTokens = v.Find(L"cacheWriteTokens") ? v.I64At(L"cacheWriteTokens", -1) : -1;
                AppendSideQuestionExchange(t, std::move(x));
            }
        }
        return t;
    }

    // ===== Claude Code's framing, and the one prompt the link's fork sends =================================================

    /// <summary>
    /// Claude Code 2.1.322's side-question note, VERBATIM (its bundle's runner, `Rhn`): the first text of the user message a
    /// /btw sends, ahead of the question. The in-session fork sends it the same way (BuildSideQuestionForkPrompt), so the
    /// model meets the framing /btw gives it: one answer, no tools, no "let me check". The headless way needs no copy: there
    /// Claude Code adds its own.
    /// </summary>
    /// <remarks>⚠ A copy, not a reference: a later Claude Code may reword its note. Compare it with the bundle after an
    /// upgrade (search the binary for "This is a side question from the user") and update the copy and the version named
    /// above.</remarks>
    inline constexpr std::wstring_view kClaudeSideQuestionReminder =
        L"<system-reminder>This is a side question from the user. You must answer this question directly in a single response.\n"
        L"\n"
        L"IMPORTANT CONTEXT:\n"
        L"- You are a separate, lightweight agent spawned to answer this one question\n"
        L"- The main agent is NOT interrupted - it continues working independently in the background\n"
        L"- You share the conversation context but are a completely separate instance\n"
        L"- Do NOT reference being interrupted or what you were \"previously doing\" - that framing is incorrect\n"
        L"\n"
        L"CRITICAL CONSTRAINTS:\n"
        L"- You have NO tools available - you cannot read files, run commands, search, or take any actions\n"
        L"- Do NOT write tool calls or tool output as text (for example invoke or function_calls XML blocks) - nothing you write here is executed; if answering would need reading files, running commands, or searching, say that can't be checked from a side question and suggest asking in the main conversation\n"
        L"- This is a one-off response - there will be no follow-up turns\n"
        L"- You can ONLY provide information based on what you already know from the conversation context\n"
        L"- NEVER say things like \"Let me try...\", \"I'll now...\", \"Let me check...\", or promise to take any action\n"
        L"- If you don't know the answer, say so - do not offer to look it up or investigate\n"
        L"\n"
        L"Simply answer the question with the information you have.</system-reminder>\n"
        L"\n";

    /// <summary>
    /// Claude Code's stand-in for an earlier answer that wrote tool calls as text (`Fl` in its bundle): the history sends this
    /// instead, so the model never reads fake tool output back as a fact.
    /// </summary>
    inline constexpr std::wstring_view kSideQuestionToolTextStandIn = L"(That answer wrote tool calls as text. Nothing was executed, so it is omitted here.)";

    /// <summary>The note Claude Code appends to an answer that was cut off before it finished (`Or` in its bundle).</summary>
    inline constexpr std::wstring_view kSideQuestionCutOffNote = L"_(This answer was cut off before it finished. Ask again to retry.)_";

    /// <summary>Claude Code's stand-in for a cut-off earlier answer in the history (`$l` in its bundle).</summary>
    inline constexpr std::wstring_view kSideQuestionCutOffStandIn = L"(That answer was cut off before it finished, so it is omitted here.)";

    /// <summary>
    /// Whether an answer wrote tool calls as TEXT (what Claude Code's history replaces with kSideQuestionToolTextStandIn): an
    /// "&lt;invoke" or "&lt;function_calls" tag, the forms the side-question note forbids. A heuristic of Agentmaster's (Claude
    /// Code's own test is internal); an answer that merely mentions the words in prose has no '&lt;' before them.
    /// </summary>
    /// <param name="response">The answer.</param>
    /// <returns>True when it holds such a tag.</returns>
    inline bool SideQuestionAnswerWroteToolText(std::wstring_view response) noexcept
    {
        return response.find(L"<invoke") != std::wstring_view::npos || response.find(L"<function_calls") != std::wstring_view::npos;
    }

    /// <summary>
    /// What an earlier exchange's answer is sent back as (Claude Code's own substitutions, `Rhn`): the tool-text stand-in, the
    /// cut-off stand-in, else the answer — with a refusal fallback's notice first ("⚠ notice\n\nanswer"), as /btw does.
    /// </summary>
    /// <param name="x">The exchange.</param>
    /// <returns>The text the history carries for its answer.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring SideQuestionHistoryAnswer(const SideQuestionExchange& x)
    {
        std::wstring answer;
        if (SideQuestionAnswerWroteToolText(x.response))
        {
            answer = kSideQuestionToolTextStandIn;
        }
        else if (x.response.size() >= kSideQuestionCutOffNote.size() &&
                 std::wstring_view{ x.response }.substr(x.response.size() - kSideQuestionCutOffNote.size()) == kSideQuestionCutOffNote)
        {
            answer = kSideQuestionCutOffStandIn;
        }
        else
        {
            answer = x.response;
        }
        return x.fallbackNotice.empty() ? answer : L"\x26A0 " + x.fallbackNotice + L"\n\n" + answer;
    }

    /// <summary>
    /// The ONE user message the in-session fork sends ($.model.fork takes one prompt; /btw sends its history as message pairs
    /// before it). The earlier exchanges come first, folded into a block the model reads as context — each question and the
    /// answer it was given, oldest first, Claude Code's substitutions applied (SideQuestionHistoryAnswer) — then Claude Code's
    /// own side-question note (kClaudeSideQuestionReminder), then the question. With no earlier exchange the message is
    /// exactly what /btw sends: the note, then the question.
    /// </summary>
    /// <param name="thread">The session's btw thread before this question (only the last kSideQuestionThreadMax count).</param>
    /// <param name="question">The question.</param>
    /// <returns>The prompt.</returns>
    /// <exception cref="std::bad_alloc">If the prompt cannot be allocated.</exception>
    inline std::wstring BuildSideQuestionForkPrompt(const SideQuestionThread& thread, std::wstring_view question)
    {
        std::wstring out;
        const size_t skip = thread.exchanges.size() > kSideQuestionThreadMax ? thread.exchanges.size() - kSideQuestionThreadMax : 0;
        if (thread.exchanges.size() > skip)
        {
            // A system-reminder like Claude Code's own context blocks, so the model takes it as context, not as the user's
            // new words. Numbered, so an answer can refer back ("as in side answer 2").
            out += L"<system-reminder>Earlier side questions the user asked about this conversation, with the answers they were given (oldest first). They were asked beside the main conversation and are not part of it:\n";
            size_t n = 0;
            for (size_t i = skip; i < thread.exchanges.size(); ++i)
            {
                const auto& x = thread.exchanges[i];
                ++n;
                out += L"\n<side_question index=\"" + std::to_wstring(n) + L"\">\n" + x.question + L"\n</side_question>\n";
                out += L"<side_answer index=\"" + std::to_wstring(n) + L"\">\n" + SideQuestionHistoryAnswer(x) + L"\n</side_answer>\n";
            }
            out += L"</system-reminder>\n\n";
        }
        out += kClaudeSideQuestionReminder;
        out += question;
        return out;
    }

    // ===== the headless way: Claude Code's own side_question over stream-json =================================================

    /// <summary>The request id Agentmaster gives the headless child's initialize control request.</summary>
    inline constexpr std::wstring_view kSideQuestionInitRequestId = L"am-init";
    /// <summary>The request id of the headless child's side_question control request (the answer comes back under it).</summary>
    inline constexpr std::wstring_view kSideQuestionRequestId = L"am-btw";

    /// <summary>
    /// The two stdin lines that make a headless `claude -p --input-format stream-json` answer a side question: the SDK's
    /// `initialize` (hooks none, agents none: what the SDK sends first), then `side_question` with the question and the
    /// thread's earlier exchanges as `history` ({question, response, fallback_notice} — the control request's own shape;
    /// Claude Code turns them into message pairs exactly as /btw does). Each line is one JSON object and a line feed.
    /// </summary>
    /// <param name="thread">The session's btw thread before this question.</param>
    /// <param name="question">The question.</param>
    /// <returns>The lines, as UTF-16 text (the caller writes them as UTF-8).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildSideQuestionInputLines(const SideQuestionThread& thread, std::wstring_view question)
    {
        auto init = json::Value::MkObj();
        init.Set(L"type", json::Value::MkStr(L"control_request"));
        init.Set(L"request_id", json::Value::MkStr(std::wstring{ kSideQuestionInitRequestId }));
        auto initRequest = json::Value::MkObj();
        initRequest.Set(L"subtype", json::Value::MkStr(L"initialize"));
        initRequest.Set(L"hooks", json::Value{});
        initRequest.Set(L"agents", json::Value::MkObj());
        init.Set(L"request", std::move(initRequest));

        auto history = json::Value::MkArr();
        const size_t skip = thread.exchanges.size() > kSideQuestionThreadMax ? thread.exchanges.size() - kSideQuestionThreadMax : 0;
        for (size_t i = skip; i < thread.exchanges.size(); ++i)
        {
            const auto& x = thread.exchanges[i];
            auto h = json::Value::MkObj();
            h.Set(L"question", json::Value::MkStr(x.question));
            // The answer as stored: Claude Code applies its own substitutions to a history entry (tool text, cut off).
            h.Set(L"response", json::Value::MkStr(x.response));
            if (!x.fallbackNotice.empty())
            {
                h.Set(L"fallback_notice", json::Value::MkStr(x.fallbackNotice));
            }
            history.Push(std::move(h));
        }
        auto sq = json::Value::MkObj();
        sq.Set(L"type", json::Value::MkStr(L"control_request"));
        sq.Set(L"request_id", json::Value::MkStr(std::wstring{ kSideQuestionRequestId }));
        auto request = json::Value::MkObj();
        request.Set(L"subtype", json::Value::MkStr(L"side_question"));
        request.Set(L"question", json::Value::MkStr(std::wstring{ question }));
        if (!history.arr.empty())
        {
            request.Set(L"history", std::move(history));
        }
        sq.Set(L"request", std::move(request));
        return json::Dump(init) + L"\n" + json::Dump(sq) + L"\n";
    }

    /// <summary>What one stdout line of the headless child says about the side question.</summary>
    enum class SideQuestionLineKind
    {
        Other, // nothing about it (a hook line, the initialize answer, a rate-limit event, a line that is not JSON)
        Progress, // a control_request_progress of the side question (started, an API retry)
        Answer, // its control_response: success, with the response object
        Error, // its control_response: error, with Claude Code's reason
    };

    /// <summary>One stdout line of the headless child, read (ReadSideQuestionLine).</summary>
    struct SideQuestionLine
    {
        SideQuestionLineKind kind{ SideQuestionLineKind::Other };
        std::wstring response; // Answer: the answer's text ("" when Claude Code answered with no response: aborted, refused)
        bool hasResponse{ false }; // Answer: the response field was a string (false: null — no answer)
        bool synthetic{ false }; // Answer: a placeholder Claude Code made, not the model's answer (never stored)
        std::wstring fallbackNotice; // Answer: refusal_fallback.content when another model answered
        std::wstring fallbackModel; // Answer: refusal_fallback.fallback_model
        std::wstring error; // Error: Claude Code's message
        std::wstring status; // Progress: "started", "api_retry", …
        int64_t retryAttempt{ 0 }; // Progress (api_retry): the attempt
        int64_t errorStatus{ 0 }; // Progress (api_retry): the HTTP status (0 = none)
    };

    /// <summary>
    /// Reads one stdout line of the headless child (stream-json): the side question's control_response (success with
    /// {response, synthetic, refusal_fallback?}, or error with its message) or its progress line. Lines of other requests
    /// (the initialize answer) and of other kinds read as Other.
    /// </summary>
    /// <param name="line">One line, without its line break.</param>
    /// <returns>The reading.</returns>
    /// <exception cref="std::bad_alloc">If the reading cannot be allocated.</exception>
    inline SideQuestionLine ReadSideQuestionLine(std::wstring_view line)
    {
        SideQuestionLine r;
        if (line.empty() || line.front() != L'{')
        {
            return r;
        }
        const auto parsed = json::Parse(line);
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return r;
        }
        const std::wstring type = parsed->StrAt(L"type");
        if (type == L"control_response")
        {
            const auto* resp = parsed->Find(L"response");
            if (!resp || resp->type != json::Value::Type::Obj || resp->StrAt(L"request_id") != kSideQuestionRequestId)
            {
                return r;
            }
            if (resp->StrAt(L"subtype") == L"error")
            {
                r.kind = SideQuestionLineKind::Error;
                r.error = resp->StrAt(L"error", L"Claude Code gave no reason");
                return r;
            }
            r.kind = SideQuestionLineKind::Answer;
            if (const auto* body = resp->Find(L"response"); body && body->type == json::Value::Type::Obj)
            {
                if (const auto* text = body->Find(L"response"); text && text->type == json::Value::Type::Str)
                {
                    r.response = text->str;
                    r.hasResponse = true;
                }
                r.synthetic = body->BoolAt(L"synthetic", false);
                if (const auto* fb = body->Find(L"refusal_fallback"); fb && fb->type == json::Value::Type::Obj)
                {
                    r.fallbackNotice = fb->StrAt(L"content");
                    r.fallbackModel = fb->StrAt(L"fallback_model");
                }
            }
            return r;
        }
        if (type == L"system" && parsed->StrAt(L"subtype") == L"control_request_progress" && parsed->StrAt(L"request_id") == kSideQuestionRequestId)
        {
            r.kind = SideQuestionLineKind::Progress;
            r.status = parsed->StrAt(L"status");
            r.retryAttempt = parsed->I64At(L"attempt", 0);
            r.errorStatus = parsed->I64At(L"error_status", 0);
        }
        return r;
    }

    /// <summary>
    /// The `--settings` value the headless child runs with (inline JSON): every hook off (`disableAllHooks` — the user's own
    /// SessionStart / SessionEnd hooks would run for a session nobody opened, and Agentmaster's hooks would report it to the
    /// registry), and an `env` block of the session's preset values (the provider's base URL, model aliases…) plus the two
    /// switches that keep the fork from CONTINUING a turn: with CLAUDE_CODE_RESUME_INTERRUPTED_TURN set (the user's
    /// settings set it), a resumed fork of a busy session re-ran the unfinished turn — tools included — before it was
    /// stopped (measured 2026-11-15); "0" plus a 1 ms age keeps it from doing so whatever another source says (a --settings
    /// value beats the user's settings file).
    /// </summary>
    /// <param name="presetEnv">The session's preset settings env (CompiledClaudeLaunch::settingsEnv), in order; may be empty.</param>
    /// <returns>The JSON text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildSideQuestionSettingsJson(const std::vector<std::pair<std::wstring, std::wstring>>& presetEnv)
    {
        auto env = json::Value::MkObj();
        for (const auto& [k, v] : presetEnv)
        {
            if (k == L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN" || k == L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN_MAX_AGE_MS")
            {
                continue; // ours below always win: a preset must never let the fork continue a turn
            }
            env.Set(k, json::Value::MkStr(v));
        }
        env.Set(L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN", json::Value::MkStr(L"0"));
        env.Set(L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN_MAX_AGE_MS", json::Value::MkStr(L"1"));
        auto root = json::Value::MkObj();
        root.Set(L"disableAllHooks", json::Value::MkBool(true));
        root.Set(L"env", std::move(env));
        return json::Dump(root);
    }

    // ===== the cost guard ===================================================================================================

    /// <summary>
    /// How long a conversation's prompt cache is taken to live after its last request: one hour. Claude Code's main thread
    /// writes its cache entries with `ttl: "1h"` on a subscription login (every `api-request` line of 2.1.322 on this PC
    /// carries it); a fork reads that entry. Past it, a side question pays for the WHOLE conversation again.
    /// </summary>
    inline constexpr int64_t kSideQuestionCacheTtlMs = 60LL * 60 * 1000;

    /// <summary>
    /// Whether a side question now most likely READS the conversation from the prompt cache (cheap) rather than writing it
    /// all again: the session is Running (its turn keeps the cache fresh), or its last API activity — the newer of the
    /// transcript's last parent request (convApiActivityUnixMs) and the last turn hook (lastTurnUnixMs) — is younger than
    /// `ttlMs`. Unknown activity (both 0) reads as warm: a session whose timing is not known yet is not refused on a guess.
    /// </summary>
    /// <param name="s">The session.</param>
    /// <param name="nowMs">Now (Unix ms).</param>
    /// <param name="ttlMs">The cache's lifetime (kSideQuestionCacheTtlMs).</param>
    /// <returns>True when the cache is most likely still warm.</returns>
    inline bool SideQuestionCacheLikelyWarm(const SessionInfo& s, int64_t nowMs, int64_t ttlMs = kSideQuestionCacheTtlMs) noexcept
    {
        if (s.state == SessionState::Running)
        {
            return true;
        }
        const int64_t last = (std::max)(s.convApiActivityUnixMs, s.lastTurnUnixMs);
        return last <= 0 || nowMs - last < ttlMs;
    }

    // ===== the answer ======================================================================================================

    /// <summary>What asking one side question produced (SideQuestion.cpp AskSideQuestion), whichever way answered.</summary>
    struct SideQuestionAnswer
    {
        bool ok{ false }; // an answer to show
        std::wstring text; // the answer (Claude's Markdown); "" unless ok
        std::wstring fallbackNotice; // a refusal fallback's notice (another model answered); "" = none
        bool synthetic{ false }; // ok, but Claude Code's placeholder rather than the model's words (shown, never stored)
        SideQuestionVia via{ SideQuestionVia::None }; // the way that answered (or the last way tried)
        std::wstring linkSkipped; // why the in-session way was not used or gave no answer ("" = it answered)
        bool usageKnown{ false }; // the token counts below were reported (the link reports them; headless does not)
        int64_t inputTokens{ 0 }; // uncached input tokens
        int64_t outputTokens{ 0 }; // output tokens
        int64_t cacheReadTokens{ 0 }; // input tokens the prompt cache served (the conversation, when warm)
        int64_t cacheWriteTokens{ 0 }; // input tokens written to the cache
        std::wstring effort; // the effort the answer ran at (the link reports it), "" = not reported
        std::wstring error; // why there is no answer (not ok); "" otherwise
        int64_t tookMs{ 0 }; // from the ask to the answer
    };

    /// <summary>
    /// Reads the in-session fork's result object (the mod posts `$.model.fork`'s result as it is, with the command's "id" and
    /// its own "ms"): an answer with its text, usage and effort, or why there is none. `nothing-to-fork` and `aborted` set
    /// `fallBack` (the headless way can still answer: the claude ran no turn since it started, or the mod's environment
    /// went); an API error or an empty reply do not (the headless way would meet the same refusal, at the cost of a start).
    /// </summary>
    /// <param name="result">The mod's result object.</param>
    /// <param name="answer">Receives the answer, or the error (via Link).</param>
    /// <param name="fallBack">Receives true when the headless way should be tried.</param>
    /// <exception cref="std::bad_alloc">If the strings cannot be copied.</exception>
    inline void ReadSideQuestionForkResult(const json::Value& result, SideQuestionAnswer& answer, bool& fallBack)
    {
        fallBack = false;
        answer.via = SideQuestionVia::Link;
        if (const auto* usage = result.Find(L"usage"); usage && usage->type == json::Value::Type::Obj)
        {
            answer.usageKnown = true;
            answer.inputTokens = usage->I64At(L"input_tokens", 0);
            answer.outputTokens = usage->I64At(L"output_tokens", 0);
            answer.cacheReadTokens = usage->I64At(L"cache_read_input_tokens", 0);
            answer.cacheWriteTokens = usage->I64At(L"cache_creation_input_tokens", 0);
        }
        answer.effort = result.StrAt(L"effort");
        if (result.BoolAt(L"isAnswered", false))
        {
            answer.text = result.StrAt(L"text");
            answer.ok = !answer.text.empty();
            if (!answer.ok)
            {
                answer.error = L"the session's claude answered with no text";
            }
            return;
        }
        const std::wstring reason = result.StrAt(L"reason");
        if (reason == L"nothing-to-fork")
        {
            fallBack = true;
            answer.linkSkipped = L"the session's claude has run no turn since it started (nothing to fork yet)";
        }
        else if (reason == L"aborted")
        {
            fallBack = true;
            answer.linkSkipped = L"the in-session fork was cut off";
        }
        else if (reason == L"api-error")
        {
            const int64_t status = result.I64At(L"status", 0);
            answer.error = L"the API refused the side question (" + result.StrAt(L"error", L"unknown") + (status > 0 ? L", HTTP " + std::to_wstring(status) : std::wstring{}) + L")";
        }
        else if (reason == L"empty-reply")
        {
            answer.error = L"the model replied without text (it tried a tool, which a side question cannot use)";
        }
        else
        {
            // The mod's own failure (its fork threw, an unknown command) or a reason this build does not know: the headless way
            // is Claude Code's own runner, so it is worth the try.
            fallBack = true;
            const std::wstring said = result.StrAt(L"error");
            answer.linkSkipped = L"the in-session fork failed (" + (said.empty() ? (reason.empty() ? std::wstring{ L"no reason given" } : reason) : said) + L")";
        }
    }

    // ===== the OS half (SideQuestion.cpp) ========================================================================================

    /// <summary>
    /// What a session's launch adds to a headless side question (its PRESET: the provider, the model, the settings env and the
    /// key), so the fork talks to the same endpoint and model as the session — never the Claude login by mistake. Filled by
    /// the app's provider (SetSideQuestionLaunchProvider; LaunchPresetsOs.h is app-only), empty in every other program.
    /// </summary>
    struct SideQuestionLaunchExtras
    {
        bool hasPreset{ false }; // the session launched on a preset or a model pick (its launchArgs snapshot)
        std::wstring model; // the preset's --model ("" = none)
        std::wstring effort; // the preset's --effort ("" = none)
        std::wstring appendSystemPrompt; // the preset's --append-system-prompt ("" = none): part of the system prompt the cache keys on
        std::wstring extraArgs; // the preset's raw extra arguments, already quoted ("" = none)
        std::vector<std::pair<std::wstring, std::wstring>> settingsEnv; // the preset's settings env block (base URL, aliases)
        std::vector<std::pair<std::wstring, std::wstring>> processEnv; // AM_PRESET and the key's carrier variable
        std::wstring authTokenEnv; // the carrier variable whose value becomes ANTHROPIC_AUTH_TOKEN ("" = the Claude login)
        std::wstring error; // the preset cannot launch (no key, no base URL): the side question is refused, never sent elsewhere
    };

    /// <summary>The app's preset lookup for a headless side question (session id, its working folder) → its extras.</summary>
    using SideQuestionLaunchProvider = SideQuestionLaunchExtras (*)(const std::wstring& sessionId, const std::wstring& workDir) noexcept;

    /// <summary>Sets the preset lookup (once per process, by the app). Unset = no preset is applied (the Claude login).</summary>
    /// <param name="provider">The lookup, or nullptr.</param>
    void SetSideQuestionLaunchProvider(SideQuestionLaunchProvider provider) noexcept;

    /// <summary>The folder the btw threads live in: &lt;profile&gt;\btw.</summary>
    /// <returns>The folder's path.</returns>
    /// <exception cref="std::bad_alloc">If the path cannot be allocated.</exception>
    std::wstring SideQuestionThreadFolder();

    /// <summary>A session's btw thread file: &lt;profile&gt;\btw\&lt;session id&gt;.json ("" for an id that is no file name).</summary>
    /// <param name="sessionId">The conversation.</param>
    /// <returns>The path, or "".</returns>
    /// <exception cref="std::bad_alloc">If the path cannot be allocated.</exception>
    std::wstring SideQuestionThreadPath(const std::wstring& sessionId);

    /// <summary>
    /// Reads a session's btw thread. A missing file is an empty thread; a file that cannot be read or parsed is an empty thread
    /// too, with `readable` false — the caller then never writes over it (Rule #16: a file Agentmaster could not read is never
    /// replaced without its bytes kept).
    /// </summary>
    /// <param name="sessionId">The conversation.</param>
    /// <param name="readable">Receives false when the file exists but could not be read.</param>
    /// <returns>The thread (sessionId set).</returns>
    SideQuestionThread LoadSideQuestionThread(const std::wstring& sessionId, bool& readable) noexcept;

    /// <summary>
    /// Adds one exchange to a session's btw thread file: a read-modify-write of the freshest file (ReadStateFileUtf8 → the
    /// thread + the exchange → WriteStateDocument, the guarded atomic write). Refused (false, logged) when the file exists
    /// but cannot be read. Callers serialize per session (the Telegram service runs one side question per session at a time).
    /// </summary>
    /// <param name="sessionId">The conversation.</param>
    /// <param name="exchange">The exchange.</param>
    /// <returns>True when the file now holds it.</returns>
    bool SaveSideQuestionExchange(const std::wstring& sessionId, const SideQuestionExchange& exchange) noexcept;

    /// <summary>
    /// Starts a session's btw thread over (/btw clear): its file is deleted (a thread file is the one record of these
    /// exchanges, and the user asked to drop them). Returns how many exchanges it held; -1 when it could not be read or
    /// deleted (it is then left as it is).
    /// </summary>
    /// <param name="sessionId">The conversation.</param>
    /// <returns>The exchanges forgotten (0 for no file), or -1.</returns>
    int ClearSideQuestionThread(const std::wstring& sessionId) noexcept;

    /// <summary>One side question to ask (AskSideQuestion).</summary>
    struct SideQuestionRequest
    {
        std::wstring sessionId; // the conversation asked about (the link is found by it)
        std::wstring resumeSessionId; // the transcript the headless way resumes ("" = sessionId; a never-messaged fork's source)
        std::wstring workDir; // the session's launch folder (--resume finds its transcript from there)
        std::wstring claudeExe; // the claude.exe the headless way runs ("" = Claude Code not found: no headless way)
        std::wstring question; // the question
        SideQuestionThread thread; // the btw thread before this question (its exchanges go with the question)
        bool tryLink{ true }; // ask the session's own claude first (the agentmaster-link mod)
        bool allowHeadless{ true }; // fall back to a headless fork
        uint32_t linkTimeoutMs{ 11 * 60 * 1000 }; // the in-session fork's limit (Claude Code's own side-question deadline: 660 s)
        uint32_t headlessTimeoutMs{ 12 * 60 * 1000 }; // the headless child's whole life (its start + the same deadline)
    };

    /// <summary>
    /// Asks one side question and WAITS for the answer (minutes at most): in the session's own claude through the link when
    /// it is live (SessionLink.h), else — or when the in-session fork could not answer (nothing to fork yet) — by a headless
    /// fork of its transcript. Logs one [btw] line per way tried. Blocks: call it from a worker thread.
    /// </summary>
    /// <param name="r">The question and the session.</param>
    /// <returns>The answer, or why there is none.</returns>
    SideQuestionAnswer AskSideQuestion(const SideQuestionRequest& r) noexcept;
}
