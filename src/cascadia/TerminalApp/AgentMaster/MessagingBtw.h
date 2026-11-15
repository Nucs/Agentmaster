// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging, /btw (MESSAGING.md §21.56): a SIDE QUESTION about a session, asked from its Telegram topic and
// answered there, while the session keeps working. The pure half: what a message is (ParseBtwMessage), and every text the
// bot sends for it. The asking itself is SideQuestion.h's (the session's own claude through the link, else a headless fork).
//
// The user's request (2026-11-15): "we should be able to do /btw in Telegram with a question that follows. Also, it's
// enough to send a message with a /btw or just 'by the way' at the beginning just to get there. ... it should begin as it
// should be quoting the /btw question that I sent. The user can also /clear btw, /btw clear ... clearing the /btw
// conversation." Their picks the same day: the answer is a reply to the question (Telegram quotes it) led by the header
// "☁️ /btw · 14:02 · #2", the session's title row under it (§21.54), then the answer.
//
// Header-only and pure: the m5 harness tests it (tests_side_question.cpp).

#pragma once

#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>

#include "TelegramFormat.h" // TgEscapeHtml, TgMdEscapeText, TgClockText, TgShortSpan

namespace Agentmaster::Msg
{
    /// <summary>What a message in a session's topic asks of /btw.</summary>
    enum class BtwVerb
    {
        None, // not a /btw message: the topic's usual handling applies
        Ask, // a side question (`question` holds it)
        Clear, // start the btw thread over (/btw clear, /clear btw, /btw_clear, /clear_btw, "btw clear")
        Usage, // a bare /btw (or "btw" alone): what it does, and how long the thread is
    };

    /// <summary>A message read as /btw (ParseBtwMessage).</summary>
    struct BtwMessage
    {
        BtwVerb verb{ BtwVerb::None }; // what it asks
        std::wstring question; // Ask: the question, the trigger words removed
        bool command{ false }; // it was typed as a /command (not as "btw …" / "by the way …" words)
    };

    /// <summary>The command word of /btw (in kAgentmasterCommands: a topic never forwards it to Claude Code).</summary>
    inline constexpr std::wstring_view kBtwCommand = L"btw";
    /// <summary>The one-word clear commands: the form Telegram's "/" menu and a tap on a printed command can send (a tap on
    /// "/btw clear" would send a bare "/btw"). Both are in kAgentmasterCommands.</summary>
    inline constexpr std::wstring_view kBtwClearCommand = L"btw_clear";
    inline constexpr std::wstring_view kClearBtwCommand = L"clear_btw";

    /// <summary>The reaction on a question while it is being answered (Telegram's reaction set: "thinking").</summary>
    inline constexpr std::wstring_view kBtwThinkingReaction = L"\U0001F914";

    namespace detail
    {
        /// <summary>Lower case of ASCII letters (the trigger words are ASCII; any other character is kept).</summary>
        /// <param name="s">The text.</param>
        /// <returns>Its lower case.</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        inline std::wstring BtwLower(std::wstring_view s)
        {
            std::wstring out{ s };
            for (auto& c : out)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return out;
        }

        /// <summary>Whether a character may separate the trigger words from the question ("btw, …", "btw: …", "btw - …").</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for white space and , : ; . ! ? - – — and the ellipsis.</returns>
        inline bool BtwSeparator(wchar_t c) noexcept
        {
            return std::iswspace(c) || c == L',' || c == L':' || c == L';' || c == L'.' || c == L'!' || c == L'?' || c == L'-' || c == L'\x2013' ||
                   c == L'\x2014' || c == L'\x2026';
        }

        /// <summary>The text with leading separators and trailing white space removed (the question after the trigger).</summary>
        /// <param name="s">The text after the trigger words.</param>
        /// <returns>The question.</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        inline std::wstring BtwRest(std::wstring_view s)
        {
            while (!s.empty() && BtwSeparator(s.front()))
            {
                s.remove_prefix(1);
            }
            while (!s.empty() && std::iswspace(s.back()))
            {
                s.remove_suffix(1);
            }
            return std::wstring{ s };
        }

        /// <summary>Whether the words after the trigger mean "start over": "clear" or "reset", any case, a closing '.' or '!'
        /// allowed — the whole rest, so "clear up why X failed" stays a question.</summary>
        /// <param name="rest">The words after the trigger (BtwRest).</param>
        /// <returns>True for the clear words.</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        inline bool BtwRestIsClear(std::wstring_view rest)
        {
            std::wstring w = BtwLower(rest);
            while (!w.empty() && (w.back() == L'.' || w.back() == L'!'))
            {
                w.pop_back();
            }
            return w == L"clear" || w == L"reset";
        }

