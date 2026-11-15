// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.56: /btw, a SIDE QUESTION asked from a session's Telegram topic and answered there (the
// user's request of 2026-11-15), and the SESSION LINK it runs over (MOD_LINK.md's first slice). This file pins:
//   1. SideQuestion.h: the btw thread (its cap, its file's document and round trip), Claude Code's framing and the one prompt
//      the in-session fork sends, the headless way's stdin lines and the reader of its stdout, its --settings value, the
//      cost guard, and the reading of the mod's fork result;
//   2. SessionLink.h: the UTF-8 codec, the request reader (heads, bodies, queries, tokens, pipelining, refusals), the response
//      writer, and LinkHub (tokens, hello, polls, queues, liveness, the choice between two links, forgetting);
//   3. MessagingBtw.h: what a topic message is (every form of /btw, the words, the clear forms, "/clear btw"), and the texts;
//   4. the OS half for real: the link server over loopback with a scripted mod (hello, a held poll, a command, its result,
//      an unknown token, a session with no link, a result under another link's token, a command never answered —
//      TimedOut —, a link that stops polling — Lost), and the stream runner with Windows PowerShell and cmd.exe standing
//      in for claude (its input written, its lines read until done, a child that never ends).
// claude is never started by the suite; the live checks are MESSAGING.md §21.56's "Check after the build".

#include "m5_tests.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../ClaudeStatus.h" // RunClaudeStreamJson
#include "../MessagingBtw.h"
#include "../SessionLink.h"
#include "../SideQuestion.h"

#pragma comment(lib, "ws2_32.lib")

using namespace Agentmaster;
using namespace Agentmaster::Link;

namespace
{
    /// <summary>An exchange with the given question and answer (the rest left at its defaults).</summary>
    /// <param name="q">The question.</param>
    /// <param name="a">The answer.</param>
    /// <returns>The exchange.</returns>
    SideQuestionExchange Exchange(const std::wstring& q, const std::wstring& a)
    {
        SideQuestionExchange x;
        x.question = q;
        x.response = a;
        return x;
    }

    /// <summary>
    /// One HTTP exchange with the link server over a fresh loopback connection (a mod's fetch, scripted): the request is sent
    /// whole, the response read until the server's Content-Length is in.
    /// </summary>
    /// <param name="port">The server's port.</param>
    /// <param name="request">The request's bytes.</param>
    /// <returns>The response's bytes ("" when the connection failed).</returns>
    std::string HttpOnce(uint16_t port, const std::string& request)
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
        {
            return {};
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::closesocket(s);
            return {};
        }
        ::send(s, request.data(), static_cast<int>(request.size()), 0);
        std::string got;
        char buf[4096];
        for (;;)
        {
            const size_t headEnd = got.find("\r\n\r\n");
            if (headEnd != std::string::npos)
            {
                const size_t cl = got.find("Content-Length: ");
                if (cl != std::string::npos && cl < headEnd)
                {
                    const size_t length = std::stoul(got.substr(cl + 16, got.find("\r\n", cl) - cl - 16));
                    if (got.size() >= headEnd + 4 + length)
                    {
                        break;
                    }
                }
            }
            const int n = ::recv(s, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
            got.append(buf, static_cast<size_t>(n));
        }
        ::closesocket(s);
        return got;
    }

    /// <summary>A request's bytes: method, target, the token header, and a JSON body when given.</summary>
    /// <param name="method">"GET" or "POST".</param>
    /// <param name="target">The target.</param>
    /// <param name="token">The x-am-token value ("" = none).</param>
    /// <param name="body">The body ("" = none).</param>
    /// <returns>The bytes.</returns>
    std::string Request(const char* method, const std::string& target, const std::string& token, const std::string& body)
    {
        std::string r = std::string{ method } + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        if (!token.empty())
        {
            r += "x-am-token: " + token + "\r\n";
        }
        if (!body.empty())
        {
            r += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        }
        r += "Connection: close\r\n\r\n" + body;
        return r;
    }

    /// <summary>The body of an HTTP response (after the blank line).</summary>
    /// <param name="response">The response's bytes.</param>
    /// <returns>The body.</returns>
    std::string BodyOf(const std::string& response)
    {
        const size_t at = response.find("\r\n\r\n");
        return at == std::string::npos ? std::string{} : response.substr(at + 4);
    }
}

