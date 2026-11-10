// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.50 / doc/agentmaster/MESSAGING_OUTBOX.md: the DURABLE TELEGRAM OUTBOX (the owner's ask of
// 2026-11-10: "all messages must be durable and queue-wise recovering. Even bad gateway"). This file pins:
//   1. the journal's encoding: UTF-8 both ways (surrogate pairs, lone surrogates, invalid bytes), the CRC-32 (zip's check
//      value), a sealed line (the checksum's place and bytes, a flipped byte, a cut line, CRLF, an empty object refused);
//   2. the lines a journal's bytes cut into (the torn tail, empty lines) and every record kind built and opened again;
//   3. the FOLD (recovery): put / try / retry / ok / drop, an unknown op, a record about an id never put, a repeated put, the
//      thread redirects (a chain, a loop), the flood hold (the newest wins), the header, damaged lines, ids and seqs;
//   4. the ANSWER TABLE, an item's CLASS (durable, the retry policy), the posts, the BACK-OFF and its jitter, the CIRCUIT BREAKER;
//   5. the per-thread LANES, the TOPIC PROBE's state machine (two "not found" = gone, the release cap, the release memory);
//   6. the recovered item's plain-text fallback and the thread member's in-place change;
//   7. the FILES on a scratch folder: the one-writer lock, the rewrite + appends read back and folded, a torn tail, dead.jsonl,
//      the lock file gone after Close, and 4 threads appending at once (every line whole, none interleaved).
// The service's half (the send loop's verdicts, the held threads' probes against Telegram, the recovery at a start) needs
// Telegram: the live check is the dev bot (MESSAGING.md §21.50, "Check after the build").