        /// <summary>
        /// Reads `text` as starting with the words `trigger` (ASCII, lower case, single spaces matching any run of white
        /// space), any case, followed by the end or a separator — so "btwhat" or "by the wayside" never match.
        /// </summary>
        /// <param name="text">The message, trimmed at its start.</param>
        /// <param name="trigger">The trigger words ("btw", "by the way").</param>
        /// <param name="after">Receives the text after the trigger when it matches.</param>
        /// <returns>True when the message starts with the trigger.</returns>
        inline bool BtwStartsWith(std::wstring_view text, std::wstring_view trigger, std::wstring_view& after) noexcept
        {
            size_t i = 0;
            for (size_t k = 0; k < trigger.size(); ++k)
            {
                if (trigger[k] == L' ')
                {
                    if (i >= text.size() || !std::iswspace(text[i]))
                    {
                        return false;
                    }
                    while (i < text.size() && std::iswspace(text[i]))
                    {
                        ++i;
                    }
                    continue;
                }
                if (i >= text.size())
                {
                    return false;
                }
                wchar_t c = text[i];
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
                if (c != trigger[k])
                {
                    return false;
                }
                ++i;
            }
            if (i < text.size() && !BtwSeparator(text[i]))
            {
                return false;
            }
            after = text.substr(i);
            return true;
        }
    }

    /// <summary>
    /// Reads a message typed in a session's topic as /btw, BEFORE any other reading — "/clear btw" must never reach Claude
    /// Code, where /clear would clear the session itself. The forms:
    ///   /btw &lt;question&gt;                       Ask (any case, a "@bot" suffix dropped, the question may span lines)
    ///   /btw                                  Usage
    ///   /btw clear · /btw reset              Clear (the whole rest is the word; "/btw clear up X" is a question)
    ///   /btw_clear · /clear_btw              Clear (the forms a "/" menu or a tapped command can send)
    ///   /clear btw                            Clear ("/clear" with anything else stays Claude Code's /clear)
    ///   btw &lt;question&gt; · by the way &lt;question&gt;   Ask — words, when `allowWords`; the same Usage and Clear forms
    /// The trigger words must be followed by the end or a separator ("btw, …", "By the way: …"), so a message that merely
    /// starts with "btwhat" is not one.
    /// </summary>
    /// <param name="text">The message text.</param>
    /// <param name="allowWords">Read "btw …" / "by the way …" too (false for a message a new thread held: written before its
    /// session existed, it was meant for the session, not as a side question about it).</param>
    /// <returns>The reading (None for any other message).</returns>
    /// <exception cref="std::bad_alloc">If the reading cannot be allocated.</exception>
    inline BtwMessage ParseBtwMessage(std::wstring_view text, bool allowWords)
    {
        BtwMessage m;
        while (!text.empty() && std::iswspace(text.front()))
        {
            text.remove_prefix(1);
        }
        if (!text.empty() && text.front() == L'/')
        {
            // The command word: up to white space, a "@bot" suffix dropped, lower case (ParseRemoteCommand's reading).
            size_t end = 1;
            while (end < text.size() && !std::iswspace(text[end]))
            {
                ++end;
            }
            std::wstring word = detail::BtwLower(text.substr(1, end - 1));
            if (const size_t at = word.find(L'@'); at != std::wstring::npos)
            {
                word.resize(at);
            }
            const std::wstring rest = detail::BtwRest(text.substr(end));
            m.command = true;
            if (word == kBtwCommand)
            {
                m.verb = rest.empty() ? BtwVerb::Usage : (detail::BtwRestIsClear(rest) ? BtwVerb::Clear : BtwVerb::Ask);
                if (m.verb == BtwVerb::Ask)
                {
                    m.question = rest;
                }
                return m;
            }
            if (word == kBtwClearCommand || word == kClearBtwCommand)
            {
                m.verb = BtwVerb::Clear;
                return m;
            }
            if (word == L"clear" && detail::BtwLower(rest) == L"btw")
            {
                m.verb = BtwVerb::Clear;
                return m;
            }
            m.command = false;
            return m; // any other command: not ours
        }
        if (!allowWords)
        {
            return m;
        }
        std::wstring_view after;
        if (detail::BtwStartsWith(text, L"btw", after) || detail::BtwStartsWith(text, L"by the way", after))
        {
            const std::wstring rest = detail::BtwRest(after);
            m.verb = rest.empty() ? BtwVerb::Usage : (detail::BtwRestIsClear(rest) ? BtwVerb::Clear : BtwVerb::Ask);
            if (m.verb == BtwVerb::Ask)
            {
                m.question = rest;
            }
        }
        return m;
    }