void TestSideQuestion()
{
    std::wprintf(L"== TestSideQuestion (MESSAGING.md 21.56: /btw - the side question, its thread, the session link, the topic's forms) ==\n");

    // ---- 1. the thread: its cap and its file ----
    {
        SideQuestionThread t;
        t.sessionId = L"s1";
        for (int i = 1; i <= 23; ++i)
        {
            AppendSideQuestionExchange(t, Exchange(L"q" + std::to_wstring(i), L"a" + std::to_wstring(i)));
        }
        CHECK(t.exchanges.size() == kSideQuestionThreadMax && kSideQuestionThreadMax == 20, "thread: 20 kept, Claude Code's own cap");
        CHECK(t.exchanges.front().question == L"q4" && t.exchanges.back().question == L"q23", "thread: the oldest go first");

        SideQuestionThread w;
        w.sessionId = L"11c33452-7cab-46d4-b5cc-31296b3f739c";
        SideQuestionExchange x = Exchange(L"What is the codename?", L"PELICAN-42 \x2014 \x05E9\x05DC\x05D5\x05DD \U0001F426");
        x.fallbackNotice = L"Another model answered";
        x.askedAtMs = 1794741749588;
        x.answeredAtMs = 1794741753512;
        x.via = SideQuestionVia::Link;
        x.cacheReadTokens = 548147;
        x.cacheWriteTokens = 899;
        AppendSideQuestionExchange(w, x);
        SideQuestionExchange h = Exchange(L"And the deadline?", L"A Friday.");
        h.via = SideQuestionVia::Headless;
        AppendSideQuestionExchange(w, h);
        const json::Value doc = SideQuestionThreadDocument(w);
        CHECK(doc.I64At(L"version") == 1 && doc.StrAt(L"sessionId") == w.sessionId, "file: version 1 and the session's id");
        const auto* arr = doc.Find(L"exchanges");
        CHECK(arr && arr->arr.size() == 2 && arr->arr[1].Find(L"cacheReadTokens") == nullptr && arr->arr[1].Find(L"fallbackNotice") == nullptr,
              "file: token counts and a notice only when there are some");
        const auto parsed = json::Parse(json::Dump(doc));
        const SideQuestionThread back = parsed ? SideQuestionThreadFromDocument(*parsed) : SideQuestionThread{};
        CHECK(back.exchanges.size() == 2 && back.exchanges[0].question == x.question && back.exchanges[0].response == x.response && back.exchanges[0].fallbackNotice == x.fallbackNotice &&
                  back.exchanges[0].askedAtMs == x.askedAtMs && back.exchanges[0].answeredAtMs == x.answeredAtMs && back.exchanges[0].via == SideQuestionVia::Link &&
                  back.exchanges[0].cacheReadTokens == 548147 && back.exchanges[0].cacheWriteTokens == 899,
              "file: an exchange round-trips whole (Hebrew and an emoji kept)");
        CHECK(back.exchanges.size() == 2 && back.exchanges[1].via == SideQuestionVia::Headless && back.exchanges[1].cacheReadTokens == -1, "file: a headless exchange reports no tokens");
        const auto messy = json::Parse(LR"({"sessionId":"s","exchanges":[{"question":"","response":"x"},{"question":"q"},"no",{"question":"ok","response":"fine","via":"teleport"}]})");
        const SideQuestionThread m = messy ? SideQuestionThreadFromDocument(*messy) : SideQuestionThread{};
        CHECK(m.exchanges.size() == 1 && m.exchanges[0].question == L"ok" && m.exchanges[0].via == SideQuestionVia::None, "file: an exchange missing its question or answer is skipped; an unknown way reads as none");
        CHECK(SideQuestionThreadFromDocument(json::Value::MkArr()).exchanges.empty(), "file: not an object, no exchanges");
    }

    // ---- 1b. Claude Code's framing and the fork's one prompt ----
    {
        SideQuestionThread empty;
        CHECK(BuildSideQuestionForkPrompt(empty, L"Why?") == std::wstring{ kClaudeSideQuestionReminder } + L"Why?", "fork prompt: no thread -> exactly what /btw sends: the note, then the question");
        CHECK(kClaudeSideQuestionReminder.rfind(L"<system-reminder>This is a side question from the user.", 0) == 0 &&
                  kClaudeSideQuestionReminder.find(L"You have NO tools available") != std::wstring_view::npos &&
                  kClaudeSideQuestionReminder.size() >= 2 && kClaudeSideQuestionReminder.substr(kClaudeSideQuestionReminder.size() - 2) == L"\n\n",
              "fork prompt: the note is Claude Code's, verbatim, and ends with its blank line");
        SideQuestionThread t;
        AppendSideQuestionExchange(t, Exchange(L"First?", L"One."));
        SideQuestionExchange cut = Exchange(L"Second?", std::wstring{ L"Half an answer " } + std::wstring{ kSideQuestionCutOffNote });
        AppendSideQuestionExchange(t, cut);
        SideQuestionExchange tool = Exchange(L"Third?", L"<function_calls><invoke name=\"Read\"></invoke></function_calls>");
        tool.fallbackNotice = L"Opus refused; Sonnet answered";
        AppendSideQuestionExchange(t, tool);
        const std::wstring p = BuildSideQuestionForkPrompt(t, L"Fourth?");
        const size_t block = p.find(L"<system-reminder>Earlier side questions");
        const size_t note = p.find(L"<system-reminder>This is a side question");
        CHECK(block == 0 && note != std::wstring::npos && note > block && p.size() > 8 && p.substr(p.size() - 7) == L"Fourth?", "fork prompt: the earlier exchanges, then the note, then the question");
        CHECK(p.find(L"<side_question index=\"1\">\nFirst?\n</side_question>") != std::wstring::npos && p.find(L"<side_answer index=\"1\">\nOne.\n</side_answer>") != std::wstring::npos,
              "fork prompt: each exchange numbered, question and answer");
        CHECK(p.find(std::wstring{ kSideQuestionCutOffStandIn }) != std::wstring::npos && p.find(L"Half an answer") == std::wstring::npos, "fork prompt: a cut-off answer goes as Claude Code's stand-in");
        CHECK(p.find(L"\x26A0 Opus refused; Sonnet answered\n\n" + std::wstring{ kSideQuestionToolTextStandIn }) != std::wstring::npos && p.find(L"<invoke") == std::wstring::npos,
              "fork prompt: tool calls written as text go as the stand-in, the fallback's notice first");
        CHECK(SideQuestionAnswerWroteToolText(L"use <invoke name=x>") && !SideQuestionAnswerWroteToolText(L"I would invoke the Read tool"), "fork prompt: only a tag counts as tool text");
    }

    // ---- 1c. the headless way's lines ----
    {
        SideQuestionThread t;
        SideQuestionExchange x = Exchange(L"Q1", L"A1");
        x.fallbackNotice = L"notice";
        AppendSideQuestionExchange(t, x);
        const std::wstring lines = BuildSideQuestionInputLines(t, L"Q2 \"quoted\"\nsecond line");
        const size_t nl = lines.find(L'\n');
        CHECK(nl != std::wstring::npos && lines.back() == L'\n' && lines.find(L'\n', nl + 1) == lines.size() - 1, "headless: exactly two lines, each ending in a line feed");
        const auto init = json::Parse(lines.substr(0, nl));
        const auto sq = json::Parse(lines.substr(nl + 1, lines.size() - nl - 2));
        CHECK(init && init->StrAt(L"type") == L"control_request" && init->StrAt(L"request_id") == kSideQuestionInitRequestId && init->Find(L"request") &&
                  init->Find(L"request")->StrAt(L"subtype") == L"initialize",
              "headless: the SDK's initialize first");
        const auto* req = sq ? sq->Find(L"request") : nullptr;
        CHECK(sq && sq->StrAt(L"request_id") == kSideQuestionRequestId && req && req->StrAt(L"subtype") == L"side_question" && req->StrAt(L"question") == L"Q2 \"quoted\"\nsecond line",
              "headless: then side_question with the question as typed");
        const auto* hist = req ? req->Find(L"history") : nullptr;
        CHECK(hist && hist->arr.size() == 1 && hist->arr[0].StrAt(L"question") == L"Q1" && hist->arr[0].StrAt(L"response") == L"A1" && hist->arr[0].StrAt(L"fallback_notice") == L"notice",
              "headless: the thread as the control request's own history shape");
        const std::wstring noHistory = BuildSideQuestionInputLines(SideQuestionThread{}, L"x");
        CHECK(noHistory.find(L"\"history\"") == std::wstring::npos, "headless: no thread, no history field");

        const auto answer = ReadSideQuestionLine(LR"({"type":"control_response","response":{"subtype":"success","request_id":"am-btw","response":{"response":"The codename is HERON-7.","synthetic":false}}})");
        CHECK(answer.kind == SideQuestionLineKind::Answer && answer.hasResponse && answer.response == L"The codename is HERON-7." && !answer.synthetic, "headless: the answer line (measured shape)");
        const auto fallback = ReadSideQuestionLine(LR"({"type":"control_response","response":{"subtype":"success","request_id":"am-btw","response":{"response":"x","synthetic":false,"refusal_fallback":{"original_model":"o","fallback_model":"claude-sonnet-5-5","content":"Opus declined"}}}})");
        CHECK(fallback.kind == SideQuestionLineKind::Answer && fallback.fallbackNotice == L"Opus declined" && fallback.fallbackModel == L"claude-sonnet-5-5", "headless: a refusal fallback's notice is kept");
        const auto none = ReadSideQuestionLine(LR"({"type":"control_response","response":{"subtype":"success","request_id":"am-btw","response":{"response":null,"synthetic":false}}})");
        CHECK(none.kind == SideQuestionLineKind::Answer && !none.hasResponse, "headless: a null response is an answer with no text");
        const auto error = ReadSideQuestionLine(LR"({"type":"control_response","response":{"subtype":"error","request_id":"am-btw","error":"side_question timed out"}})");
        CHECK(error.kind == SideQuestionLineKind::Error && error.error == L"side_question timed out", "headless: an error line");
        const auto progress = ReadSideQuestionLine(LR"({"type":"system","subtype":"control_request_progress","request_id":"am-btw","status":"api_retry","attempt":2,"error_status":529})");
        CHECK(progress.kind == SideQuestionLineKind::Progress && progress.status == L"api_retry" && progress.retryAttempt == 2 && progress.errorStatus == 529, "headless: an API retry's progress");
        CHECK(ReadSideQuestionLine(LR"({"type":"control_response","response":{"subtype":"success","request_id":"am-init","response":{"commands":[]}}})").kind == SideQuestionLineKind::Other,
              "headless: the initialize answer is not the side question's");
        CHECK(ReadSideQuestionLine(L"[stderr] noise").kind == SideQuestionLineKind::Other && ReadSideQuestionLine(L"{torn").kind == SideQuestionLineKind::Other &&
                  ReadSideQuestionLine(LR"({"type":"system","subtype":"hook_started"})").kind == SideQuestionLineKind::Other,
              "headless: other lines and junk read as Other");

        const auto settings = json::Parse(BuildSideQuestionSettingsJson({ { L"ANTHROPIC_BASE_URL", L"https://example.com" }, { L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN", L"1" } }));
        const auto* env = settings ? settings->Find(L"env") : nullptr;
        CHECK(settings && settings->BoolAt(L"disableAllHooks") && env && env->StrAt(L"ANTHROPIC_BASE_URL") == L"https://example.com", "headless settings: hooks off, the preset's env kept");
        size_t resumeCount = 0;
        for (const auto& [k, v] : env ? env->members : std::vector<std::pair<std::wstring, json::Value>>{})
        {
            resumeCount += k == L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN" ? 1 : 0;
        }
        CHECK(env && env->StrAt(L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN") == L"0" && env->StrAt(L"CLAUDE_CODE_RESUME_INTERRUPTED_TURN_MAX_AGE_MS") == L"1" && resumeCount == 1,
              "headless settings: the fork never continues a turn, whatever the preset says");
    }

    // ---- 1d. the cost guard ----
    {
        const int64_t now = 1794741749588;
        SessionInfo s;
        s.state = SessionState::WaitingForInput;
        s.convApiActivityUnixMs = now - 10 * 60 * 1000;
        CHECK(SideQuestionCacheLikelyWarm(s, now), "cache: 10 minutes after the last request -> warm");
        s.convApiActivityUnixMs = now - 61 * 60 * 1000;
        CHECK(!SideQuestionCacheLikelyWarm(s, now), "cache: 61 minutes -> cold (ask first)");
        s.lastTurnUnixMs = now - 30 * 1000;
        CHECK(SideQuestionCacheLikelyWarm(s, now), "cache: the newer of the two clocks counts");
        s.lastTurnUnixMs = 0;
        s.state = SessionState::Running;
        CHECK(SideQuestionCacheLikelyWarm(s, now), "cache: a running turn keeps it warm");
        SessionInfo unknown;
        CHECK(SideQuestionCacheLikelyWarm(unknown, now), "cache: unknown timing is not refused on a guess");
    }

    // ---- 1e. the mod's fork result ----
    {
        bool fallBack = true;
        SideQuestionAnswer a;
        const auto ok = json::Parse(LR"({"id":"c1","isAnswered":true,"text":"Yes.","usage":{"input_tokens":425,"output_tokens":724,"cache_read_input_tokens":548147,"cache_creation_input_tokens":899},"effort":"max","ms":8342})");
        ReadSideQuestionForkResult(*ok, a, fallBack);
        CHECK(a.ok && a.text == L"Yes." && !fallBack && a.via == SideQuestionVia::Link && a.usageKnown && a.cacheReadTokens == 548147 && a.cacheWriteTokens == 899 && a.inputTokens == 425 &&
                  a.outputTokens == 724 && a.effort == L"max",
              "fork result: an answer with its usage (the measured 548k read)");
        for (const wchar_t* text : { LR"({"isAnswered":false,"reason":"nothing-to-fork"})", LR"({"isAnswered":false,"reason":"aborted","usage":{}})",
                                     LR"({"isAnswered":false,"reason":"mod-error","error":"boom"})", LR"({"isAnswered":false})" })
        {
            SideQuestionAnswer b;
            fallBack = false;
            ReadSideQuestionForkResult(*json::Parse(text), b, fallBack);
            CHECK(!b.ok && fallBack && !b.linkSkipped.empty(), "fork result: nothing to fork, an abort, the mod's own failure -> the headless way");
        }
        SideQuestionAnswer c;
        ReadSideQuestionForkResult(*json::Parse(LR"({"isAnswered":false,"reason":"api-error","status":529,"error":"overloaded"})"), c, fallBack);
        CHECK(!c.ok && !fallBack && c.error.find(L"overloaded") != std::wstring::npos && c.error.find(L"529") != std::wstring::npos, "fork result: an API error is told, not retried headless");
        SideQuestionAnswer d;
        ReadSideQuestionForkResult(*json::Parse(LR"({"isAnswered":false,"reason":"empty-reply"})"), d, fallBack);
        CHECK(!d.ok && !fallBack && !d.error.empty(), "fork result: an empty reply is told");
    }

    // ---- 2. the link's protocol: the codec, the reader, the writer ----
    {
        const std::wstring text = L"ascii \x00E9 \x05E9\x05DC\x05D5\x05DD \U0001F426 end";
        CHECK(Link::detail::Utf8ToWide(Link::detail::WideToUtf8(text)) == text, "codec: a round trip keeps every plane");
        CHECK(Link::detail::WideToUtf8(L"\x00E9") == "\xC3\xA9" && Link::detail::WideToUtf8(L"\U0001F426") == "\xF0\x9F\x90\xA6", "codec: the UTF-8 bytes");
        CHECK(Link::detail::Utf8ToWide("a\xFF" "b") == L"a\xFFFD" L"b" && Link::detail::Utf8ToWide("\xC0\x80") == L"\xFFFD\xFFFD" && Link::detail::Utf8ToWide("\xE2\x82") == L"\xFFFD\xFFFD",
              "codec: a bad byte, an overlong form and a cut sequence become U+FFFD, the rest decodes");
        CHECK(Link::detail::PercentDecode("a%20b%26c+d%zz%2") == "a b&c d%zz%2", "codec: %XX and '+' decoded, a bad escape kept");

        std::string buf = "GET /v1/commands?hold=5000&session=a%2Db HTTP/1.1\r\nHost: x\r\nX-AM-Token: abc\r\n\r\n";
        LinkHttpRequest r;
        CHECK(TakeLinkHttpRequest(buf, r) == LinkHttpTake::Request && buf.empty(), "reader: a GET is read and consumed");
        CHECK(r.method == "GET" && r.path == "/v1/commands" && r.query["hold"] == "5000" && r.query["session"] == "a-b" && r.token == "abc" && r.keepAlive && r.body.empty(),
              "reader: method, path, the decoded query, the token header (any case), keep-alive by default");
        // The head is 49 bytes ("POST /v1/results HTTP/1.1" 25 + CRLF, "Content-Length: 10" 18 + CRLF, the blank CRLF); 5 of
        // the body's 10 are in.
        std::string half = "POST /v1/results HTTP/1.1\r\nContent-Length: 10\r\n\r\n{\"a\":";
        CHECK(TakeLinkHttpRequest(half, r) == LinkHttpTake::NeedMore && half.size() == 54, "reader: a body not all in waits, nothing consumed");
        half += "1234}GET /x HTTP/1.0\r\n\r\n";
        CHECK(TakeLinkHttpRequest(half, r) == LinkHttpTake::Request && r.body == "{\"a\":1234}" && r.method == "POST", "reader: the body by its Content-Length");
        CHECK(TakeLinkHttpRequest(half, r) == LinkHttpTake::Request && r.path == "/x" && !r.keepAlive && half.empty(), "reader: a pipelined second request; HTTP/1.0 closes");
        std::string closeIt = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n";
        CHECK(TakeLinkHttpRequest(closeIt, r) == LinkHttpTake::Request && !r.keepAlive, "reader: Connection: close");
        std::string chunked = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
        CHECK(TakeLinkHttpRequest(chunked, r) == LinkHttpTake::LengthRequired, "reader: a chunked body is refused (411)");
        std::string bad = "get / HTTP/1.1\r\n\r\n";
        CHECK(TakeLinkHttpRequest(bad, r) == LinkHttpTake::Bad, "reader: a malformed request line is a 400");
        std::string badLength = "POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n";
        CHECK(TakeLinkHttpRequest(badLength, r) == LinkHttpTake::Bad, "reader: a malformed length is a 400");
        std::string huge = "POST / HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n";
        CHECK(TakeLinkHttpRequest(huge, r) == LinkHttpTake::TooLarge, "reader: a body past the limit is a 413");
        std::string endless(kLinkMaxHeadBytes + 10, 'a');
        CHECK(TakeLinkHttpRequest(endless, r) == LinkHttpTake::TooLarge, "reader: a head that never ends is a 413");

        const std::string resp = BuildLinkHttpResponse(200, "OK", "{\"ok\":true}", true);
        CHECK(resp.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && resp.find("Content-Length: 11\r\n") != std::string::npos && resp.find("Connection: keep-alive") != std::string::npos &&
                  BodyOf(resp) == "{\"ok\":true}",
              "writer: status, length, keep-alive, body");
        CHECK(BuildLinkHttpResponse(404, "Not Found", "", false).find("Connection: close") != std::string::npos, "writer: a closing answer says so");
        CHECK(LinkJsonBody(LinkPollAnswer({})) == "{\"commands\":[]}", "writer: an empty poll answer");
    }

    // ---- 2b. LinkHub ----
    {
        LinkHub hub;
        const std::wstring tA = L"0123456789abcdef0123456789abcdef";
        const std::wstring tB = L"fedcba9876543210fedcba9876543210";
        hub.RegisterLaunch(tA, L"s1", 1000);
        CHECK(hub.Knows(tA) && !hub.Knows(tB), "hub: a registered token is known, another is not");
        CHECK(!hub.IsLive(tA, 1000) && hub.LiveTokenFor(L"s1", 1000).empty(), "hub: registered is not linked (no hello yet)");
        CHECK(!hub.Hello(tB, L"s9", L"0.1.0", L"2.1.322", 1000), "hub: an unknown token's hello is refused");
        CHECK(hub.Enqueue(L"s1", json::Value::MkObj(), 1000).empty(), "hub: nothing is queued for a session with no live link");
        CHECK(hub.Hello(tA, L"s1", L"0.1.0", L"2.1.322", 2000) && hub.IsLive(tA, 2000), "hub: a hello makes it live");
        const LinkInfo info = hub.Info(tA);
        CHECK(info.modVersion == L"0.1.0" && info.claudeVersion == L"2.1.322" && info.launchSessionId == L"s1", "hub: the hello's facts are kept");
        auto cmd = json::Value::MkObj();
        cmd.Set(L"id", json::Value::MkStr(L"c1"));
        cmd.Set(L"kind", json::Value::MkStr(L"fork"));
        CHECK(hub.Enqueue(L"s1", cmd, 2100) == tA && hub.HasQueued(tA), "hub: a command goes to the live link of its session");
        std::vector<json::Value> taken;
        CHECK(hub.Poll(tA, L"s1", 2200, taken) && taken.size() == 1 && taken[0].StrAt(L"id") == L"c1" && !hub.HasQueued(tA), "hub: a poll takes what is queued");
        CHECK(!hub.Poll(tB, L"x", 2200, taken), "hub: an unknown token's poll is refused");
        hub.SetParked(tA, true, 5000, 2300);
        CHECK(hub.IsLive(tA, 2300 + 600000), "hub: a parked poll keeps the link live however long it waits");
        hub.SetParked(tA, false, 5000, 10000);
        CHECK(hub.IsLive(tA, 10000 + 5000 + kLinkLiveGraceMs) && !hub.IsLive(tA, 10000 + 5000 + kLinkLiveGraceMs + 1), "hub: answered, it stays live for the hold plus the grace, then not");
        // A /clear: the mod names its new conversation in a poll; the link follows it.
        hub.Poll(tA, L"s2", 20000, taken);
        CHECK(hub.LiveTokenFor(L"s2", 20000) == tA && hub.LiveTokenFor(L"s1", 20000).empty(), "hub: the link follows the conversation the mod names");
        // Two tabs on one conversation: the one seen last wins.
        hub.RegisterLaunch(tB, L"s2", 20000);
        hub.Hello(tB, L"s2", L"0.1.0", L"2.1.322", 20500);
        CHECK(hub.LiveTokenFor(L"s2", 20600) == tB, "hub: of two live links on one conversation, the one seen last");
        auto c2 = json::Value::MkObj();
        c2.Set(L"id", json::Value::MkStr(L"c2"));
        CHECK(hub.Enqueue(L"s2", c2, 20600) == tB && hub.Withdraw(tB, L"c2") && !hub.HasQueued(tB) && !hub.Withdraw(tB, L"c2"), "hub: a queued command can be taken back, once");
        CHECK(hub.Counts(20600).first == 2 && hub.Counts(20600).second == 2, "hub: two registered, two live");
        hub.RegisterLaunch(L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", L"s3", 0);
        CHECK(hub.Forget(20600 + 25LL * 3600 * 1000, 24LL * 3600 * 1000, 3600LL * 1000) == 3, "hub: a never-linked token after a day and silent links after an hour are forgotten");
        CHECK(hub.Counts(0).first == 0, "hub: nothing left");
    }

    // ---- 3. the topic's forms of /btw ----
    {
        using namespace Agentmaster::Msg;
        auto ask = ParseBtwMessage(L"/btw what is the codename?", true);
        CHECK(ask.verb == BtwVerb::Ask && ask.question == L"what is the codename?" && ask.command, "btw: /btw <question>");
        ask = ParseBtwMessage(L"  /BTW@SomeBot   why\nthe second line  ", true);
        CHECK(ask.verb == BtwVerb::Ask && ask.question == L"why\nthe second line", "btw: any case, a @bot suffix, every line");
        CHECK(ParseBtwMessage(L"/btw", true).verb == BtwVerb::Usage && ParseBtwMessage(L"/btw   ", true).verb == BtwVerb::Usage, "btw: a bare /btw is the usage");
        for (const wchar_t* clear : { L"/btw clear", L"/btw Clear.", L"/btw reset", L"/btw_clear", L"/clear_btw", L"/clear btw", L"/CLEAR BTW", L"/btw_clear@Bot extra" })
        {
            CHECK(ParseBtwMessage(clear, true).verb == BtwVerb::Clear, "btw: every clear form");
        }
        CHECK(ParseBtwMessage(L"/btw clear up why it failed", true).verb == BtwVerb::Ask, "btw: 'clear' starting a question is a question");
        CHECK(ParseBtwMessage(L"/clear", true).verb == BtwVerb::None && ParseBtwMessage(L"/clear my-session", true).verb == BtwVerb::None,
              "btw: /clear with anything else stays Claude Code's");
        CHECK(ParseBtwMessage(L"/btwx hi", true).verb == BtwVerb::None && ParseBtwMessage(L"/steer btw", true).verb == BtwVerb::None, "btw: another command is not");
        auto words = ParseBtwMessage(L"btw, did the tests pass?", true);
        CHECK(words.verb == BtwVerb::Ask && words.question == L"did the tests pass?" && !words.command, "btw: \"btw, ...\"");
        words = ParseBtwMessage(L"By the way: which file was it?", true);
        CHECK(words.verb == BtwVerb::Ask && words.question == L"which file was it?", "btw: \"By the way: ...\"");
        words = ParseBtwMessage(L"BTW \x2014 how long did the build take", true);
        CHECK(words.verb == BtwVerb::Ask && words.question == L"how long did the build take", "btw: an em dash after the words");
        CHECK(ParseBtwMessage(L"by  the\tway why", true).verb == BtwVerb::Ask, "btw: any run of white space between the words");
        CHECK(ParseBtwMessage(L"btw", true).verb == BtwVerb::Usage && ParseBtwMessage(L"btw clear", true).verb == BtwVerb::Clear && ParseBtwMessage(L"By the way.", true).verb == BtwVerb::Usage,
              "btw: the words alone, and the clear word");
        CHECK(ParseBtwMessage(L"btwhat is this", true).verb == BtwVerb::None && ParseBtwMessage(L"by the wayside", true).verb == BtwVerb::None &&
                  ParseBtwMessage(L"bytheway x", true).verb == BtwVerb::None && ParseBtwMessage(L"fix the btw parser", true).verb == BtwVerb::None,
              "btw: the words must stand alone at the start");
        CHECK(ParseBtwMessage(L"btw what?", false).verb == BtwVerb::None && ParseBtwMessage(L"/btw what?", false).verb == BtwVerb::Ask,
              "btw: a held message counts only as a typed /btw");
    }

    // ---- 3b. the texts ----
    {
        using namespace Agentmaster::Msg;
        const std::wstring md = BtwHeaderMarkdown(0, 1);
        CHECK(md == L"**\x2601\xFE0F /\x2060" L"btw**", "texts: the header, the cloud, and /btw that is no command link");
        CHECK(BtwHeaderMarkdown(0, 3).find(L" \x00B7 #3") != std::wstring::npos && BtwHeaderMarkdown(0, 1).find(L"#") == std::wstring::npos, "texts: the number once the thread continues");
        CHECK(BtwHeaderHtml(0, 2) == L"<b>\x2601\xFE0F /\x2060" L"btw</b> \x00B7 #2", "texts: the HTML header");
        CHECK(BtwTokenCount(899) == L"899" && BtwTokenCount(33010) == L"33.0k" && BtwTokenCount(548147) == L"548.1k" && BtwTokenCount(1250000) == L"1.3M" && BtwTokenCount(-5) == L"0",
              "texts: token counts");
        CHECK(BtwFooterText(true, true, 548147, 899, 425, 8342, L"") == L"in the session \x00B7 548.1k tokens from the cache, 1.3k new \x00B7 8 s", "texts: the in-session footer");
        CHECK(BtwFooterText(false, false, 0, 0, 0, 6000, L"no live link") == L"a headless fork (no live link) \x00B7 6 s", "texts: the headless footer says why");
        CHECK(BtwUsageHtml(0).find(L"is empty") != std::wstring::npos && BtwUsageHtml(1).find(L"1 exchange:") != std::wstring::npos && BtwUsageHtml(3).find(L"3 exchanges") != std::wstring::npos,
              "texts: the usage counts the thread");
        CHECK(BtwClearedHtml(-1).find(L"could not") != std::wstring::npos && BtwClearedHtml(0).find(L"already empty") != std::wstring::npos &&
                  BtwClearedHtml(2).find(L"2 exchanges forgotten") != std::wstring::npos,
              "texts: the clear's three outcomes");
        const std::wstring cold = BtwColdCacheQuestionHtml(2LL * 3600 * 1000, 548147);
        CHECK(cold.find(L"548.1k") != std::wstring::npos && cold.find(L"Ask anyway?") != std::wstring::npos, "texts: the cold-cache question names the size");
        CHECK(BtwFailedHtml(L"a <b> reason").find(L"a &lt;b&gt; reason") != std::wstring::npos, "texts: a failure's reason is escaped");
    }

    // ---- 4. the link server for real, over loopback, with a scripted mod ----
    {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        SessionLinkServer server;
        std::wstring error;
        CHECK(server.Start(error) && server.Running(), "server: it starts on loopback");
        const auto env = server.LaunchEnv(L"sess-live");
        CHECK(env.size() == 2 && env[0].first == L"AM_LINK" && env[1].first == L"AM_LINK_TOKEN" && env[0].second.rfind(L"http://127.0.0.1:", 0) == 0 && env[1].second.size() == 32,
              "server: a launch gets the endpoint and a 32-digit token");
        if (env.size() == 2)
        {
            const uint16_t port = static_cast<uint16_t>(std::stoi(env[0].second.substr(17)));
            const std::string token = Link::detail::WideToUtf8(env[1].second);
            CHECK(BodyOf(HttpOnce(port, Request("POST", "/v1/hello", "00000000000000000000000000000000", "{\"protocol\":1}"))).find("unknown token") != std::string::npos,
                  "server: an unknown token is answered 404");
            CHECK(HttpOnce(port, Request("POST", "/v1/hello", token, "{\"protocol\":2}")).find("409") != std::string::npos, "server: another protocol is answered 409");
            CHECK(!server.HasLiveLink(L"sess-live"), "server: registered is not linked");
            const std::string hello = HttpOnce(port, Request("POST", "/v1/hello", token, "{\"protocol\":1,\"mod\":\"0.1.0\",\"claude\":\"2.1.322\",\"session\":\"sess-live\"}"));
            CHECK(hello.find("200 OK") != std::string::npos && BodyOf(hello).find("\"hold\":5000") != std::string::npos && server.HasLiveLink(L"sess-live"), "server: a hello links the session");
            CHECK(server.Run(L"no-such-session", json::Value::MkObj(), 1000).outcome == LinkRunOutcome::NoLink, "server: a session with no link -> NoLink at once");
            // The scripted mod: one held poll (it parks), the command it brings back, then its result.
            std::atomic<bool> modDone{ false };
            std::string polled;
            std::thread mod([&]() {
                polled = HttpOnce(port, Request("GET", "/v1/commands?hold=5000&session=sess-live", token, ""));
                const auto body = json::Parse(Link::detail::Utf8ToWide(BodyOf(polled)));
                const auto* cmds = body ? body->Find(L"commands") : nullptr;
                const std::wstring id = cmds && !cmds->arr.empty() ? cmds->arr[0].StrAt(L"id") : L"";
                const std::string result = "{\"results\":[{\"id\":\"" + Link::detail::WideToUtf8(id) +
                                           "\",\"isAnswered\":true,\"text\":\"PELICAN-42\",\"usage\":{\"input_tokens\":1,\"output_tokens\":2,\"cache_read_input_tokens\":33010,\"cache_creation_input_tokens\":122}}]}";
                HttpOnce(port, Request("POST", "/v1/results", token, result));
                modDone.store(true);
            });
            ::Sleep(200); // the poll parks first, so the command goes to a HELD poll (the realistic order)
            auto command = json::Value::MkObj();
            command.Set(L"kind", json::Value::MkStr(L"fork"));
            command.Set(L"prompt", json::Value::MkStr(L"What is the codename?"));
            const LinkRunResult run = server.Run(L"sess-live", std::move(command), 10000);
            mod.join();
            CHECK(run.outcome == LinkRunOutcome::Answered && run.result.StrAt(L"text") == L"PELICAN-42" && run.result.Find(L"usage") &&
                      run.result.Find(L"usage")->I64At(L"cache_read_input_tokens") == 33010,
                  "server: the command reaches the held poll and the mod's result comes back to Run");
            CHECK(BodyOf(polled).find("\"kind\":\"fork\"") != std::string::npos && BodyOf(polled).find("What is the codename?") != std::string::npos && modDone.load(),
                  "server: the poll's answer carried the command whole");
            CHECK(run.tookMs < 5000, "server: the parked poll was answered at once, not at its hold");
            const auto counts = server.Counts();
            CHECK(counts.first >= 1, "server: the census counts the token");

            // A result posted under ANOTHER link's token never completes this link's command (a waiter answers only to the token
            // its command went to: one tab's mod cannot answer, or spoof, another tab's side question); the right token still can.
            const auto envOther = server.LaunchEnv(L"sess-other");
            CHECK(envOther.size() == 2, "server: a second launch gets a link of its own");
            if (envOther.size() == 2)
            {
                const std::string tokenOther = Link::detail::WideToUtf8(envOther[1].second);
                HttpOnce(port, Request("POST", "/v1/hello", tokenOther, "{\"protocol\":1,\"mod\":\"0.1.0\",\"claude\":\"2.1.322\",\"session\":\"sess-other\"}"));
                std::string foreignAnswer;
                std::string ownAnswer;
                std::thread twoMods([&]() {
                    const std::string got = HttpOnce(port, Request("GET", "/v1/commands?hold=5000&session=sess-live", token, ""));
                    const auto body = json::Parse(Link::detail::Utf8ToWide(BodyOf(got)));
                    const auto* cmds = body ? body->Find(L"commands") : nullptr;
                    const std::string id = cmds && !cmds->arr.empty() ? Link::detail::WideToUtf8(cmds->arr[0].StrAt(L"id")) : std::string{};
                    // The other tab's token first, with the right id: refused (taken 0); then this link's own token.
                    foreignAnswer = HttpOnce(port, Request("POST", "/v1/results", tokenOther, "{\"results\":[{\"id\":\"" + id + "\",\"isAnswered\":true,\"text\":\"FOREIGN\"}]}"));
                    ownAnswer = HttpOnce(port, Request("POST", "/v1/results", token, "{\"results\":[{\"id\":\"" + id + "\",\"isAnswered\":true,\"text\":\"OWN\"}]}"));
                });
                ::Sleep(200); // the poll parks first
                auto forkCommand = json::Value::MkObj();
                forkCommand.Set(L"kind", json::Value::MkStr(L"fork"));
                const LinkRunResult own = server.Run(L"sess-live", std::move(forkCommand), 10000);
                twoMods.join();
                CHECK(BodyOf(foreignAnswer).find("\"taken\":0") != std::string::npos && BodyOf(ownAnswer).find("\"taken\":1") != std::string::npos &&
                          own.outcome == LinkRunOutcome::Answered && own.result.StrAt(L"text") == L"OWN",
                      "server: another link's token cannot answer this link's command; its own token can");
            }

            // A mod that took the command and never answers, its link still live: the wait ends at its deadline (TimedOut) — the
            // case where SideQuestion.cpp does NOT fall back (the fork may still run in the session; a second would pay twice).
            {
                std::thread silentMod([&]() { HttpOnce(port, Request("GET", "/v1/commands?hold=5000&session=sess-live", token, "")); });
                ::Sleep(200); // the poll parks first
                auto forkCommand = json::Value::MkObj();
                forkCommand.Set(L"kind", json::Value::MkStr(L"fork"));
                const LinkRunResult late = server.Run(L"sess-live", std::move(forkCommand), 1500);
                silentMod.join();
                CHECK(late.outcome == LinkRunOutcome::TimedOut && late.tookMs >= 1400 && late.tookMs < 5000, "server: a command never answered ends at its deadline (TimedOut)");
            }

            // A mod that took the command with a hold of 0 and then stopped polling (its claude ended or hung): the link is live
            // only for the grace after that poll, and the wait ends as Lost long before its 30 s deadline.
            {
                std::thread goneMod([&]() {
                    ::Sleep(300); // after Run queued the command: a hold-0 poll is answered at once, with it
                    HttpOnce(port, Request("GET", "/v1/commands?hold=0&session=sess-live", token, ""));
                });
                auto forkCommand = json::Value::MkObj();
                forkCommand.Set(L"kind", json::Value::MkStr(L"fork"));
                const LinkRunResult lost = server.Run(L"sess-live", std::move(forkCommand), 30000);
                goneMod.join();
                CHECK(lost.outcome == LinkRunOutcome::Lost && lost.tookMs >= kLinkLiveGraceMs && lost.tookMs < 10000,
                      "server: a link that stops polling ends the wait as Lost, after the grace and before the deadline");
            }
        }
        server.Stop();
        CHECK(!server.Running() && server.LaunchEnv(L"after").empty(), "server: stopped, a launch gets no link");
        ::WSACleanup();
    }

    // ---- 4b. the stream runner, with Windows PowerShell (and cmd.exe) standing in for claude ----
    {
        wchar_t sys[MAX_PATH]{};
        const UINT n = ::GetSystemDirectoryW(sys, MAX_PATH);
        const std::wstring cmd = std::wstring{ sys, n } + L"\\cmd.exe";
        ClaudeStreamRun run;
        // A line echo that writes each line AS IT COMES (findstr and more hold their output until their input ends on a pipe,
        // measured): UTF-8 both ways without a BOM, so the bytes come back as they went.
        run.exe = std::wstring{ sys, n } + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        run.args = L"-NoProfile -NonInteractive -Command \"$e=New-Object Text.UTF8Encoding $false;[Console]::InputEncoding=$e;[Console]::OutputEncoding=$e;"
                   L"$i=[Console]::In;while(($l=$i.ReadLine()) -ne $null){[Console]::Out.WriteLine($l);[Console]::Out.Flush()}\"";
        run.harnessName = L"m5-stream";
        run.stdinUtf8 = "first line\nthe answer \xC3\xA9\nnever read\n";
        std::vector<std::string> lines;
        run.onLine = [&lines](std::string_view line) noexcept -> bool {
            lines.emplace_back(line);
            return line.rfind("the answer", 0) == 0;
        };
        run.timeoutMs = 20000;
        run.exitGraceMs = 5000;
        const ClaudeStreamResult r = RunClaudeStreamJson(run);
        CHECK(r.started && r.done && !r.timedOut && lines.size() >= 2 && lines[0] == "first line" && lines[1] == "the answer \xC3\xA9",
              "stream: the input is written, its lines come back until onLine says done");
        CHECK(r.exitedByItself, "stream: once done, the closed input ends the child by itself");

        ClaudeStreamRun stuck;
        stuck.exe = cmd;
        stuck.args = L"/d /c ping -n 30 127.0.0.1 >nul";
        stuck.harnessName = L"m5-stream";
        stuck.onLine = [](std::string_view) noexcept -> bool { return false; };
        stuck.timeoutMs = 800;
        const ClaudeStreamResult s = RunClaudeStreamJson(stuck);
        CHECK(s.started && s.timedOut && !s.done && s.tookMs < 10000, "stream: a child that never answers is ended at the timeout, with what it started");

        ClaudeStreamRun missing;
        missing.exe = L"C:\\no\\such\\claude.exe";
        missing.onLine = [](std::string_view) noexcept -> bool { return true; };
        const ClaudeStreamResult m = RunClaudeStreamJson(missing);
        CHECK(!m.started && !m.error.empty(), "stream: a missing executable says why");
    }
}
