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
//
// Since §21.11 Claude's own Markdown (replies, remarks, plans) goes out as a RICH message (Bot API
// 10.1), which Telegram renders from the Markdown itself — tables included, which HTML mode has no
// form for. TgPrepareRichMarkdown / TgChunkMarkdown / TgMdEscapeText below serve that path; the HTML
// converter stays the fallback when a rich send is refused.

#pragma once

#include <algorithm>
#include <cmath> // std::fabs / std::fmod: TgTitleColorSquare's hue (§21.13)
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility> // std::pair: the rich converter's stack of open inline tags (§21.15)
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
                        // The url's closing ')' — parentheses INSIDE the url must balance (CommonMark's rule), or a
                        // Wikipedia link like .../Jordan_King_(basketball) lost its last ')' and left a stray one after
                        // the link (seen in the §21.9 history replay). A url has no whitespace: one ends the search.
                        size_t closeUrl = std::wstring_view::npos;
                        int depth = 0;
                        for (size_t k = closeText + 2; k < raw.size(); ++k)
                        {
                            const wchar_t c = raw[k];
                            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n')
                            {
                                break;
                            }
                            if (c == L'(')
                            {
                                ++depth;
                            }
                            else if (c == L')')
                            {
                                if (depth == 0)
                                {
                                    closeUrl = k;
                                    break;
                                }
                                --depth;
                            }
                        }
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

    // ===== Rich messages (Bot API 10.1, MESSAGING.md §21.11) =================================
    //
    // A RICH message (sendRichMessage) takes GitHub-flavored Markdown as it is — tables, headings, lists, task lists,
    // fenced code, quotes, collapsible <details> blocks — and Telegram renders it natively: what a Claude reply IS.
    // Measured with the dev bot on 2026-10-17: a table with links, bold and code in its cells came back as a real table
    // block; "$5 and $10", "a == b" and "x || y" stayed plain text; but every '<' that looks like a tag is DROPPED
    // ("List<int> … <T>" arrived as "List … "), "&lt;" restores it, and "\<" is shown as typed. A single line break
    // inside a paragraph is a space (GitHub's rule); two trailing spaces keep it. The limits: 32,768 characters, 500
    // blocks (a list item, a table row, a quote each count), 20 table columns.

    // A rich message's text limit is 32,768 UTF-8 characters; a chunk stays well under it in UTF-16 units (every
    // character is at least one unit, so this many units are at most this many characters). The room above it is for
    // what TgPrepareRichMarkdown adds after the split (an "&lt;" for each '<', two spaces per hard break).
    inline constexpr size_t kRichChunkMaxChars = 24000;
    // The most a prepared part may hold and still go as a rich message (under Telegram's 32,768 with a margin): a part
    // that grew past it in preparation (thousands of '<') goes as HTML instead (Messaging.cpp).
    inline constexpr size_t kRichMessageMaxChars = 32000;
    // The 500-block limit, kept with room: a chunk holds at most this many lines (a line is at most one block — a list
    // item, a table row, a paragraph line).
    inline constexpr size_t kRichChunkMaxLines = 400;

    namespace detail
    {
        // A fenced code block's opening or closing line: up to three spaces, then a run of at least three '`' or '~'.
        // Returns the run's length and character, or 0 when the line is no fence.
        inline size_t MdFenceRun(std::wstring_view line, wchar_t& ch) noexcept
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
            const wchar_t c = line[i];
            size_t n = 0;
            while (i + n < line.size() && line[i + n] == c)
            {
                ++n;
            }
            if (n < 3)
            {
                return 0;
            }
            ch = c;
            return n;
        }

        // Whether a line closes the fence opened by a run of `n` × `c`: the same character, at least as long, nothing
        // after it but spaces.
        inline bool MdClosesFence(std::wstring_view line, wchar_t c, size_t n) noexcept
        {
            wchar_t ch = 0;
            const size_t run = MdFenceRun(line, ch);
            if (run < n || ch != c)
            {
                return false;
            }
            size_t i = line.find_first_not_of(L' ');
            i += run;
            for (; i < line.size(); ++i)
            {
                if (line[i] != L' ' && line[i] != L'\t')
                {
                    return false;
                }
            }
            return true;
        }

        // Splits a text into lines (LF; a CR before it is dropped). The last line has no LF after it.
        inline std::vector<std::wstring_view> MdLines(std::wstring_view md)
        {
            std::vector<std::wstring_view> lines;
            size_t p = 0;
            while (p <= md.size())
            {
                const size_t nl = md.find(L'\n', p);
                std::wstring_view line = md.substr(p, nl == std::wstring_view::npos ? std::wstring_view::npos : nl - p);
                if (!line.empty() && line.back() == L'\r')
                {
                    line.remove_suffix(1);
                }
                lines.push_back(line);
                if (nl == std::wstring_view::npos)
                {
                    break;
                }
                p = nl + 1;
            }
            return lines;
        }

        // Escapes '<' as "&lt;" outside inline code spans of one line (a span is a run of N backticks closed by the next
        // run of exactly N on the line; an unclosed run is literal text). Inside a span nothing changes: no entity is
        // decoded in code, so an escape there would show as typed.
        inline void MdEscapeTagsInLine(std::wstring_view line, std::wstring& out)
        {
            size_t i = 0;
            while (i < line.size())
            {
                const wchar_t c = line[i];
                if (c == L'`')
                {
                    size_t n = 0;
                    while (i + n < line.size() && line[i + n] == L'`')
                    {
                        ++n;
                    }
                    // Find the closing run of exactly n backticks.
                    size_t j = i + n;
                    size_t close = std::wstring_view::npos;
                    while (j < line.size())
                    {
                        if (line[j] == L'`')
                        {
                            size_t m = 0;
                            while (j + m < line.size() && line[j + m] == L'`')
                            {
                                ++m;
                            }
                            if (m == n)
                            {
                                close = j;
                                break;
                            }
                            j += m;
                        }
                        else
                        {
                            ++j;
                        }
                    }
                    if (close == std::wstring_view::npos)
                    {
                        out.append(line.substr(i, n)); // no closing run: the backticks are text
                        i += n;
                        continue;
                    }
                    out.append(line.substr(i, close + n - i)); // the whole span, as written
                    i = close + n;
                    continue;
                }
                if (c == L'<')
                {
                    out += L"&lt;";
                }
                else
                {
                    out += c;
                }
                ++i;
            }
        }

        // A COLLAPSIBLE BLOCK's tag line: "<details>", "<details open>" or "</details>" at the start of a line (up to three
        // spaces before it), a "<summary>…</summary>" right after the opening tag. Telegram's rich Markdown renders
        // <details> as a collapsible block and parses Markdown inside it (the history's tool groups are such blocks,
        // MessagingModels.h RenderHistory); these tags are kept, the text around them escaped like any line. Returns
        // false (writing nothing) for any other line.
        inline bool MdKeepCollapsibleTags(std::wstring_view line, std::wstring& out)
        {
            size_t i = 0;
            while (i < line.size() && i < 3 && line[i] == L' ')
            {
                ++i;
            }
            const std::wstring_view rest = line.substr(i);
            std::wstring_view tag;
            for (const std::wstring_view t : { std::wstring_view{ L"<details>" }, std::wstring_view{ L"<details open>" }, std::wstring_view{ L"</details>" } })
            {
                if (rest.substr(0, t.size()) == t)
                {
                    tag = t;
                    break;
                }
            }
            if (tag.empty())
            {
                return false;
            }
            out.append(line.substr(0, i));
            out.append(tag);
            std::wstring_view tail = rest.substr(tag.size());
            constexpr std::wstring_view kOpen{ L"<summary>" };
            constexpr std::wstring_view kClose{ L"</summary>" };
            if (tag != L"</details>" && tail.substr(0, kOpen.size()) == kOpen)
            {
                const size_t close = tail.rfind(kClose);
                if (close != std::wstring_view::npos && close >= kOpen.size())
                {
                    out.append(kOpen);
                    MdEscapeTagsInLine(tail.substr(kOpen.size(), close - kOpen.size()), out);
                    out.append(kClose);
                    tail = tail.substr(close + kClose.size());
                }
            }
            MdEscapeTagsInLine(tail, out);
            return true;
        }
    }

    /// <summary>
    /// Prepares Markdown — a Claude reply or remark as written — for a Telegram RICH message's `markdown` field: every
    /// '&lt;' outside code becomes "&amp;lt;" (Telegram drops a tag it does not know, and with it the text: "List&lt;int&gt;"
    /// came out as "List"), and every line that another non-blank line follows ends with two spaces (a hard break: Claude
    /// writes for a terminal that keeps its line breaks, where Markdown would join the lines). Fenced code blocks and
    /// inline code spans are copied exactly. Two exceptions: a table row (a line that starts with '|') gets no hard break
    /// — the rows are separate lines of the table already — and a collapsible block's tag line ("&lt;details&gt;" … at
    /// the start of a line, see detail::MdKeepCollapsibleTags) keeps its tags. Lines are joined with LF.
    /// </summary>
    /// <param name="md">The Markdown.</param>
    /// <returns>The text to send as `rich_message.markdown`.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring TgPrepareRichMarkdown(std::wstring_view md)
    {
        const auto lines = detail::MdLines(md);
        std::wstring out;
        out.reserve(md.size() + md.size() / 16 + 16);
        bool inFence = false;
        wchar_t fenceCh = 0;
        size_t fenceLen = 0;
        for (size_t k = 0; k < lines.size(); ++k)
        {
            const std::wstring_view line = lines[k];
            if (k > 0)
            {
                out += L'\n';
            }
            if (inFence)
            {
                out.append(line);
                if (detail::MdClosesFence(line, fenceCh, fenceLen))
                {
                    inFence = false;
                }
                continue;
            }
            wchar_t ch = 0;
            if (const size_t run = detail::MdFenceRun(line, ch); run > 0)
            {
                out.append(line); // the opening line, its info string included
                inFence = true;
                fenceCh = ch;
                fenceLen = run;
                continue;
            }
            if (!detail::MdKeepCollapsibleTags(line, out))
            {
                detail::MdEscapeTagsInLine(line, out);
            }
            // A hard break when a non-blank line follows (not a fence's opening line: the code block starts a block of
            // its own, and a break at the end of a paragraph is ignored anyway). Not after a table row: a row is a line of
            // the table already, and two spaces after its closing pipe would be a cell's text to a strict parser.
            const size_t firstChar = line.find_first_not_of(L" \t");
            const bool blank = firstChar == std::wstring_view::npos;
            const bool tableRow = !blank && line[firstChar] == L'|';
            if (!blank && !tableRow && k + 1 < lines.size())
            {
                const std::wstring_view next = lines[k + 1];
                const bool nextBlank = next.find_first_not_of(L" \t") == std::wstring_view::npos;
                const bool endsWithBreak = line.size() >= 2 ? (line.substr(line.size() - 2) == L"  " || line.back() == L'\\') : line.back() == L'\\';
                if (!nextBlank && !endsWithBreak)
                {
                    out += L"  ";
                }
            }
        }
        return out;
    }

    /// <summary>
    /// Splits Markdown into parts that each fit one rich message: at most <paramref name="maxChars"/> UTF-16 units and
    /// <paramref name="maxLines"/> lines (the 500-block limit), cut at a blank line outside code where one exists, else
    /// at a line end. A part that ends inside a fenced code block closes the fence, and the next part opens it again with
    /// the same opening line, so every part is valid Markdown. A text that fits is one part, unchanged.
    /// </summary>
    /// <param name="md">The Markdown (unprepared: TgPrepareRichMarkdown runs on each part).</param>
    /// <param name="maxChars">The most UTF-16 units in one part.</param>
    /// <param name="maxLines">The most lines in one part.</param>
    /// <returns>The parts, in order; at least one (possibly empty).</returns>
    /// <exception cref="std::bad_alloc">If the parts cannot be allocated.</exception>
    inline std::vector<std::wstring> TgChunkMarkdown(std::wstring_view md, size_t maxChars = kRichChunkMaxChars, size_t maxLines = kRichChunkMaxLines)
    {
        std::vector<std::wstring> parts;
        const auto lines = detail::MdLines(md);
        if (md.size() <= maxChars && lines.size() <= maxLines)
        {
            parts.emplace_back(md);
            return parts;
        }
        maxChars = std::max<size_t>(maxChars, 200);
        maxLines = std::max<size_t>(maxLines, 10);
        std::wstring cur;
        size_t curLines = 0;
        size_t lastBlankCut = std::wstring::npos; // a cut point in `cur`: just after a blank line outside code
        size_t lastBlankLines = 0;
        bool inFence = false;
        wchar_t fenceCh = 0;
        size_t fenceLen = 0;
        std::wstring fenceOpen; // the open fence's opening line (re-opened in the next part)
        const auto closeAndPush = [&](std::wstring part, bool fenceOpenAtCut) {
            if (fenceOpenAtCut)
            {
                if (!part.empty() && part.back() != L'\n')
                {
                    part += L'\n';
                }
                part += std::wstring(fenceLen, fenceCh);
            }
            while (!part.empty() && (part.back() == L'\n' || part.back() == L' '))
            {
                part.pop_back();
            }
            parts.push_back(std::move(part));
        };
        for (size_t k = 0; k < lines.size(); ++k)
        {
            const std::wstring_view line = lines[k];
            const size_t add = line.size() + 1;
            if (!cur.empty() && (cur.size() + add > maxChars || curLines + 1 > maxLines))
            {
                if (!inFence && lastBlankCut != std::wstring::npos && lastBlankCut > 0)
                {
                    // Cut at the last blank line: what follows it starts the next part.
                    std::wstring rest = cur.substr(lastBlankCut);
                    const size_t restLines = curLines - lastBlankLines;
                    closeAndPush(cur.substr(0, lastBlankCut), false);
                    cur = std::move(rest);
                    curLines = restLines;
                }
                else
                {
                    // No blank line to cut at (one long paragraph, list, table or code block): cut here.
                    closeAndPush(cur, inFence);
                    cur.clear();
                    curLines = 0;
                    if (inFence)
                    {
                        cur = fenceOpen + L'\n';
                        curLines = 1;
                    }
                }
                lastBlankCut = std::wstring::npos;
                lastBlankLines = 0;
            }
            // A single line longer than a part: hard pieces (never between the two halves of a surrogate pair: a lone half
            // is not valid UTF-16, and the UTF-8 the request carries could not encode it).
            std::wstring_view rem = line;
            while (rem.size() + 1 > maxChars)
            {
                size_t take = maxChars - 1 - std::min<size_t>(cur.size(), maxChars - 2);
                if (take > 1 && rem[take - 1] >= 0xD800 && rem[take - 1] <= 0xDBFF)
                {
                    --take;
                }
                cur.append(rem.substr(0, take));
                closeAndPush(cur, inFence);
                cur = inFence ? fenceOpen + L'\n' : std::wstring{};
                curLines = inFence ? 1 : 0;
                rem.remove_prefix(take);
            }
            cur.append(rem);
            cur += L'\n';
            ++curLines;
            if (inFence)
            {
                if (detail::MdClosesFence(line, fenceCh, fenceLen))
                {
                    inFence = false;
                }
            }
            else
            {
                wchar_t ch = 0;
                if (const size_t run = detail::MdFenceRun(line, ch); run > 0)
                {
                    inFence = true;
                    fenceCh = ch;
                    fenceLen = run;
                    fenceOpen.assign(line);
                }
                else if (line.find_first_not_of(L" \t") == std::wstring_view::npos)
                {
                    lastBlankCut = cur.size();
                    lastBlankLines = curLines;
                }
            }
        }
        if (!cur.empty() || parts.empty())
        {
            closeAndPush(cur, inFence);
        }
        return parts;
    }

    /// <summary>
    /// Plain text (a question, an option, a tool line, an error) as Markdown that shows it as typed: the characters
    /// Markdown would read as formatting are backslash-escaped, '&lt;' becomes "&amp;lt;" (the one escape Telegram's rich
    /// Markdown honours for it), a line that starts like an ordered list item ("1. ", "2) ") keeps its number as text, and
    /// every line break is kept (a hard break).
    /// </summary>
    /// <param name="text">The plain text.</param>
    /// <returns>The Markdown.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring TgMdEscapeText(std::wstring_view text)
    {
        std::wstring out;
        out.reserve(text.size() + text.size() / 8 + 8);
        bool lineStart = true; // only spaces since the start of the line
        for (size_t i = 0; i < text.size(); ++i)
        {
            const wchar_t c = text[i];
            if (lineStart && c >= L'0' && c <= L'9')
            {
                // "12. text" / "3) text" at a line's start is an ordered list item to Markdown: the digits stay, the
                // '.' or ')' after them is escaped. Any other run of digits is plain text.
                size_t j = i;
                while (j < text.size() && text[j] >= L'0' && text[j] <= L'9')
                {
                    ++j;
                }
                out.append(text.substr(i, j - i));
                if (j < text.size() && (text[j] == L'.' || text[j] == L')'))
                {
                    out += L'\\';
                    out += text[j];
                    ++j;
                }
                i = j - 1;
                lineStart = false;
                continue;
            }
            lineStart = (c == L'\n') || (lineStart && (c == L' ' || c == L'\r'));
            switch (c)
            {
            case L'\\':
            case L'`':
            case L'*':
            case L'_':
            case L'[':
            case L']':
            case L'#':
            case L'|':
            case L'~':
            case L'=':
            case L'$':
            case L'!':
            case L'>':
            case L'-':
            case L'+':
                out += L'\\';
                out += c;
                break;
            case L'<':
                out += L"&lt;";
                break;
            case L'&':
                out += L"&amp;";
                break;
            case L'\r':
                break;
            case L'\n':
                out += L"  \n";
                break;
            default:
                out += c;
                break;
            }
        }
        return out;
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

    // ===== the title's color marker (MESSAGING.md §21.13) ==========================================

    /// <summary>
    /// Parses a tab color, "#RRGGBB" or "#AARRGGBB" (the alpha byte is ignored), into 0xRRGGBB. Total: a malformed value
    /// is refused, never half-read. (TgTopicIconColor keeps its own parse, which falls back to blue instead of refusing.)
    /// </summary>
    /// <param name="hex">The color text.</param>
    /// <param name="rgb">Receives 0xRRGGBB when the text parses; untouched otherwise.</param>
    /// <returns>True when the text is a well-formed color.</returns>
    inline bool TgParseRgbHex(std::wstring_view hex, uint32_t& rgb) noexcept
    {
        if (hex.empty() || hex.front() != L'#' || (hex.size() != 7 && hex.size() != 9))
        {
            return false;
        }
        uint32_t v = 0;
        for (size_t i = 1; i < hex.size(); ++i)
        {
            const wchar_t c = hex[i];
            uint32_t d = 0;
            if (c >= L'0' && c <= L'9')
            {
                d = static_cast<uint32_t>(c - L'0');
            }
            else if (c >= L'a' && c <= L'f')
            {
                d = static_cast<uint32_t>(c - L'a' + 10);
            }
            else if (c >= L'A' && c <= L'F')
            {
                d = static_cast<uint32_t>(c - L'A' + 10);
            }
            else
            {
                return false;
            }
            v = (v << 4) | d;
        }
        rgb = v & 0xFFFFFFu; // "#AARRGGBB": the alpha byte shifted out above the low 24 bits
        return true;
    }

    /// <summary>
    /// The COLORED SQUARE emoji that stands for a tab's color wherever the bot prints a session's title (§21.13: the topic
    /// name, the card, the Search answers, the command replies) — the closest Telegram has to a colored background behind
    /// a title. Measured against the API on 2026-10-17 (scratchpad colortest.py, thread 26785 of the dev chat): classic
    /// HTML refuses &lt;span style&gt; and &lt;mark&gt;; a rich message keeps &lt;mark&gt; / ==marked== but the client
    /// paints the highlight in its OWN color and drops a style; a custom emoji entity is dropped unless the bot's owner has
    /// Telegram Premium; a topic icon color is one of six and fixed at creation. A square emoji works in a topic name (plain
    /// text) and in every message, and an edit changes it, so it can follow the tab.
    /// The pick goes by HUE, not by distance: the tab palette is pastel (One Dark), and a plain RGB or Lab distance to the
    /// vivid emoji maps its reds and corals to the brown square. HSL buckets: very dark ⇒ ⬛, very light ⇒ ⬜, unsaturated ⇒
    /// ⬛ / ⬜ by lightness, a dark warm hue ⇒ 🟫, then red [330°, 12°) · orange [12°, 35°) · yellow [35°, 70°) · green
    /// [70°, 165°) · blue [165°, 255°) (cyan joins blue: there is no cyan square) · purple [255°, 330°). The 14 palette
    /// colors land on 6 squares (🟥 🟧 🟨 🟩 🟦 🟪), so two folders can share a square — it marks a color, it is not unique.
    /// </summary>
    /// <param name="hex">The tab color ("#RRGGBB" / "#AARRGGBB"); "" (the "Remove colors" mode, or no color known) or a
    /// malformed value gives no square.</param>
    /// <returns>The square as a static UTF-16 string, or "" (never null).</returns>
    inline const wchar_t* TgTitleColorSquare(std::wstring_view hex) noexcept
    {
        uint32_t rgb = 0;
        if (!TgParseRgbHex(hex, rgb))
        {
            return L"";
        }
        const double r = static_cast<double>((rgb >> 16) & 0xFF) / 255.0;
        const double g = static_cast<double>((rgb >> 8) & 0xFF) / 255.0;
        const double b = static_cast<double>(rgb & 0xFF) / 255.0;
        const double mx = (std::max)({ r, g, b }); // (std::max): the <windows.h> macro
        const double mn = (std::min)({ r, g, b });
        const double l = (mx + mn) / 2.0;
        const double d = mx - mn;
        // HSL saturation; d == 0 is a grey (and covers pure black and white, where the divisor would be 0).
        const double s = d == 0.0 ? 0.0 : d / (1.0 - std::fabs(2.0 * l - 1.0));
        double h = 0.0;
        if (d != 0.0)
        {
            if (mx == r)
            {
                h = 60.0 * std::fmod((g - b) / d, 6.0);
            }
            else if (mx == g)
            {
                h = 60.0 * ((b - r) / d + 2.0);
            }
            else
            {
                h = 60.0 * ((r - g) / d + 4.0);
            }
            if (h < 0.0)
            {
                h += 360.0;
            }
        }
        if (l < 0.15)
        {
            return L"\x2B1B";
        }
        if (l > 0.92)
        {
            return L"\x2B1C";
        }
        if (s < 0.18)
        {
            return l < 0.5 ? L"\x2B1B" : L"\x2B1C";
        }
        if (h < 45.0 && l < 0.42)
        {
            return L"\U0001F7EB"; // brown: a dark red / orange / amber
        }
        if (h >= 330.0 || h < 12.0)
        {
            return L"\U0001F7E5";
        }
        if (h < 35.0)
        {
            return L"\U0001F7E7";
        }
        if (h < 70.0)
        {
            return L"\U0001F7E8";
        }
        if (h < 165.0)
        {
            return L"\U0001F7E9";
        }
        if (h < 255.0)
        {
            return L"\U0001F7E6";
        }
        return L"\U0001F7EA";
    }

    /// <summary>
    /// The prefix a printed title takes (§21.13): the tab color's square and a space ("🟦 "), or "" when there is no color —
    /// so a caller writes <c>TgTitleColorPrefix(hex) + "&lt;b&gt;" + title + "&lt;/b&gt;"</c> and a colorless session reads
    /// as before.
    /// </summary>
    /// <param name="hex">The tab color ("" = none).</param>
    /// <returns>"🟦 " or "".</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring TgTitleColorPrefix(std::wstring_view hex)
    {
        const wchar_t* sq = TgTitleColorSquare(hex);
        return *sq ? std::wstring{ sq } + L" " : std::wstring{};
    }

    // ===== the title chip (§21.15) ===============================================================================================
    //
    // The user's "Go for 10 with copy_text button is great, even better if we can specify hex color code or equivalent of hex
    // color code" (2026-10-17, after the color tests in thread 26785 of the dev chat): a session's title, wherever the bot prints
    // it, is a CHIP — an in-text copy_text button of a rich message (Bot API 10.3), in the Telegram look nearest the tab's color,
    // the §21.13 square inside its label; a tap copies the title. A builder writes a title TOKEN where the title goes
    // (TgTitleToken), and the send layer renders the message one of two ways: classic HTML — the §21.13 square before the title,
    // byte for byte what the builders printed before (TgRenderTitleTokensClassic), so a card's hash, taken over that render, does
    // not change with the upgrade — or a rich message whose titles are chips (TgRichHtmlFromClassic). The rich form needs the
    // cog's rich switch; without it, or when Telegram refuses the rich message, the classic render goes in its place.

    /// <summary>
    /// The Telegram button LOOK nearest a tab color (§21.15). The Bot API gives a button no color of its own: `style` is one of
    /// “danger” (red), “success” (green) and “primary” (blue) — a rich message's button adds “link”, a borderless link — and
    /// "Apps may use theme-specific colors for the button background and text based on the style" (the docs). So no hex color
    /// can be asked for, and the shade on the screen is the app's. The square's color FAMILY picks the look when the family has
    /// one (🟦 primary, 🟩 success, 🟥 danger); every other square (🟧 🟨 🟪 🟫 ⬛ ⬜) and no square get the app's plain button
    /// rather than a neighbor's color — an orange tab with a red chip would read as a red tab — and the chip's label keeps the
    /// square, which names the family either way. Of the 14 tab palette colors, 8 get a colored chip.
    /// </summary>
    /// <param name="colorSquare">TgTitleColorSquare of the tab color ("" = none).</param>
    /// <returns>"primary", "success", "danger", or "" for the plain look (a static string, never null).</returns>
    inline const wchar_t* TgTitleChipStyle(std::wstring_view colorSquare) noexcept
    {
        if (colorSquare == L"\U0001F7E6")
        {
            return L"primary";
        }
        if (colorSquare == L"\U0001F7E9")
        {
            return L"success";
        }
        if (colorSquare == L"\U0001F7E5")
        {
            return L"danger";
        }
        return L"";
    }

    /// <summary>
    /// How a title TOKEN reads in a CLASSIC (HTML) message (§21.15). Each form is what its place printed before the chips, so a
    /// classic render is byte for byte the old text. A rich message shows the chip alone in every form: its quotes, its bold
    /// and a bare square all give way to the button.
    /// </summary>
    enum class TgTitleForm : wchar_t
    {
        Plain = L'p', // "🟦 title" — the §21.13 mention (TitleMentionHtml's text); the caller's own tags wrap it
        Bold = L'b', // "🟦 <b>title</b>" — the title line of the card and of a Search answer's row
        Quoted = L'q', // "“🟦 title”" — a title the change line names
        Square = L's', // "🟦" — the change line's new color (the plain mention when there is no square)
    };

    // The private-use characters that frame a title token: OPEN, the form, the square, MID, the escaped title, CLOSE. They never
    // reach Telegram: every send path renders the tokens, and the outbox renders a stray one classic (MessagingService::_enqueue).
    inline constexpr wchar_t kTgTitleTokenOpen = L'\xE000';
    inline constexpr wchar_t kTgTitleTokenMid = L'\xE001';
    inline constexpr wchar_t kTgTitleTokenClose = L'\xE002';

    // A chip's label shows at most this many UTF-16 units of the title (an ellipsis past it: a button is no place for a
    // paragraph); its copy is the whole title up to Telegram's CopyTextButton limit, "1-256 characters".
    inline constexpr size_t kTgTitleChipLabelMaxChars = 100;
    inline constexpr size_t kTgCopyTextMaxChars = 256;

    namespace detail
    {
        /// <summary>Whether a character frames a title token (§21.15); such a character is dropped from a token's parts.</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for OPEN, MID and CLOSE.</returns>
        inline constexpr bool TgIsTitleTokenMarker(wchar_t c) noexcept
        {
            return c == kTgTitleTokenOpen || c == kTgTitleTokenMid || c == kTgTitleTokenClose;
        }

        /// <summary>
        /// Undoes TgEscapeHtml exactly (§21.15): a token carries its title escaped, so the token can sit inside HTML, and the
        /// chip needs the text back to cut it and to quote it in an attribute. Only the three entities TgEscapeHtml writes are
        /// read; any other '&amp;' stays as it is.
        /// </summary>
        /// <param name="s">Text escaped by TgEscapeHtml.</param>
        /// <returns>The text before escaping.</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        inline std::wstring TgUnescapeOwnHtml(std::wstring_view s)
        {
            std::wstring out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size();)
            {
                if (s[i] == L'&')
                {
                    const std::wstring_view rest = s.substr(i);
                    if (rest.rfind(L"&amp;", 0) == 0)
                    {
                        out += L'&';
                        i += 5;
                        continue;
                    }
                    if (rest.rfind(L"&lt;", 0) == 0)
                    {
                        out += L'<';
                        i += 4;
                        continue;
                    }
                    if (rest.rfind(L"&gt;", 0) == 0)
                    {
                        out += L'>';
                        i += 4;
                        continue;
                    }
                }
                out += s[i];
                ++i;
            }
            return out;
        }

        /// <summary>
        /// The longest prefix of at most `maxUnits` UTF-16 units that does not end inside a surrogate pair: a lone high
        /// surrogate would make the text invalid UTF-16, which Telegram refuses.
        /// </summary>
        /// <param name="s">The text.</param>
        /// <param name="maxUnits">The cap, in UTF-16 units.</param>
        /// <returns>The prefix (the whole text when it fits).</returns>
        inline std::wstring_view TgPrefixUnits(std::wstring_view s, size_t maxUnits) noexcept
        {
            if (s.size() <= maxUnits)
            {
                return s;
            }
            size_t n = maxUnits;
            if (n > 0 && s[n - 1] >= 0xD800 && s[n - 1] <= 0xDBFF)
            {
                --n; // the cut would split a pair: the whole pair stays out
            }
            return s.substr(0, n);
        }

        /// <summary>TgEscapeHtml plus the double quote: a value for a double-quoted HTML attribute.</summary>
        /// <param name="s">The raw text.</param>
        /// <returns>The escaped text.</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        inline std::wstring TgEscapeHtmlAttribute(std::wstring_view s)
        {
            std::wstring out;
            out.reserve(s.size() + 16);
            for (const wchar_t c : TgEscapeHtml(s))
            {
                if (c == L'"')
                {
                    out += L"&quot;";
                }
                else
                {
                    out += c;
                }
            }
            return out;
        }

        /// <summary>Reads the title token that starts at `pos` (§21.15): its form, its square and its escaped title.</summary>
        /// <param name="s">The text.</param>
        /// <param name="pos">The index of a kTgTitleTokenOpen.</param>
        /// <param name="form">Receives the form (Plain for an unknown form character).</param>
        /// <param name="square">Receives the color square ("" = none).</param>
        /// <param name="escapedTitle">Receives the title, escaped by TgEscapeHtml.</param>
        /// <returns>The index just past the token's CLOSE; npos when no well-formed token starts at `pos` (the caller drops
        /// the stray OPEN and goes on).</returns>
        /// <exception cref="std::bad_alloc">If a part cannot be copied.</exception>
        inline size_t TgParseTitleToken(std::wstring_view s, size_t pos, TgTitleForm& form, std::wstring& square, std::wstring& escapedTitle)
        {
            if (pos + 2 >= s.size() || s[pos] != kTgTitleTokenOpen)
            {
                return std::wstring_view::npos;
            }
            const size_t mid = s.find(kTgTitleTokenMid, pos + 2);
            if (mid == std::wstring_view::npos)
            {
                return std::wstring_view::npos;
            }
            const size_t close = s.find(kTgTitleTokenClose, mid + 1);
            const size_t nextOpen = s.find(kTgTitleTokenOpen, pos + 1);
            if (close == std::wstring_view::npos || (nextOpen != std::wstring_view::npos && nextOpen < close))
            {
                return std::wstring_view::npos; // a token that never closes, or one cut off by the next token
            }
            switch (s[pos + 1])
            {
            case static_cast<wchar_t>(TgTitleForm::Bold):
                form = TgTitleForm::Bold;
                break;
            case static_cast<wchar_t>(TgTitleForm::Quoted):
                form = TgTitleForm::Quoted;
                break;
            case static_cast<wchar_t>(TgTitleForm::Square):
                form = TgTitleForm::Square;
                break;
            default:
                form = TgTitleForm::Plain;
                break;
            }
            square.assign(s.substr(pos + 2, mid - pos - 2));
            escapedTitle.assign(s.substr(mid + 1, close - mid - 1));
            return close + 1;
        }
    }

    /// <summary>
    /// A title TOKEN (§21.15): what a builder writes where a message names a session, instead of the title. The send layer
    /// renders it, classic (TgRenderTitleTokensClassic: the §21.13 square and title, in the given form) or as a chip
    /// (TgRichHtmlFromClassic). To every HTML helper in between (chunking, the plain-text fallback) a token is ordinary text:
    /// private-use characters frame it, and the title inside is already escaped. A title or square that carries one of the
    /// framing characters loses it here, so a token always parses.
    /// </summary>
    /// <param name="form">How the classic render reads (TgTitleForm).</param>
    /// <param name="colorSquare">TgTitleColorSquare of the tab color ("" = none).</param>
    /// <param name="title">The title as is (escaped here); the caller cuts it where its place wants it cut.</param>
    /// <returns>The token.</returns>
    /// <exception cref="std::bad_alloc">If the token cannot be allocated.</exception>
    inline std::wstring TgTitleToken(TgTitleForm form, std::wstring_view colorSquare, std::wstring_view title)
    {
        std::wstring clean;
        clean.reserve(title.size());
        for (const wchar_t c : title)
        {
            if (!detail::TgIsTitleTokenMarker(c))
            {
                clean += c;
            }
        }
        std::wstring token;
        token.reserve(colorSquare.size() + clean.size() + 16);
        token += kTgTitleTokenOpen;
        token += static_cast<wchar_t>(form);
        for (const wchar_t c : colorSquare)
        {
            if (!detail::TgIsTitleTokenMarker(c))
            {
                token += c;
            }
        }
        token += kTgTitleTokenMid;
        token += TgEscapeHtml(clean);
        token += kTgTitleTokenClose;
        return token;
    }

    /// <summary>Whether a text carries a title token (§21.15) — what makes the send layer render it.</summary>
    /// <param name="text">The text.</param>
    /// <returns>True when a token OPEN is in it.</returns>
    inline bool TgHasTitleTokens(std::wstring_view text) noexcept
    {
        return text.find(kTgTitleTokenOpen) != std::wstring_view::npos;
    }

    /// <summary>
    /// A text's title tokens rendered CLASSIC (§21.15): each token becomes the §21.13 mention in its form — "🟦 title",
    /// "🟦 &lt;b&gt;title&lt;/b&gt;", "“🟦 title”" or the bare square — so the result is byte for byte what the builders
    /// printed before the chips, and a card's hash taken over it is the hash its topic record already holds. A stray framing
    /// character is dropped.
    /// </summary>
    /// <param name="text">Telegram HTML with title tokens.</param>
    /// <returns>Telegram HTML without them (the text itself when it has none).</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring TgRenderTitleTokensClassic(std::wstring_view text)
    {
        if (std::find_if(text.begin(), text.end(), detail::TgIsTitleTokenMarker) == text.end())
        {
            return std::wstring{ text };
        }
        std::wstring out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size();)
        {
            const wchar_t c = text[i];
            if (c == kTgTitleTokenOpen)
            {
                TgTitleForm form{ TgTitleForm::Plain };
                std::wstring square;
                std::wstring title;
                const size_t end = detail::TgParseTitleToken(text, i, form, square, title);
                if (end == std::wstring_view::npos)
                {
                    ++i; // a stray OPEN: dropped
                    continue;
                }
                const std::wstring mention = square.empty() ? title : square + L" " + title;
                switch (form)
                {
                case TgTitleForm::Bold:
                    out += (square.empty() ? std::wstring{} : square + L" ") + L"<b>" + title + L"</b>";
                    break;
                case TgTitleForm::Quoted:
                    out += L"\x201C" + mention + L"\x201D";
                    break;
                case TgTitleForm::Square:
                    out += square.empty() ? mention : square;
                    break;
                default:
                    out += mention;
                    break;
                }
                i = end;
                continue;
            }
            if (!detail::TgIsTitleTokenMarker(c))
            {
                out += c;
            }
            ++i;
        }
        return out;
    }

    /// <summary>
    /// A title CHIP (§21.15): a copy_text button inside a rich message's text, in the look nearest the tab's color
    /// (TgTitleChipStyle), its label the square and the title, a tap copying the title. Measured on the dev bot (thread 26785,
    /// messages 904 / 905, 2026-10-17): Telegram keeps the button and its style inside a paragraph. The label is one line cut at
    /// kTgTitleChipLabelMaxChars; the copy is that line up to kTgCopyTextMaxChars. A blank title has nothing to copy (a copy text
    /// needs one character), so it stays the plain mention.
    /// </summary>
    /// <param name="colorSquare">TgTitleColorSquare of the tab color ("" = none: the plain look, no square in the label).</param>
    /// <param name="escapedTitle">The title escaped by TgEscapeHtml, as a token carries it.</param>
    /// <returns>Rich-message HTML: one &lt;tg-button&gt; element (or the plain mention for a blank title).</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring TgTitleChipHtml(std::wstring_view colorSquare, std::wstring_view escapedTitle)
    {
        // One line, trimmed: a line break or a tab would split the label of a button that sits inside a line of text.
        std::wstring line = detail::TgUnescapeOwnHtml(escapedTitle);
        for (auto& c : line)
        {
            if (c == L'\r' || c == L'\n' || c == L'\t')
            {
                c = L' ';
            }
        }
        const size_t a = line.find_first_not_of(L' ');
        if (a == std::wstring::npos)
        {
            return colorSquare.empty() ? std::wstring{ escapedTitle } : std::wstring{ colorSquare } + L" " + std::wstring{ escapedTitle };
        }
        line = line.substr(a, line.find_last_not_of(L' ') - a + 1);
        std::wstring label = line;
        if (label.size() > kTgTitleChipLabelMaxChars)
        {
            label = std::wstring{ detail::TgPrefixUnits(label, kTgTitleChipLabelMaxChars - 1) } + L"\x2026";
        }
        std::wstring chip = L"<tg-button type=\"copy_text\"";
        if (const wchar_t* style = TgTitleChipStyle(colorSquare); *style)
        {
            chip += L" style=\"";
            chip += style;
            chip += L"\"";
        }
        chip += L" text=\"" + detail::TgEscapeHtmlAttribute(detail::TgPrefixUnits(line, kTgCopyTextMaxChars)) + L"\">";
        if (!colorSquare.empty())
        {
            chip += std::wstring{ colorSquare } + L" ";
        }
        chip += TgEscapeHtml(label) + L"</tg-button>";
        return chip;
    }

    namespace detail
    {
        /// <summary>The index just past the HTML tag that starts at `pos` (a '&lt;'), quotes in its attributes respected.</summary>
        /// <param name="s">The text.</param>
        /// <param name="pos">The index of the '&lt;'.</param>
        /// <returns>The index after the tag's '&gt;'; npos when the tag never ends.</returns>
        inline size_t TgTagEnd(std::wstring_view s, size_t pos) noexcept
        {
            wchar_t quote = 0;
            for (size_t i = pos + 1; i < s.size(); ++i)
            {
                const wchar_t c = s[i];
                if (quote != 0)
                {
                    if (c == quote)
                    {
                        quote = 0;
                    }
                }
                else if (c == L'"' || c == L'\'')
                {
                    quote = c;
                }
                else if (c == L'>')
                {
                    return i + 1;
                }
            }
            return std::wstring_view::npos;
        }

        /// <summary>A tag's name in lower case: "b" for "&lt;b&gt;", "&lt;/b&gt;" and "&lt;b/&gt;".</summary>
        /// <param name="tag">The whole tag, '&lt;' to '&gt;'.</param>
        /// <returns>The name ("" for a malformed tag).</returns>
        /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
        inline std::wstring TgTagName(std::wstring_view tag)
        {
            size_t i = 1;
            if (i < tag.size() && tag[i] == L'/')
            {
                ++i;
            }
            std::wstring name;
            for (; i < tag.size(); ++i)
            {
                const wchar_t c = tag[i];
                if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-')
                {
                    name += c;
                }
                else if (c >= L'A' && c <= L'Z')
                {
                    name += static_cast<wchar_t>(c - L'A' + L'a');
                }
                else
                {
                    break;
                }
            }
            return name;
        }

        /// <summary>Whether a tag formats text inside a line (bold, a link, a spoiler …) rather than making a block.</summary>
        /// <param name="name">The tag's lower-case name.</param>
        /// <returns>True for an inline tag of Telegram's rich tag set.</returns>
        inline bool TgIsInlineTag(std::wstring_view name) noexcept
        {
            static constexpr std::wstring_view kInline[] = { L"b", L"strong", L"i", L"em", L"u", L"ins", L"s", L"strike", L"del", L"code", L"a",
                                                              L"tg-spoiler", L"span", L"mark", L"sub", L"sup", L"tg-emoji", L"tg-time" };
            for (const auto n : kInline)
            {
                if (n == name)
                {
                    return true;
                }
            }
            return false;
        }

        /// <summary>
        /// Drops the EMPTY inline pairs ("&lt;b&gt;&lt;/b&gt;", an empty link …) that a chip leaves when it is lifted out of the
        /// formatting around it (TgRichFromClassicCore), until none is left, so nested empty pairs go too.
        /// </summary>
        /// <param name="s">Rich-message HTML.</param>
        /// <returns>The same HTML without empty inline pairs.</returns>
        /// <exception cref="std::bad_alloc">If a tag name cannot be allocated.</exception>
        inline std::wstring TgDropEmptyInlinePairs(std::wstring s)
        {
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (size_t i = s.find(L'<'); i != std::wstring::npos; i = s.find(L'<', i + 1))
                {
                    if (i + 1 >= s.size() || s[i + 1] == L'/')
                    {
                        continue;
                    }
                    const size_t end = TgTagEnd(s, i);
                    if (end == std::wstring::npos)
                    {
                        break;
                    }
                    const std::wstring name = TgTagName(std::wstring_view{ s }.substr(i, end - i));
                    if (name.empty() || !TgIsInlineTag(name))
                    {
                        continue;
                    }
                    const std::wstring close = L"</" + name + L">";
                    if (s.compare(end, close.size(), close) == 0)
                    {
                        s.erase(i, end - i + close.size());
                        changed = true;
                        break; // the positions moved: scan again from the start
                    }
                }
            }
            return s;
        }

        /// <summary>Trims a rich paragraph's edges: spaces, tabs and line breaks ("&lt;br&gt;") before its first and after its last visible part.</summary>
        /// <param name="p">The paragraph's HTML, trimmed in place.</param>
        inline void TgTrimRichBreaks(std::wstring& p)
        {
            bool trimmed = true;
            while (trimmed)
            {
                trimmed = false;
                if (p.rfind(L"<br>", 0) == 0)
                {
                    p.erase(0, 4);
                    trimmed = true;
                }
                else if (!p.empty() && (p.front() == L' ' || p.front() == L'\t'))
                {
                    p.erase(0, 1);
                    trimmed = true;
                }
                if (p.size() >= 4 && p.compare(p.size() - 4, 4, L"<br>") == 0)
                {
                    p.erase(p.size() - 4);
                    trimmed = true;
                }
                else if (!p.empty() && (p.back() == L' ' || p.back() == L'\t'))
                {
                    p.pop_back();
                    trimmed = true;
                }
            }
        }

        /// <summary>
        /// The converter's core (TgRichHtmlFromClassic): with `blocks`, paragraphs and block elements; without (the body of a
        /// quote), one inline run in which a blank line is two line breaks.
        /// </summary>
        /// <param name="html">Classic Telegram HTML with title tokens.</param>
        /// <param name="blocks">True for a whole message, false for a quote's body.</param>
        /// <returns>Rich-message HTML.</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        inline std::wstring TgRichFromClassicCore(std::wstring_view html, bool blocks)
        {
            std::wstring out; // the finished blocks (blocks == true)
            std::wstring run; // the paragraph being built (the whole result when blocks == false)
            // The inline tags open in `run`, outermost first: names and opening tags. A chip closes them before itself and opens
            // them again after it, so a button never sits inside bold or italic: Telegram's own examples never put one there,
            // and a button's label may hold plain text only.
            std::vector<std::pair<std::wstring, std::wstring>> open;
            const auto flush = [&]() {
                std::wstring p = TgDropEmptyInlinePairs(std::move(run));
                run.clear();
                TgTrimRichBreaks(p);
                if (!p.empty())
                {
                    out += L"<p>" + p + L"</p>";
                }
            };
            const size_t n = html.size();
            size_t i = 0;
            while (i < n)
            {
                const wchar_t c = html[i];
                if (c == kTgTitleTokenOpen)
                {
                    TgTitleForm form{ TgTitleForm::Plain };
                    std::wstring square;
                    std::wstring title;
                    const size_t end = TgParseTitleToken(html, i, form, square, title);
                    if (end == std::wstring_view::npos)
                    {
                        ++i; // a stray OPEN: dropped
                        continue;
                    }
                    for (auto it = open.rbegin(); it != open.rend(); ++it)
                    {
                        run += L"</" + it->first + L">";
                    }
                    run += TgTitleChipHtml(square, title);
                    for (const auto& o : open)
                    {
                        run += o.second;
                    }
                    i = end;
                    continue;
                }
                if (TgIsTitleTokenMarker(c) || c == L'\r')
                {
                    ++i;
                    continue;
                }
                if (c == L'<')
                {
                    const size_t end = TgTagEnd(html, i);
                    if (end == std::wstring_view::npos)
                    {
                        run += L"&lt;"; // never from our builders (they escape '<'): shown as text, not as a broken tag
                        ++i;
                        continue;
                    }
                    const std::wstring_view tag = html.substr(i, end - i);
                    const bool closing = tag.size() > 1 && tag[1] == L'/';
                    const bool selfClosing = tag.size() > 2 && tag[tag.size() - 2] == L'/';
                    const std::wstring name = TgTagName(tag);
                    if (blocks && !closing && open.empty() && (name == L"blockquote" || name == L"pre"))
                    {
                        // A block: the paragraph before it ends, and the block goes whole. Our builders write these tags in lower
                        // case and never nest a quote, so the first closing tag is this block's.
                        const std::wstring closeTag = L"</" + name + L">";
                        const size_t closeAt = html.find(closeTag, end);
                        const size_t innerEnd = closeAt == std::wstring_view::npos ? n : closeAt;
                        flush();
                        std::wstring_view inner = html.substr(end, innerEnd - end);
                        out += tag;
                        if (name == L"pre")
                        {
                            out += TgRenderTitleTokensClassic(inner); // code keeps its line breaks; a title inside code stays text
                        }
                        else
                        {
                            // The quote's body: one run whose line breaks are "<br>" (the docs' rich quote is written that way).
                            while (!inner.empty() && (inner.front() == L'\n' || inner.front() == L'\r'))
                            {
                                inner.remove_prefix(1);
                            }
                            while (!inner.empty() && (inner.back() == L'\n' || inner.back() == L'\r'))
                            {
                                inner.remove_suffix(1);
                            }
                            std::wstring body = TgRichFromClassicCore(inner, false);
                            TgTrimRichBreaks(body);
                            out += body;
                        }
                        out += closeTag;
                        i = closeAt == std::wstring_view::npos ? n : closeAt + closeTag.size();
                        if (i < n && html[i] == L'\n')
                        {
                            ++i; // the line break that ends the block's line belongs to the block
                        }
                        continue;
                    }
                    if (closing)
                    {
                        for (size_t k = open.size(); k-- > 0;)
                        {
                            if (open[k].first == name)
                            {
                                open.erase(open.begin() + static_cast<std::ptrdiff_t>(k));
                                break;
                            }
                        }
                    }
                    else if (!selfClosing && TgIsInlineTag(name))
                    {
                        open.emplace_back(name, std::wstring{ tag });
                    }
                    run += tag;
                    i = end;
                    continue;
                }
                if (c == L'\n')
                {
                    size_t breaks = 0;
                    while (i < n && (html[i] == L'\n' || html[i] == L'\r'))
                    {
                        breaks += html[i] == L'\n' ? 1 : 0;
                        ++i;
                    }
                    if (blocks && breaks >= 2 && open.empty())
                    {
                        flush(); // a blank line: the paragraph ends
                    }
                    else
                    {
                        run += breaks >= 2 ? L"<br><br>" : L"<br>"; // inside formatting a paragraph cannot end: two breaks
                    }
                    continue;
                }
                run += c;
                ++i;
            }
            if (blocks)
            {
                flush();
                return out;
            }
            return TgDropEmptyInlinePairs(std::move(run));
        }
    }

    /// <summary>
    /// A classic Telegram HTML message as RICH-message HTML (§21.15), its title tokens as chips. Measured on the dev bot
    /// (thread 26785, 2026-10-17): rich HTML reads a raw line break as a space and runs the whole text into one paragraph
    /// (message 907), so the structure is made explicit — a blank line ends a paragraph ("&lt;p&gt;"), a line break is
    /// "&lt;br&gt;" (message 908: kept as a break), a quote or a code block is a block of its own. Every other tag (b, i, u, s,
    /// code, a, tg-spoiler — all in the rich tag set) and every entity stays as written. A chip is lifted out of the inline
    /// formatting around it (closed before it, opened again after it; the empty pairs that leaves are dropped).
    /// </summary>
    /// <param name="html">Classic Telegram HTML as the builders write it, with title tokens.</param>
    /// <returns>Rich-message HTML for rich_message.html ("" for a text with nothing visible).</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring TgRichHtmlFromClassic(std::wstring_view html)
    {
        return detail::TgRichFromClassicCore(html, true);
    }

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

    /// <summary>
    /// Formats a duration for a one-shot reply line: "3m", "1h12m", "2d4h", "45s". Never in a text that is re-rendered
    /// and compared (the card): a span changes every minute — use TgClockText there.
    /// </summary>
    /// <param name="ms">The span in milliseconds; a negative value reads as 0.</param>
    /// <returns>The short text.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
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

    /// <summary>
    /// The LOCAL calendar day of a moment as a number, yyyymmdd — what a history replay compares to put a day line
    /// between turns of different days (MESSAGING.md §21.9).
    /// </summary>
    /// <param name="unixMs">The moment (Unix ms); &lt;= 0 = unknown.</param>
    /// <returns>yyyymmdd; 0 when the moment is unknown or the CRT cannot convert it.</returns>
    inline int64_t TgDayKey(int64_t unixMs) noexcept
    {
        if (unixMs <= 0)
        {
            return 0;
        }
        const time_t t = static_cast<time_t>(unixMs / 1000);
        std::tm when{};
        if (localtime_s(&when, &t) != 0) // the CRT's thread-safe form; it writes nothing on failure
        {
            return 0;
        }
        return static_cast<int64_t>(when.tm_year + 1900) * 10000 + static_cast<int64_t>(when.tm_mon + 1) * 100 + when.tm_mday;
    }

    /// <summary>A history replay's day line: the LOCAL day of a moment, "Fri 16 Oct 2026".</summary>
    /// <param name="unixMs">The moment (Unix ms).</param>
    /// <returns>The day; "?" when the CRT cannot convert the moment.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring TgDayText(int64_t unixMs)
    {
        const time_t t = static_cast<time_t>(unixMs / 1000);
        std::tm when{};
        if (unixMs <= 0 || localtime_s(&when, &t) != 0)
        {
            return L"?";
        }
        static constexpr const wchar_t* kDays[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
        static constexpr const wchar_t* kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        const int wday = when.tm_wday >= 0 && when.tm_wday < 7 ? when.tm_wday : 0;
        const int mon = when.tm_mon >= 0 && when.tm_mon < 12 ? when.tm_mon : 0;
        return std::wstring{ kDays[wday] } + L" " + std::to_wstring(when.tm_mday) + L" " + kMonths[mon] + L" " + std::to_wstring(when.tm_year + 1900);
    }
}