    /// <summary>
    /// "/btw" as the bot WRITES it: a word joiner (U+2060) after the slash, so Telegram draws it as text and never as a
    /// tappable command (a tap would send a bare /btw, which only answers its usage).
    /// </summary>
    inline constexpr std::wstring_view kBtwWritten = L"/\x2060" L"btw";

    /// <summary>
    /// The answer's header line, as Markdown for the rich message: "**☁️ /btw** · 14:02", with "· #3" once the btw thread
    /// holds earlier exchanges (the user's pick, 2026-11-15). The session's title row follows on line 2 (the sender's
    /// MarkdownTitleRowAfterHeader).
    /// </summary>
    /// <param name="answeredAtMs">When the answer came (Unix ms; 0 = no clock).</param>
    /// <param name="number">The exchange's number in the btw thread (1 = the first).</param>
    /// <returns>The header line.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BtwHeaderMarkdown(int64_t answeredAtMs, size_t number)
    {
        std::wstring out = L"**\x2601\xFE0F " + std::wstring{ kBtwWritten } + L"**";
        if (answeredAtMs > 0)
        {
            // The answer's own time as "now": the clock is "HH:MM" (the message is sent as it is answered, never re-rendered).
            out += L" \x00B7 " + TgClockText(answeredAtMs, answeredAtMs);
        }
        if (number > 1)
        {
            out += L" \x00B7 #" + std::to_wstring(number);
        }
        return out;
    }

    /// <summary>The same header as Telegram HTML (the fallback when the rich message is refused).</summary>
    /// <param name="answeredAtMs">When the answer came (Unix ms; 0 = no clock).</param>
    /// <param name="number">The exchange's number in the btw thread.</param>
    /// <returns>The header line.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BtwHeaderHtml(int64_t answeredAtMs, size_t number)
    {
        std::wstring out = L"<b>\x2601\xFE0F " + std::wstring{ kBtwWritten } + L"</b>";
        if (answeredAtMs > 0)
        {
            out += L" \x00B7 " + TgEscapeHtml(TgClockText(answeredAtMs, answeredAtMs));
        }
        if (number > 1)
        {
            out += L" \x00B7 #" + std::to_wstring(number);
        }
        return out;
    }

    /// <summary>
    /// A token count the way the reply's footer writes it: "899", "33.0k", "548.1k", "1.2M".
    /// </summary>
    /// <param name="n">The count.</param>
    /// <returns>The words.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BtwTokenCount(int64_t n)
    {
        if (n < 1000)
        {
            return std::to_wstring(n < 0 ? 0 : n);
        }
        const bool mega = n >= 1000000;
        const int64_t tenths = mega ? (n + 50000) / 100000 : (n + 50) / 100;
        return std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + (mega ? L"M" : L"k");
    }

    /// <summary>
    /// The answer's last line, small and in italics: how it was answered and what it cost the cache — the user's "confirm it
    /// forks cache hit so we don't waste tokens" made visible on every answer:
    ///   "in the session · 548.1k tokens from the cache, 899 new · 8 s"
    ///   "a headless fork (the session's claude has run no turn since it started) · 6 s"
    /// </summary>
    /// <param name="viaLink">Answered inside the session (the link) rather than by a headless fork.</param>
    /// <param name="usageKnown">The token counts were reported.</param>
    /// <param name="cacheRead">Input tokens the cache served.</param>
    /// <param name="cacheWrite">Input tokens written to the cache.</param>
    /// <param name="input">Uncached input tokens.</param>
    /// <param name="tookMs">How long the answer took.</param>
    /// <param name="why">Why the session's own claude did not answer ("" = it did), shown in brackets after "a headless fork".</param>
    /// <returns>The line as Markdown (escaped), without italics markers — the caller wraps it.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BtwFooterText(bool viaLink, bool usageKnown, int64_t cacheRead, int64_t cacheWrite, int64_t input, int64_t tookMs, std::wstring_view why)
    {
        std::wstring out = viaLink ? L"in the session" : L"a headless fork";
        if (!viaLink && !why.empty())
        {
            out += L" (" + std::wstring{ why } + L")";
        }
        if (usageKnown)
        {
            out += L" \x00B7 " + BtwTokenCount(cacheRead) + L" tokens from the cache, " + BtwTokenCount(cacheWrite + input) + L" new";
        }
        if (tookMs > 0)
        {
            out += L" \x00B7 " + std::to_wstring((tookMs + 500) / 1000) + L" s";
        }
        return out;
    }