#include "m5_tests.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "../MessagingOutbox.h"
#include "../MessagingOutboxJournal.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    /// <summary>A Telegram answer with the given HTTP status and description (ok false), as TelegramApi would return it.</summary>
    /// <param name="status">The HTTP status (0 = none).</param>
    /// <param name="description">Telegram's words.</param>
    /// <param name="transport">No HTTP answer at all.</param>
    /// <returns>The answer.</returns>
    TgResult Answer(int status, std::wstring description, bool transport = false)
    {
        TgResult r;
        r.ok = false;
        r.httpStatus = status;
        r.description = std::move(description);
        r.transportFailed = transport;
        return r;
    }

    /// <summary>A successful answer whose result is a message with the given id.</summary>
    /// <param name="messageId">The message id.</param>
    /// <returns>The answer.</returns>
    TgResult Sent(int64_t messageId)
    {
        TgResult r;
        r.ok = true;
        r.httpStatus = 200;
        r.result = json::Value::MkObj();
        r.result.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        return r;
    }

    /// <summary>A put of a sendMessage into `thread` with the given text.</summary>
    /// <param name="id">The journal id.</param>
    /// <param name="seq">Its sequence number.</param>
    /// <param name="thread">Its thread (0 = the main view).</param>
    /// <param name="text">The text.</param>
    /// <returns>The put.</returns>
    OutboxPut Put(uint64_t id, uint64_t seq, int64_t thread, const std::wstring& text)
    {
        OutboxPut p;
        p.id = id;
        p.prio = 0;
        p.seq = seq;
        p.sub = 0;
        p.method = L"sendMessage";
        p.params = json::Value::MkObj();
        p.params.Set(L"chat_id", json::Value::MkNum(12345.0));
        if (thread > 0)
        {
            p.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(thread)));
        }
        p.params.Set(L"text", json::Value::MkStr(text));
        p.params.Set(L"parse_mode", json::Value::MkStr(L"HTML"));
        p.queuedWallMs = 1762770000000;
        return p;
    }

    /// <summary>The scratch folder of this run's file tests (inside the run's SCRATCH profile: run-m5-tests.bat points
    /// AGENTMASTER_PROFILE at a wiped %TEMP% folder), emptied first.</summary>
    /// <param name="leaf">The test's own sub-folder.</param>
    /// <returns>The folder (created).</returns>
    std::filesystem::path ScratchOutbox(const wchar_t* leaf)
    {
        auto dir = std::filesystem::path{ AgentmasterStateDir() } / L"outbox-tests" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    /// <summary>A file's bytes ("" when it cannot be read).</summary>
    /// <param name="p">The path.</param>
    /// <returns>The bytes.</returns>
    std::string ReadAllBytes(const std::filesystem::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    void TestEncoding()
    {
        // UTF-8 both ways.
        CHECK(OutboxUtf8(L"abc") == "abc", "utf8: ASCII as is");
        CHECK(OutboxUtf8(L"\x05E9\x05DC\x05D5\x05DD") == "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", "utf8: Hebrew is two bytes a letter");
        CHECK(OutboxUtf8(L"\xD83D\xDE00") == "\xF0\x9F\x98\x80", "utf8: a surrogate pair is one 4-byte sequence");
        CHECK(OutboxUtf8(L"a\xD83D" L"b") == "a\xEF\xBF\xBD" "b", "utf8: a lone high surrogate becomes U+FFFD");
        CHECK(OutboxUtf8(L"\xDE00") == "\xEF\xBF\xBD", "utf8: a lone low surrogate becomes U+FFFD");
        CHECK(OutboxWide("\xF0\x9F\x98\x80") == L"\xD83D\xDE00", "wide: a 4-byte sequence becomes a surrogate pair");
        CHECK(OutboxWide("\xD7\xA9" "x") == L"\x05E9x", "wide: Hebrew back");
        CHECK(OutboxWide("a\x80" "b") == L"a\xFFFD" "b", "wide: a stray continuation byte is U+FFFD");
        CHECK(OutboxWide("\xC0\xAF") == L"\xFFFD\xFFFD", "wide: an overlong form is refused byte by byte");
        CHECK(OutboxWide("\xE2\x82") == L"\xFFFD\xFFFD", "wide: a truncated sequence is refused byte by byte");
        CHECK(OutboxWide("\xED\xA0\x80") == L"\xFFFD\xFFFD\xFFFD", "wide: an encoded surrogate is refused");
        const std::wstring mixed = L"\x05E9 \xD83D\xDE00 <b>x</b> \"q\"";
        CHECK(OutboxWide(OutboxUtf8(mixed)) == mixed, "utf8 then wide is the identity on valid text");

        // CRC-32: zip's check value.
        CHECK(OutboxCrc32("123456789") == 0xCBF43926u, "crc32: the standard check value");
        CHECK(OutboxCrc32("") == 0u, "crc32: of nothing");

        // A sealed line.
        const std::string sealed = SealOutboxRecord(R"({"op":"ok","id":7,"mid":90311})");
        CHECK(!sealed.empty() && sealed.back() == '\n', "seal: the line ends in a newline");
        CHECK(sealed.find(",\"crc\":\"") != std::string::npos, "seal: the checksum member is there");
        CHECK(sealed.rfind(",\"crc\":\"") + 8 + 8 + 2 + 1 == sealed.size(), "seal: the checksum is the LAST member, 8 hex digits");
        {
            const std::string body = sealed.substr(0, sealed.rfind(",\"crc\":\""));
            wchar_t hex[16]{};
            ::swprintf(hex, 16, L"%08x", OutboxCrc32(body));
            CHECK(sealed.find(OutboxUtf8(hex)) != std::string::npos, "seal: the checksum is of the bytes before it");
        }
        json::Value rec;
        const std::string_view line{ sealed.data(), sealed.size() - 1 };
        CHECK(OpenOutboxRecord(line, rec) == OutboxLineRead::Ok, "open: a sealed line opens");
        CHECK(rec.StrAt(L"op") == L"ok" && rec.I64At(L"id") == 7 && rec.I64At(L"mid") == 90311, "open: its members read back");
        std::string flipped{ line };
        flipped[10] = flipped[10] == '7' ? '8' : '7';
        json::Value untouched;
        CHECK(OpenOutboxRecord(flipped, untouched) == OutboxLineRead::BadCrc, "open: one changed byte fails the checksum");
        CHECK(untouched.type == json::Value::Type::Null, "open: a failed line leaves the out-value untouched");
        CHECK(OpenOutboxRecord(line.substr(0, line.size() - 5), rec) == OutboxLineRead::Unreadable, "open: a cut line is unreadable");
        CHECK(OpenOutboxRecord(std::string{ line } + "\r", rec) == OutboxLineRead::Ok, "open: a CRLF line (an editor's save) opens");
        std::string upper{ line };
        for (size_t k = upper.size() - 10; k < upper.size() - 2; ++k)
        {
            upper[k] = static_cast<char>(::toupper(static_cast<unsigned char>(upper[k])));
        }
        CHECK(OpenOutboxRecord(upper, rec) == OutboxLineRead::Ok, "open: upper-case hex is accepted");
        CHECK(SealOutboxRecord("{}").empty(), "seal: an empty object is refused (nothing to put the checksum after)");
        CHECK(SealOutboxRecord("[1]").empty(), "seal: a non-object is refused");
        const std::string noOp = SealOutboxRecord(R"({"x":1})");
        CHECK(OpenOutboxRecord(std::string_view{ noOp.data(), noOp.size() - 1 }, rec) == OutboxLineRead::Unreadable, "open: a record without an op is unreadable");
        const std::string nul(40, '\0');
        CHECK(OpenOutboxRecord(nul, rec) == OutboxLineRead::Unreadable, "open: a zero-filled block (a power cut) is unreadable");

        // A text with a newline, quotes and non-ASCII survives the round trip inside ONE line.
        OutboxPut p = Put(3, 9, 777, L"line one\nline \"two\" \x05E9 \xD83D\xDE00");
        const std::string putLine = OutboxPutLine(p);
        CHECK(std::count(putLine.begin(), putLine.end(), '\n') == 1, "put: a text's newlines are escaped (one line)");
        json::Value back;
        CHECK(OpenOutboxRecord(std::string_view{ putLine.data(), putLine.size() - 1 }, back) == OutboxLineRead::Ok, "put: the line opens");
        const auto* params = back.Find(L"params");
        CHECK(params && params->StrAt(L"text") == L"line one\nline \"two\" \x05E9 \xD83D\xDE00", "put: the text comes back as it was");
        CHECK(OutboxPutLine(p, OutboxUtf8(json::Dump(p.params))) == putLine, "put: pre-serialized parameters give the same line");
        CHECK(back.StrAt(L"method") == L"sendMessage" && back.I64At(L"seq") == 9 && back.I64At(L"id") == 3, "put: the order keys and method");
        CHECK(back.Find(L"mir") == nullptr && back.Find(L"fb") == nullptr, "put: no mirror mark and no fallback unless set");
        p.mirror = true;
        p.fallbackHtml = L"<b>x</b>";
        const std::string marked = OutboxPutLine(p);
        CHECK(OpenOutboxRecord(std::string_view{ marked.data(), marked.size() - 1 }, back) == OutboxLineRead::Ok && back.I64At(L"mir") == 1 &&
                  back.StrAt(L"fb") == L"<b>x</b>",
              "put: the mirror mark and the fallback");
        CHECK(marked.rfind("\"params\":") > marked.find("\"fb\""), "put: the parameters are the last member before the checksum");
    }

    void TestSplitAndFold()
    {
        // Lines, an empty line, a torn tail.
        const std::string a = OutboxHeaderLine(L"h1", 12345, 1, L"test");
        const std::string b = OutboxPutLine(Put(1, 1, 100, L"one"));
        std::string bytes = a + "\n" + b + "{\"op\":\"pu";
        const OutboxJournalText t = SplitOutboxJournal(bytes);
        CHECK(t.lines.size() == 2, "split: the whole lines (an empty one skipped)");
        CHECK(t.tornBytes == 9 && t.wholeBytes == bytes.size() - 9, "split: the torn tail's size and where the file is cut");
        CHECK(SplitOutboxJournal("").lines.empty() && SplitOutboxJournal("").tornBytes == 0, "split: an empty journal");

        // The fold.
        std::string j = OutboxHeaderLine(L"bot-hash", 12345, 1, L"test");
        j += OutboxPutLine(Put(1, 10, 100, L"sent"));
        j += OutboxPutLine(Put(2, 11, 100, L"cut off"));
        j += OutboxPutLine(Put(3, 12, 200, L"backing off"));
        j += OutboxPutLine(Put(4, 13, 200, L"dropped"));
        j += OutboxPutLine(Put(5, 14, 300, L"new"));
        j += OutboxTryLine(1, 1, 2);
        j += OutboxOkLine(1, 9001);
        j += OutboxTryLine(2, 1, 3);
        j += OutboxTryLine(3, 1, 4);
        j += OutboxRetryLine(3, 2, 1762770009000, L"HTTP 502");
        j += OutboxDropLine(4, L"its topic was deleted (closed)");
        j += OutboxOkLine(99, 1); // about an id never put (compacted away): ignored
        j += SealOutboxRecord(R"({"op":"later-op","id":5})"); // a newer build's record: ignored
        j += "garbage that is no record\n";
        j += OutboxLaneLine(300, 301);
        j += OutboxLaneLine(301, 302); // a chain: 300 -> 301 -> 302
        j += OutboxFloodLine(1762770000000, { { L"editMessageText", 1762769000000 } });
        j += OutboxFloodLine(1762790000000, { { L"sendRichMessage", 1762780000000 } }); // the newest wins entirely
        j += "{\"op\":\"put\",\"id\":6"; // a torn tail
        size_t torn = 0;
        const OutboxRecovery r = FoldOutboxJournal(j, torn);
        CHECK(r.header && r.bot == L"bot-hash" && r.chat == 12345 && r.version == kOutboxJournalVersion, "fold: the header");
        CHECK(torn == std::string{ "{\"op\":\"put\",\"id\":6" }.size(), "fold: the torn tail is reported, never read");
        CHECK(r.badLines == 1, "fold: the garbage line is counted, never fatal");
        CHECK(r.live.size() == 3, "fold: three items still queued (2, 3, 5)");
        CHECK(r.live.size() == 3 && r.live[0].put.id == 2 && r.live[1].put.id == 3 && r.live[2].put.id == 5, "fold: the live items in id order");
        if (r.live.size() == 3)
        {
            CHECK(r.live[0].tried && r.live[0].attempts == 0, "fold: a try with no outcome = cut off mid-call (sent again)");
            CHECK(!r.live[1].tried && r.live[1].attempts == 2 && r.live[1].notBeforeWallMs == 1762770009000, "fold: a retry = an outcome, its back-off kept");
            CHECK(!r.live[2].tried && r.live[2].attempts == 0, "fold: an item never tried");
            CHECK(OutboxThreadOf(r.live[2].put.params) == 302, "fold: the redirect chain moves an item to the newest thread");
            CHECK(OutboxThreadOf(r.live[0].put.params) == 100, "fold: an item of another thread is not moved");
            CHECK(r.live[1].put.params.StrAt(L"text") == L"backing off", "fold: the parameters come back");
        }
        CHECK(r.sent == 1 && r.dropped == 1, "fold: one sent, one dropped");
        CHECK(r.maxId == 99 && r.maxSeq == 14, "fold: the highest id seen (an outcome's too) and the live items' highest seq");
        CHECK(r.redirects.size() == 2, "fold: both redirects");
        CHECK(r.floodAllUntilWallMs == 1762790000000 && r.floodMethodUntilWallMs.size() == 1 && r.floodMethodUntilWallMs.count(L"sendRichMessage") == 1,
              "fold: the newest flood line is the whole hold");

        // A repeated put keeps its newest form; a put without its method or parameters is no item.
        std::string k = OutboxHeaderLine(L"h", 1, 1, L"t");
        k += OutboxPutLine(Put(8, 1, 100, L"first"));
        k += OutboxPutLine(Put(8, 1, 100, L"second"));
        k += SealOutboxRecord(R"({"op":"put","id":9,"params":{}})");
        k += SealOutboxRecord(R"({"op":"put","id":10,"method":"sendMessage"})");
        const OutboxRecovery r2 = FoldOutboxJournal(k, torn);
        CHECK(r2.live.size() == 1 && r2.live[0].put.params.StrAt(L"text") == L"second", "fold: a repeated id keeps its newest put");
        CHECK(r2.badLines == 2, "fold: puts that cannot be sent are counted as bad");
        CHECK(torn == 0, "fold: no torn tail");

        // Redirects: a loop is bounded, a self-redirect is refused.
        std::map<int64_t, int64_t> loop{ { 1, 2 }, { 2, 1 } };
        const int64_t landed = ResolveOutboxThread(loop, 1);
        CHECK(landed == 1 || landed == 2, "redirect: a loop ends (bounded hops)");
        CHECK(ResolveOutboxThread({}, 5) == 5, "redirect: none leaves the thread");
        std::string self = OutboxHeaderLine(L"h", 1, 1, L"t") + OutboxLaneLine(7, 7);
        CHECK(FoldOutboxJournal(self, torn).redirects.empty(), "fold: a thread redirected to itself is ignored");
        CHECK(!FoldOutboxJournal(OutboxPutLine(Put(1, 1, 1, L"x")), torn).header, "fold: a journal without its header says so (the start discards it)");
    }

    void TestAnswersAndClasses()
    {
        CHECK(ClassifyOutboxAnswer(Sent(1)) == OutboxVerdict::Ok, "answer: ok");
        CHECK(ClassifyOutboxAnswer(Answer(400, L"Bad Request: message is not modified: specified new message content ...")) == OutboxVerdict::Ok, "answer: not modified = done");
        CHECK(ClassifyOutboxAnswer(Answer(400, L"Bad Request: TOPIC_NOT_MODIFIED")) == OutboxVerdict::Ok, "answer: TOPIC_NOT_MODIFIED = done");
        CHECK(ClassifyOutboxAnswer(Answer(429, L"Too Many Requests: retry after 30")) == OutboxVerdict::Flood, "answer: 429 = flood");
        CHECK(ClassifyOutboxAnswer(Answer(502, L"unreadable answer (HTTP 502, 166 bytes)")) == OutboxVerdict::Transient, "answer: 502 (Telegram's proxy page) = transient");
        CHECK(ClassifyOutboxAnswer(Answer(500, L"Internal Server Error")) == OutboxVerdict::Transient, "answer: 500 = transient");
        CHECK(ClassifyOutboxAnswer(Answer(0, L"request failed (12002)", true)) == OutboxVerdict::Transient, "answer: no answer (a timeout) = transient");
        CHECK(ClassifyOutboxAnswer(Answer(0, L"aborted", true)) == OutboxVerdict::Transient, "answer: an abort (a stop) = transient: it goes at the next start");
        CHECK(ClassifyOutboxAnswer(Answer(400, L"Bad Request: message thread not found")) == OutboxVerdict::ThreadGone, "answer: thread not found");
        CHECK(ClassifyOutboxAnswer(Answer(400, L"Bad Request: TOPIC_ID_INVALID")) == OutboxVerdict::ThreadGone, "answer: TOPIC_ID_INVALID = thread gone");
        CHECK(ClassifyOutboxAnswer(Answer(400, L"Bad Request: can't parse entities: ...")) == OutboxVerdict::Refused, "answer: a parse refusal (its fallback is the caller's)");
        CHECK(ClassifyOutboxAnswer(Answer(403, L"Forbidden: bot was blocked by the user")) == OutboxVerdict::Refused, "answer: 403 = refused");
        CHECK(ClassifyOutboxAnswer(Answer(0, L"the secretary bot is not running")) == OutboxVerdict::Refused, "answer: a synthetic refusal (no status, no transport failure)");

        // The class.
        OutboxItemFacts post;
        post.method = L"sendMessage";
        CHECK(OutboxItemIsDurable(post), "durable: a plain post");
        post.method = L"sendRichMessage";
        CHECK(OutboxItemIsDurable(post), "durable: a plain rich post");
        OutboxItemFacts f = post;
        f.keyed = true;
        CHECK(!OutboxItemIsDurable(f), "durable: not a keyed post (a card, a picker)");
        f = post;
        f.history = true;
        CHECK(!OutboxItemIsDurable(f), "durable: not a history replay item (the replay commits its own place)");
        f = post;
        f.asUser = true;
        CHECK(!OutboxItemIsDurable(f), "durable: not a send as the user (its echo guard expires)");
        f = post;
        f.asSecretary = true;
        CHECK(!OutboxItemIsDurable(f), "durable: not the secretary bot's sends");
        f = post;
        f.buttons = true;
        CHECK(!OutboxItemIsDurable(f), "durable: not a message with buttons");
        f = post;
        f.upload = true;
        CHECK(!OutboxItemIsDurable(f), "durable: not an upload");
        OutboxItemFacts edit;
        edit.method = L"editMessageText";
        CHECK(!OutboxItemIsDurable(edit), "durable: not an edit");

        // The retry policy.
        CHECK(OutboxRetryFor(post, true) == OutboxRetry::Forever, "retry: a durable item until Telegram answers");
        edit.keyed = true;
        CHECK(OutboxRetryFor(edit, false) == OutboxRetry::Forever, "retry: a keyed item until it goes (a newer one replaces it)");
        OutboxItemFacts create;
        create.method = L"createForumTopic";
        CHECK(OutboxRetryFor(create, false) == OutboxRetry::Never, "retry: never a topic create (a second topic)");
        OutboxItemFacts answer;
        answer.method = L"answerCallbackQuery";
        CHECK(OutboxRetryFor(answer, false) == OutboxRetry::Never, "retry: never a tap's answer (its deadline is seconds)");
        OutboxItemFacts photo;
        photo.method = L"sendPhoto";
        photo.upload = true;
        CHECK(OutboxRetryFor(photo, false) == OutboxRetry::Never, "retry: never an upload (a second picture)");
        f = post;
        f.asUser = true;
        CHECK(OutboxRetryFor(f, false) == OutboxRetry::Never, "retry: never a send as the user");
        OutboxItemFacts del;
        del.method = L"deleteMessage";
        CHECK(OutboxRetryFor(del, false) == OutboxRetry::Bounded, "retry: a plain delete a few times");
        f = post;
        f.history = true;
        CHECK(OutboxRetryFor(f, false) == OutboxRetry::Bounded, "retry: a history item a few times");

        CHECK(OutboxMethodPostsIntoThread(L"sendMessage") && OutboxMethodPostsIntoThread(L"sendRichMessage") && OutboxMethodPostsIntoThread(L"sendPhoto") &&
                  OutboxMethodPostsIntoThread(L"sendDocument"),
              "posts: the sends");
        CHECK(!OutboxMethodPostsIntoThread(L"editMessageText") && !OutboxMethodPostsIntoThread(L"deleteMessage") && !OutboxMethodPostsIntoThread(L"setMessageReaction"),
              "posts: not edits, deletes, reactions");
    }

    void TestBackoffAndBreaker()
    {
        CHECK(OutboxBackoffMs(0, 1) == 0, "backoff: none before a failure");
        const int64_t bases[] = { 1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000 };
        bool inRange = true;
        for (uint32_t a = 1; a <= 9; ++a)
        {
            for (uint64_t seed = 0; seed < 64; ++seed)
            {
                const int64_t ms = OutboxBackoffMs(a, seed * 7919);
                const int64_t base = bases[a - 1];
                inRange = inRange && ms >= base * 8 / 10 && ms <= base * 12 / 10;
            }
        }
        CHECK(inRange, "backoff: 1, 2, 4, 8, 16, 32 s, then 60 s, each within +-20 %");
        CHECK(OutboxBackoffMs(3, 42) == OutboxBackoffMs(3, 42), "backoff: deterministic for an item");
        bool spread = false;
        for (uint64_t seed = 1; seed < 16 && !spread; ++seed)
        {
            spread = OutboxBackoffMs(5, seed) != OutboxBackoffMs(5, 0);
        }
        CHECK(spread, "backoff: items failed together come back at different moments");
        CHECK(OutboxBackoffMs(1000, 5) <= 72000, "backoff: a huge attempt count stays capped");

        OutboxBreaker b;
        bool opened = false;
        for (int k = 0; k < 7; ++k)
        {
            opened = NoteOutboxFailure(b, 1000) || opened;
        }
        CHECK(!opened && OutboxBreakerHoldsUntil(b, 1000) == 0, "breaker: closed below the threshold");
        CHECK(NoteOutboxFailure(b, 2000) && OutboxBreakerHoldsUntil(b, 2000) == 2000 + kOutboxBreakerFirstPauseMs, "breaker: the 8th failure in a row opens it for 15 s");
        CHECK(b.openedAtMs == 2000, "breaker: when it opened");
        CHECK(OutboxBreakerHoldsUntil(b, 2000 + kOutboxBreakerFirstPauseMs) == 0, "breaker: the pause ends (one item probes)");
        CHECK(NoteOutboxFailure(b, 20000) && b.pauseMs == 2 * kOutboxBreakerFirstPauseMs, "breaker: a failed probe doubles the pause");
        for (int k = 0; k < 10; ++k)
        {
            NoteOutboxFailure(b, 30000);
        }
        CHECK(b.pauseMs == kOutboxBreakerMaxPauseMs, "breaker: the pause is capped at 5 minutes");
        CHECK(b.openedAtMs == 2000, "breaker: 'unreachable since' keeps the first opening");
        CHECK(NoteOutboxSuccess(b) && b.failuresInRow == 0 && b.openUntilMs == 0 && b.pauseMs == 0, "breaker: an answer closes it and forgets the pauses");
        CHECK(!NoteOutboxSuccess(b), "breaker: a success while closed reports nothing");
    }

    void TestLanesAndProbes()
    {
        OutboxLaneBlocks lanes;
        CHECK(lanes.Empty() && !lanes.Blocks(5, 1, 0), "lanes: nothing waits");
        lanes.NoteWaiting(5, 10, 2);
        lanes.NoteWaiting(5, 12, 0);
        CHECK(!lanes.Empty(), "lanes: something waits");
        CHECK(lanes.Blocks(5, 10, 3) && lanes.Blocks(5, 11, 0) && lanes.Blocks(5, 12, 0), "lanes: a later post of the thread waits behind the earliest waiting one");
        CHECK(!lanes.Blocks(5, 10, 2) && !lanes.Blocks(5, 9, 0) && !lanes.Blocks(5, 10, 1), "lanes: the waiting post itself and earlier ones do not");
        CHECK(!lanes.Blocks(6, 99, 0), "lanes: another thread does not wait");
        lanes.NoteWaiting(0, 3, 0);
        CHECK(lanes.Blocks(0, 4, 0), "lanes: the main view is a lane too");

        CHECK(ClassifyTopicProbe(Sent(0)) == TopicProbeAnswer::Exists, "probe: ok = exists (a drifted name was put back)");
        CHECK(ClassifyTopicProbe(Answer(400, L"Bad Request: TOPIC_NOT_MODIFIED")) == TopicProbeAnswer::Exists, "probe: TOPIC_NOT_MODIFIED = exists, its name right");
        CHECK(ClassifyTopicProbe(Answer(400, L"Bad Request: message thread not found")) == TopicProbeAnswer::NotFound, "probe: not found");
        CHECK(ClassifyTopicProbe(Answer(400, L"Bad Request: TOPIC_ID_INVALID")) == TopicProbeAnswer::NotFound, "probe: TOPIC_ID_INVALID = not found");
        CHECK(ClassifyTopicProbe(Answer(502, L"Bad Gateway")) == TopicProbeAnswer::Inconclusive, "probe: a 5xx has no verdict");
        CHECK(ClassifyTopicProbe(Answer(0, L"timeout", true)) == TopicProbeAnswer::Inconclusive, "probe: no answer has no verdict");
        CHECK(ClassifyTopicProbe(Answer(429, L"Too Many Requests")) == TopicProbeAnswer::Inconclusive, "probe: a 429 has no verdict");
        CHECK(ClassifyTopicProbe(Answer(400, L"Bad Request: not enough rights to manage topics")) == TopicProbeAnswer::Exists, "probe: a refusal about something else proves the thread is there");

        // Two "not found" in a row = gone.
        HeldThread h;
        h.threadId = 42;
        CHECK(HoldThread(h, 1000) == HeldThreadPhase::Probing && h.nextProbeMs == 1000 + kTopicProbeFirstMs, "hold: probing, the first probe 10 s later");
        CHECK(HeldThreadHoldsPosts(h), "hold: its posts wait");
        CHECK(HoldThread(h, 1500) == HeldThreadPhase::Probing && h.heldSinceMs == 1000, "hold: a second refusal changes nothing");
        CHECK(NoteTopicProbe(h, TopicProbeAnswer::NotFound, 11000) == TopicProbeStep::Wait && h.nextProbeMs == 11000 + kTopicProbeSecondMs, "probe: one not found waits 30 s");
        CHECK(NoteTopicProbe(h, TopicProbeAnswer::NotFound, 41000) == TopicProbeStep::Gone && h.phase == HeldThreadPhase::Gone && h.goneSinceMs == 41000,
              "probe: a second not found in a row = gone");
        CHECK(HeldThreadHoldsPosts(h), "gone: its posts wait for the topic made again");
        CHECK(NoteTopicProbe(h, TopicProbeAnswer::Exists, 50000) == TopicProbeStep::Wait, "probe: a gone thread takes no more probes");

        // A probe with no verdict waits longer each time; a verdict in between resets the count.
        HeldThread q;
        q.threadId = 43;
        HoldThread(q, 0);
        NoteTopicProbe(q, TopicProbeAnswer::Inconclusive, 10000);
        const int64_t first = q.nextProbeMs - 10000;
        NoteTopicProbe(q, TopicProbeAnswer::Inconclusive, 20000);
        const int64_t second = q.nextProbeMs - 20000;
        for (int k = 0; k < 10; ++k)
        {
            NoteTopicProbe(q, TopicProbeAnswer::Inconclusive, 30000);
        }
        CHECK(first == kTopicProbeSecondMs && second == 2 * kTopicProbeSecondMs && q.nextProbeMs - 30000 == kTopicProbeRetryMaxMs, "probe: no verdict waits 30 s, 60 s, ... up to 5 min");
        NoteTopicProbe(q, TopicProbeAnswer::NotFound, 40000);
        CHECK(q.notFound == 1 && q.inconclusive == 0 && q.phase == HeldThreadPhase::Probing, "probe: not found after no verdicts is the first of two");
        CHECK(NoteTopicProbe(q, TopicProbeAnswer::Inconclusive, 50000) == TopicProbeStep::Wait && q.notFound == 1, "probe: no verdict between two not-founds keeps the count");

        // The release cap: a thread probes find again and again while it keeps refusing posts counts as gone.
        HeldThread r;
        r.threadId = 44;
        int64_t now = 0;
        for (int k = 0; k < kTopicProbeReleasesMax; ++k)
        {
            CHECK(HoldThread(r, now) == HeldThreadPhase::Probing, "release cap: held and probed");
            CHECK(NoteTopicProbe(r, TopicProbeAnswer::Exists, now + 10000) == TopicProbeStep::Release && !HeldThreadHoldsPosts(r), "release cap: released, its posts go");
            now += 20000;
        }
        CHECK(r.releases == kTopicProbeReleasesMax, "release cap: the releases are counted");
        CHECK(HoldThread(r, now) == HeldThreadPhase::Gone && r.goneSinceMs == now && !r.goneSettled, "release cap: the next refusal counts as gone at once");
        HeldThread s;
        s.threadId = 45;
        HoldThread(s, 0);
        NoteTopicProbe(s, TopicProbeAnswer::Exists, 10000);
        NoteTopicProbe(s, TopicProbeAnswer::Exists, 10001); // released already: no change
        CHECK(s.releases == 1, "release: a probe of a released thread counts nothing");
        CHECK(HoldThread(s, 10000 + kTopicReleaseMemoryMs + 1) == HeldThreadPhase::Probing && s.releases == 0, "release memory: a refusal long after starts a fresh count");
    }

    void TestFallbackAndThreads()
    {
        CHECK(OutboxPlainTextOfHtml(L"<b>Bold</b> &lt;tag&gt; <a href=\"x\">link</a>") == L"Bold &lt;tag&gt; link", "plain: tags out, entities kept");
        CHECK(OutboxPlainTextOfHtml(L"a > b") == L"a > b", "plain: a lone '>' outside a tag stays");

        auto params = json::Value::MkObj();
        params.Set(L"chat_id", json::Value::MkNum(1));
        CHECK(OutboxThreadOf(params) == 0, "thread: none = the main view");
        SetOutboxThread(params, 0);
        CHECK(params.members.size() == 1, "thread: a main-view item gets no thread member");
        SetOutboxThread(params, 77);
        CHECK(OutboxThreadOf(params) == 77, "thread: added when missing");
        SetOutboxThread(params, 78);
        size_t count = 0;
        for (const auto& [k, v] : params.members)
        {
            count += k == L"message_thread_id" ? 1 : 0;
        }
        CHECK(count == 1 && OutboxThreadOf(params) == 78, "thread: changed in place, never appended twice");
    }

    void TestJournalFiles()
    {
        const auto dir = ScratchOutbox(L"files");
        const std::wstring folder = dir.wstring();
        std::string bytes;
        std::wstring error;
        {
            OutboxJournal j;
            CHECK(j.Open(folder, bytes, error) && bytes.empty(), "files: a fresh folder opens with an empty journal");
            CHECK(std::filesystem::exists(dir / L".lock"), "files: the lock file exists while held");
            {
                OutboxJournal second;
                std::string other;
                std::wstring why;
                CHECK(!second.Open(folder, other, why) && why.find(L"another process") != std::wstring::npos, "files: a second writer is refused by the lock");
            }
            CHECK(!j.IsOpen(), "files: nothing is open for appends before the rewrite");
            const std::string header = OutboxHeaderLine(L"bot", 12345, 1, L"test");
            CHECK(j.Rewrite(header, error) && j.IsOpen() && j.Bytes() == header.size(), "files: the rewrite writes the compact journal and opens it");
            CHECK(j.Append(OutboxPutLine(Put(1, 1, 100, L"one"))) && j.Append(OutboxPutLine(Put(2, 2, 100, L"two"))) && j.Append(OutboxOkLine(1, 5)),
                  "files: appends");
            j.FlushNow();
            CHECK(j.AppendDead(OutboxDeadLine(Put(3, 3, 100, L"dead"), 9, L"HTTP 400: Bad Request")), "files: a dead letter");
            j.Close();
            CHECK(!j.IsOpen(), "files: closed");
            j.Close(); // twice is safe
        }
        CHECK(!std::filesystem::exists(dir / L".lock"), "files: the lock file is gone after Close (delete-on-close)");
        CHECK(std::filesystem::exists(dir / L"dead.jsonl"), "files: dead.jsonl was written");
        {
            json::Value dead;
            const std::string d = ReadAllBytes(dir / L"dead.jsonl");
            CHECK(!d.empty() && OpenOutboxRecord(std::string_view{ d.data(), d.size() - 1 }, dead) == OutboxLineRead::Ok && dead.StrAt(L"op") == L"dead" &&
                      dead.Find(L"params") && dead.Find(L"params")->StrAt(L"text") == L"dead",
                  "files: the dead letter keeps the whole message");
        }
        // Reopen: read back, fold, with a torn tail appended by hand (a crash mid-write).
        {
            std::ofstream f(dir / L"outbox.jsonl", std::ios::binary | std::ios::app);
            f << "{\"op\":\"put\",\"id\":3,\"pri";
        }
        {
            OutboxJournal j;
            CHECK(j.Open(folder, bytes, error), "files: reopens");
            size_t torn = 0;
            const OutboxRecovery r = FoldOutboxJournal(bytes, torn);
            CHECK(r.header && r.bot == L"bot", "files: the header read back");
            CHECK(r.live.size() == 1 && r.live[0].put.id == 2, "files: item 1 was sent, item 2 is still queued");
            CHECK(torn > 0, "files: the torn tail is seen");
            const std::string compact = OutboxHeaderLine(L"bot", 12345, 2, L"test") + OutboxPutLine(r.live[0].put);
            CHECK(j.Rewrite(compact, error) && ReadAllBytes(dir / L"outbox.jsonl") == compact, "files: the compaction replaces the file (torn tail gone)");
            CHECK(!std::filesystem::exists(dir / L"outbox.jsonl.tmp"), "files: no temporary file left");
            j.Close();
        }
        // Four threads appending at once: every line whole and valid, none interleaved, none lost.
        {
            OutboxJournal j;
            CHECK(j.Open(folder, bytes, error) && j.Rewrite(OutboxHeaderLine(L"bot", 1, 1, L"t"), error), "files: reopened for the concurrency run");
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t)
            {
                threads.emplace_back([&j, t]() {
                    for (int k = 0; k < 250; ++k)
                    {
                        // ~2 KB each, as a real reply.
                        const uint64_t id = static_cast<uint64_t>(t) * 1000 + static_cast<uint64_t>(k) + 1;
                        j.Append(OutboxPutLine(Put(id, id, 100 + t, std::wstring(2000, static_cast<wchar_t>(L'a' + t)))));
                    }
                });
            }
            for (auto& th : threads)
            {
                th.join();
            }
            j.Close();
            const std::string all = ReadAllBytes(dir / L"outbox.jsonl");
            size_t torn = 1;
            const OutboxRecovery r = FoldOutboxJournal(all, torn);
            CHECK(r.live.size() == 1000 && r.badLines == 0 && torn == 0, "files: 4 x 250 concurrent appends, every line whole and valid");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

void TestMessagingOutbox()
{
    // ASCII only: the harness's wprintf stops at the first character the C locale cannot convert (the line and its newline lost).
    wprintf(L"== TestMessagingOutbox (MESSAGING.md 21.50: the durable outbox - the journal's lines, the fold, the answer table, classes, back-off, breaker, lanes, probes, the files) ==\n");
    TestEncoding();
    TestSplitAndFold();
    TestAnswersAndClasses();
    TestBackoffAndBreaker();
    TestLanesAndProbes();
    TestFallbackAndThreads();
    TestJournalFiles();
}
