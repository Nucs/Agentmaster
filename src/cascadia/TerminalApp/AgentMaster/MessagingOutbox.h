// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MessagingOutbox.h: the PURE core of the durable Telegram outbox (doc/agentmaster/MESSAGING_OUTBOX.md,
// MESSAGING.md §21.50). The owner's ask of 2026-11-10: "all messages must be durable and queue-wise recovering. Even bad
// gateway". What lives here has no file, no clock and no thread of its own, so the m5 harness pins all of it
// (tests_messaging_outbox.cpp):
//   - the JOURNAL's line format: one UTF-8 JSON object per line, "op" first, a CRC-32 as its last member, '\n' at its end
//     (§4: a line is whole or absent, and a power cut that filled a line with garbage that still parses is caught);
//   - the RECORDS (hdr / put / try / ok / retry / drop / lane / flood, and dead.jsonl's dead) and the FOLD that rebuilds the
//     queue from them at a start (§6), a torn tail cut off;
//   - the ANSWER table (§7): what Telegram's answer makes of an item — sent, a flood hold, a transient failure to back off,
//     a topic that may be gone, a refusal;
//   - an item's CLASS (which items are journaled, which are retried and how long), the BACK-OFF, the CIRCUIT BREAKER, the
//     per-thread LANE order and the TOPIC PROBE's state machine (§8: a topic is found by its thread id, never its title,
//     and "gone" is confirmed by two probes).
// The files, the flusher thread and the lock file are MessagingOutboxJournal.{h,cpp}; the service wires both into its send
// loop, its tick and its start (Messaging.cpp).

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Json.h"
#include "TelegramApi.h" // TgResult: the answer the classifiers read (the struct only; no call is made here)

namespace Agentmaster::Msg
{
    // ===== constants ================================================================================================

    /// <summary>The journal format's version, written in every header line. A journal of another version is not read (its
    /// messages are left for the build that wrote them: the service logs the refusal and starts with an empty queue).</summary>
    inline constexpr int kOutboxJournalVersion = 1;
    /// <summary>How many attempts a BOUNDED item gets in all (the first send and two retries after a transient failure): the
    /// non-journaled items whose state machines rebuild them anyway (history replay, deletes, pins). Kept short, so a late item
    /// never lands long after the state it belonged to.</summary>
    inline constexpr uint32_t kOutboxBoundedAttempts = 3;
    /// <summary>The back-off after the first transient failure (doubled per attempt: 1, 2, 4, 8, 16, 32 s, then the cap).</summary>
    inline constexpr int64_t kOutboxBackoffFirstMs = 1000;
    /// <summary>The longest back-off: a durable item is retried at least once a minute for as long as Telegram does not answer.</summary>
    inline constexpr int64_t kOutboxBackoffMaxMs = 60 * 1000;
    /// <summary>Transient failures IN A ROW, over every item, that open the circuit breaker (Telegram unreachable: nothing is
    /// sent for a pause, then one item probes).</summary>
    inline constexpr uint32_t kOutboxBreakerThreshold = 8;
    /// <summary>The breaker's first pause; each failed probe doubles it, up to kOutboxBreakerMaxPauseMs.</summary>
    inline constexpr int64_t kOutboxBreakerFirstPauseMs = 15 * 1000;
    /// <summary>The breaker's longest pause.</summary>
    inline constexpr int64_t kOutboxBreakerMaxPauseMs = 5 * 60 * 1000;
    /// <summary>A thread that refused a post with "message thread not found" is probed this long after the refusal (§8.2).</summary>
    inline constexpr int64_t kTopicProbeFirstMs = 10 * 1000;
    /// <summary>The second probe after a first "not found": two in a row confirm the topic is gone (§8.2).</summary>
    inline constexpr int64_t kTopicProbeSecondMs = 30 * 1000;
    /// <summary>The longest wait between probes that got no verdict (no answer, a 5xx, a 429): the first waits
    /// kTopicProbeSecondMs, each next one twice as long, up to this.</summary>
    inline constexpr int64_t kTopicProbeRetryMaxMs = 5 * 60 * 1000;
    /// <summary>How many times a probe may answer "the topic exists" while the thread keeps refusing posts within
    /// kTopicReleaseMemoryMs: the next refusal then counts as gone (Telegram has answered transient nonsense before —
    /// BOT_TOPIC_WRONG_HOME for 7 minutes on 2026-11-09 — and a thread must not ping-pong between held and released forever).</summary>
    inline constexpr int kTopicProbeReleasesMax = 3;
    /// <summary>How long a released thread's release count is remembered: a refusal after that starts a fresh count.</summary>
    inline constexpr int64_t kTopicReleaseMemoryMs = 10 * 60 * 1000;
    /// <summary>How long a gone thread's messages wait for its session's topic to be made again (the topic sync makes it when
    /// the session still deserves one); past it they are given up (the session went idle or closed meanwhile).</summary>
    inline constexpr int64_t kTopicGoneWaitMs = 15 * 60 * 1000;
    /// <summary>The journal is compacted (only the live items written again) once it grows past this many bytes.</summary>
    inline constexpr uint64_t kOutboxCheckpointBytes = 16ull * 1024ull * 1024ull;
    /// <summary>The longest chain of thread redirects followed (a topic made again, then again): a loop in a damaged journal
    /// can never hang the start.</summary>
    inline constexpr int kOutboxRedirectHopsMax = 16;

    // ===== UTF-8 ====================================================================================================

    /// <summary>
    /// UTF-16 to UTF-8, the journal's encoding. A surrogate pair becomes its 4-byte sequence; a LONE surrogate (invalid
    /// UTF-16, which a transcript can carry) becomes U+FFFD, as WideCharToMultiByte makes it — so a line always holds valid
    /// UTF-8 and its CRC is taken over exactly the bytes written.
    /// </summary>
    /// <param name="s">The text.</param>
    /// <returns>Its UTF-8 bytes.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::string OutboxUtf8(std::wstring_view s)
    {
        std::string out;
        out.reserve(s.size() + s.size() / 4);
        for (size_t i = 0; i < s.size(); ++i)
        {
            uint32_t cp = static_cast<uint32_t>(s[i]);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(s[i + 1]) - 0xDC00);
                ++i;
            }
            else if (cp >= 0xD800 && cp <= 0xDFFF)
            {
                cp = 0xFFFD; // a lone surrogate: what Windows' own conversion writes in its place
            }
            if (cp < 0x80)
            {
                out += static_cast<char>(cp);
            }
            else if (cp < 0x800)
            {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        return out;
    }