    /// <summary>
    /// The usage reply (a bare /btw): what /btw does, its other forms, and how many exchanges this topic's btw thread holds.
    /// </summary>
    /// <param name="exchanges">The thread's exchanges now.</param>
    /// <returns>Telegram HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BtwUsageHtml(size_t exchanges)
    {
        std::wstring out = L"\x2601\xFE0F <b>/btw &lt;question&gt;</b> asks a side question about this session, the way Claude Code's own /btw does: "
                           L"Claude answers from what the conversation already holds \x2014 no tools, nothing added to the session \x2014 while the "
                           L"session keeps working. A message that starts with \x201C" L"btw\x201D or \x201C" L"by the way\x201D asks one too.\n"
                           L"/btw_clear (or <code>/btw clear</code>, <code>/clear btw</code>) starts the btw thread over.\n";
        out += exchanges == 0 ? std::wstring{ L"This topic's btw thread is empty." } :
                                L"This topic's btw thread holds " + std::to_wstring(exchanges) + (exchanges == 1 ? L" exchange" : L" exchanges") +
                                    L": each new /btw sees them.";
        return out;
    }

    /// <summary>The reply to a clear: how many exchanges went (-1 = the thread could not be cleared).</summary>
    /// <param name="forgotten">ClearSideQuestionThread's answer.</param>
    /// <returns>Telegram HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BtwClearedHtml(int forgotten)
    {
        if (forgotten < 0)
        {
            return L"\x26A0\xFE0F The btw thread could not be cleared (hooks.log says why).";
        }
        if (forgotten == 0)
        {
            return L"\x2601\xFE0F The btw thread is already empty.";
        }
        return L"\x2601\xFE0F The btw thread is cleared (" + std::to_wstring(forgotten) + (forgotten == 1 ? L" exchange" : L" exchanges") +
               L" forgotten). The next /btw starts fresh.";
    }

    /// <summary>
    /// The question asked before a side question that would pay for the whole conversation again: the session's prompt
    /// cache most likely expired (its last request is older than the cache's hour).
    /// </summary>
    /// <param name="idleMs">How long ago the session's last request was.</param>
    /// <param name="contextTokens">The conversation's size (the newest response's tokens; 0 = unknown).</param>
    /// <returns>Telegram HTML (the buttons are the caller's).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BtwColdCacheQuestionHtml(int64_t idleMs, int64_t contextTokens)
    {
        std::wstring out = L"\x2601\xFE0F This session's prompt cache has most likely expired: its last request was " + TgEscapeHtml(TgShortSpan(idleMs)) +
                           L" ago, and Claude Code keeps it an hour. This /btw would pay for the whole conversation again";
        if (contextTokens > 0)
        {
            out += L" (~" + BtwTokenCount(contextTokens) + L" tokens)";
        }
        out += L". Ask anyway?";
        return out;
    }

    /// <summary>The reply when no answer came.</summary>
    /// <param name="error">Why (SideQuestionAnswer::error).</param>
    /// <returns>Telegram HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BtwFailedHtml(std::wstring_view error)
    {
        return L"\x26A0\xFE0F " + std::wstring{ kBtwWritten } + L" got no answer: " + TgEscapeHtml(error.empty() ? std::wstring_view{ L"no reason given" } : error) + L".";
    }

    /// <summary>The reply for a Codex session (Claude Code's side question is Claude's alone).</summary>
    inline constexpr std::wstring_view kBtwNotClaudeHtml = L"\x2601\xFE0F /btw asks Claude Code about its conversation; this session runs Codex.";

    /// <summary>The reply when the session has no conversation yet (nothing to ask about).</summary>
    inline constexpr std::wstring_view kBtwNoConversationHtml = L"\x2601\xFE0F There is nothing to ask about yet: this session has no conversation. Send it a message first.";
}
