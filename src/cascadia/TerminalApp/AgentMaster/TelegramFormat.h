// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TelegramFormat: the PURE text half of the Telegram connector (MESSAGING.md §10.3).
//
// Everything a topic shows is text that Telegram renders in its HTML parse mode, with hard limits
// Telegram enforces (4,096 characters per message, balanced tags, only `<`, `>` and `&` escaped).
// This header turns Agentmaster's texts — Claude's Markdown replies, tool-call lines, raw screen
// text — into that HTML and splits it into messages that fit. It is header-only and pure (no I/O,
// no Win32, no WinRT), so the m5 harness pins every rule (tests_messaging.cpp) and the service
// (Messaging.cpp) is the only other includer.
//
// Why a converter of our own instead of Telegram's MarkdownV2 mode: MarkdownV2 requires escaping
// 18 punctuation characters everywhere outside code, and one missed escape makes Telegram REFUSE
// the whole message (a 400 with the offset). HTML mode fails only on unbalanced tags and bare
// `<`, which this converter never produces: it escapes first and adds tags from its own structure.
// The conversion is deliberately conservative — a Markdown feature it does not recognize comes
// through as plain (escaped) text, never as a broken tag.

#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster::Msg
{
    // Telegram's limit for a message's text (characters). A longer text must be split (TgChunkHtml).
    inline constexpr size_t kTelegramMessageMaxChars = 4096;

    // Telegram's limit for a caption and for a `copy_text` button's text.
    inline constexpr size_t kTelegramCopyTextMaxChars = 256;

    // Telegram's six topic icon colors (createForumTopic `icon_color`), as 0xRRGGBB. Only these values
    // are accepted; a topic keeps its color for life (editForumTopic cannot change it).
    inline constexpr uint32_t kTelegramTopicColors[] = { 0x6FB9F0, 0xFFD67E, 0xCB86DB, 0x8EEE98, 0xFF93B2, 0xFB6F5F };

    /// <summary>
    /// Escapes text for Telegram's HTML parse mode. Call it on EVERY piece of text that is not a tag
    /// this header itself emits: an unescaped `<` in a reply ("a &lt;T&gt; generic") is a 400 from
    /// Telegram, and an unescaped `&` is silently mis-rendered.
    /// </summary>
    /// <param name="s">The raw text.</param>
    /// <returns>The text with `&`, `<` and `>` replaced by their entities; nothing else changed.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring TgEscapeHtml(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 16);
        for (const wchar_t c : s)
        {
            switch (c)
            {
            case L'&':
                out += L"&amp;";
                break;
            case L'<':
                out += L"&lt;";
                break;
            case L'>':
                out += L"&gt;";
                break;
            default:
                out += c;
                break;
            }
        }
        return out;
    }

    /// <summary>
    /// Escapes text for an HTML attribute value this header emits (`href="…"`): the three entities of
    /// TgEscapeHtml plus the double quote, which would otherwise end the attribute early.
    /// </summary>
    /// <param name="s">The raw attribute text.</param>
    /// <returns>The escaped text.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring TgEscapeAttr(std::wstring_view s)
    {
        std::wstring out = TgEscapeHtml(s);
        std::wstring quoted;
        quoted.reserve(out.size() + 8);
        for (const wchar_t c : out)
        {
            if (c == L'"')
            {
                quoted += L"&quot;";
            }
            else
            {
                quoted += c;
            }
        }
        return quoted;
    }

    /// <summary>
    /// The first line of a text, trimmed and capped — what a work-log line or a queue preview shows.
    /// </summary>
    /// <param name="text">The whole text (any line breaks).</param>
    /// <param name="maxChars">The cap; a longer first line is cut here and ends with "…".</param>
    /// <returns>The first non-empty line, or "" for a blank text. Never longer than maxChars.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring TgFirstLine(std::wstring_view text, size_t maxChars)
    {
        size_t start = 0;
        // Skip leading blank lines: the first NON-EMPTY line is the one that says what the text is.
        while (start < text.size())
        {
            const size_t end = text.find_first_of(L"\r\n", start);
            const std::wstring_view line = text.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
            const size_t a = line.find_first_not_of(L" \t");
            if (a != std::wstring_view::npos)
            {
                const size_t b = line.find_last_not_of(L" \t");
                std::wstring_view trimmed = line.substr(a, b - a + 1);
                if (maxChars > 0 && trimmed.size() > maxChars)
                {
                    // Cut one short of the cap so the ellipsis keeps the result within it.
                    return std::wstring{ trimmed.substr(0, maxChars > 1 ? maxChars - 1 : 0) } + L"\x2026";
                }
                return std::wstring{ trimmed };
            }
            if (end == std::wstring_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        return {};
    }

    namespace detail
    {
        // The inline Markdown of one text run (no code spans inside): escaped, then `**bold**`,
        // `[text](url)` links and `~~strike~~`. Italic with `*` or `_` is deliberately NOT converted:
        // `_` is in every identifier (snake_case) and a lone `*` in prose is more often a bullet or a
        // glob than emphasis, and a wrong pair would eat the characters. Paired `**` is unambiguous.
        //   raw — the run's raw text (never contains a backtick span; the caller split those out).
        // Returns HTML with balanced <b>, <s> and <a> tags. Throws std::bad_alloc only.
        inline std::wstring InlineRun(std::wstring_view raw)
        {
            std::wstring out;
            out.reserve(raw.size() + 16);
            size_t i = 0;
            bool bold = false;
            bool strike = false;
            while (i < raw.size())
            {
                // Bold: `**` toggles, but only when the pair closes on this run (an unpaired `**` stays text).
                if (raw.compare(i, 2, L"**") == 0)
                {
                    if (!bold)
                    {
                        const size_t close = raw.find(L"**", i + 2);
                        if (close != std::wstring_view::npos && close > i + 2)
                        {
                            out += L"<b>";
                            bold = true;
                            i += 2;
                            continue;
                        }
                    }
                    else
                    {
                        out += L"</b>";
                        bold = false;
                        i += 2;
                        continue;
                    }
                }
                if (raw.compare(i, 2, L"~~") == 0)
                {
                    if (!strike)
                    {
                        const size_t close = raw.find(L"~~", i + 2);
                        if (close != std::wstring_view::npos && close > i + 2)
                        {
                            out += L"<s>";
                            strike = true;
                            i += 2;
                            continue;
                        }
                    }
                    else
                    {
                        out += L"</s>";
                        strike = false;
                        i += 2;
                        continue;
                    }
                }
                // A link: `[text](url)` with no line break inside and a scheme-looking url.
                if (raw[i] == L'[')
                {
                    const size_t closeText = raw.find(L']', i + 1);
                    if (closeText != std::wstring_view::npos && closeText + 1 < raw.size() && raw[closeText + 1] == L'(')
                    {
                        const size_t closeUrl = raw.find(L')', closeText + 2);
                        if (closeUrl != std::wstring_view::npos)
                        {
                            const std::wstring_view text = raw.substr(i + 1, closeText - i - 1);
                            const std::wstring_view url = raw.substr(closeText + 2, closeUrl - closeText - 2);
                            const bool isUrl = (url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0 || url.rfind(L"tg://", 0) == 0) &&
                                               url.find_first_of(L" \t\r\n") == std::wstring_view::npos;
                            if (isUrl && !text.empty() && text.find_first_of(L"\r\n") == std::wstring_view::npos)
                            {
                                out += L"<a href=\"" + TgEscapeAttr(url) + L"\">" + TgEscapeHtml(text) + L"</a>";
                                i = closeUrl + 1;
                                continue;
                            }
                        }
                    }
                }
                // Plain character, escaped.
                switch (raw[i])
                {
                case L'&':
                    out += L"&amp;";
                    break;
                case L'<':
                    out += L"&lt;";
                    break;
                case L'>':
                    out += L"&gt;";
                    break;
                default:
                    out += raw[i];
                    break;
                }
                ++i;
            }
            // Balance: a run can end while a tag is open only when the opener was accepted without its
            // closer having been consumed (a closer inside a later code span); close it so Telegram
            // never sees an unbalanced tag.
            if (bold)
            {
                out += L"</b>";
            }
            if (strike)
            {
                out += L"</s>";
            }
            return out;
        }

        // One Markdown LINE outside a fenced block: backtick code spans become <code>, the runs between
        // them go through InlineRun. A backtick without a partner stays a literal backtick.
        //   line — the raw line.
        // Returns the HTML. Throws std::bad_alloc only.
        inline std::wstring InlineLine(std::wstring_view line)
        {
            std::wstring out;
            size_t i = 0;
            while (i < line.size())
            {
                const size_t tick = line.find(L'`', i);
                if (tick == std::wstring_view::npos)
                {
                    out += InlineRun(line.substr(i));
                    break;
                }
                // A span may be delimited by one or more backticks (`` a`b `` style); match the same count.
                size_t ticks = 0;
                while (tick + ticks < line.size() && line[tick + ticks] == L'`')
                {
                    ++ticks;
                }
                const std::wstring_view delim = line.substr(tick, ticks);
                const size_t close = line.find(delim, tick + ticks);
                if (close == std::wstring_view::npos)
                {
                    // No partner: the backticks are text.
                    out += InlineRun(line.substr(i, tick + ticks - i));
                    i = tick + ticks;
                    continue;
                }
                out += InlineRun(line.substr(i, tick - i));
                std::wstring_view code = line.substr(tick + ticks, close - tick - ticks);
                // CommonMark strips one leading and one trailing space when both are present.
                if (code.size() >= 2 && code.front() == L' ' && code.back() == L' ')
                {
                    code = code.substr(1, code.size() - 2);
                }
                out += L"<code>" + TgEscapeHtml(code) + L"</code>";
                i = close + ticks;
            }
            return out;
        }

        // Is this line a fence opener/closer (``` or ~~~, at most three leading spaces)? Returns the
        // fence characters' count (0 = not a fence) and the info string after it (the language).
        inline size_t FenceOf(std::wstring_view line, wchar_t& ch, std::wstring_view& info)
        {
            size_t i = 0;
            while (i < line.size() && i < 3 && line[i] == L' ')
            {
                ++i;
            }
            if (i >= line.size() || (line[i] != L'`' && line[i] != L'~'))
            {
                return 0;
            }
            ch = line[i];
            size_t n = 0;
            while (i + n < line.size() && line[i + n] == ch)
            {
                ++n;
            }
            if (n < 3)
            {
                return 0;
            }
            info = line.substr(i + n);
            const size_t a = info.find_first_not_of(L" \t");
            info = a == std::wstring_view::npos ? std::wstring_view{} : info.substr(a);
            const size_t b = info.find_first_of(L" \t`");
            if (b != std::wstring_view::npos)
            {
                info = info.substr(0, b);
            }
            return n;
        }
    }

    /// <summary>
    /// Converts Claude's Markdown to Telegram HTML. Fenced code blocks become `<pre><code>` (the
    /// language as `class="language-x"` when given — Telegram shows it), inline code `<code>`,
    /// `**bold**` `<b>`, `~~strike~~` `<s>`, `[text](url)` `<a>`, `# headings` bold lines, `> quotes`
    /// `<blockquote>`, and list bullets `-`/`*` the bullet "•". Everything else is escaped text.
    /// </summary>
    /// <param name="md">The Markdown text (any line breaks).</param>
    /// <returns>HTML whose tags are balanced, ready for parse_mode=HTML. Line breaks are kept as "\n".</returns>
    /// <remarks>
    /// Italic and tables are NOT converted (see detail::InlineRun). An unterminated fence runs to the
    /// end of the text and is closed there, so the result is always balanced. The result can exceed
    /// Telegram's size limit: pass it through TgChunkHtml before sending.
    /// </remarks>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring TgMarkdownToHtml(std::wstring_view md)
    {
        std::wstring out;
        out.reserve(md.size() + md.size() / 8 + 64);
        bool inFence = false;
        wchar_t fenceChar = L'`';
        size_t fenceLen = 0;
        bool inQuote = false;
        // Closes an open <blockquote>: the quoted lines each ended with "\n", and a line break INSIDE the
        // tag would render as an empty last line of the quote, so it moves outside.
        const auto closeQuote = [&]() {
            if (!out.empty() && out.back() == L'\n')
            {
                out.pop_back();
            }
            out += L"</blockquote>\n";
            inQuote = false;
        };
        size_t pos = 0;
        while (pos <= md.size())
        {
            const size_t nl = md.find(L'\n', pos);
            std::wstring_view line = md.substr(pos, nl == std::wstring_view::npos ? std::wstring_view::npos : nl - pos);
            if (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            const bool last = (nl == std::wstring_view::npos);

            wchar_t ch = L'`';
            std::wstring_view info;
            const size_t fence = detail::FenceOf(line, ch, info);
            if (inFence)
            {
                if (fence >= fenceLen && ch == fenceChar && info.empty())
                {
                    out += L"</code></pre>";
                    inFence = false;
                }
                else
                {
                    out += TgEscapeHtml(line);
                    out += L'\n';
                }
            }
            else if (fence)
            {
                if (inQuote)
                {
                    closeQuote();
                }
                inFence = true;
                fenceChar = ch;
                fenceLen = fence;
                out += L"<pre><code";
                if (!info.empty())
                {
                    out += L" class=\"language-" + TgEscapeAttr(info) + L"\"";
                }
                out += L">";
            }
            else
            {
                std::wstring_view body = line;
                // Block quote lines: consecutive `> ` lines share one <blockquote>.
                const size_t firstNs = body.find_first_not_of(L' ');
                const bool quoted = firstNs != std::wstring_view::npos && firstNs <= 3 && body[firstNs] == L'>';
                if (quoted)
                {
                    body = body.substr(firstNs + 1);
                    if (!body.empty() && body.front() == L' ')
                    {
                        body.remove_prefix(1);
                    }
                    if (!inQuote)
                    {
                        out += L"<blockquote>";
                        inQuote = true;
                    }
                }
                else if (inQuote)
                {
                    closeQuote();
                }
                // Heading: `#`… + space → a bold line.
                size_t hashes = 0;
                while (hashes < body.size() && hashes < 6 && body[hashes] == L'#')
                {
                    ++hashes;
                }
                if (hashes > 0 && hashes < body.size() && body[hashes] == L' ')
                {
                    std::wstring_view text = body.substr(hashes + 1);
                    out += L"<b>" + detail::InlineLine(text) + L"</b>";
                }
                else
                {
                    // List bullet: `- ` / `* ` / `+ ` after indentation → "• ".
                    const size_t ind = body.find_first_not_of(L' ');
                    if (ind != std::wstring_view::npos && ind + 1 < body.size() && (body[ind] == L'-' || body[ind] == L'*' || body[ind] == L'+') && body[ind + 1] == L' ')
                    {
                        out.append(body.substr(0, ind));
                        out += L"\x2022 ";
                        out += detail::InlineLine(body.substr(ind + 2));
                    }
                    else
                    {
                        out += detail::InlineLine(body);
                    }
                }
                if (!last)
                {
                    out += L'\n';
                }
            }
            if (last)
            {
                break;
            }
            pos = nl + 1;
        }
        if (inFence)
        {
            out += L"</code></pre>";
        }
        if (inQuote)
        {
            closeQuote();
            out.pop_back(); // the closer's own "\n": nothing follows it
        }
        return out;
    }

    namespace detail
    {
        // Splits a long HTML text at a safe point at or before `limit`: the last "\n" before it, else
        // the last space, else exactly at the limit. Never splits inside an entity ("&amp;") or a tag.
        inline size_t SafeCut(std::wstring_view s, size_t limit)
        {
            if (s.size() <= limit)
            {
                return s.size();
            }
            size_t cut = s.rfind(L'\n', limit);
            if (cut == std::wstring_view::npos || cut < limit / 2)
            {
                cut = s.rfind(L' ', limit);
            }
            if (cut == std::wstring_view::npos || cut < limit / 2)
            {
                cut = limit;
            }
            // Back out of an entity or a tag the cut landed in.
            const size_t amp = s.rfind(L'&', cut);
            if (amp != std::wstring_view::npos && s.find(L';', amp) >= cut && cut - amp < 8)
            {
                cut = amp;
            }
            const size_t lt = s.rfind(L'<', cut);
            if (lt != std::wstring_view::npos)
            {
                const size_t gt = s.find(L'>', lt);
                if (gt == std::wstring_view::npos || gt >= cut)
                {
                    cut = lt;
                }
            }
            return cut == 0 ? limit : cut;
        }
    }

    /// <summary>
    /// Splits HTML (TgMarkdownToHtml's output, or escaped text wrapped in `<pre>`) into messages
    /// that fit Telegram's limit. A `<pre>` block is split at line breaks and re-opened in the next
    /// chunk; anything else is split at paragraph, then line, then word boundaries.
    /// </summary>
    /// <param name="html">The HTML to split.</param>
    /// <param name="limit">The per-message limit (kTelegramMessageMaxChars by default).</param>
    /// <returns>At least one chunk (an empty text yields one empty chunk); every chunk has balanced
    /// `<pre>`/`<code>` tags and is within the limit, except a single unbreakable token longer than it.</returns>
    /// <remarks>
    /// Only `<pre>`/`<code>` are tracked across chunks: the other inline tags this header emits are
    /// line-local (InlineRun balances them per run), and a cut never lands inside a tag or an entity
    /// (detail::SafeCut). A chunk opened with `<pre><code>` is closed before the cut and re-opened after,
    /// so the code's language class is lost from the second chunk on (cosmetic).
    /// </remarks>
    /// <exception cref="std::bad_alloc">If the chunks cannot be allocated.</exception>
    inline std::vector<std::wstring> TgChunkHtml(std::wstring_view html, size_t limit = kTelegramMessageMaxChars)
    {
        std::vector<std::wstring> chunks;
        if (html.size() <= limit)
        {
            chunks.emplace_back(html);
            return chunks;
        }
        constexpr std::wstring_view kPreOpen = L"<pre><code";
        constexpr std::wstring_view kPreClose = L"</code></pre>";
        constexpr std::wstring_view kReopen = L"<pre><code>";
        const size_t reserve = kPreClose.size() + 1;
        std::wstring_view rest = html;
        bool inPre = false;
        while (!rest.empty())
        {
            size_t budget = limit - (inPre ? reserve + kReopen.size() : reserve);
            if (budget < 64)
            {
                budget = 64;
            }
            // Where a <pre> opens or closes inside the window decides the tracking after the cut.
            size_t cut = detail::SafeCut(rest, budget);
            std::wstring piece{ rest.substr(0, cut) };
            // Track pre state through the piece.
            bool preAfter = inPre;
            {
                size_t p = 0;
                for (;;)
                {
                    const size_t o = piece.find(kPreOpen, p);
                    const size_t c = piece.find(kPreClose, p);
                    if (o == std::wstring::npos && c == std::wstring::npos)
                    {
                        break;
                    }
                    if (c != std::wstring::npos && (o == std::wstring::npos || c < o))
                    {
                        preAfter = false;
                        p = c + kPreClose.size();
                    }
                    else
                    {
                        preAfter = true;
                        p = o + kPreOpen.size();
                    }
                }
            }
            std::wstring chunk;
            if (inPre)
            {
                chunk += kReopen;
            }
            chunk += piece;
            rest.remove_prefix(cut);
            // Swallow the line break the cut landed on, so chunks do not start with a blank line.
            if (!rest.empty() && rest.front() == L'\n')
            {
                rest.remove_prefix(1);
            }
            if (preAfter && !rest.empty())
            {
                chunk += kPreClose;
            }
            inPre = preAfter;
            chunks.push_back(std::move(chunk));
        }
        return chunks;
    }

    /// <summary>
    /// Picks the nearest of Telegram's six topic icon colors to a tab color, so a topic reads like
    /// its tab. Telegram accepts ONLY those six values, and only at creation.
    /// </summary>
    /// <param name="hex">The tab color as "#RRGGBB" or "#AARRGGBB" (the alpha is ignored); "" or a
    /// malformed value picks the first color (blue).</param>
    /// <returns>One of kTelegramTopicColors, as 0xRRGGBB.</returns>
    inline uint32_t TgTopicIconColor(std::wstring_view hex) noexcept
    {
        uint32_t rgb = 0;
        bool ok = false;
        if (!hex.empty() && hex.front() == L'#' && (hex.size() == 7 || hex.size() == 9))
        {
            ok = true;
            for (size_t i = 1; i < hex.size(); ++i)
            {
                const wchar_t c = hex[i];
                uint32_t v = 0;
                if (c >= L'0' && c <= L'9')
                {
                    v = static_cast<uint32_t>(c - L'0');
                }
                else if (c >= L'a' && c <= L'f')
                {
                    v = static_cast<uint32_t>(c - L'a' + 10);
                }
                else if (c >= L'A' && c <= L'F')
                {
                    v = static_cast<uint32_t>(c - L'A' + 10);
                }
                else
                {
                    ok = false;
                    break;
                }
                rgb = (rgb << 4) | v;
            }
            rgb &= 0xFFFFFFu; // drop a leading alpha byte
        }
        if (!ok)
        {
            return kTelegramTopicColors[0];
        }
        const int r = static_cast<int>((rgb >> 16) & 0xFF);
        const int g = static_cast<int>((rgb >> 8) & 0xFF);
        const int b = static_cast<int>(rgb & 0xFF);
        uint32_t best = kTelegramTopicColors[0];
        long bestD = -1;
        for (const uint32_t c : kTelegramTopicColors)
        {
            const int cr = static_cast<int>((c >> 16) & 0xFF);
            const int cg = static_cast<int>((c >> 8) & 0xFF);
            const int cb = static_cast<int>(c & 0xFF);
            // A weighted squared distance (the common 2/4/3 luminance weighting), good enough to pick
            // "the blue-ish one" for a blue tab.
            const long d = 2L * (r - cr) * (r - cr) + 4L * (g - cg) * (g - cg) + 3L * (b - cb) * (b - cb);
            if (bestD < 0 || d < bestD)
            {
                bestD = d;
                best = c;
            }
        }
        return best;
    }

    /// <summary>
    /// Formats a duration for a card line: "3m", "1h12m", "2d4h", "45s".
    /// </summary>
    /// <param name="ms">The span in milliseconds; a negative value reads as 0.</param>
    /// <returns>The short text.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    /// <summary>
    /// A moment as a LOCAL clock time for a message that is edited in place: "19:04" when it falls on the same
    /// local calendar day as <paramref name="nowMs"/>, else "16 Oct 19:04". Use it, never a relative span, in
    /// any text the bot re-renders and compares (the topic card, an idle note): a span ("3m", "45s") changes
    /// every minute or every render, so 47 cards re-edited themselves 47 times a minute and Telegram answered
    /// with a 429 and a 21-minute `retry_after` (2026-10-16); a clock time changes once, at the next midnight.
    /// </summary>
    /// <param name="unixMs">The moment (Unix ms).</param>
    /// <param name="nowMs">The clock the "same day" test is made against (Unix ms).</param>
    /// <returns>"HH:MM" or "D Mon HH:MM" in local time; "?" when the CRT cannot convert the moment.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring TgClockText(int64_t unixMs, int64_t nowMs)
    {
        const auto toLocal = [](int64_t ms, std::tm& out) noexcept -> bool {
            const time_t t = static_cast<time_t>(ms / 1000);
            return localtime_s(&out, &t) == 0; // the CRT's thread-safe form; it writes nothing on failure
        };
        std::tm when{};
        std::tm now{};
        if (!toLocal(unixMs, when))
        {
            return L"?";
        }
        const auto two = [](int v) { return (v < 10 ? L"0" : L"") + std::to_wstring(v); };
        std::wstring clock = two(when.tm_hour) + L":" + two(when.tm_min);
        if (toLocal(nowMs, now) && now.tm_year == when.tm_year && now.tm_yday == when.tm_yday)
        {
            return clock;
        }
        static constexpr const wchar_t* kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        const int mon = when.tm_mon >= 0 && when.tm_mon < 12 ? when.tm_mon : 0;
        return std::to_wstring(when.tm_mday) + L" " + kMonths[mon] + L" " + clock;
    }

    /// <summary>
    /// Telegram HTML back to plain text: the tags this header emits are dropped and the three entities
    /// `TgEscapeHtml` writes are turned back. For text that is REUSED as data — the recap's "last reply" head
    /// taken from a message already rendered for Telegram — never for display (a `<pre>` block loses its line
    /// structure only in the sense that nothing marks it any more).
    /// </summary>
    /// <param name="html">Telegram HTML as this header produces it.</param>
    /// <returns>The text without tags, entities unescaped.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring TgStripTags(std::wstring_view html)
    {
        std::wstring out;
        out.reserve(html.size());
        bool inTag = false;
        for (size_t i = 0; i < html.size(); ++i)
        {
            const wchar_t c = html[i];
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
                out += c;
            }
        }
        // The entities, longest first so "&amp;lt;" (an escaped "&lt;") comes back as "&lt;", not "<".
        const auto replaceAll = [&out](std::wstring_view from, std::wstring_view to) {
            size_t pos = 0;
            while ((pos = out.find(from, pos)) != std::wstring::npos)
            {
                out.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replaceAll(L"&lt;", L"<");
        replaceAll(L"&gt;", L">");
        replaceAll(L"&amp;", L"&");
        return out;
    }

    inline std::wstring TgShortSpan(int64_t ms)
    {
        if (ms < 0)
        {
            ms = 0;
        }
        const int64_t s = ms / 1000;
        if (s < 60)
        {
            return std::to_wstring(s) + L"s";
        }
        const int64_t m = s / 60;
        if (m < 60)
        {
            return std::to_wstring(m) + L"m";
        }
        const int64_t h = m / 60;
        if (h < 24)
        {
            return std::to_wstring(h) + L"h" + (m % 60 ? std::to_wstring(m % 60) + L"m" : std::wstring{});
        }
        const int64_t d = h / 24;
        return std::to_wstring(d) + L"d" + (h % 24 ? std::to_wstring(h % 24) + L"h" : std::wstring{});
    }
}