    /// <summary>
    /// UTF-8 to UTF-16, for reading a journal line back. Every byte that does not start a valid, shortest-form sequence (a
    /// stray continuation byte, a truncated or overlong sequence, an encoded surrogate, a code point past U+10FFFF) becomes
    /// U+FFFD and the decoder moves on by one byte — a damaged line still decodes (its CRC has already said whether to trust it).
    /// </summary>
    /// <param name="s">The bytes.</param>
    /// <returns>The text.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring OutboxWide(std::string_view s)
    {
        std::wstring out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size())
        {
            const auto b0 = static_cast<unsigned char>(s[i]);
            if (b0 < 0x80)
            {
                out += static_cast<wchar_t>(b0);
                ++i;
                continue;
            }
            // The sequence's length and the smallest code point it may carry (anything smaller is an overlong form).
            size_t len = 0;
            uint32_t cp = 0;
            uint32_t minCp = 0;
            if ((b0 & 0xE0) == 0xC0)
            {
                len = 2;
                cp = b0 & 0x1F;
                minCp = 0x80;
            }
            else if ((b0 & 0xF0) == 0xE0)
            {
                len = 3;
                cp = b0 & 0x0F;
                minCp = 0x800;
            }
            else if ((b0 & 0xF8) == 0xF0)
            {
                len = 4;
                cp = b0 & 0x07;
                minCp = 0x10000;
            }
            bool ok = len != 0 && i + len <= s.size();
            for (size_t k = 1; ok && k < len; ++k)
            {
                const auto b = static_cast<unsigned char>(s[i + k]);
                ok = (b & 0xC0) == 0x80;
                cp = (cp << 6) | (b & 0x3F);
            }
            ok = ok && cp >= minCp && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
            if (!ok)
            {
                out += static_cast<wchar_t>(0xFFFD);
                ++i;
                continue;
            }
            if (cp >= 0x10000)
            {
                cp -= 0x10000;
                out += static_cast<wchar_t>(0xD800 + (cp >> 10));
                out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
            }
            else
            {
                out += static_cast<wchar_t>(cp);
            }
            i += len;
        }
        return out;
    }

    // ===== CRC-32 ===================================================================================================

    namespace detail
    {
        /// <summary>The reflected CRC-32 table (IEEE 802.3, polynomial 0xEDB88320: zip's and PNG's CRC), built at compile time.</summary>
        /// <returns>The 256 entries.</returns>
        constexpr std::array<uint32_t, 256> MakeOutboxCrcTable() noexcept
        {
            std::array<uint32_t, 256> t{};
            for (uint32_t n = 0; n < 256; ++n)
            {
                uint32_t c = n;
                for (int k = 0; k < 8; ++k)
                {
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                t[n] = c;
            }
            return t;
        }
        /// <summary>The table every OutboxCrc32 call reads (one copy per program: an inline variable).</summary>
        inline constexpr std::array<uint32_t, 256> kOutboxCrcTable = MakeOutboxCrcTable();
    }

    /// <summary>
    /// The CRC-32 of a line's bytes (the IEEE polynomial, as zip and PNG take it: "123456789" gives 0xCBF43926). Table-driven:
    /// ~1 GB/s, so a 4 KB line costs a few microseconds under the outbox's lock.
    /// </summary>
    /// <param name="bytes">The bytes.</param>
    /// <returns>The checksum.</returns>
    inline uint32_t OutboxCrc32(std::string_view bytes) noexcept
    {
        uint32_t c = 0xFFFFFFFFu;
        for (const char ch : bytes)
        {
            c = detail::kOutboxCrcTable[(c ^ static_cast<unsigned char>(ch)) & 0xFFu] ^ (c >> 8);
        }
        return c ^ 0xFFFFFFFFu;
    }

    // ===== a record line ============================================================================================

    /// <summary>The member every sealed line ends with, before its 8 hex digits and the closing quote and brace.</summary>
    inline constexpr std::string_view kOutboxCrcMember = ",\"crc\":\"";

    /// <summary>
    /// Seals one record into its journal line: `{…}` becomes `{…,"crc":"xxxxxxxx"}\n`, the CRC taken over every byte before
    /// `,"crc":` (MESSAGING_OUTBOX.md §4). The caller writes the whole line in ONE WriteFile, so a reader sees it whole or not
    /// at all.
    /// </summary>
    /// <param name="objectJson">The record as a UTF-8 JSON object with at least one member ("op"): `{"op":…}`.</param>
    /// <returns>The sealed line, '\n' included; empty when `objectJson` is not an object with a member (nothing to seal).</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string SealOutboxRecord(std::string_view objectJson)
    {
        // `{}` (or anything that does not end in '}') has no member to put the checksum after: refused, never half-sealed.
        if (objectJson.size() < 3 || objectJson.front() != '{' || objectJson.back() != '}')
        {
            return {};
        }
        std::string line{ objectJson.substr(0, objectJson.size() - 1) };
        const uint32_t crc = OutboxCrc32(line);
        static constexpr char kHex[] = "0123456789abcdef";
        line += kOutboxCrcMember;
        for (int shift = 28; shift >= 0; shift -= 4)
        {
            line += kHex[(crc >> shift) & 0xFu];
        }
        line += "\"}\n";
        return line;
    }

    /// <summary>What reading one journal line found.</summary>
    enum class OutboxLineRead
    {
        /// <summary>A sealed record whose checksum matches and whose JSON is an object with an "op".</summary>
        Ok,
        /// <summary>The line ends like a sealed record but its checksum does not match its bytes: damaged (a power cut's garbage).</summary>
        BadCrc,
        /// <summary>No checksum member where one must be, or JSON that does not parse into an object with an "op".</summary>
        Unreadable,
    };

    /// <summary>
    /// Opens one sealed line: checks its CRC, then parses it. The line comes WITHOUT its '\n' (SplitOutboxJournal cuts there); a
    /// trailing '\r' (an editor that saved the file with CRLF) is ignored.
    /// </summary>
    /// <param name="line">The line's bytes.</param>
    /// <param name="out">Receives the record (its "crc" member included) when the result is Ok; untouched otherwise.</param>
    /// <returns>Ok, BadCrc or Unreadable (see OutboxLineRead).</returns>
    /// <exception cref="std::bad_alloc">If the decoded text or the parsed record cannot be allocated.</exception>
    inline OutboxLineRead OpenOutboxRecord(std::string_view line, json::Value& out)
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        // The fixed tail: ,"crc":"<8 hex>"}
        constexpr size_t kTail = kOutboxCrcMember.size() + 8 + 2;
        if (line.size() < kTail + 2 || line.substr(line.size() - 2) != "\"}")
        {
            return OutboxLineRead::Unreadable;
        }
        const size_t at = line.size() - kTail;
        if (line.substr(at, kOutboxCrcMember.size()) != kOutboxCrcMember)
        {
            return OutboxLineRead::Unreadable;
        }
        uint32_t stated = 0;
        for (size_t k = 0; k < 8; ++k)
        {
            const char h = line[at + kOutboxCrcMember.size() + k];
            uint32_t v = 0;
            if (h >= '0' && h <= '9')
            {
                v = static_cast<uint32_t>(h - '0');
            }
            else if (h >= 'a' && h <= 'f')
            {
                v = static_cast<uint32_t>(h - 'a' + 10);
            }
            else if (h >= 'A' && h <= 'F')
            {
                v = static_cast<uint32_t>(h - 'A' + 10);
            }
            else
            {
                return OutboxLineRead::Unreadable;
            }
            stated = (stated << 4) | v;
        }
        if (OutboxCrc32(line.substr(0, at)) != stated)
        {
            return OutboxLineRead::BadCrc;
        }
        auto parsed = json::Parse(OutboxWide(line));
        if (!parsed || parsed->type != json::Value::Type::Obj || parsed->StrAt(L"op").empty())
        {
            return OutboxLineRead::Unreadable;
        }
        out = std::move(*parsed);
        return OutboxLineRead::Ok;
    }

    /// <summary>A journal's bytes cut into lines: every '\n'-terminated line, and how many bytes follow the last '\n'.</summary>
    struct OutboxJournalText
    {
        /// <summary>The whole lines, in file order, each WITHOUT its '\n' (views into the bytes given to SplitOutboxJournal).</summary>
        std::vector<std::string_view> lines;
        /// <summary>The bytes up to and including the last '\n': where a torn tail is cut (SetEndOfFile).</summary>
        size_t wholeBytes{ 0 };
        /// <summary>The bytes after the last '\n': a line a live writer has not finished, or one a crash tore (§5.2). The start
        /// cuts them off; nothing in them is read.</summary>
        size_t tornBytes{ 0 };
    };

    /// <summary>
    /// Cuts a journal's bytes into whole lines. Only a '\n' ends a line (UTF-8 never carries the byte 0x0A inside a character,
    /// and a sealed record has none inside: Json.h escapes every control character). Empty lines (a zero-filled block after a
    /// power cut reads as one long line of NULs, which the CRC refuses; a bare "\n" is skipped) are left out.
    /// </summary>
    /// <param name="bytes">The file's bytes (the views in the result point into them: keep them alive).</param>
    /// <returns>The lines and the tail's size.</returns>
    /// <exception cref="std::bad_alloc">If the line list cannot grow.</exception>
    inline OutboxJournalText SplitOutboxJournal(std::string_view bytes)
    {
        OutboxJournalText t;
        size_t start = 0;
        while (start < bytes.size())
        {
            const size_t nl = bytes.find('\n', start);
            if (nl == std::string_view::npos)
            {
                break;
            }
            if (nl > start)
            {
                t.lines.push_back(bytes.substr(start, nl - start));
            }
            start = nl + 1;
        }
        t.wholeBytes = start;
        t.tornBytes = bytes.size() - start;
        return t;
    }

    // ===== the records ==============================================================================================

    /// <summary>
    /// One journaled item: what a `put` line carries, everything the sender needs to make the call again after a restart —
    /// with no callback (a recovered item's completion is lost with the run; MESSAGING_OUTBOX.md §9's `then` is not built).
    /// </summary>
    struct OutboxPut
    {
        /// <summary>The journal id: monotonic, never reused within a journal (the key of every later record about it).</summary>
        uint64_t id{ 0 };
        /// <summary>The item's SendPriority as an int (0 = Final … 5 = History): the sender's first order key.</summary>
        int prio{ 1 };
        /// <summary>The outbox sequence number: the second order key (the chunks of one message share it).</summary>
        uint64_t seq{ 0 };
        /// <summary>The slot within the sequence: the third order key (a chunk's index, a fallback's place).</summary>
        uint32_t sub{ 0 };
        /// <summary>The Bot API method ("sendMessage", "sendRichMessage").</summary>
        std::wstring method;
        /// <summary>The call's parameters (chat_id, message_thread_id, the text …), exactly as they go on the wire.</summary>
        json::Value params;
        /// <summary>A RICH message's classic HTML form ("" for none): a recovered rich message that Telegram refuses goes as this
        /// HTML in its place, as its lost callback would have sent it (§7's fallback row).</summary>
        std::wstring fallbackHtml;
        /// <summary>Queued by the live MIRROR's pass (the transcript's content): a topic made again replays its whole history,
        /// so these are dropped instead of moved to the new thread (they would show twice).</summary>
        bool mirror{ false };
        /// <summary>When it was queued (Unix ms): the census's "oldest" and the log's ages.</summary>
        int64_t queuedWallMs{ 0 };
    };

    namespace detail
    {
        /// <summary>A JSON value's UTF-8 text (Json.h dumps UTF-16).</summary>
        /// <param name="v">The value.</param>
        /// <returns>Its JSON as UTF-8.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::string OutboxJsonUtf8(const json::Value& v)
        {
            return OutboxUtf8(json::Dump(v));
        }

        /// <summary>A number member (Json.h's numbers are doubles: every id, seq and Unix ms here is far below 2^53, exact).</summary>
        /// <param name="v">The integer.</param>
        /// <returns>The JSON number.</returns>
        inline json::Value OutboxNum(int64_t v)
        {
            return json::Value::MkNum(static_cast<double>(v));
        }
    }

    /// <summary>
    /// The journal's first line: which bot and chat its items belong to. A start whose token or owner differs discards the
    /// journal's items (they would go to another chat, or name another bot's threads).
    /// </summary>
    /// <param name="bot">The bot's token HASH (MessagingService's TokenHash — never the token).</param>
    /// <param name="chat">The owner's chat id every item goes to.</param>
    /// <param name="wallMs">When the journal was (re)written, Unix ms.</param>
    /// <param name="writer">The build that wrote it ("0.8.0.13 release"), for a person reading the file.</param>
    /// <returns>The sealed line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string OutboxHeaderLine(std::wstring_view bot, int64_t chat, int64_t wallMs, std::wstring_view writer)
    {
        auto r = json::Value::MkObj();
        r.Set(L"op", json::Value::MkStr(L"hdr"));
        r.Set(L"v", detail::OutboxNum(kOutboxJournalVersion));
        r.Set(L"bot", json::Value::MkStr(std::wstring{ bot }));
        r.Set(L"chat", detail::OutboxNum(chat));
        r.Set(L"at", detail::OutboxNum(wallMs));
        r.Set(L"writer", json::Value::MkStr(std::wstring{ writer }));
        return SealOutboxRecord(detail::OutboxJsonUtf8(r));
    }

    /// <summary>
    /// The `put` line of an item: its order keys, its method, its fallback, its mirror mark, and its parameters LAST (the big
    /// part). The parameters may come pre-serialized, so the caller builds that JSON BEFORE it takes the outbox's lock (§5.1:
    /// the lock is held for the small wrapper and the write only).
    /// </summary>
    /// <param name="put">The item.</param>
    /// <param name="paramsJson">`put.params` as UTF-8 JSON, already serialized; empty = serialize `put.params` here.</param>
    /// <returns>The sealed line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string OutboxPutLine(const OutboxPut& put, std::string_view paramsJson = {})
    {
        auto r = json::Value::MkObj();
        r.Set(L"op", json::Value::MkStr(L"put"));
        r.Set(L"id", detail::OutboxNum(static_cast<int64_t>(put.id)));
        r.Set(L"prio", detail::OutboxNum(put.prio));
        r.Set(L"seq", detail::OutboxNum(static_cast<int64_t>(put.seq)));
        r.Set(L"sub", detail::OutboxNum(put.sub));
        r.Set(L"method", json::Value::MkStr(put.method));
        r.Set(L"at", detail::OutboxNum(put.queuedWallMs));
        if (put.mirror)
        {
            r.Set(L"mir", detail::OutboxNum(1));
        }
        if (!put.fallbackHtml.empty())
        {
            r.Set(L"fb", json::Value::MkStr(put.fallbackHtml));
        }
        std::string head = detail::OutboxJsonUtf8(r);
        head.pop_back(); // the closing '}': the parameters go last
        head += ",\"params\":";
        if (paramsJson.empty())
        {
            head += detail::OutboxJsonUtf8(put.params.type == json::Value::Type::Obj ? put.params : json::Value::MkObj());
        }
        else
        {
            head.append(paramsJson.data(), paramsJson.size());
        }
        head += '}';
        return SealOutboxRecord(head);
    }

    /// <summary>The `try` line, appended right before an item's call: an item whose last record is a `try` was cut off mid-call
    /// and is sent again at the next start (D-O2, at-least-once: Telegram may then show it twice).</summary>
    /// <param name="id">The item's journal id.</param>
    /// <param name="attempt">The attempt number (1 = the first send).</param>
    /// <param name="wallMs">When, Unix ms.</param>
    /// <returns>The sealed line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string OutboxTryLine(uint64_t id, uint32_t attempt, int64_t wallMs)
    {
        auto r = json::Value::MkObj();
        r.Set(L"op", json::Value::MkStr(L"try"));
        r.Set(L"id", detail::OutboxNum(static_cast<int64_t>(id)));
        r.Set(L"n", detail::OutboxNum(attempt));
        r.Set(L"at", detail::OutboxNum(wallMs));
        return SealOutboxRecord(detail::OutboxJsonUtf8(r));
    }

    /// <summary>The `ok` line: Telegram took the item; it leaves the queue for good.</summary>
    /// <param name="id">The item's journal id.</param>
    /// <param name="messageId">The message id Telegram gave it (0 when the answer named none).</param>
    /// <returns>The sealed line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string OutboxOkLine(uint64_t id, int64_t messageId)
    {
        auto r = json::Value::MkObj();
        r.Set(L"op", json::Value::MkStr(L"ok"));
        r.Set(L"id", detail::OutboxNum(static_cast<int64_t>(id)));
        r.Set(L"mid", detail::OutboxNum(messageId));
        return SealOutboxRecord(detail::OutboxJsonUtf8(r));
    }

    /// <summary>The `retry` line: a transient failure (a 5xx, no answer); the item stays, backing off until `notBeforeWallMs`.</summary>
    /// <param name="id">The item's journal id.</param>
    /// <param name="attempts">The transient failures so far (the back-off's step).</param>
    /// <param name="notBeforeWallMs">When it may go again, Unix ms.</param>
    /// <param name="why">Telegram's words or the transport's ("HTTP 502", "request failed (12002)").</param>
    /// <returns>The sealed line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string OutboxRetryLine(uint64_t id, uint32_t attempts, int64_t notBeforeWallMs, std::wstring_view why)
    {
        auto r = json::Value::MkObj();
        r.Set(L"op", json::Value::MkStr(L"retry"));
        r.Set(L"id", detail::OutboxNum(static_cast<int64_t>(id)));
        r.Set(L"n", detail::OutboxNum(attempts));
        r.Set(L"nb", detail::OutboxNum(notBeforeWallMs));
        r.Set(L"why", json::Value::MkStr(std::wstring{ why }));
        return SealOutboxRecord(detail::OutboxJsonUtf8(r));
    }

    /// <summary>The `drop` line: the item leaves the queue unsent — refused for good (also copied to dead.jsonl), replaced by
    /// its fallback, its topic deleted with its session, or replayed in a topic made again.</summary>
    /// <param name="id">The item's journal id.</param>
    /// <param name="why">Why, in words.</param>
    /// <returns>The sealed line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string OutboxDropLine(uint64_t id, std::wstring_view why)
    {
        auto r = json::Value::MkObj();
        r.Set(L"op", json::Value::MkStr(L"drop"));
        r.Set(L"id", detail::OutboxNum(static_cast<int64_t>(id)));
        r.Set(L"why", json::Value::MkStr(std::wstring{ why }));
        return SealOutboxRecord(detail::OutboxJsonUtf8(r));
    }

    /// <summary>The `lane` line: a topic was made again, so every item for `fromThread` goes to `toThread` (§8.3) — also the
    /// items a restart recovers from older lines.</summary>
    /// <param name="fromThread">The gone thread.</param>
    /// <param name="toThread">Its new thread.</param>
    /// <returns>The sealed line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string OutboxLaneLine(int64_t fromThread, int64_t toThread)
    {
        auto r = json::Value::MkObj();
        r.Set(L"op", json::Value::MkStr(L"lane"));
        r.Set(L"from", detail::OutboxNum(fromThread));
        r.Set(L"to", detail::OutboxNum(toThread));
        return SealOutboxRecord(detail::OutboxJsonUtf8(r));
    }

    /// <summary>The `flood` line: Telegram's flood hold as it stands, in WALL time (a restart must not call straight into a hold
    /// it forgot — the hold of 2026-11-09 lasted 21,890 s). Each line is the whole parking: the newest one wins.</summary>
    /// <param name="allUntilWallMs">Every method waits until then, Unix ms (0 = no chat-wide hold).</param>
    /// <param name="methodUntilWallMs">Each held method and its deadline, Unix ms.</param>
    /// <returns>The sealed line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string OutboxFloodLine(int64_t allUntilWallMs, const std::map<std::wstring, int64_t>& methodUntilWallMs)
    {
        auto r = json::Value::MkObj();
        r.Set(L"op", json::Value::MkStr(L"flood"));
        r.Set(L"all", detail::OutboxNum(allUntilWallMs));
        auto m = json::Value::MkObj();
        for (const auto& [method, until] : methodUntilWallMs)
        {
            m.Set(method, detail::OutboxNum(until));
        }
        r.Set(L"m", std::move(m));
        return SealOutboxRecord(detail::OutboxJsonUtf8(r));
    }

    /// <summary>A dead letter's line in dead.jsonl: the whole item (its method and parameters, so a person can still read what
    /// it said) and Telegram's refusal. dead.jsonl is never read back by the service; it only grows (an item lands there only
    /// when Telegram refused it in every form, §2's first guarantee).</summary>
    /// <param name="put">The refused item.</param>
    /// <param name="wallMs">When it was given up, Unix ms.</param>
    /// <param name="why">Telegram's refusal.</param>
    /// <returns>The sealed line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string OutboxDeadLine(const OutboxPut& put, int64_t wallMs, std::wstring_view why)
    {
        auto r = json::Value::MkObj();
        r.Set(L"op", json::Value::MkStr(L"dead"));
        r.Set(L"id", detail::OutboxNum(static_cast<int64_t>(put.id)));
        r.Set(L"at", detail::OutboxNum(wallMs));
        r.Set(L"queued", detail::OutboxNum(put.queuedWallMs));
        r.Set(L"why", json::Value::MkStr(std::wstring{ why }));
        r.Set(L"method", json::Value::MkStr(put.method));
        r.Set(L"params", put.params.type == json::Value::Type::Obj ? put.params : json::Value::MkObj());
        return SealOutboxRecord(detail::OutboxJsonUtf8(r));
    }

    // ===== the thread an item posts into ============================================================================

    /// <summary>The thread an item's parameters name (`message_thread_id`); 0 = none (the chat's main view, "All").</summary>
    /// <param name="params">The call's parameters.</param>
    /// <returns>The thread id, or 0.</returns>
    inline int64_t OutboxThreadOf(const json::Value& params) noexcept
    {
        for (const auto& [k, v] : params.members)
        {
            if (k == L"message_thread_id" && v.type == json::Value::Type::Num)
            {
                return static_cast<int64_t>(v.num);
            }
        }
        return 0;
    }

    /// <summary>
    /// Points an item's parameters at another thread (a topic made again, §8.3): the existing `message_thread_id` member is
    /// changed in place, never appended a second time (a duplicate key would leave which one Telegram reads to chance). A
    /// parameter object without one gets it only when `thread` is a thread (&gt; 0).
    /// </summary>
    /// <param name="params">The call's parameters (an object).</param>
    /// <param name="thread">The new thread id.</param>
    /// <exception cref="std::bad_alloc">If the member cannot be added.</exception>
    inline void SetOutboxThread(json::Value& params, int64_t thread)
    {
        for (auto& [k, v] : params.members)
        {
            if (k == L"message_thread_id")
            {
                v = json::Value::MkNum(static_cast<double>(thread));
                return;
            }
        }
        if (thread > 0 && params.type == json::Value::Type::Obj)
        {
            params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(thread)));
        }
    }

    /// <summary>
    /// Where a thread's items go now: follows the redirects (a topic made again, maybe again) to the newest thread. A chain
    /// longer than kOutboxRedirectHopsMax (only a damaged journal can make a loop) stops where it is.
    /// </summary>
    /// <param name="redirects">gone thread → its new thread.</param>
    /// <param name="thread">The thread an item names.</param>
    /// <returns>The thread it goes to (the same one when none redirects it).</returns>
    inline int64_t ResolveOutboxThread(const std::map<int64_t, int64_t>& redirects, int64_t thread)
    {
        for (int hop = 0; hop < kOutboxRedirectHopsMax; ++hop)
        {
            const auto it = redirects.find(thread);
            if (it == redirects.end() || it->second == thread)
            {
                break;
            }
            thread = it->second;
        }
        return thread;
    }

    // ===== the fold (recovery, §6) ==================================================================================

    /// <summary>One item the fold found still queued: its put, and what its later records said.</summary>
    struct OutboxRecoveredItem
    {
        /// <summary>The item (its parameters already pointed at the newest thread by the journal's `lane` lines).</summary>
        OutboxPut put;
        /// <summary>The transient failures recorded so far (the back-off continues from there).</summary>
        uint32_t attempts{ 0 };
        /// <summary>The back-off's deadline recorded by the last `retry` line, Unix ms (0 = none: it may go at once).</summary>
        int64_t notBeforeWallMs{ 0 };
        /// <summary>Its last record is a `try`: the call was cut off with no outcome (a kill, a crash, a power cut mid-call).
        /// It goes again — the census counts it as "sent again after a cut-off call".</summary>
        bool tried{ false };
    };

    /// <summary>What the journal holds, folded: the queue to restore and the facts that come with it.</summary>
    struct OutboxRecovery
    {
        /// <summary>A header line was found (a journal without one is not trusted: its items may be another bot's).</summary>
        bool header{ false };
        /// <summary>The header's journal format version.</summary>
        int version{ 0 };
        /// <summary>The header's bot (token hash).</summary>
        std::wstring bot;
        /// <summary>The header's chat.</summary>
        int64_t chat{ 0 };
        /// <summary>The items still queued, by journal id (the order they were queued in).</summary>
        std::vector<OutboxRecoveredItem> live;
        /// <summary>The highest journal id seen (the next start's ids go on above it).</summary>
        uint64_t maxId{ 0 };
        /// <summary>The highest sequence number among the live items (new items must sort after the recovered ones).</summary>
        uint64_t maxSeq{ 0 };
        /// <summary>Every thread redirect (`lane` lines): gone thread → new thread.</summary>
        std::map<int64_t, int64_t> redirects;
        /// <summary>The last `flood` line's chat-wide hold, Unix ms (0 = none).</summary>
        int64_t floodAllUntilWallMs{ 0 };
        /// <summary>The last `flood` line's per-method holds, Unix ms.</summary>
        std::map<std::wstring, int64_t> floodMethodUntilWallMs;
        /// <summary>Records read (every op).</summary>
        size_t records{ 0 };
        /// <summary>Items Telegram took (an `ok` line).</summary>
        size_t sent{ 0 };
        /// <summary>Items dropped (a `drop` line).</summary>
        size_t dropped{ 0 };
        /// <summary>Lines skipped: a bad checksum, unreadable JSON, a put that lacks its method or parameters.</summary>
        size_t badLines{ 0 };
    };

    /// <summary>
    /// The fold: reads the journal's records in file order and keeps what is still queued (§6). A `put` makes an item (a
    /// repeated id keeps its newest put), `try` marks a call in flight, `retry` records a back-off, `ok` and `drop` end the item,
    /// `lane` redirects a thread, `flood` replaces the hold, `hdr` names the bot and chat. A record about an id the fold never
    /// saw put (its put was compacted away by a checkpoint) is ignored; an unknown op is ignored (a newer build's record).
    /// </summary>
    class OutboxFolder
    {
    public:
        /// <summary>Folds one opened record (OpenOutboxRecord's Ok).</summary>
        /// <param name="record">The record.</param>
        /// <exception cref="std::bad_alloc">If the item table cannot grow.</exception>
        void Add(const json::Value& record)
        {
            ++_r.records;
            const std::wstring op = record.StrAt(L"op");
            const uint64_t id = static_cast<uint64_t>((std::max)(int64_t{ 0 }, record.I64At(L"id", 0)));
            if (op == L"put")
            {
                const auto* params = record.Find(L"params");
                const std::wstring method = record.StrAt(L"method");
                if (id == 0 || method.empty() || !params || params->type != json::Value::Type::Obj)
                {
                    ++_r.badLines; // a put that cannot be sent is no item
                    return;
                }
                OutboxRecoveredItem item;
                item.put.id = id;
                item.put.prio = static_cast<int>(record.I64At(L"prio", 1));
                item.put.seq = static_cast<uint64_t>((std::max)(int64_t{ 0 }, record.I64At(L"seq", 0)));
                item.put.sub = static_cast<uint32_t>((std::max)(int64_t{ 0 }, record.I64At(L"sub", 0)));
                item.put.method = method;
                item.put.params = *params;
                item.put.fallbackHtml = record.StrAt(L"fb");
                item.put.mirror = record.I64At(L"mir", 0) != 0;
                item.put.queuedWallMs = record.I64At(L"at", 0);
                _r.maxId = (std::max)(_r.maxId, id);
                _items[id] = std::move(item); // a repeated id (a compaction cut short) keeps its newest put
                return;
            }
            if (op == L"hdr")
            {
                _r.header = true;
                _r.version = static_cast<int>(record.I64At(L"v", 0));
                _r.bot = record.StrAt(L"bot");
                _r.chat = record.I64At(L"chat", 0);
                return;
            }
            if (op == L"lane")
            {
                const int64_t from = record.I64At(L"from", 0);
                const int64_t to = record.I64At(L"to", 0);
                if (from > 0 && to > 0 && from != to)
                {
                    _r.redirects[from] = to;
                }
                return;
            }
            if (op == L"flood")
            {
                // Each flood line is the whole parking: the newest replaces what an older one said.
                _r.floodAllUntilWallMs = record.I64At(L"all", 0);
                _r.floodMethodUntilWallMs.clear();
                if (const auto* m = record.Find(L"m"); m && m->type == json::Value::Type::Obj)
                {
                    for (const auto& [method, until] : m->members)
                    {
                        if (until.type == json::Value::Type::Num)
                        {
                            _r.floodMethodUntilWallMs[method] = static_cast<int64_t>(until.num);
                        }
                    }
                }
                return;
            }
            _r.maxId = (std::max)(_r.maxId, id);
            const auto it = _items.find(id);
            if (it == _items.end())
            {
                return; // about an item compacted away (or a newer op): nothing to change
            }
            if (op == L"try")
            {
                it->second.tried = true;
            }
            else if (op == L"retry")
            {
                it->second.tried = false; // the call had an outcome: a transient failure
                it->second.attempts = static_cast<uint32_t>((std::max)(int64_t{ 0 }, record.I64At(L"n", 0)));
                it->second.notBeforeWallMs = record.I64At(L"nb", 0);
            }
            else if (op == L"ok")
            {
                ++_r.sent;
                _items.erase(it);
            }
            else if (op == L"drop")
            {
                ++_r.dropped;
                _items.erase(it);
            }
        }

        /// <summary>Counts a line that could not be opened (a bad checksum, unreadable JSON).</summary>
        void NoteBadLine() noexcept
        {
            ++_r.badLines;
        }

        /// <summary>
        /// Ends the fold: the live items in id order, each pointed at the newest thread the `lane` lines name, and the highest
        /// sequence among them. Call once; the folder is spent afterwards.
        /// </summary>
        /// <returns>The recovery.</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        OutboxRecovery Finish()
        {
            _r.live.reserve(_items.size());
            for (auto& [id, item] : _items)
            {
                const int64_t thread = OutboxThreadOf(item.put.params);
                if (thread > 0)
                {
                    const int64_t now = ResolveOutboxThread(_r.redirects, thread);
                    if (now != thread)
                    {
                        SetOutboxThread(item.put.params, now);
                    }
                }
                _r.maxSeq = (std::max)(_r.maxSeq, item.put.seq);
                _r.live.push_back(std::move(item));
            }
            _items.clear();
            return std::move(_r);
        }

    private:
        /// <summary>What the fold has found so far (the live items are kept in `_items` until Finish).</summary>
        OutboxRecovery _r;
        /// <summary>The items still queued, by journal id (a map: Finish lists them in the order they were queued).</summary>
        std::map<uint64_t, OutboxRecoveredItem> _items;
    };

    /// <summary>
    /// Folds a whole journal's bytes: split into lines (the torn tail left out — the caller cuts it off the file), each line
    /// opened (a bad one counted, never fatal: §6 step 2), every record folded.
    /// </summary>
    /// <param name="bytes">The journal's bytes.</param>
    /// <param name="tornBytes">Receives how many bytes follow the last '\n' (the caller truncates the file there).</param>
    /// <returns>The recovery.</returns>
    /// <exception cref="std::bad_alloc">If memory runs out.</exception>
    inline OutboxRecovery FoldOutboxJournal(std::string_view bytes, size_t& tornBytes)
    {
        const OutboxJournalText text = SplitOutboxJournal(bytes);
        tornBytes = text.tornBytes;
        OutboxFolder folder;
        for (const auto line : text.lines)
        {
            json::Value record;
            if (OpenOutboxRecord(line, record) == OutboxLineRead::Ok)
            {
                folder.Add(record);
            }
            else
            {
                folder.NoteBadLine();
            }
        }
        return folder.Finish();
    }

    // ===== the answer (§7) ==========================================================================================

    /// <summary>What Telegram's answer makes of an item.</summary>
    enum class OutboxVerdict
    {
        /// <summary>Sent — or "message is not modified" / TOPIC_NOT_MODIFIED: what the call asked for is already so.</summary>
        Ok,
        /// <summary>A 429: Telegram's flood control (the send loop's parking; the item keeps its place).</summary>
        Flood,
        /// <summary>No answer (DNS, TLS, a timeout, an abort) or a 5xx: Telegram may never have seen it — back off and send again.</summary>
        Transient,
        /// <summary>"message thread not found" / TOPIC_ID_INVALID: the topic may be gone — the thread is held and probed (§8).</summary>
        ThreadGone,
        /// <summary>Any other refusal (a 400 about the message, a 403, a 401, a synthetic refusal): final for this form.</summary>
        Refused,
    };

    /// <summary>
    /// The one answer table every outbox item goes through (MESSAGING_OUTBOX.md §7). An HTTP answer Telegram's front sends in
    /// HTML (a 502 from its proxy) arrives as httpStatus 502 with an "unreadable answer" description: a 5xx either way.
    /// </summary>
    /// <param name="r">Telegram's answer.</param>
    /// <returns>The verdict.</returns>
    inline OutboxVerdict ClassifyOutboxAnswer(const TgResult& r) noexcept
    {
        if (r.ok || r.NotModified() || r.TopicNotModified())
        {
            return OutboxVerdict::Ok;
        }
        if (r.httpStatus == 429)
        {
            return OutboxVerdict::Flood;
        }
        if (r.transportFailed || r.httpStatus >= 500)
        {
            return OutboxVerdict::Transient;
        }
        if (r.ThreadNotFound())
        {
            return OutboxVerdict::ThreadGone;
        }
        return OutboxVerdict::Refused;
    }

    // ===== an item's class ==========================================================================================

    /// <summary>What decides an item's class: the facts _enqueue reads off an OutItem.</summary>
    struct OutboxItemFacts
    {
        /// <summary>The Bot API method as the census names it ("sendPhoto" / "sendDocument" for an upload).</summary>
        std::wstring_view method;
        /// <summary>It carries a key (a newer item with the key replaces it: an edit, a card post).</summary>
        bool keyed{ false };
        /// <summary>It is a HISTORY replay item (§21.9): the replay commits its own position and sends a batch again.</summary>
        bool history{ false };
        /// <summary>It goes with the SECRETARY bot's client (§21.11).</summary>
        bool asSecretary{ false };
        /// <summary>It is sent AS THE USER (a business_connection_id): its echo guard expires in minutes, so a late copy would be
        /// read back as a new prompt — never retried, never journaled.</summary>
        bool asUser{ false };
        /// <summary>It uploads a file (a document, a photo, a card picture's multipart).</summary>
        bool upload{ false };
        /// <summary>It carries buttons (reply_markup): an interactive message whose meaning lives in this run's memory.</summary>
        bool buttons{ false };
    };

    /// <summary>
    /// Whether an item is DURABLE: journaled, recovered at every start, retried until Telegram takes or refuses it. The content
    /// of the conversation as plain posts — a prompt line, a remark, a final reply, an answer, a command's reply, a change or
    /// idle notice: sendMessage / sendRichMessage with no key, no buttons, not history, not as the user, no upload. Everything
    /// else is rebuilt by its own state machine after a restart (a card, an edit, a reaction, a replay batch) or is worth
    /// nothing late (a tap's answer, a chat action), so it stays in memory (MESSAGING_OUTBOX.md "as built").
    /// </summary>
    /// <param name="f">The item's facts.</param>
    /// <returns>True for a durable item.</returns>
    inline bool OutboxItemIsDurable(const OutboxItemFacts& f) noexcept
    {
        const bool post = f.method == L"sendMessage" || f.method == L"sendRichMessage";
        return post && !f.keyed && !f.history && !f.asSecretary && !f.asUser && !f.upload && !f.buttons;
    }

    /// <summary>How long a transient failure is retried.</summary>
    enum class OutboxRetry
    {
        /// <summary>Not at all: the item's callback hears the failure at once (today's behavior for these calls).</summary>
        Never,
        /// <summary>Up to kOutboxBoundedAttempts attempts in all, then the callback hears the failure.</summary>
        Bounded,
        /// <summary>Until Telegram answers (a durable item; a keyed item, which a newer one with its key replaces anyway).</summary>
        Forever,
    };

    /// <summary>
    /// How an item's transient failures are retried. Never for a call whose repeat could do harm or is worthless late: a topic
    /// create (a second topic — the sync plans it again on its own), a tap's answer (Telegram's deadline is seconds), a chat
    /// action, a send as the user (its echo guard expires), the secretary bot's sends and every upload (a second picture or
    /// file in the topic; the user taps again). Forever for durable and keyed items. Bounded for the rest.
    /// </summary>
    /// <param name="f">The item's facts.</param>
    /// <param name="durable">The item is durable (OutboxItemIsDurable, or its override).</param>
    /// <returns>The retry policy.</returns>
    inline OutboxRetry OutboxRetryFor(const OutboxItemFacts& f, bool durable) noexcept
    {
        if (durable)
        {
            return OutboxRetry::Forever;
        }
        if (f.method == L"createForumTopic" || f.method == L"answerCallbackQuery" || f.method == L"sendChatAction" || f.asSecretary || f.asUser || f.upload)
        {
            return OutboxRetry::Never;
        }
        return f.keyed ? OutboxRetry::Forever : OutboxRetry::Bounded;
    }

    /// <summary>
    /// Whether a method POSTS a new message into a thread: the calls whose order inside one topic the lanes keep (a reply never
    /// above its prompt). Edits, deletes, reactions and pins change messages that exist already and wait for nothing.
    /// </summary>
    /// <param name="method">The Bot API method (as the census names it: "sendPhoto" / "sendDocument" for an upload).</param>
    /// <returns>True for a post.</returns>
    inline bool OutboxMethodPostsIntoThread(std::wstring_view method) noexcept
    {
        return method == L"sendMessage" || method == L"sendRichMessage" || method == L"sendPhoto" || method == L"sendDocument" ||
               method == L"sendMediaGroup" || method == L"copyMessage" || method == L"forwardMessage";
    }

    // ===== the back-off and the circuit breaker =====================================================================

    /// <summary>
    /// How long an item waits after its `attempts`-th transient failure: 1, 2, 4, 8, 16, 32 s, then 60 s for good, each ±20 %
    /// (a deterministic jitter from `jitterSeed`, so items failed together do not all come back in the same second).
    /// </summary>
    /// <param name="attempts">The transient failures so far (1 after the first).</param>
    /// <param name="jitterSeed">Any per-item number (its sequence and attempt mixed): the jitter's source.</param>
    /// <returns>The wait in ms (0 for attempts 0).</returns>
    inline int64_t OutboxBackoffMs(uint32_t attempts, uint64_t jitterSeed) noexcept
    {
        if (attempts == 0)
        {
            return 0;
        }
        int64_t base = kOutboxBackoffFirstMs;
        for (uint32_t k = 1; k < attempts && base < kOutboxBackoffMaxMs; ++k)
        {
            base *= 2;
        }
        base = (std::min)(base, kOutboxBackoffMaxMs);
        // splitmix64's finalizer spreads any seed over 64 bits; its top 16 bits pick the factor in [0.8, 1.2).
        uint64_t z = jitterSeed + 0x9E3779B97F4A7C15ull * (attempts + 1ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        const double unit = static_cast<double>(z >> 48) / 65536.0;
        return (std::max)(int64_t{ 1 }, static_cast<int64_t>(static_cast<double>(base) * (0.8 + 0.4 * unit)));
    }

    /// <summary>
    /// The circuit breaker (§7): kOutboxBreakerThreshold transient failures in a row — over every item — mean Telegram cannot
    /// be reached: nothing is sent for a pause, then ONE item probes; a success closes it, a failure doubles the pause (up to
    /// kOutboxBreakerMaxPauseMs). GetTickCount64 ms.
    /// </summary>
    struct OutboxBreaker
    {
        /// <summary>Transient failures since the last answered call.</summary>
        uint32_t failuresInRow{ 0 };
        /// <summary>Nothing is sent before this (0 = closed).</summary>
        int64_t openUntilMs{ 0 };
        /// <summary>The current pause (0 = never opened since the last success).</summary>
        int64_t pauseMs{ 0 };
        /// <summary>When it first opened since the last success (0 = not open): the census's "unreachable since".</summary>
        int64_t openedAtMs{ 0 };
    };

    /// <summary>Records a transient failure; at the threshold (and at every failure past it: a failed probe) the breaker opens
    /// for the next pause.</summary>
    /// <param name="b">The breaker.</param>
    /// <param name="nowMs">The clock (GetTickCount64 ms).</param>
    /// <returns>True when this failure opened (or re-opened) it — the caller logs it once.</returns>
    inline bool NoteOutboxFailure(OutboxBreaker& b, int64_t nowMs) noexcept
    {
        ++b.failuresInRow;
        if (b.failuresInRow < kOutboxBreakerThreshold)
        {
            return false;
        }
        b.pauseMs = b.pauseMs == 0 ? kOutboxBreakerFirstPauseMs : (std::min)(b.pauseMs * 2, kOutboxBreakerMaxPauseMs);
        b.openUntilMs = nowMs + b.pauseMs;
        if (b.openedAtMs == 0)
        {
            b.openedAtMs = nowMs;
        }
        return true;
    }

    /// <summary>Records an ANSWERED call (any HTTP answer below 500, a refusal included: Telegram is reachable): closes the
    /// breaker and forgets its pauses.</summary>
    /// <param name="b">The breaker.</param>
    /// <returns>True when it was open (the caller logs the recovery once).</returns>
    inline bool NoteOutboxSuccess(OutboxBreaker& b) noexcept
    {
        const bool wasOpen = b.openedAtMs != 0;
        b = OutboxBreaker{};
        return wasOpen;
    }

    /// <summary>Whether the breaker holds every send now.</summary>
    /// <param name="b">The breaker.</param>
    /// <param name="nowMs">The clock (GetTickCount64 ms).</param>
    /// <returns>Its deadline when it holds (the caller waits until then), else 0.</returns>
    inline int64_t OutboxBreakerHoldsUntil(const OutboxBreaker& b, int64_t nowMs) noexcept
    {
        return b.openUntilMs > nowMs ? b.openUntilMs : 0;
    }

    // ===== the lanes ================================================================================================

    /// <summary>
    /// The per-thread ORDER of posts (§3's lanes): inside one thread a post never overtakes an earlier post that waits — in its
    /// back-off, or under a flood hold of its method. Built per pick from the queue's waiting posts; across threads (and for
    /// everything that is not waiting) the sender's (priority, seq, sub) order is unchanged.
    /// </summary>
    class OutboxLaneBlocks
    {
    public:
        /// <summary>Notes a post into `thread` that waits.</summary>
        /// <param name="thread">Its thread (0 = the main view, a lane too).</param>
        /// <param name="seq">Its sequence number.</param>
        /// <param name="sub">Its slot within the sequence.</param>
        /// <exception cref="std::bad_alloc">If the table cannot grow.</exception>
        void NoteWaiting(int64_t thread, uint64_t seq, uint32_t sub)
        {
            const auto it = _first.find(thread);
            if (it == _first.end() || std::make_pair(seq, sub) < it->second)
            {
                _first[thread] = { seq, sub };
            }
        }

        /// <summary>Whether a post at (seq, sub) into `thread` must wait behind an earlier waiting post of the same thread.</summary>
        /// <param name="thread">Its thread.</param>
        /// <param name="seq">Its sequence number.</param>
        /// <param name="sub">Its slot.</param>
        /// <returns>True when an earlier post of the thread waits.</returns>
        bool Blocks(int64_t thread, uint64_t seq, uint32_t sub) const
        {
            const auto it = _first.find(thread);
            return it != _first.end() && it->second < std::make_pair(seq, sub);
        }

        /// <summary>Whether no post waits anywhere (the pick can skip the check).</summary>
        /// <returns>True when nothing was noted.</returns>
        bool Empty() const noexcept
        {
            return _first.empty();
        }

    private:
        /// <summary>Each thread's EARLIEST waiting post: (seq, sub).</summary>
        std::map<int64_t, std::pair<uint64_t, uint32_t>> _first;
    };

    // ===== a topic that may be gone (§8) ============================================================================

    /// <summary>What one probe of a held thread (editForumTopic with the topic's own name) found.</summary>
    enum class TopicProbeAnswer
    {
        /// <summary>The topic exists: ok, TOPIC_NOT_MODIFIED, or a refusal that is not about the thread (rights, a name).</summary>
        Exists,
        /// <summary>"message thread not found" / TOPIC_ID_INVALID.</summary>
        NotFound,
        /// <summary>No verdict: no answer, a 5xx, a 429 — probed again later; the thread stays held and nothing is lost.</summary>
        Inconclusive,
    };

    /// <summary>Reads a probe's answer.</summary>
    /// <param name="r">Telegram's answer to the probe.</param>
    /// <returns>The probe's finding.</returns>
    inline TopicProbeAnswer ClassifyTopicProbe(const TgResult& r) noexcept
    {
        if (r.ok || r.TopicNotModified())
        {
            return TopicProbeAnswer::Exists;
        }
        if (r.httpStatus == 429 || r.transportFailed || r.httpStatus >= 500)
        {
            return TopicProbeAnswer::Inconclusive;
        }
        if (r.ThreadNotFound())
        {
            return TopicProbeAnswer::NotFound;
        }
        return TopicProbeAnswer::Exists; // a refusal about something else proves the thread is there
    }

    /// <summary>Where a held thread stands.</summary>
    enum class HeldThreadPhase
    {
        /// <summary>Its posts wait while probes find out whether the topic exists.</summary>
        Probing,
        /// <summary>Confirmed gone: its posts wait for the session's topic to be made again (or are given up after kTopicGoneWaitMs).</summary>
        Gone,
        /// <summary>A probe found it: its posts go again; remembered for kTopicReleaseMemoryMs to count releases.</summary>
        Released,
    };

    /// <summary>One thread whose posts are held because Telegram said "message thread not found" (§8). Wall-clock ms.</summary>
    struct HeldThread
    {
        /// <summary>The thread.</summary>
        int64_t threadId{ 0 };
        /// <summary>The session whose topic it was when it was held ("" = no session topic: the Search topic or another thread).</summary>
        std::wstring sessionId;
        /// <summary>It was the 🔎 Search topic's thread.</summary>
        bool searchTopic{ false };
        /// <summary>Where it stands.</summary>
        HeldThreadPhase phase{ HeldThreadPhase::Probing };
        /// <summary>When the current hold began.</summary>
        int64_t heldSinceMs{ 0 };
        /// <summary>When the next probe is due (Probing only).</summary>
        int64_t nextProbeMs{ 0 };
        /// <summary>"Not found" probes in a row.</summary>
        int notFound{ 0 };
        /// <summary>Probes in a row with no verdict (their wait doubles).</summary>
        int inconclusive{ 0 };
        /// <summary>How many times a probe released it within kTopicReleaseMemoryMs.</summary>
        int releases{ 0 };
        /// <summary>When a probe last released it.</summary>
        int64_t releasedAtMs{ 0 };
        /// <summary>When it was confirmed gone.</summary>
        int64_t goneSinceMs{ 0 };
        /// <summary>The service has acted on "gone" once (forgot the topic's record, dropped what its replay re-sends): the later
        /// ticks only wait for the new thread.</summary>
        bool goneSettled{ false };
        /// <summary>The gone topic's own verbose mode (§21.11), carried to the topic made again.</summary>
        int verbose{ -1 };
    };

    /// <summary>
    /// Holds a thread after a "message thread not found": a thread not held yet (or released long ago) starts probing; a thread
    /// a probe released kTopicProbeReleasesMax times within kTopicReleaseMemoryMs counts as gone at once (Telegram says it exists
    /// but keeps refusing posts into it). A thread already Probing or Gone is left as it is.
    /// </summary>
    /// <param name="h">The thread's entry (a new one: default-constructed with its threadId set).</param>
    /// <param name="nowMs">The wall clock, ms.</param>
    /// <returns>The phase it is in now.</returns>
    inline HeldThreadPhase HoldThread(HeldThread& h, int64_t nowMs) noexcept
    {
        if (h.heldSinceMs != 0 && h.phase != HeldThreadPhase::Released)
        {
            return h.phase; // held already: the refusal changes nothing
        }
        if (h.phase == HeldThreadPhase::Released && nowMs - h.releasedAtMs > kTopicReleaseMemoryMs)
        {
            h.releases = 0; // released long ago: a fresh count
        }
        h.heldSinceMs = nowMs;
        h.notFound = 0;
        h.inconclusive = 0;
        if (h.releases >= kTopicProbeReleasesMax)
        {
            h.phase = HeldThreadPhase::Gone;
            h.goneSinceMs = nowMs;
            return h.phase;
        }
        h.phase = HeldThreadPhase::Probing;
        h.nextProbeMs = nowMs + kTopicProbeFirstMs;
        return h.phase;
    }

    /// <summary>What a probe's finding does to a held thread.</summary>
    enum class TopicProbeStep
    {
        /// <summary>Nothing yet: the next probe is scheduled (h.nextProbeMs).</summary>
        Wait,
        /// <summary>The topic exists: its posts go again.</summary>
        Release,
        /// <summary>Two "not found" in a row: the topic is gone — made again by the topic sync (§8.3).</summary>
        Gone,
    };

    /// <summary>Applies one probe's finding to a Probing thread (a thread in another phase is not changed: Wait).</summary>
    /// <param name="h">The thread's entry.</param>
    /// <param name="a">The probe's finding.</param>
    /// <param name="nowMs">The wall clock, ms.</param>
    /// <returns>What to do.</returns>
    inline TopicProbeStep NoteTopicProbe(HeldThread& h, TopicProbeAnswer a, int64_t nowMs) noexcept
    {
        if (h.phase != HeldThreadPhase::Probing)
        {
            return TopicProbeStep::Wait;
        }
        switch (a)
        {
        case TopicProbeAnswer::Exists:
            h.phase = HeldThreadPhase::Released;
            h.releasedAtMs = nowMs;
            ++h.releases;
            h.notFound = 0;
            h.inconclusive = 0;
            return TopicProbeStep::Release;
        case TopicProbeAnswer::NotFound:
            h.inconclusive = 0;
            if (++h.notFound >= 2)
            {
                h.phase = HeldThreadPhase::Gone;
                h.goneSinceMs = nowMs;
                return TopicProbeStep::Gone;
            }
            h.nextProbeMs = nowMs + kTopicProbeSecondMs;
            return TopicProbeStep::Wait;
        case TopicProbeAnswer::Inconclusive:
        default:
        {
            ++h.inconclusive;
            int64_t wait = kTopicProbeSecondMs;
            for (int k = 1; k < h.inconclusive && wait < kTopicProbeRetryMaxMs; ++k)
            {
                wait *= 2;
            }
            h.nextProbeMs = nowMs + (std::min)(wait, kTopicProbeRetryMaxMs);
            return TopicProbeStep::Wait;
        }
        }
    }

    /// <summary>Whether a held thread's posts wait (Probing or Gone; a Released thread's go).</summary>
    /// <param name="h">The thread's entry.</param>
    /// <returns>True while its posts are held.</returns>
    inline bool HeldThreadHoldsPosts(const HeldThread& h) noexcept
    {
        return h.phase == HeldThreadPhase::Probing || h.phase == HeldThreadPhase::Gone;
    }

    // ===== the fallbacks of a recovered item ========================================================================

    /// <summary>
    /// The PLAIN-TEXT form of an HTML message: every tag removed, the entities left as they are (plain mode shows "&amp;lt;"
    /// as typed — a readable degradation). The same rule as the in-run fallback of a message Telegram refused as HTML ("can't
    /// parse entities", MessagingService::_htmlItems); a RECOVERED item has no callback, so the sender applies this itself.
    /// </summary>
    /// <param name="html">The HTML text.</param>
    /// <returns>The text without its tags.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring OutboxPlainTextOfHtml(std::wstring_view html)
    {
        std::wstring stripped;
        stripped.reserve(html.size());
        bool inTag = false;
        for (const wchar_t c : html)
        {
            if (c == L'<')
            {
                inTag = true;
            }
            else if (c == L'>' && inTag)
            {
                inTag = false;
            }
            else if (!inTag)
            {
                stripped += c;
            }
        }
        return stripped;
    }
}
