// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TEAMS_CHANNEL.md: the bot-less Teams connector's PURE core (header-only, no Win32, no I/O; tested by
// tests\tests_teams.cpp). The connector writes AS THE OWNER through Microsoft Graph into one private channel: a thread per
// session (the root post is the session's card), emoji reactions as the buttons, an @mention of the owner as the alert,
// and polling for the owner's replies and reactions. Everything here decides or formats; TeamsService does the calls.
//
//   1. Text helpers: UTF-8 <-> UTF-16, Base64url, Graph's ISO times, the plain text of a Teams HTML body.
//   2. Emoji: the legend's emoji in the exact forms Graph accepts, the keycaps, the comparison without U+FE0F.
//   3. Actions: which emoji on which kind of message is which action (TEAMS_CHANNEL.md §8), and which need a confirm.
//   4. The legend post and the session card (subject, body, the card's own legend line, the change hash).
//   5. Post bodies: the Graph JSON of a post, with the owner's @mention and the importance of an alert (§6.4).
//   6. Telegram HTML -> Teams HTML, and a post split under Graph's size limit (§6.2).
//   7. The poll's cadence (§7.3).
//   8. Reactions: read from a message, compared with the last read, and the journal of Agentmaster's own.
//   9. Tokens: the JWT claims a token carries, and which token may make which Graph call (§4.2).
//  10. Inbound messages: parsed, and the rule for "the owner wrote a new post" (§7.2, §9.1).
//  11. The channel state (state-<channel>.json): threads, watched messages, the journal, the delta link.
//  12. Graph addresses (a token only ever goes to Graph), the paths of a channel's messages, the sign-in forms, errors.
//  13. The sign-in's answers: the device code, the token endpoint, where a sign-in stands, the Azure CLI's output.
//  14. The refresh token's parts in Credential Manager (one credential holds 2,560 bytes).
//  15. Pacing (the send gap, retries, a 429, a 403's memory), the state file's name, the cog's status line.
//  16. The service's settings (TeamsConfig from AppSettings) and when a change restarts it.
//  17. Threads, the card and the mirror: who gets a thread, the card's facts from a session, the posts' Teams forms, the
//      live mirror's decisions (held texts, the final reply, alerts, the commit that never passes a held text), the
//      history's packing and where it hands over to the live mirror.
//  18. Inbound: a delta page, the owner's new replies in a thread, a prompt's status emoji, the backlog offer, the
//      answers to refused commands and the commands' help.
//  19. Emoji actions: the confirm of a restart or a park mid-turn, the screen's text as a post, what is not here yet.
//  20. Pictures in a post: Base64, and a reply that shows a PNG as its own hosted content.
//  21. A new session from a new post: the folder step's text and the payload its watch keeps.
//  22. The whole conversation as pictures (📜, /allscreen): the part height, the captions, the cut notice, the usage.
//  23. A fork from Teams (🔀, /forksession): the situations, what each answer launches, the question, its payload.
//  24. The 🔎 Search thread: its root, an answer's lines, a reply classified.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "InterruptedTurns.h" // ForkTurnChoice: what a fork question's answers launch (section 23)
#include "Json.h"
#include "MessagingModels.h" // StateGlyph, StateLabel, TopicDisplayName, TgTitleColorSquare, TgEscapeHtml, CardButtonFacts
#include "SessionModels.h"

namespace Agentmaster::Teams
{
    // ===== 1. Text helpers =================================================================================================

    /// <summary>
    /// The number of UTF-8 bytes a UTF-16 text takes, counted without building it: Graph limits a post by its size on the
    /// wire (JSON in UTF-8), and the split (§6.2) measures every piece with this. A lone surrogate counts as U+FFFD (3 bytes),
    /// which is what Utf8FromUtf16 writes for it.
    /// </summary>
    /// <param name="w">The text.</param>
    /// <returns>Its size in UTF-8 bytes.</returns>
    inline size_t Utf8Length(std::wstring_view w) noexcept
    {
        size_t n = 0;
        for (size_t i = 0; i < w.size(); ++i)
        {
            const uint32_t c = static_cast<uint16_t>(w[i]);
            if (c < 0x80)
            {
                n += 1;
            }
            else if (c < 0x800)
            {
                n += 2;
            }
            else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && static_cast<uint16_t>(w[i + 1]) >= 0xDC00 &&
                     static_cast<uint16_t>(w[i + 1]) <= 0xDFFF)
            {
                // A valid surrogate pair: one code point above U+FFFF, 4 bytes.
                n += 4;
                ++i;
            }
            else
            {
                // A BMP code point, or a lone surrogate written as U+FFFD: 3 bytes either way.
                n += 3;
            }
        }
        return n;
    }

    /// <summary>
    /// UTF-16 to UTF-8: what goes on the wire to Graph. A lone surrogate becomes U+FFFD, the same rule as the Telegram
    /// connector's FlUtf8 (a text that cannot round-trip is never sent half-broken).
    /// </summary>
    /// <param name="w">The text.</param>
    /// <returns>The UTF-8 bytes.</returns>
    /// <exception cref="std::bad_alloc">If the bytes cannot be allocated.</exception>
    inline std::string Utf8FromUtf16(std::wstring_view w)
    {
        std::string out;
        out.reserve(w.size() + w.size() / 2);
        for (size_t i = 0; i < w.size(); ++i)
        {
            uint32_t cp = static_cast<uint16_t>(w[i]);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size())
            {
                const uint32_t lo = static_cast<uint16_t>(w[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                }
                else
                {
                    cp = 0xFFFD;
                }
            }
            else if (cp >= 0xD800 && cp <= 0xDFFF)
            {
                cp = 0xFFFD;
            }
            if (cp < 0x80)
            {
                out.push_back(static_cast<char>(cp));
            }
            else if (cp < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        return out;
    }

    /// <summary>
    /// UTF-8 to UTF-16: what Graph's answers and a token's claims become. A malformed or cut sequence becomes U+FFFD and the
    /// decoding goes on at the next byte, so one bad byte never hides the rest of an answer.
    /// </summary>
    /// <param name="s">The UTF-8 bytes.</param>
    /// <returns>The text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring Utf16FromUtf8(std::string_view s)
    {
        std::wstring out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size())
        {
            const unsigned char lead = static_cast<unsigned char>(s[i]);
            uint32_t cp = 0xFFFD;
            size_t len = 1;
            if (lead < 0x80)
            {
                cp = lead;
            }
            else if ((lead & 0xE0) == 0xC0)
            {
                len = 2;
                cp = lead & 0x1F;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                len = 3;
                cp = lead & 0x0F;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                len = 4;
                cp = lead & 0x07;
            }
            if (len > 1)
            {
                if (i + len > s.size())
                {
                    // A sequence cut by the end of the input: one replacement character, then the next byte.
                    cp = 0xFFFD;
                    len = 1;
                }
                else
                {
                    for (size_t k = 1; k < len; ++k)
                    {
                        const unsigned char cont = static_cast<unsigned char>(s[i + k]);
                        if ((cont & 0xC0) != 0x80)
                        {
                            cp = 0xFFFD;
                            len = 1;
                            break;
                        }
                        cp = (cp << 6) | (cont & 0x3F);
                    }
                }
            }
            if (cp >= 0x10000 && cp <= 0x10FFFF)
            {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            }
            else if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            {
                out.push_back(L'\xFFFD');
            }
            else
            {
                out.push_back(static_cast<wchar_t>(cp));
            }
            i += len;
        }
        return out;
    }

    /// <summary>
    /// Base64url (RFC 4648 §5) to bytes, padding optional: a JWT's payload (§9). The standard alphabet's '+' and '/' are
    /// read too, so a token copied through a tool that rewrote it still decodes.
    /// </summary>
    /// <param name="s">The encoded text.</param>
    /// <returns>The bytes, or nothing when a character is outside both alphabets.</returns>
    /// <exception cref="std::bad_alloc">If the bytes cannot be allocated.</exception>
    inline std::optional<std::string> Base64UrlDecode(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() * 3 / 4 + 3);
        uint32_t acc = 0;
        int bits = 0;
        for (const char c : s)
        {
            if (c == '=')
            {
                break;
            }
            int v = -1;
            if (c >= 'A' && c <= 'Z')
            {
                v = c - 'A';
            }
            else if (c >= 'a' && c <= 'z')
            {
                v = c - 'a' + 26;
            }
            else if (c >= '0' && c <= '9')
            {
                v = c - '0' + 52;
            }
            else if (c == '-' || c == '+')
            {
                v = 62;
            }
            else if (c == '_' || c == '/')
            {
                v = 63;
            }
            if (v < 0)
            {
                return std::nullopt;
            }
            // Six bits in; a byte out every time eight are waiting. The high bits that overflow `acc` are already written.
            acc = (acc << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(static_cast<char>((acc >> bits) & 0xFF));
            }
        }
        return out;
    }

    /// <summary>
    /// The days from 1970-01-01 to a civil date (proleptic Gregorian; H. Hinnant's days_from_civil): the clock of Graph's
    /// ISO times without the C runtime's time zone.
    /// </summary>
    /// <param name="y">The year.</param>
    /// <param name="m">The month, 1-12.</param>
    /// <param name="d">The day, 1-31.</param>
    /// <returns>The day number (0 = 1970-01-01).</returns>
    inline int64_t DaysFromCivil(int64_t y, int m, int d) noexcept
    {
        y -= (m <= 2) ? 1 : 0;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const int64_t yoe = y - era * 400; // [0, 399]
        const int64_t mp = (m + 9) % 12; // March = 0 … February = 11
        const int64_t doy = (153 * mp + 2) / 5 + d - 1; // [0, 365]
        const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
        return era * 146097 + doe - 719468;
    }

    /// <summary>
    /// A Graph time ("2026-11-18T11:42:29.199Z", any number of fraction digits, or an offset "+02:00") as Unix ms. Graph
    /// writes UTC with "Z"; the offset form is read for safety.
    /// </summary>
    /// <param name="iso">The time.</param>
    /// <returns>Unix ms, or 0 when the text is not such a time (0 never counts as "newer").</returns>
    inline int64_t ParseGraphTimeMs(std::wstring_view iso) noexcept
    {
        // Reads `count` digits at `at` into `out`; false when one is not a digit or the text is too short.
        const auto digits = [&](size_t at, size_t count, int64_t& out) noexcept {
            if (at + count > iso.size())
            {
                return false;
            }
            out = 0;
            for (size_t k = 0; k < count; ++k)
            {
                const wchar_t c = iso[at + k];
                if (c < L'0' || c > L'9')
                {
                    return false;
                }
                out = out * 10 + (c - L'0');
            }
            return true;
        };
        int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        if (!digits(0, 4, y) || iso.size() < 19 || iso[4] != L'-' || !digits(5, 2, mo) || iso[7] != L'-' || !digits(8, 2, d) ||
            (iso[10] != L'T' && iso[10] != L't' && iso[10] != L' ') || !digits(11, 2, h) || iso[13] != L':' || !digits(14, 2, mi) ||
            iso[16] != L':' || !digits(17, 2, s))
        {
            return 0;
        }
        if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
        {
            return 0;
        }
        size_t at = 19;
        int64_t ms = 0;
        if (at < iso.size() && iso[at] == L'.')
        {
            // The first three fraction digits are the ms; any more are read past.
            ++at;
            int taken = 0;
            while (at < iso.size() && iso[at] >= L'0' && iso[at] <= L'9')
            {
                if (taken < 3)
                {
                    ms = ms * 10 + (iso[at] - L'0');
                    ++taken;
                }
                ++at;
            }
            while (taken < 3)
            {
                ms *= 10;
                ++taken;
            }
        }
        int64_t offsetMs = 0;
        if (at < iso.size() && (iso[at] == L'+' || iso[at] == L'-'))
        {
            int64_t oh = 0, om = 0;
            if (digits(at + 1, 2, oh) && at + 3 < iso.size() && iso[at + 3] == L':' && digits(at + 4, 2, om))
            {
                offsetMs = (oh * 60 + om) * 60'000 * (iso[at] == L'+' ? 1 : -1);
            }
        }
        const int64_t days = DaysFromCivil(y, static_cast<int>(mo), static_cast<int>(d));
        return ((days * 24 + h) * 60 + mi) * 60'000 + s * 1000 + ms - offsetMs;
    }

    namespace detail
    {
        /// <summary>
        /// The lower-case name of an HTML tag ("&lt;Pre class=x&gt;" → "pre", "&lt;/p&gt;" → "/p", "&lt;br/&gt;" → "br"): what the
        /// adapter, the split and the plain-text reader decide by. ASCII letters only are folded; HTML tag names are ASCII.
        /// </summary>
        /// <param name="tag">The whole tag, from '&lt;' to '&gt;'.</param>
        /// <returns>The name, with a leading '/' for a closing tag.</returns>
        /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
        inline std::wstring TagNameLower(std::wstring_view tag)
        {
            std::wstring name;
            size_t i = 1;
            if (i < tag.size() && tag[i] == L'/')
            {
                name.push_back(L'/');
                ++i;
            }
            while (i < tag.size())
            {
                const wchar_t c = tag[i];
                if (c == L'>' || c == L'/' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n')
                {
                    break;
                }
                name.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c);
                ++i;
            }
            return name;
        }

        /// <summary>Is a character white space for the trims here (space, tab, CR, LF, NBSP)?</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for white space.</returns>
        inline bool IsSpace(wchar_t c) noexcept
        {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\xA0';
        }

        /// <summary>A text without the white space at both ends (IsSpace).</summary>
        /// <param name="s">The text.</param>
        /// <returns>The trimmed text.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring Trim(std::wstring_view s)
        {
            size_t a = 0;
            size_t b = s.size();
            while (a < b && IsSpace(s[a]))
            {
                ++a;
            }
            while (b > a && IsSpace(s[b - 1]))
            {
                --b;
            }
            return std::wstring{ s.substr(a, b - a) };
        }

        /// <summary>Appends a code point (any plane) as UTF-16; one outside Unicode becomes U+FFFD.</summary>
        /// <param name="out">The text to append to.</param>
        /// <param name="cp">The code point.</param>
        /// <exception cref="std::bad_alloc">If the text cannot grow.</exception>
        inline void AppendCodePoint(std::wstring& out, uint32_t cp)
        {
            if (cp >= 0x10000 && cp <= 0x10FFFF)
            {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            }
            else if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0)
            {
                out.push_back(L'\xFFFD');
            }
            else
            {
                out.push_back(static_cast<wchar_t>(cp));
            }
        }

        /// <summary>
        /// Escapes text for an HTML attribute value in double quotes (an href): &amp;, &lt;, &gt;, " and '. The Telegram
        /// escaper (TgEscapeHtml) leaves quotes alone, which is right for text and wrong inside an attribute.
        /// </summary>
        /// <param name="s">The value.</param>
        /// <returns>The escaped value.</returns>
        /// <exception cref="std::bad_alloc">If the value cannot be allocated.</exception>
        inline std::wstring EscapeAttribute(std::wstring_view s)
        {
            std::wstring out;
            out.reserve(s.size() + 8);
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
                case L'"':
                    out += L"&quot;";
                    break;
                case L'\'':
                    out += L"&#39;";
                    break;
                default:
                    out.push_back(c);
                    break;
                }
            }
            return out;
        }
    }

    /// <summary>
    /// The plain text of a Teams message body (HTML): what the owner typed, as a prompt or a command (§7.5). Tags go; a line
    /// break, a paragraph end, a list item end and a table row end become a newline; the common entities and numeric
    /// references are decoded; the result is trimmed. A plain-text body (contentType "text") passes through unchanged but
    /// trimmed, as it has no tags.
    /// </summary>
    /// <remarks>Not an HTML parser: Teams writes simple, well-formed bodies (&lt;p&gt;, &lt;br&gt;, &lt;div&gt;, inline
    /// styles), and an unclosed '&lt;' ends the reading rather than guessing.</remarks>
    /// <param name="html">The body.</param>
    /// <returns>The text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring PlainTextFromTeamsHtml(std::wstring_view html)
    {
        std::wstring out;
        out.reserve(html.size());
        size_t i = 0;
        while (i < html.size())
        {
            const wchar_t c = html[i];
            if (c == L'<')
            {
                const size_t close = html.find(L'>', i);
                if (close == std::wstring_view::npos)
                {
                    break;
                }
                const std::wstring name = detail::TagNameLower(html.substr(i, close - i + 1));
                if (name == L"br" || name == L"/p" || name == L"/div" || name == L"/li" || name == L"/tr" || name == L"/h1" ||
                    name == L"/h2" || name == L"/h3" || name == L"/blockquote" || name == L"/pre")
                {
                    out.push_back(L'\n');
                }
                i = close + 1;
                continue;
            }
            if (c == L'&')
            {
                const size_t semi = html.find(L';', i);
                if (semi != std::wstring_view::npos && semi - i <= 10)
                {
                    const std::wstring_view ent = html.substr(i + 1, semi - i - 1);
                    bool decoded = true;
                    if (ent == L"amp")
                    {
                        out.push_back(L'&');
                    }
                    else if (ent == L"lt")
                    {
                        out.push_back(L'<');
                    }
                    else if (ent == L"gt")
                    {
                        out.push_back(L'>');
                    }
                    else if (ent == L"quot")
                    {
                        out.push_back(L'"');
                    }
                    else if (ent == L"apos")
                    {
                        out.push_back(L'\'');
                    }
                    else if (ent == L"nbsp")
                    {
                        out.push_back(L' ');
                    }
                    else if (ent.size() >= 2 && ent[0] == L'#')
                    {
                        // &#NNN; or &#xHH;
                        uint32_t cp = 0;
                        const bool hex = ent[1] == L'x' || ent[1] == L'X';
                        bool ok = ent.size() > (hex ? 2u : 1u);
                        for (size_t k = hex ? 2 : 1; ok && k < ent.size(); ++k)
                        {
                            const wchar_t d = ent[k];
                            uint32_t v = 0;
                            if (d >= L'0' && d <= L'9')
                            {
                                v = d - L'0';
                            }
                            else if (hex && d >= L'a' && d <= L'f')
                            {
                                v = d - L'a' + 10;
                            }
                            else if (hex && d >= L'A' && d <= L'F')
                            {
                                v = d - L'A' + 10;
                            }
                            else
                            {
                                ok = false;
                                break;
                            }
                            cp = cp * (hex ? 16 : 10) + v;
                            if (cp > 0x10FFFF)
                            {
                                ok = false;
                            }
                        }
                        if (ok)
                        {
                            detail::AppendCodePoint(out, cp);
                        }
                        else
                        {
                            decoded = false;
                        }
                    }
                    else
                    {
                        decoded = false;
                    }
                    if (decoded)
                    {
                        i = semi + 1;
                        continue;
                    }
                }
            }
            out.push_back(c);
            ++i;
        }
        return detail::Trim(out);
    }

    // ===== 2. Emoji =========================================================================================================
    //
    // Graph checks a reaction against the Teams emoji set and refuses a form without its variation selector (🖼 without
    // U+FE0F answered 400, TEAMS.md §15.4), so every emoji Agentmaster SENDS is written here in its fully qualified form,
    // and every emoji it READS is compared through NormalizeEmoji, which drops the selectors.

    inline constexpr std::wstring_view kEmojiScreen = L"\U0001F5BC\uFE0F"; // 🖼️ a card: send the screen
    inline constexpr std::wstring_view kEmojiStatus = L"\U0001F4CA"; // 📊 a card: /status
    inline constexpr std::wstring_view kEmojiEsc = L"\U0001F6D1"; // 🛑 a card: one Esc into the running turn
    inline constexpr std::wstring_view kEmojiEnter = L"↩\uFE0F"; // ↩️ a card: one Enter on the waiting prompt
    inline constexpr std::wstring_view kEmojiAllScreen = L"\U0001F4DC"; // 📜 a card: the whole conversation as pictures
    inline constexpr std::wstring_view kEmojiFork = L"\U0001F500"; // 🔀 a card: fork the session
    inline constexpr std::wstring_view kEmojiPark = L"\U0001F319"; // 🌙 a card: park (deactivate) the tab
    inline constexpr std::wstring_view kEmojiWake = L"☀\uFE0F"; // ☀️ a card: wake a parked tab
    inline constexpr std::wstring_view kEmojiRestart = L"\U0001F504"; // 🔄 a card: restart the session (always confirmed)
    inline constexpr std::wstring_view kEmojiYes = L"✅"; // ✅ a question's send, a confirm's yes, a backlog's send
    inline constexpr std::wstring_view kEmojiNo = L"❌"; // ❌ a question's cancel, a queued prompt's unqueue, a backlog's drop
    inline constexpr std::wstring_view kEmojiNew = L"➕"; // ➕ the legend: start a new session
    inline constexpr std::wstring_view kEmojiKeycapTen = L"\U0001F51F"; // 🔟 the tenth option (1️⃣–9️⃣ are built by KeycapEmoji)

    // Agentmaster's own STATUS reactions on the owner's prompts (§6.1). None of them is an action on any kind of message, so
    // a status reaction that reads as the owner's (Agentmaster writes as the owner) can never run an action.
    inline constexpr std::wstring_view kEmojiSeen = L"\U0001F440"; // 👀 queued, seen by Agentmaster
    inline constexpr std::wstring_view kEmojiWorking = L"\U0001F468\u200D\U0001F4BB"; // 👨‍💻 its turn is running
    inline constexpr std::wstring_view kEmojiFromQueue = L"\U0001FAE1"; // 🫡 sent from the queue (it waited)
    inline constexpr std::wstring_view kEmojiDone = L"\U0001F44C"; // 👌 its turn ended; an action was done
    inline constexpr std::wstring_view kEmojiWarn = L"⚠\uFE0F"; // ⚠️ an error; an action was refused

    /// <summary>
    /// An emoji without its variation selectors (U+FE0F, U+FE0E): the form two emoji are compared in. Teams may report a
    /// reaction with or without the selector, and Agentmaster sends the selector form Graph insists on.
    /// </summary>
    /// <param name="emoji">The emoji.</param>
    /// <returns>The emoji without selectors.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NormalizeEmoji(std::wstring_view emoji)
    {
        std::wstring out;
        out.reserve(emoji.size());
        for (const wchar_t c : emoji)
        {
            if (c != L'\xFE0F' && c != L'\xFE0E')
            {
                out.push_back(c);
            }
        }
        return out;
    }

    /// <summary>Are two emoji the same, selectors aside (NormalizeEmoji)?</summary>
    /// <param name="a">One emoji.</param>
    /// <param name="b">The other.</param>
    /// <returns>True when they are the same emoji.</returns>
    /// <exception cref="std::bad_alloc">If a normalized form cannot be allocated.</exception>
    inline bool SameEmoji(std::wstring_view a, std::wstring_view b)
    {
        return NormalizeEmoji(a) == NormalizeEmoji(b);
    }

    /// <summary>
    /// The keycap emoji of an option number: 1️⃣–9️⃣ (the digit, U+FE0F, U+20E3 — the fully qualified form Graph accepts) and
    /// 🔟 for 10. A question offers at most ten options this way (AskUserQuestion offers at most four).
    /// </summary>
    /// <param name="n">The number, 1-10.</param>
    /// <returns>The emoji, or "" for a number outside 1-10.</returns>
    /// <exception cref="std::bad_alloc">If the emoji cannot be allocated.</exception>
    inline std::wstring KeycapEmoji(int n)
    {
        if (n >= 1 && n <= 9)
        {
            std::wstring s;
            s.push_back(static_cast<wchar_t>(L'0' + n));
            s.push_back(L'\xFE0F');
            s.push_back(L'\x20E3');
            return s;
        }
        if (n == 10)
        {
            return std::wstring{ kEmojiKeycapTen };
        }
        return {};
    }

    /// <summary>The option number a keycap emoji stands for (KeycapEmoji's inverse, selectors ignored).</summary>
    /// <param name="emoji">The emoji.</param>
    /// <returns>1-10, or nothing when the emoji is no keycap of 1-10.</returns>
    /// <exception cref="std::bad_alloc">If a normalized form cannot be allocated.</exception>
    inline std::optional<int> KeycapNumber(std::wstring_view emoji)
    {
        const std::wstring e = NormalizeEmoji(emoji);
        if (e.size() == 2 && e[0] >= L'1' && e[0] <= L'9' && e[1] == L'\x20E3')
        {
            return static_cast<int>(e[0] - L'0');
        }
        if (e == NormalizeEmoji(kEmojiKeycapTen))
        {
            return 10;
        }
        return std::nullopt;
    }

    /// <summary>Is an emoji one of Agentmaster's own status reactions (👀 👨‍💻 🫡 👌 ⚠️), which are never actions?</summary>
    /// <param name="emoji">The emoji.</param>
    /// <returns>True for a status emoji.</returns>
    /// <exception cref="std::bad_alloc">If a normalized form cannot be allocated.</exception>
    inline bool IsStatusEmoji(std::wstring_view emoji)
    {
        const std::wstring e = NormalizeEmoji(emoji);
        for (const std::wstring_view s : { kEmojiSeen, kEmojiWorking, kEmojiFromQueue, kEmojiDone, kEmojiWarn })
        {
            if (e == NormalizeEmoji(s))
            {
                return true;
            }
        }
        return false;
    }

    // ===== 3. Actions =======================================================================================================

    /// <summary>What a message in the channel is, for the reactions it takes (§8). Persisted by word (MessageKindWord).</summary>
    enum class MessageKind
    {
        /// <summary>A session thread's root: the card. Takes the card actions.</summary>
        SessionCard,
        /// <summary>A question (AskUserQuestion, an approval, a menu): takes the keycaps, ✅ and ❌.</summary>
        Question,
        /// <summary>A confirm request ("Restart …? React ✅ within 60 s"): takes ✅ and ❌.</summary>
        Confirm,
        /// <summary>The owner's prompt while it waits in the queue: takes ❌ (unqueue).</summary>
        QueuedPrompt,
        /// <summary>The legend post: takes ➕ (a new session).</summary>
        Legend,
        /// <summary>The answer to a reply written while Agentmaster was closed: takes ✅ (send it) and ❌ (drop it).</summary>
        Backlog,
        /// <summary>A step of the new-session wizard (folder, model, window): takes the keycaps.</summary>
        Step,
        /// <summary>
        /// A fork question (🔀, HOOKS.md "Interrupted turns"): takes 1️⃣ (fork interrupted), 2️⃣ (fork uninterrupted) and ❌ (no
        /// fork). Persisted as "fork".
        /// </summary>
        ForkChoice,
        /// <summary>Anything else: takes nothing.</summary>
        Other
    };

    /// <summary>An action a reaction asks for (§8).</summary>
    enum class Action
    {
        /// <summary>No action: the emoji means nothing on this kind of message.</summary>
        None,
        /// <summary>🖼️: post the session's screen.</summary>
        Screen,
        /// <summary>📊: post the /status report.</summary>
        Status,
        /// <summary>🛑: one Esc into the running turn.</summary>
        Esc,
        /// <summary>↩️: one Enter on the prompt waiting in the box.</summary>
        Enter,
        /// <summary>📜: the whole conversation as pictures.</summary>
        AllScreen,
        /// <summary>🔀: fork the session.</summary>
        Fork,
        /// <summary>🌙: park the tab.</summary>
        Park,
        /// <summary>☀️: wake a parked tab.</summary>
        Wake,
        /// <summary>🔄: restart the session.</summary>
        Restart,
        /// <summary>A keycap: pick option ReactionAction::pick.</summary>
        Pick,
        /// <summary>✅: yes, send, confirm.</summary>
        Yes,
        /// <summary>❌: no, cancel, drop, unqueue.</summary>
        No,
        /// <summary>➕: start a new session.</summary>
        NewSession
    };

    /// <summary>The action of one reaction, with the option number of a pick.</summary>
    struct ReactionAction
    {
        /// <summary>The action (None = the reaction is ignored).</summary>
        Action action{ Action::None };
        /// <summary>The option number, 1-10, for Action::Pick; 0 otherwise.</summary>
        int pick{ 0 };
    };

    /// <summary>
    /// What an emoji asks for on a message of a given kind (§8, TEAMS.md §15.4's legend). Selectors are ignored. An emoji
    /// outside the kind's set — Agentmaster's own status emoji included — is no action, so a reaction meant as a feeling
    /// never runs anything.
    /// </summary>
    /// <param name="kind">The message's kind.</param>
    /// <param name="emoji">The reaction's emoji.</param>
    /// <returns>The action (None when it means nothing here).</returns>
    /// <exception cref="std::bad_alloc">If a normalized form cannot be allocated.</exception>
    inline ReactionAction ActionForReaction(MessageKind kind, std::wstring_view emoji)
    {
        const std::wstring e = NormalizeEmoji(emoji);
        const auto is = [&](std::wstring_view candidate) { return e == NormalizeEmoji(candidate); };
        ReactionAction r;
        switch (kind)
        {
        case MessageKind::SessionCard:
            if (is(kEmojiScreen))
            {
                r.action = Action::Screen;
            }
            else if (is(kEmojiStatus))
            {
                r.action = Action::Status;
            }
            else if (is(kEmojiEsc))
            {
                r.action = Action::Esc;
            }
            else if (is(kEmojiEnter))
            {
                r.action = Action::Enter;
            }
            else if (is(kEmojiAllScreen))
            {
                r.action = Action::AllScreen;
            }
            else if (is(kEmojiFork))
            {
                r.action = Action::Fork;
            }
            else if (is(kEmojiPark))
            {
                r.action = Action::Park;
            }
            else if (is(kEmojiWake))
            {
                r.action = Action::Wake;
            }
            else if (is(kEmojiRestart))
            {
                r.action = Action::Restart;
            }
            break;
        case MessageKind::Question:
        case MessageKind::Step:
            if (const auto n = KeycapNumber(e))
            {
                r.action = Action::Pick;
                r.pick = *n;
            }
            else if (kind == MessageKind::Question && is(kEmojiYes))
            {
                r.action = Action::Yes;
            }
            else if (kind == MessageKind::Question && is(kEmojiNo))
            {
                r.action = Action::No;
            }
            break;
        case MessageKind::Confirm:
        case MessageKind::Backlog:
            if (is(kEmojiYes))
            {
                r.action = Action::Yes;
            }
            else if (is(kEmojiNo))
            {
                r.action = Action::No;
            }
            break;
        case MessageKind::QueuedPrompt:
            if (is(kEmojiNo))
            {
                r.action = Action::No;
            }
            break;
        case MessageKind::Legend:
            if (is(kEmojiNew))
            {
                r.action = Action::NewSession;
            }
            break;
        case MessageKind::ForkChoice:
            // Only the question's two numbers: a 3️⃣ means nothing here (the step's own keycaps go to MessageKind::Step).
            if (const auto pick = KeycapNumber(e); pick && (*pick == 1 || *pick == 2))
            {
                r.action = Action::Pick;
                r.pick = *pick;
            }
            else if (is(kEmojiNo))
            {
                r.action = Action::No;
            }
            break;
        case MessageKind::Other:
        default:
            break;
        }
        return r;
    }

    /// <summary>
    /// Does an action wait for a ✅ on a confirm reply (§8)? A restart always does (it ends a claude); a park does while a
    /// turn runs (it cuts the turn off), the same rule as Telegram's 🌙 ("asks first mid-turn").
    /// </summary>
    /// <param name="a">The action.</param>
    /// <param name="turnInFlight">A turn runs now (CardButtonFacts::turnInFlight).</param>
    /// <returns>True when Agentmaster asks first.</returns>
    inline bool ActionNeedsConfirm(Action a, bool turnInFlight) noexcept
    {
        return a == Action::Restart || (a == Action::Park && turnInFlight);
    }

    /// <summary>An action's word for the log and the replies ("screen", "restart", "pick", …).</summary>
    /// <param name="a">The action.</param>
    /// <returns>The word (a static string).</returns>
    inline const wchar_t* ActionWord(Action a) noexcept
    {
        switch (a)
        {
        case Action::Screen:
            return L"screen";
        case Action::Status:
            return L"status";
        case Action::Esc:
            return L"esc";
        case Action::Enter:
            return L"enter";
        case Action::AllScreen:
            return L"allscreen";
        case Action::Fork:
            return L"fork";
        case Action::Park:
            return L"park";
        case Action::Wake:
            return L"wake";
        case Action::Restart:
            return L"restart";
        case Action::Pick:
            return L"pick";
        case Action::Yes:
            return L"yes";
        case Action::No:
            return L"no";
        case Action::NewSession:
            return L"newsession";
        case Action::None:
        default:
            return L"none";
        }
    }

    // ===== 4. The legend post and the session card ========================================================================

    /// <summary>One row of the legend table (§8): on which message, which emoji, what it does.</summary>
    struct LegendRow
    {
        /// <summary>The kind of message ("a session card"); "" continues the row above.</summary>
        std::wstring_view on;
        /// <summary>The emoji, as shown.</summary>
        std::wstring_view emoji;
        /// <summary>What the emoji does.</summary>
        std::wstring_view action;
    };

    /// <summary>
    /// The legend's rows, in the order the legend post shows them (TEAMS.md §15.4). One table, so the post, the card's line
    /// and the docs cannot drift apart: the tests hold ActionForReaction to it.
    /// </summary>
    /// <returns>The rows (a static table).</returns>
    inline const std::vector<LegendRow>& LegendRows()
    {
        static const std::vector<LegendRow> rows{
            { L"a session card", kEmojiScreen, L"send the screen now" },
            { L"", kEmojiStatus, L"status (model, context, usage)" },
            { L"", kEmojiEsc, L"Esc: stop the running turn" },
            { L"", kEmojiEnter, L"Enter: send the prompt waiting in the box" },
            { L"", kEmojiAllScreen, L"the whole conversation as pictures" },
            { L"", kEmojiFork, L"fork the session" },
            { L"", kEmojiPark, L"park the tab (asks first during a turn)" },
            { L"", kEmojiWake, L"wake a parked tab" },
            { L"", kEmojiRestart, L"restart (always asks first)" },
            { L"a question", L"1\uFE0F\u20E3 … \U0001F51F", L"pick that option (several for a multi-pick)" },
            { L"", L"✅ / ❌", L"send the picks / cancel; a text reply is \"Other\"" },
            { L"a confirm request", kEmojiYes, L"yes, within 60 s" },
            { L"your queued prompt", kEmojiNo, L"take it out of the queue" },
            { L"this post", kEmojiNew, L"start a new session" },
        };
        return rows;
    }

    /// <summary>One session on the legend's fleet board.</summary>
    struct FleetLine
    {
        /// <summary>The session's title.</summary>
        std::wstring title;
        /// <summary>Its tab color ("#RRGGBB"; "" = none).</summary>
        std::wstring colorHex;
        /// <summary>Its state.</summary>
        SessionState state{ SessionState::Idle };
        /// <summary>Its thread's deep link (the root's webUrl; "" = no thread yet).</summary>
        std::wstring webUrl;
    };

    /// <summary>
    /// The most sessions the legend's fleet board lists (the most recent first): a fleet of hundreds of tabs would pass Graph's
    /// size limit, and the channel's own thread list shows every session anyway. The rest are counted ("… and N more").
    /// </summary>
    inline constexpr size_t kLegendFleetMax = 50;

    /// <summary>
    /// The legend post's body (§5): the legend table, then the fleet board — each session's color square and title, linked to
    /// its thread, and its state. Agentmaster edits this post in place; the owner pins it once by hand.
    /// </summary>
    /// <param name="fleet">The sessions, in the board's order (the caller keeps at most kLegendFleetMax).</param>
    /// <param name="updatedWord">When the board was made, as the owner reads it ("14:09"); "" = not shown.</param>
    /// <param name="moreCount">How many sessions the caller left out of `fleet` (0 = none: no "… and N more" line).</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the body cannot be allocated.</exception>
    inline std::wstring BuildLegendHtml(const std::vector<FleetLine>& fleet, std::wstring_view updatedWord, size_t moreCount = 0)
    {
        std::wstring h;
        h += L"<b>\U0001F4CC Agentmaster — legend</b> (react on the right message; I remove your emoji when I act)";
        h += L"<table><tr><th>On</th><th>Emoji</th><th>Action</th></tr>";
        for (const LegendRow& row : LegendRows())
        {
            h += L"<tr><td>";
            h += Msg::TgEscapeHtml(row.on);
            h += L"</td><td>";
            h += Msg::TgEscapeHtml(row.emoji);
            h += L"</td><td>";
            h += Msg::TgEscapeHtml(row.action);
            h += L"</td></tr>";
        }
        h += L"</table><b>Fleet</b>";
        if (!updatedWord.empty())
        {
            h += L" <span style=\"color:#888\">(updated ";
            h += Msg::TgEscapeHtml(updatedWord);
            h += L")</span>";
        }
        if (fleet.empty())
        {
            h += L"<br><i>No sessions yet.</i>";
            return h;
        }
        h += L"<ul>";
        for (const FleetLine& f : fleet)
        {
            const std::wstring_view square = Msg::TgTitleColorSquare(f.colorHex);
            std::wstring name = Msg::TgEscapeHtml(Msg::TgOneLineTitle(f.title));
            if (!square.empty())
            {
                name = std::wstring{ square } + L" " + name;
            }
            h += L"<li>";
            if (!f.webUrl.empty())
            {
                h += L"<a href=\"";
                h += detail::EscapeAttribute(f.webUrl);
                h += L"\">";
                h += name;
                h += L"</a>";
            }
            else
            {
                h += name;
            }
            h += L" · ";
            h += Msg::StateLabel(f.state);
            h += L"</li>";
        }
        if (moreCount > 0)
        {
            h += L"<li><i>… and " + std::to_wstring(moreCount) + L" more</i></li>";
        }
        h += L"</ul>";
        return h;
    }

    /// <summary>
    /// The most of the owner's first prompt a card shows (§9.1), in UTF-16 units: every card edit carries the whole card, and a
    /// post has a size limit, so a long pasted prompt is cut on the card (the session has all of it).
    /// </summary>
    inline constexpr size_t kCardFirstPromptMaxChars = 4000;

    /// <summary>What the session card shows (§2, §5); the caller formats the times in the owner's local clock.</summary>
    struct CardFacts
    {
        /// <summary>The session's state.</summary>
        SessionState state{ SessionState::Idle };
        /// <summary>Since when, as the owner reads it ("since 14:09"); "" = not shown.</summary>
        std::wstring sinceWord;
        /// <summary>The session's title.</summary>
        std::wstring title;
        /// <summary>The tab color ("#RRGGBB"; "" = none).</summary>
        std::wstring colorHex;
        /// <summary>The effective working folder.</summary>
        std::wstring folder;
        /// <summary>The git branch ("" = none).</summary>
        std::wstring branch;
        /// <summary>The model's short label ("opus").</summary>
        std::wstring model;
        /// <summary>The effort ("max"; "" = default).</summary>
        std::wstring effort;
        /// <summary>The context in use ("182K"; "" = unknown).</summary>
        std::wstring contextWord;
        /// <summary>The Prompt Queue Runner is Auto (else Paused).</summary>
        bool runnerAuto{ false };
        /// <summary>How many prompts wait in the queue.</summary>
        size_t queued{ 0 };
        /// <summary>The session has an open tab this run (SessionInfo::live).</summary>
        bool live{ true };
        /// <summary>
        /// The text of the owner's post that this card replaced (§9.1: a new session from a new post), kept on the card;
        /// "" = none.
        /// </summary>
        std::wstring firstPrompt;
        /// <summary>The actions the session offers now (CardButtonFactsFor).</summary>
        Msg::CardButtonFacts buttons;
    };

    /// <summary>
    /// The card's subject (the thread's name in the channel): the state glyph, the color square and the title, exactly as a
    /// Telegram topic's name in its glyph form (`🟡 🟦 drag crash fix`), cut like it. A title-less session shows "session".
    /// </summary>
    /// <param name="s">The state.</param>
    /// <param name="colorHex">The tab color ("" = no square).</param>
    /// <param name="title">The title.</param>
    /// <returns>The subject.</returns>
    /// <exception cref="std::bad_alloc">If the subject cannot be allocated.</exception>
    inline std::wstring CardSubject(SessionState s, std::wstring_view colorHex, std::wstring_view title)
    {
        return Msg::TopicDisplayName(s, Msg::TgTitleColorSquare(colorHex), title.empty() ? std::wstring_view{ L"session" } : title);
    }

    /// <summary>
    /// The card's own legend line (§2): the emoji that do something on THIS session now. 🖼️ and ↩️ need a tab with a
    /// screen (a started tab Agentmaster launched, or a claude that reads keys); 🛑 needs a running turn too; 🌙 a tab
    /// that can be parked; ☀️ a parked, errored or exited session; 🔄 a session with a tab or one that ended; 📊, 📜 and
    /// 🔀 always (they read the transcript).
    /// </summary>
    /// <param name="f">The session's action facts (CardButtonFactsFor).</param>
    /// <returns>Plain text ("🖼️ screen · 📊 status · …").</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring CardLegendLine(const Msg::CardButtonFacts& f)
    {
        std::vector<std::wstring> items;
        const bool hasScreen = f.takesEsc || f.deactivatable;
        if (hasScreen)
        {
            items.push_back(std::wstring{ kEmojiScreen } + L" screen");
        }
        items.push_back(std::wstring{ kEmojiStatus } + L" status");
        if (f.takesEsc && f.turnInFlight)
        {
            items.push_back(std::wstring{ kEmojiEsc } + L" Esc");
        }
        if (f.takesEsc)
        {
            items.push_back(std::wstring{ kEmojiEnter } + L" Enter");
        }
        items.push_back(std::wstring{ kEmojiAllScreen } + L" all");
        items.push_back(std::wstring{ kEmojiFork } + L" fork");
        if (f.deactivatable)
        {
            items.push_back(std::wstring{ kEmojiPark } + L" park");
        }
        if (f.reactivatable)
        {
            items.push_back(std::wstring{ kEmojiWake } + L" wake");
        }
        if (hasScreen || f.error || f.done)
        {
            items.push_back(std::wstring{ kEmojiRestart } + L" restart");
        }
        std::wstring line;
        for (size_t k = 0; k < items.size(); ++k)
        {
            if (k > 0)
            {
                line += L" · ";
            }
            line += items[k];
        }
        return line;
    }

    /// <summary>
    /// The card's body (§2): the state line, the title line, the facts line, the runner line, the kept first prompt, and the
    /// legend line. Every value is escaped; an empty fact is left out of its line.
    /// </summary>
    /// <param name="f">What the card shows.</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the body cannot be allocated.</exception>
    inline std::wstring BuildCardHtml(const CardFacts& f)
    {
        std::wstring h;
        h += L"<b>";
        h += Msg::StateLabel(f.state);
        if (!f.live)
        {
            h += L" · closed";
        }
        if (!f.sinceWord.empty())
        {
            h += L" ";
            h += Msg::TgEscapeHtml(f.sinceWord);
        }
        h += L"</b><br><b>";
        const std::wstring_view square = Msg::TgTitleColorSquare(f.colorHex);
        if (!square.empty())
        {
            h += square;
            h += L" ";
        }
        h += Msg::TgEscapeHtml(Msg::TgOneLineTitle(f.title.empty() ? std::wstring_view{ L"session" } : std::wstring_view{ f.title }));
        h += L"</b>";
        std::wstring facts;
        for (const std::wstring* part : { &f.folder, &f.branch, &f.model, &f.effort, &f.contextWord })
        {
            if (part->empty())
            {
                continue;
            }
            if (!facts.empty())
            {
                facts += L" · ";
            }
            facts += Msg::TgEscapeHtml(*part);
        }
        if (!facts.empty())
        {
            h += L"<br><code>";
            h += facts;
            h += L"</code>";
        }
        h += L"<br><i>Runner ";
        h += f.runnerAuto ? L"Auto" : L"Paused";
        h += L" · ";
        h += std::to_wstring(f.queued);
        h += L" queued</i>";
        if (!f.firstPrompt.empty())
        {
            // At most kCardFirstPromptMaxChars of it, never cut inside a surrogate pair: every card edit carries it.
            const std::wstring_view shown = Msg::detail::TgPrefixUnits(f.firstPrompt, kCardFirstPromptMaxChars);
            h += L"<br>\U0001F4DD first prompt: <i>";
            h += Msg::TgEscapeHtml(shown);
            if (shown.size() < f.firstPrompt.size())
            {
                h += L"\x2026 (cut here: the session has all of it)";
            }
            h += L"</i>";
        }
        h += L"<br><span style=\"color:#888\">";
        h += Msg::TgEscapeHtml(CardLegendLine(f.buttons));
        h += L"</span>";
        return h;
    }

    /// <summary>FNV-1a over UTF-16 code units: a cheap, stable fingerprint (not security).</summary>
    /// <param name="s">The text.</param>
    /// <param name="seed">The running hash (the offset basis to start).</param>
    /// <returns>The hash.</returns>
    inline uint64_t Fnv1a64(std::wstring_view s, uint64_t seed = 14695981039346656037ull) noexcept
    {
        uint64_t h = seed;
        for (const wchar_t c : s)
        {
            h ^= static_cast<uint16_t>(c);
            h *= 1099511628211ull;
        }
        return h;
    }

    /// <summary>
    /// The card's change hash: its subject and body together, so a change of either is one edit, and an unchanged card is
    /// never edited (§5: at most one edit per card per 10 s, and none when nothing changed).
    /// </summary>
    /// <param name="subject">The subject.</param>
    /// <param name="body">The body.</param>
    /// <returns>The hash.</returns>
    inline uint64_t CardHash(std::wstring_view subject, std::wstring_view body) noexcept
    {
        // A separator that neither part contains keeps "ab"+"c" and "a"+"bc" apart.
        return Fnv1a64(body, Fnv1a64(L"\x1F", Fnv1a64(subject)));
    }

    // ===== 5. Post bodies ===================================================================================================

    /// <summary>Why a post alerts the owner (§6.4).</summary>
    enum class AlertKind
    {
        /// <summary>A silent post (everything that is not an alert).</summary>
        None,
        /// <summary>The final reply of a turn: one sound per turn.</summary>
        TurnEnd,
        /// <summary>Claude needs the owner (a question, an approval, a menu): with high importance.</summary>
        NeedsYou,
        /// <summary>An error.</summary>
        Error
    };

    /// <summary>The owner, as Graph names them: the @mention's target.</summary>
    struct Owner
    {
        /// <summary>The Entra object id (the token's `oid`).</summary>
        std::wstring id;
        /// <summary>The display name (the token's `name`; "" = "you").</summary>
        std::wstring name;
    };

    /// <summary>
    /// The JSON body of a post or a reply (POST …/messages, …/replies) or of an edit (PATCH): the subject (a root's only), the
    /// HTML, and — for an alert — the owner's @mention (an `&lt;at id="0"&gt;` in the HTML and its `mentions` entry, which
    /// must name the same text) and, for NeedsYou, `importance: high`. A post that @mentions the owner notifies them; nothing
    /// else does (TEAMS.md §15.1 #11).
    /// </summary>
    /// <remarks>⚠ An edit of a ROOT must carry its subject: a PATCH without it clears the subject (TEAMS.md §15.4).</remarks>
    /// <param name="html">The body's HTML (Teams HTML).</param>
    /// <param name="subject">The subject ("" = none: a reply, or a root without one).</param>
    /// <param name="alert">Why the post alerts the owner (None = silent).</param>
    /// <param name="owner">The owner (an alert without an owner id stays silent).</param>
    /// <returns>The body.</returns>
    /// <exception cref="std::bad_alloc">If the body cannot be allocated.</exception>
    inline json::Value BuildPostJson(std::wstring_view html, std::wstring_view subject, AlertKind alert, const Owner& owner)
    {
        json::Value post = json::Value::MkObj();
        if (!subject.empty())
        {
            post.Set(L"subject", json::Value::MkStr(std::wstring{ subject }));
        }
        std::wstring content{ html };
        if (alert != AlertKind::None && !owner.id.empty())
        {
            if (alert == AlertKind::NeedsYou)
            {
                post.Set(L"importance", json::Value::MkStr(L"high"));
            }
            const std::wstring name = owner.name.empty() ? std::wstring{ L"you" } : owner.name;
            content += L"<br><at id=\"0\">";
            content += Msg::TgEscapeHtml(name);
            content += L"</at>";
            json::Value user = json::Value::MkObj();
            user.Set(L"id", json::Value::MkStr(owner.id));
            user.Set(L"displayName", json::Value::MkStr(name));
            user.Set(L"userIdentityType", json::Value::MkStr(L"aadUser"));
            json::Value mentioned = json::Value::MkObj();
            mentioned.Set(L"user", std::move(user));
            json::Value mention = json::Value::MkObj();
            mention.Set(L"id", json::Value::MkNum(0));
            mention.Set(L"mentionText", json::Value::MkStr(name));
            mention.Set(L"mentioned", std::move(mentioned));
            json::Value mentions = json::Value::MkArr();
            mentions.Push(std::move(mention));
            post.Set(L"mentions", std::move(mentions));
        }
        json::Value body = json::Value::MkObj();
        body.Set(L"contentType", json::Value::MkStr(L"html"));
        body.Set(L"content", json::Value::MkStr(std::move(content)));
        post.Set(L"body", std::move(body));
        return post;
    }

    // ===== 6. Telegram HTML -> Teams HTML, and the split ===================================================================

    /// <summary>The size a post is split at (§6.2): Graph accepted 80 KB and refused 120 KB (TEAMS.md §15.1 #9).</summary>
    inline constexpr size_t kTeamsPostMaxBytes = 60 * 1024;

    /// <summary>
    /// Telegram HTML (what the Telegram connector's builders write) as Teams HTML (§6.2): a newline outside &lt;pre&gt;
    /// becomes &lt;br&gt; (Telegram keeps newlines as text; Teams collapses them); a CR goes; &lt;tg-spoiler&gt; becomes a
    /// plain &lt;span&gt;; &lt;blockquote expandable&gt; a plain &lt;blockquote&gt;; &lt;tg-emoji&gt; keeps only its emoji;
    /// every other tag and all text pass unchanged (they are already escaped).
    /// </summary>
    /// <param name="tg">The Telegram HTML.</param>
    /// <returns>The Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the HTML cannot be allocated.</exception>
    inline std::wstring TeamsHtmlFromTelegramHtml(std::wstring_view tg)
    {
        std::wstring out;
        out.reserve(tg.size() + tg.size() / 8);
        bool inPre = false;
        size_t i = 0;
        while (i < tg.size())
        {
            const wchar_t c = tg[i];
            if (c == L'<')
            {
                const size_t close = tg.find(L'>', i);
                if (close == std::wstring_view::npos)
                {
                    // Telegram HTML escapes a literal '<'; an unclosed one is copied as it is rather than guessed at.
                    out.append(tg.substr(i));
                    break;
                }
                const std::wstring_view tag = tg.substr(i, close - i + 1);
                const std::wstring name = detail::TagNameLower(tag);
                if (name == L"pre")
                {
                    inPre = true;
                    out.append(tag);
                }
                else if (name == L"/pre")
                {
                    inPre = false;
                    out.append(tag);
                }
                else if (name == L"tg-spoiler")
                {
                    out += L"<span>";
                }
                else if (name == L"/tg-spoiler")
                {
                    out += L"</span>";
                }
                else if (name == L"blockquote")
                {
                    out += L"<blockquote>";
                }
                else if (name == L"tg-emoji" || name == L"/tg-emoji")
                {
                    // The custom emoji's tag goes; the plain emoji between the tags stays.
                }
                else
                {
                    out.append(tag);
                }
                i = close + 1;
                continue;
            }
            if (c == L'\r')
            {
                ++i;
                continue;
            }
            if (c == L'\n' && !inPre)
            {
                out += L"<br>";
                ++i;
                continue;
            }
            out.push_back(c);
            ++i;
        }
        return out;
    }

    /// <summary>
    /// Splits a post's HTML into pieces of at most `maxBytes` UTF-8 bytes (§6.2), each cut after a line break or the end of
    /// a block (&lt;br&gt;, &lt;/p&gt;, &lt;/pre&gt;, &lt;/blockquote&gt;, &lt;/table&gt;, &lt;/ul&gt;, &lt;/ol&gt;,
    /// &lt;/li&gt;, &lt;/h1-3&gt;) when the piece has one, else between two characters — never inside a tag, an entity or a
    /// surrogate pair. Joined again, the pieces are the input exactly. A single tag larger than the limit stays whole.
    /// </summary>
    /// <param name="html">The HTML.</param>
    /// <param name="maxBytes">The limit (at least 64 is used).</param>
    /// <returns>The pieces, in order (one piece when the HTML fits; none for an empty input).</returns>
    /// <exception cref="std::bad_alloc">If the pieces cannot be allocated.</exception>
    inline std::vector<std::wstring> SplitTeamsHtml(std::wstring_view html, size_t maxBytes = kTeamsPostMaxBytes)
    {
        if (maxBytes < 64)
        {
            maxBytes = 64;
        }
        std::vector<std::wstring> parts;
        std::wstring cur;
        size_t curBytes = 0;
        size_t lastBreak = std::wstring::npos; // index in `cur` just after the last safe break
        size_t i = 0;
        while (i < html.size())
        {
            // The next unit: a whole tag, a whole entity, a surrogate pair, or one character.
            size_t end = i + 1;
            const wchar_t c = html[i];
            if (c == L'<')
            {
                const size_t close = html.find(L'>', i);
                end = (close == std::wstring_view::npos) ? html.size() : close + 1;
            }
            else if (c == L'&')
            {
                const size_t semi = html.find(L';', i);
                if (semi != std::wstring_view::npos && semi - i <= 10)
                {
                    end = semi + 1;
                }
            }
            else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < html.size())
            {
                end = i + 2;
            }
            const std::wstring_view unit = html.substr(i, end - i);
            const size_t unitBytes = Utf8Length(unit);
            if (!cur.empty() && curBytes + unitBytes > maxBytes)
            {
                if (lastBreak != std::wstring::npos && lastBreak > 0 && lastBreak < cur.size())
                {
                    // Cut at the last safe break; what follows it starts the next piece.
                    parts.push_back(cur.substr(0, lastBreak));
                    cur.erase(0, lastBreak);
                    curBytes = Utf8Length(cur);
                }
                else
                {
                    parts.push_back(std::move(cur));
                    cur.clear();
                    curBytes = 0;
                }
                lastBreak = std::wstring::npos;
                if (!cur.empty() && curBytes + unitBytes > maxBytes)
                {
                    // The rest after the break and this unit still do not fit together: the rest goes alone.
                    parts.push_back(std::move(cur));
                    cur.clear();
                    curBytes = 0;
                }
            }
            cur.append(unit);
            curBytes += unitBytes;
            if (unit.size() > 1 && unit[0] == L'<')
            {
                const std::wstring name = detail::TagNameLower(unit);
                if (name == L"br" || name == L"/p" || name == L"/pre" || name == L"/blockquote" || name == L"/table" ||
                    name == L"/ul" || name == L"/ol" || name == L"/li" || name == L"/h1" || name == L"/h2" || name == L"/h3")
                {
                    lastBreak = cur.size();
                }
            }
            i = end;
        }
        if (!cur.empty())
        {
            parts.push_back(std::move(cur));
        }
        return parts;
    }

    // ===== 7. The poll's cadence ============================================================================================

    /// <summary>The poll's delay while hot (§7.3): the owner acts, or a session waits for the owner.</summary>
    inline constexpr int64_t kPollHotMs = 5'000;
    /// <summary>The poll's delay normally.</summary>
    inline constexpr int64_t kPollNormalMs = 30'000;
    /// <summary>The poll's delay after kPollIdleAfterMs with no change (the owner's 90 s, 2026-11-18).</summary>
    inline constexpr int64_t kPollIdleMs = 90'000;
    /// <summary>How long after the owner's last action in Teams the poll stays hot.</summary>
    inline constexpr int64_t kPollHotWindowMs = 120'000;
    /// <summary>How long without any change before the poll slows to kPollIdleMs.</summary>
    inline constexpr int64_t kPollIdleAfterMs = 3'600'000;
    /// <summary>The least time between two reads (Graph allows 1 read per second per channel).</summary>
    inline constexpr int64_t kMinReadGapMs = 1'100;

    /// <summary>What the poll's cadence is decided by.</summary>
    struct PollFacts
    {
        /// <summary>The clock (Unix ms).</summary>
        int64_t nowMs{ 0 };
        /// <summary>When the owner last acted in Teams (a reply, a reaction, a new post); 0 = not this run.</summary>
        int64_t lastOwnerActionMs{ 0 };
        /// <summary>A session waits for the owner (Waiting for you, Needs approval, an open question).</summary>
        bool ownerAwaited{ false };
        /// <summary>When the channel last changed (a delta read with items); the service's start counts as a change.</summary>
        int64_t lastChangeMs{ 0 };
    };

    /// <summary>
    /// The delay before the next poll (§7.3, D-C7): 5 s while the owner acted in the last 2 minutes or a session waits for
    /// them, 90 s after an hour with no change, 30 s otherwise.
    /// </summary>
    /// <param name="f">The facts.</param>
    /// <returns>The delay in ms.</returns>
    inline int64_t NextPollDelayMs(const PollFacts& f) noexcept
    {
        if (f.ownerAwaited)
        {
            return kPollHotMs;
        }
        if (f.lastOwnerActionMs > 0 && f.nowMs - f.lastOwnerActionMs < kPollHotWindowMs)
        {
            return kPollHotMs;
        }
        if (f.lastChangeMs > 0 && f.nowMs - f.lastChangeMs >= kPollIdleAfterMs)
        {
            return kPollIdleMs;
        }
        return kPollNormalMs;
    }

    // ===== 8. Reactions =====================================================================================================

    /// <summary>One reaction on a message, as Graph reports it.</summary>
    struct Reaction
    {
        /// <summary>The emoji, normalized (NormalizeEmoji).</summary>
        std::wstring emoji;
        /// <summary>Who reacted (an Entra object id).</summary>
        std::wstring userId;
        /// <summary>When (Unix ms; 0 = unknown).</summary>
        int64_t atMs{ 0 };
    };

    /// <summary>A message's reactions (its `reactions` array), each emoji normalized; an entry without an emoji is skipped.</summary>
    /// <param name="message">The chatMessage JSON.</param>
    /// <returns>The reactions, in Graph's order.</returns>
    /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
    inline std::vector<Reaction> ParseReactions(const json::Value& message)
    {
        std::vector<Reaction> out;
        const json::Value* arr = message.Find(L"reactions");
        if (!arr || arr->type != json::Value::Type::Arr)
        {
            return out;
        }
        for (const json::Value& r : arr->arr)
        {
            Reaction x;
            x.emoji = NormalizeEmoji(r.StrAt(L"reactionType"));
            if (const json::Value* who = r.Find(L"user"))
            {
                if (const json::Value* user = who->Find(L"user"))
                {
                    x.userId = user->StrAt(L"id");
                }
            }
            x.atMs = ParseGraphTimeMs(r.StrAt(L"createdDateTime"));
            if (!x.emoji.empty())
            {
                out.push_back(std::move(x));
            }
        }
        return out;
    }

    /// <summary>What changed in a message's reactions between two reads.</summary>
    struct ReactionDiff
    {
        /// <summary>Reactions in the new read that the old one did not have.</summary>
        std::vector<Reaction> added;
        /// <summary>Reactions the old read had that the new one does not.</summary>
        std::vector<Reaction> removed;
    };

    /// <summary>
    /// Compares two reads of a message's reactions (§7.2). A reaction is the same when its emoji, its user AND its time are:
    /// an emoji the owner removed and put back between two reads has a new time and counts as added again — so pressing
    /// the same emoji twice runs the action twice, like a button. Graph keeps no history of reactions for channel messages
    /// (messageHistory is empty, TEAMS.md §15.4), so this comparison is the only way to see one.
    /// </summary>
    /// <param name="before">The last read.</param>
    /// <param name="now">This read.</param>
    /// <returns>What was added and removed.</returns>
    /// <exception cref="std::bad_alloc">If the lists cannot be allocated.</exception>
    inline ReactionDiff DiffReactions(const std::vector<Reaction>& before, const std::vector<Reaction>& now)
    {
        const auto key = [](const Reaction& r) { return r.emoji + L"|" + r.userId + L"|" + std::to_wstring(r.atMs); };
        std::set<std::wstring> had;
        for (const Reaction& r : before)
        {
            had.insert(key(r));
        }
        std::set<std::wstring> has;
        for (const Reaction& r : now)
        {
            has.insert(key(r));
        }
        ReactionDiff d;
        for (const Reaction& r : now)
        {
            if (!had.count(key(r)))
            {
                d.added.push_back(r);
            }
        }
        for (const Reaction& r : before)
        {
            if (!has.count(key(r)))
            {
                d.removed.push_back(r);
            }
        }
        return d;
    }

    /// <summary>A reaction Agentmaster set itself (the journal, §7.2): it reads as the owner's, and must never act.</summary>
    struct OwnReaction
    {
        /// <summary>The message it is on.</summary>
        std::wstring messageId;
        /// <summary>The emoji, normalized.</summary>
        std::wstring emoji;
        /// <summary>When Agentmaster set it (its own clock, Unix ms).</summary>
        int64_t atMs{ 0 };
    };

    /// <summary>How far apart Agentmaster's clock and Graph's time of the same reaction may be and still match.</summary>
    inline constexpr int64_t kOwnReactionClockSlackMs = 120'000;

    /// <summary>
    /// The owner's NEW reactions on a message that may be actions: the added ones by the owner, minus those Agentmaster set
    /// itself (the journal: same message, same emoji, times within kOwnReactionClockSlackMs) and minus status emoji (a belt:
    /// ActionForReaction ignores them anyway).
    /// </summary>
    /// <param name="messageId">The message.</param>
    /// <param name="diff">The comparison of its two reads.</param>
    /// <param name="ownerId">The owner's object id.</param>
    /// <param name="ours">The journal of Agentmaster's own reactions.</param>
    /// <returns>The owner's reactions to act on, oldest first.</returns>
    /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
    inline std::vector<Reaction> OwnerReactionsToAct(std::wstring_view messageId, const ReactionDiff& diff, std::wstring_view ownerId,
                                                     const std::vector<OwnReaction>& ours)
    {
        std::vector<Reaction> out;
        for (const Reaction& r : diff.added)
        {
            if (r.userId != ownerId || IsStatusEmoji(r.emoji))
            {
                continue;
            }
            bool own = false;
            for (const OwnReaction& o : ours)
            {
                if (o.messageId == messageId && o.emoji == r.emoji &&
                    (r.atMs == 0 || (r.atMs >= o.atMs - kOwnReactionClockSlackMs && r.atMs <= o.atMs + kOwnReactionClockSlackMs)))
                {
                    own = true;
                    break;
                }
            }
            if (!own)
            {
                out.push_back(r);
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const Reaction& a, const Reaction& b) { return a.atMs < b.atMs; });
        return out;
    }

    // ===== 9. Tokens ========================================================================================================

    /// <summary>What a Graph access token says about itself (its JWT payload; §4).</summary>
    struct JwtClaims
    {
        /// <summary>The delegated scopes (`scp`, space-separated in the token).</summary>
        std::vector<std::wstring> scopes;
        /// <summary>The signed-in user's object id (`oid`): the owner.</summary>
        std::wstring oid;
        /// <summary>The user's display name (`name`).</summary>
        std::wstring name;
        /// <summary>The app's display name (`app_displayname`), for the log.</summary>
        std::wstring appName;
        /// <summary>The tenant (`tid`).</summary>
        std::wstring tenantId;
        /// <summary>When the token expires (`exp` in Unix ms; 0 = unknown).</summary>
        int64_t expiresMs{ 0 };
    };

    /// <summary>
    /// Reads a JWT's payload without checking its signature: Agentmaster only reads its OWN tokens to know their scopes,
    /// owner and expiry; Graph checks the signature. Never logs or keeps the token.
    /// </summary>
    /// <param name="token">The access token ("header.payload.signature").</param>
    /// <returns>The claims, or nothing for a token that is not a readable JWT.</returns>
    /// <exception cref="std::bad_alloc">If the claims cannot be allocated.</exception>
    inline std::optional<JwtClaims> ReadJwtClaims(std::wstring_view token)
    {
        const size_t a = token.find(L'.');
        if (a == std::wstring_view::npos)
        {
            return std::nullopt;
        }
        const size_t b = token.find(L'.', a + 1);
        if (b == std::wstring_view::npos)
        {
            return std::nullopt;
        }
        std::string payload;
        payload.reserve(b - a);
        for (size_t i = a + 1; i < b; ++i)
        {
            const wchar_t c = token[i];
            if (c > 0x7F)
            {
                return std::nullopt;
            }
            payload.push_back(static_cast<char>(c));
        }
        const auto bytes = Base64UrlDecode(payload);
        if (!bytes)
        {
            return std::nullopt;
        }
        const auto v = json::Parse(Utf16FromUtf8(*bytes));
        if (!v || v->type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        JwtClaims c;
        const std::wstring scp = v->StrAt(L"scp");
        size_t at = 0;
        while (at < scp.size())
        {
            const size_t space = scp.find(L' ', at);
            const size_t end = space == std::wstring::npos ? scp.size() : space;
            if (end > at)
            {
                c.scopes.push_back(scp.substr(at, end - at));
            }
            at = end + 1;
        }
        c.oid = v->StrAt(L"oid");
        c.name = v->StrAt(L"name");
        c.appName = v->StrAt(L"app_displayname");
        c.tenantId = v->StrAt(L"tid");
        c.expiresMs = static_cast<int64_t>(v->NumAt(L"exp")) * 1000;
        return c;
    }

    /// <summary>A Graph call's kind, for the token router (§4.2).</summary>
    enum class GraphOp
    {
        /// <summary>Post a root or a reply.</summary>
        Post,
        /// <summary>Edit (PATCH) a message.</summary>
        Edit,
        /// <summary>Read messages, replies, the delta.</summary>
        Read,
        /// <summary>Set or unset a reaction.</summary>
        React,
        /// <summary>Upload a file to OneDrive.</summary>
        Upload,
        /// <summary>Delete (softDelete) a message.</summary>
        Delete
    };

    /// <summary>
    /// The scopes that let a token make a kind of call, best first (§4.2, as MEASURED: `Group.ReadWrite.All` posts and edits
    /// but is refused for reads and reactions, although the docs list it for reads).
    /// </summary>
    /// <param name="op">The kind of call.</param>
    /// <returns>The scopes (a static table).</returns>
    inline const std::vector<std::wstring_view>& ScopesFor(GraphOp op)
    {
        static const std::vector<std::wstring_view> post{ L"ChannelMessage.Send", L"Group.ReadWrite.All" };
        static const std::vector<std::wstring_view> edit{ L"ChannelMessage.ReadWrite", L"Group.ReadWrite.All" };
        static const std::vector<std::wstring_view> read{ L"ChannelMessage.Read.All" };
        static const std::vector<std::wstring_view> react{ L"ChannelMessage.Send" };
        static const std::vector<std::wstring_view> upload{ L"Files.ReadWrite", L"Files.ReadWrite.All" };
        static const std::vector<std::wstring_view> remove{ L"ChannelMessage.ReadWrite" };
        switch (op)
        {
        case GraphOp::Post:
            return post;
        case GraphOp::Edit:
            return edit;
        case GraphOp::Read:
            return read;
        case GraphOp::React:
            return react;
        case GraphOp::Upload:
            return upload;
        case GraphOp::Delete:
        default:
            return remove;
        }
    }

    /// <summary>A kind of call's word for the log and the status line ("post", "edit", …).</summary>
    /// <param name="op">The kind of call.</param>
    /// <returns>The word (a static string).</returns>
    inline const wchar_t* GraphOpWord(GraphOp op) noexcept
    {
        switch (op)
        {
        case GraphOp::Post:
            return L"post";
        case GraphOp::Edit:
            return L"edit";
        case GraphOp::Read:
            return L"read";
        case GraphOp::React:
            return L"react";
        case GraphOp::Upload:
            return L"upload";
        case GraphOp::Delete:
        default:
            return L"delete";
        }
    }

    namespace detail
    {
        /// <summary>Do two scope names match? Scope names are case-insensitive (ASCII).</summary>
        /// <param name="a">One name.</param>
        /// <param name="b">The other.</param>
        /// <returns>True when they name the same scope.</returns>
        inline bool ScopeEquals(std::wstring_view a, std::wstring_view b) noexcept
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                wchar_t x = a[i];
                wchar_t y = b[i];
                x = (x >= L'A' && x <= L'Z') ? static_cast<wchar_t>(x - L'A' + L'a') : x;
                y = (y >= L'A' && y <= L'Z') ? static_cast<wchar_t>(y - L'A' + L'a') : y;
                if (x != y)
                {
                    return false;
                }
            }
            return true;
        }
    }

    /// <summary>
    /// The best rank of a token's scopes for a kind of call: 0 = it holds the first (most specific) scope of ScopesFor, 1 =
    /// the second, …; -1 = it holds none and cannot make the call.
    /// </summary>
    /// <param name="scopes">The token's scopes.</param>
    /// <param name="op">The kind of call.</param>
    /// <returns>The rank, or -1.</returns>
    inline int ScopeRank(const std::vector<std::wstring>& scopes, GraphOp op) noexcept
    {
        const auto& wanted = ScopesFor(op);
        for (size_t rank = 0; rank < wanted.size(); ++rank)
        {
            for (const std::wstring& s : scopes)
            {
                if (detail::ScopeEquals(s, wanted[rank]))
                {
                    return static_cast<int>(rank);
                }
            }
        }
        return -1;
    }

    /// <summary>A token source as the router sees it (§4.1): its name, its scopes, and whether it has a token now.</summary>
    struct TokenSourceView
    {
        /// <summary>"azure-cli" or "app", for the log.</summary>
        std::wstring name;
        /// <summary>The scopes its current token holds.</summary>
        std::vector<std::wstring> scopes;
        /// <summary>It has a token now (signed in, not expired).</summary>
        bool available{ false };
    };

    /// <summary>A (source index, kind of call) pair that answered 403, so the router skips it for a while (§4.2).</summary>
    using RefusedPair = std::pair<size_t, GraphOp>;

    /// <summary>
    /// Which token source makes a kind of call (§4.2): among the available sources not refused for it, the one with the best
    /// ScopeRank; a tie goes to the lower index. With the Azure CLI (Group.ReadWrite.All) and an app (ChannelMessage.Send,
    /// ChannelMessage.Read.All, Files.ReadWrite): posts, reads, reactions and uploads go to the app, edits to the CLI.
    /// </summary>
    /// <param name="op">The kind of call.</param>
    /// <param name="sources">The sources.</param>
    /// <param name="refused">Pairs that answered 403 recently.</param>
    /// <returns>The source's index, or nothing when no source can make the call (the feature then degrades).</returns>
    inline std::optional<size_t> PickTokenFor(GraphOp op, const std::vector<TokenSourceView>& sources, const std::vector<RefusedPair>& refused)
    {
        std::optional<size_t> best;
        int bestRank = -1;
        for (size_t k = 0; k < sources.size(); ++k)
        {
            if (!sources[k].available)
            {
                continue;
            }
            if (std::find(refused.begin(), refused.end(), RefusedPair{ k, op }) != refused.end())
            {
                continue;
            }
            const int rank = ScopeRank(sources[k].scopes, op);
            if (rank < 0)
            {
                continue;
            }
            if (!best || rank < bestRank)
            {
                best = k;
                bestRank = rank;
            }
        }
        return best;
    }

    // ===== 10. Inbound messages =============================================================================================

    /// <summary>A channel message as the poll reads it (§7).</summary>
    struct InboundMessage
    {
        /// <summary>Its id (also its creation time in ms, as Teams makes ids; never relied on).</summary>
        std::wstring id;
        /// <summary>The root it replies to ("" = it is a root).</summary>
        std::wstring replyToId;
        /// <summary>The user who wrote it ("" = an app or the system).</summary>
        std::wstring fromUserId;
        /// <summary>Its body's HTML (or text).</summary>
        std::wstring html;
        /// <summary>Its plain text (PlainTextFromTeamsHtml).</summary>
        std::wstring text;
        /// <summary>A root's subject.</summary>
        std::wstring subject;
        /// <summary>"message", "systemEventMessage", …</summary>
        std::wstring messageType;
        /// <summary>The deep link.</summary>
        std::wstring webUrl;
        /// <summary>When it was written (Unix ms).</summary>
        int64_t createdMs{ 0 };
        /// <summary>When it last changed, a reaction included (Unix ms).</summary>
        int64_t modifiedMs{ 0 };
        /// <summary>It was deleted (deletedDateTime set).</summary>
        bool deleted{ false };
        /// <summary>Its reactions.</summary>
        std::vector<Reaction> reactions;
    };

    /// <summary>Reads a chatMessage JSON into an InboundMessage; a missing field is left empty.</summary>
    /// <param name="m">The chatMessage.</param>
    /// <returns>The message.</returns>
    /// <exception cref="std::bad_alloc">If the message cannot be allocated.</exception>
    inline InboundMessage ParseInboundMessage(const json::Value& m)
    {
        InboundMessage x;
        x.id = m.StrAt(L"id");
        x.replyToId = m.StrAt(L"replyToId");
        x.messageType = m.StrAt(L"messageType");
        x.subject = m.StrAt(L"subject");
        x.webUrl = m.StrAt(L"webUrl");
        x.createdMs = ParseGraphTimeMs(m.StrAt(L"createdDateTime"));
        x.modifiedMs = ParseGraphTimeMs(m.StrAt(L"lastModifiedDateTime"));
        if (const json::Value* del = m.Find(L"deletedDateTime"))
        {
            x.deleted = del->type == json::Value::Type::Str && !del->str.empty();
        }
        if (const json::Value* from = m.Find(L"from"))
        {
            if (const json::Value* user = from->Find(L"user"))
            {
                x.fromUserId = user->StrAt(L"id");
            }
        }
        if (const json::Value* body = m.Find(L"body"))
        {
            x.html = body->StrAt(L"content");
            x.text = body->StrAt(L"contentType") == L"text" ? detail::Trim(x.html) : PlainTextFromTeamsHtml(x.html);
        }
        x.reactions = ParseReactions(m);
        return x;
    }

    /// <summary>
    /// Does a text start with one of the markers in a space-separated list (`teamsForeignMarkers`, §10)? Leading white space
    /// of the text is skipped; markers are compared selectors aside, so "🤖" matches "🤖️".
    /// </summary>
    /// <param name="text">The text.</param>
    /// <param name="markers">The markers, separated by spaces ("" = none).</param>
    /// <returns>True when the text starts with one.</returns>
    /// <exception cref="std::bad_alloc">If a normalized form cannot be allocated.</exception>
    inline bool StartsWithAnyMarker(std::wstring_view text, std::wstring_view markers)
    {
        const std::wstring t = NormalizeEmoji(detail::Trim(text));
        size_t at = 0;
        while (at < markers.size())
        {
            const size_t space = markers.find(L' ', at);
            const size_t end = space == std::wstring_view::npos ? markers.size() : space;
            if (end > at)
            {
                const std::wstring m = NormalizeEmoji(markers.substr(at, end - at));
                if (!m.empty() && t.compare(0, m.size(), m) == 0)
                {
                    return true;
                }
            }
            at = end + 1;
        }
        return false;
    }

    /// <summary>
    /// Did the owner write this message (§7.2)? Both the owner and Agentmaster write as the owner, so it is the owner's when
    /// its user is the owner AND Agentmaster's journal does not hold its id; a deleted message and a system message are
    /// nobody's.
    /// </summary>
    /// <param name="m">The message.</param>
    /// <param name="ownerId">The owner's object id.</param>
    /// <param name="ownPostIds">The ids of the posts Agentmaster made.</param>
    /// <returns>True for the owner's own message.</returns>
    inline bool IsOwnerMessage(const InboundMessage& m, std::wstring_view ownerId, const std::map<std::wstring, int64_t>& ownPostIds)
    {
        return !m.deleted && m.messageType == L"message" && !ownerId.empty() && m.fromUserId == ownerId && ownPostIds.find(m.id) == ownPostIds.end();
    }

    /// <summary>What a new root in the channel is (§7.2, §9.1).</summary>
    enum class NewRootVerdict
    {
        /// <summary>Not for Agentmaster: its own post, another tool's (a marker), someone else's, a system message, an empty post.</summary>
        Ignore,
        /// <summary>The owner's new post: it opens a new session, its text the first prompt (D-C9).</summary>
        NewSession
    };

    /// <summary>The rule for a new root post (§9.1, D-C9).</summary>
    /// <param name="m">The root.</param>
    /// <param name="ownerId">The owner's object id.</param>
    /// <param name="ownPostIds">The ids of the posts Agentmaster made.</param>
    /// <param name="foreignMarkers">Other tools' markers (`teamsForeignMarkers`).</param>
    /// <returns>The verdict.</returns>
    /// <exception cref="std::bad_alloc">If a normalized form cannot be allocated.</exception>
    inline NewRootVerdict ClassifyNewRoot(const InboundMessage& m, std::wstring_view ownerId, const std::map<std::wstring, int64_t>& ownPostIds,
                                          std::wstring_view foreignMarkers)
    {
        if (!m.replyToId.empty() || !IsOwnerMessage(m, ownerId, ownPostIds) || m.text.empty() || StartsWithAnyMarker(m.text, foreignMarkers))
        {
            return NewRootVerdict::Ignore;
        }
        return NewRootVerdict::NewSession;
    }

    // ===== 11. The channel state ============================================================================================

    /// <summary>The state file's format version (§5).</summary>
    inline constexpr int kChannelStateVersion = 1;
    /// <summary>How long the journal keeps Agentmaster's own posts and reactions (§5).</summary>
    inline constexpr int64_t kJournalKeepMs = 14LL * 24 * 3'600'000;

    /// <summary>A session's thread (§5).</summary>
    struct Thread
    {
        /// <summary>The session.</summary>
        std::wstring sessionId;
        /// <summary>The root post (the card).</summary>
        std::wstring rootId;
        /// <summary>The root's deep link.</summary>
        std::wstring webUrl;
        /// <summary>The card's last posted hash (CardHash); 0 = not posted.</summary>
        uint64_t cardHash{ 0 };
        /// <summary>When the thread was made (Unix ms).</summary>
        int64_t createdMs{ 0 };
        /// <summary>When the card was last edited (Unix ms).</summary>
        int64_t lastCardEditMs{ 0 };
        /// <summary>The transcript the mirror reads.</summary>
        std::wstring transcriptPath;
        /// <summary>How far the mirror has read it (bytes).</summary>
        int64_t transcriptOffset{ 0 };
        /// <summary>The history replay is finished.</summary>
        bool historyDone{ false };
        /// <summary>The newest reply the poll has read (its creation time, Unix ms).</summary>
        int64_t lastReplyMs{ 0 };
        /// <summary>The root's reactions at the last read.</summary>
        std::vector<Reaction> rootReactions;
        /// <summary>
        /// The owner's words of the post this thread's root WAS (§9.1: a new session from a new post): the card keeps them,
        /// since Agentmaster edits that post into the card. "" = a root Agentmaster posted itself.
        /// </summary>
        std::wstring firstPrompt;
    };

    /// <summary>A message whose reactions the poll watches (§7.2): an open question, a confirm, a queued prompt, a backlog answer, a wizard step.</summary>
    struct Watched
    {
        /// <summary>The message.</summary>
        std::wstring messageId;
        /// <summary>Its thread's root.</summary>
        std::wstring rootId;
        /// <summary>What it is.</summary>
        MessageKind kind{ MessageKind::Other };
        /// <summary>The session it is about ("" = none: the legend, a wizard before its session).</summary>
        std::wstring sessionId;
        /// <summary>What it carries: a question's id, a queued prompt's id, the confirmed action's word, a backlog's text.</summary>
        std::wstring payload;
        /// <summary>When it stops being watched (Unix ms; 0 = never on its own).</summary>
        int64_t expiresMs{ 0 };
        /// <summary>Its reactions at the last read.</summary>
        std::vector<Reaction> reactions;
    };

    /// <summary>The connector's persisted state (`messaging\teams\state-&lt;channel&gt;.json`, one file per channel: ChannelStateFileLeaf, §5).</summary>
    struct ChannelState
    {
        /// <summary>The format version.</summary>
        int version{ kChannelStateVersion };
        /// <summary>The team the state belongs to (a changed setting starts a new state).</summary>
        std::wstring teamId;
        /// <summary>The channel the state belongs to.</summary>
        std::wstring channelId;
        /// <summary>The legend post's id ("" = not made yet).</summary>
        std::wstring legendId;
        /// <summary>The 🔎 Search thread's root ("" = not made yet).</summary>
        std::wstring searchId;
        /// <summary>The delta link of the last read ("" = start with a full sync).</summary>
        std::wstring deltaLink;
        /// <summary>The threads, by session id.</summary>
        std::map<std::wstring, Thread> threads;
        /// <summary>The watched messages.</summary>
        std::vector<Watched> watched;
        /// <summary>The journal of Agentmaster's own posts: id → when (Unix ms).</summary>
        std::map<std::wstring, int64_t> ownPosts;
        /// <summary>The journal of Agentmaster's own reactions.</summary>
        std::vector<OwnReaction> ownReactions;
        /// <summary>The legend's reactions at the last read.</summary>
        std::vector<Reaction> legendReactions;
    };

    /// <summary>A message kind's word in the state file.</summary>
    /// <param name="k">The kind.</param>
    /// <returns>The word (a static string).</returns>
    inline const wchar_t* MessageKindWord(MessageKind k) noexcept
    {
        switch (k)
        {
        case MessageKind::SessionCard:
            return L"card";
        case MessageKind::Question:
            return L"question";
        case MessageKind::Confirm:
            return L"confirm";
        case MessageKind::QueuedPrompt:
            return L"queued";
        case MessageKind::Legend:
            return L"legend";
        case MessageKind::Backlog:
            return L"backlog";
        case MessageKind::Step:
            return L"step";
        case MessageKind::ForkChoice:
            return L"fork";
        case MessageKind::Other:
        default:
            return L"other";
        }
    }

    /// <summary>A message kind from its word (MessageKindWord's inverse; an unknown word = Other).</summary>
    /// <param name="w">The word.</param>
    /// <returns>The kind.</returns>
    inline MessageKind MessageKindFromWord(std::wstring_view w) noexcept
    {
        for (const MessageKind k : { MessageKind::SessionCard, MessageKind::Question, MessageKind::Confirm, MessageKind::QueuedPrompt,
                                     MessageKind::Legend, MessageKind::Backlog, MessageKind::Step, MessageKind::ForkChoice })
        {
            if (w == MessageKindWord(k))
            {
                return k;
            }
        }
        return MessageKind::Other;
    }

    namespace detail
    {
        /// <summary>A 64-bit value as 16 hex digits (a JSON number would lose the low bits above 2^53).</summary>
        /// <param name="v">The value.</param>
        /// <returns>The hex text.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring HexU64(uint64_t v)
        {
            static constexpr wchar_t digits[] = L"0123456789abcdef";
            std::wstring s(16, L'0');
            for (int k = 15; k >= 0; --k)
            {
                s[static_cast<size_t>(k)] = digits[v & 0xF];
                v >>= 4;
            }
            return s;
        }

        /// <summary>HexU64's inverse; anything that is not hex reads as 0 (a card that is then edited once more).</summary>
        /// <param name="s">The hex text.</param>
        /// <returns>The value.</returns>
        inline uint64_t ParseHexU64(std::wstring_view s) noexcept
        {
            uint64_t v = 0;
            for (const wchar_t c : s)
            {
                uint64_t d = 0;
                if (c >= L'0' && c <= L'9')
                {
                    d = static_cast<uint64_t>(c - L'0');
                }
                else if (c >= L'a' && c <= L'f')
                {
                    d = static_cast<uint64_t>(c - L'a' + 10);
                }
                else if (c >= L'A' && c <= L'F')
                {
                    d = static_cast<uint64_t>(c - L'A' + 10);
                }
                else
                {
                    return 0;
                }
                v = (v << 4) | d;
            }
            return v;
        }

        /// <summary>Reactions as JSON: [{"e": emoji, "u": user, "t": ms}].</summary>
        /// <param name="rs">The reactions.</param>
        /// <returns>The array.</returns>
        /// <exception cref="std::bad_alloc">If the array cannot be allocated.</exception>
        inline json::Value ReactionsToJson(const std::vector<Reaction>& rs)
        {
            json::Value arr = json::Value::MkArr();
            for (const Reaction& r : rs)
            {
                json::Value o = json::Value::MkObj();
                o.Set(L"e", json::Value::MkStr(r.emoji));
                o.Set(L"u", json::Value::MkStr(r.userId));
                o.Set(L"t", json::Value::MkNum(static_cast<double>(r.atMs)));
                arr.Push(std::move(o));
            }
            return arr;
        }

        /// <summary>ReactionsToJson's inverse; a missing or malformed member gives no reactions.</summary>
        /// <param name="v">The array (may be null).</param>
        /// <returns>The reactions.</returns>
        /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
        inline std::vector<Reaction> ReactionsFromJson(const json::Value* v)
        {
            std::vector<Reaction> out;
            if (!v || v->type != json::Value::Type::Arr)
            {
                return out;
            }
            for (const json::Value& o : v->arr)
            {
                Reaction r;
                r.emoji = o.StrAt(L"e");
                r.userId = o.StrAt(L"u");
                r.atMs = o.I64At(L"t");
                if (!r.emoji.empty())
                {
                    out.push_back(std::move(r));
                }
            }
            return out;
        }
    }

    /// <summary>The state as the JSON of its file (§5).</summary>
    /// <param name="s">The state.</param>
    /// <returns>The document.</returns>
    /// <exception cref="std::bad_alloc">If the document cannot be allocated.</exception>
    inline json::Value ChannelStateToJson(const ChannelState& s)
    {
        json::Value root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(s.version));
        root.Set(L"teamId", json::Value::MkStr(s.teamId));
        root.Set(L"channelId", json::Value::MkStr(s.channelId));
        root.Set(L"legendId", json::Value::MkStr(s.legendId));
        root.Set(L"searchId", json::Value::MkStr(s.searchId));
        root.Set(L"deltaLink", json::Value::MkStr(s.deltaLink));
        json::Value threads = json::Value::MkArr();
        for (const auto& [sid, t] : s.threads)
        {
            json::Value o = json::Value::MkObj();
            o.Set(L"sessionId", json::Value::MkStr(sid));
            o.Set(L"rootId", json::Value::MkStr(t.rootId));
            o.Set(L"webUrl", json::Value::MkStr(t.webUrl));
            o.Set(L"cardHash", json::Value::MkStr(detail::HexU64(t.cardHash)));
            o.Set(L"createdMs", json::Value::MkNum(static_cast<double>(t.createdMs)));
            o.Set(L"lastCardEditMs", json::Value::MkNum(static_cast<double>(t.lastCardEditMs)));
            o.Set(L"transcriptPath", json::Value::MkStr(t.transcriptPath));
            o.Set(L"transcriptOffset", json::Value::MkNum(static_cast<double>(t.transcriptOffset)));
            o.Set(L"historyDone", json::Value::MkBool(t.historyDone));
            o.Set(L"lastReplyMs", json::Value::MkNum(static_cast<double>(t.lastReplyMs)));
            o.Set(L"rootReactions", detail::ReactionsToJson(t.rootReactions));
            o.Set(L"firstPrompt", json::Value::MkStr(t.firstPrompt));
            threads.Push(std::move(o));
        }
        root.Set(L"threads", std::move(threads));
        json::Value watched = json::Value::MkArr();
        for (const Watched& w : s.watched)
        {
            json::Value o = json::Value::MkObj();
            o.Set(L"messageId", json::Value::MkStr(w.messageId));
            o.Set(L"rootId", json::Value::MkStr(w.rootId));
            o.Set(L"kind", json::Value::MkStr(MessageKindWord(w.kind)));
            o.Set(L"sessionId", json::Value::MkStr(w.sessionId));
            o.Set(L"payload", json::Value::MkStr(w.payload));
            o.Set(L"expiresMs", json::Value::MkNum(static_cast<double>(w.expiresMs)));
            o.Set(L"reactions", detail::ReactionsToJson(w.reactions));
            watched.Push(std::move(o));
        }
        root.Set(L"watched", std::move(watched));
        json::Value posts = json::Value::MkArr();
        for (const auto& [id, ms] : s.ownPosts)
        {
            json::Value o = json::Value::MkObj();
            o.Set(L"id", json::Value::MkStr(id));
            o.Set(L"t", json::Value::MkNum(static_cast<double>(ms)));
            posts.Push(std::move(o));
        }
        root.Set(L"ownPosts", std::move(posts));
        json::Value reactions = json::Value::MkArr();
        for (const OwnReaction& r : s.ownReactions)
        {
            json::Value o = json::Value::MkObj();
            o.Set(L"m", json::Value::MkStr(r.messageId));
            o.Set(L"e", json::Value::MkStr(r.emoji));
            o.Set(L"t", json::Value::MkNum(static_cast<double>(r.atMs)));
            reactions.Push(std::move(o));
        }
        root.Set(L"ownReactions", std::move(reactions));
        root.Set(L"legendReactions", detail::ReactionsToJson(s.legendReactions));
        return root;
    }

    /// <summary>
    /// The state from its file (§5). A document of a NEWER format (a greater version) is refused, so an older build never
    /// writes over what it does not understand (Rule #16); a missing member reads as empty.
    /// </summary>
    /// <param name="v">The document.</param>
    /// <returns>The state, or nothing for a document that is not an object or is of a newer format.</returns>
    /// <exception cref="std::bad_alloc">If the state cannot be allocated.</exception>
    inline std::optional<ChannelState> ChannelStateFromJson(const json::Value& v)
    {
        if (v.type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        ChannelState s;
        s.version = static_cast<int>(v.I64At(L"version", kChannelStateVersion));
        if (s.version > kChannelStateVersion)
        {
            return std::nullopt;
        }
        s.version = kChannelStateVersion;
        s.teamId = v.StrAt(L"teamId");
        s.channelId = v.StrAt(L"channelId");
        s.legendId = v.StrAt(L"legendId");
        s.searchId = v.StrAt(L"searchId");
        s.deltaLink = v.StrAt(L"deltaLink");
        if (const json::Value* threads = v.Find(L"threads"); threads && threads->type == json::Value::Type::Arr)
        {
            for (const json::Value& o : threads->arr)
            {
                Thread t;
                t.sessionId = o.StrAt(L"sessionId");
                t.rootId = o.StrAt(L"rootId");
                if (t.sessionId.empty() || t.rootId.empty())
                {
                    continue;
                }
                t.webUrl = o.StrAt(L"webUrl");
                t.cardHash = detail::ParseHexU64(o.StrAt(L"cardHash"));
                t.createdMs = o.I64At(L"createdMs");
                t.lastCardEditMs = o.I64At(L"lastCardEditMs");
                t.transcriptPath = o.StrAt(L"transcriptPath");
                t.transcriptOffset = o.I64At(L"transcriptOffset");
                t.historyDone = o.BoolAt(L"historyDone");
                t.lastReplyMs = o.I64At(L"lastReplyMs");
                t.rootReactions = detail::ReactionsFromJson(o.Find(L"rootReactions"));
                t.firstPrompt = o.StrAt(L"firstPrompt");
                // The key is copied first: the map's slot must be found before `t` is moved into it.
                const std::wstring key = t.sessionId;
                s.threads[key] = std::move(t);
            }
        }
        if (const json::Value* watched = v.Find(L"watched"); watched && watched->type == json::Value::Type::Arr)
        {
            for (const json::Value& o : watched->arr)
            {
                Watched w;
                w.messageId = o.StrAt(L"messageId");
                if (w.messageId.empty())
                {
                    continue;
                }
                w.rootId = o.StrAt(L"rootId");
                w.kind = MessageKindFromWord(o.StrAt(L"kind"));
                w.sessionId = o.StrAt(L"sessionId");
                w.payload = o.StrAt(L"payload");
                w.expiresMs = o.I64At(L"expiresMs");
                w.reactions = detail::ReactionsFromJson(o.Find(L"reactions"));
                s.watched.push_back(std::move(w));
            }
        }
        if (const json::Value* posts = v.Find(L"ownPosts"); posts && posts->type == json::Value::Type::Arr)
        {
            for (const json::Value& o : posts->arr)
            {
                const std::wstring id = o.StrAt(L"id");
                if (!id.empty())
                {
                    s.ownPosts[id] = o.I64At(L"t");
                }
            }
        }
        if (const json::Value* reactions = v.Find(L"ownReactions"); reactions && reactions->type == json::Value::Type::Arr)
        {
            for (const json::Value& o : reactions->arr)
            {
                OwnReaction r;
                r.messageId = o.StrAt(L"m");
                r.emoji = o.StrAt(L"e");
                r.atMs = o.I64At(L"t");
                if (!r.messageId.empty() && !r.emoji.empty())
                {
                    s.ownReactions.push_back(std::move(r));
                }
            }
        }
        s.legendReactions = detail::ReactionsFromJson(v.Find(L"legendReactions"));
        return s;
    }

    /// <summary>
    /// Drops what the state no longer needs (§5): journal entries older than kJournalKeepMs, and watched messages past their
    /// expiry. A thread is never dropped (a session's thread lives for good).
    /// </summary>
    /// <param name="s">The state.</param>
    /// <param name="nowMs">The clock (Unix ms).</param>
    /// <returns>True when anything was dropped (the state is then saved).</returns>
    inline bool PruneChannelState(ChannelState& s, int64_t nowMs)
    {
        bool changed = false;
        for (auto it = s.ownPosts.begin(); it != s.ownPosts.end();)
        {
            if (nowMs - it->second > kJournalKeepMs)
            {
                it = s.ownPosts.erase(it);
                changed = true;
            }
            else
            {
                ++it;
            }
        }
        const size_t reactionsBefore = s.ownReactions.size();
        s.ownReactions.erase(std::remove_if(s.ownReactions.begin(), s.ownReactions.end(),
                                            [&](const OwnReaction& r) { return nowMs - r.atMs > kJournalKeepMs; }),
                             s.ownReactions.end());
        const size_t watchedBefore = s.watched.size();
        s.watched.erase(std::remove_if(s.watched.begin(), s.watched.end(),
                                       [&](const Watched& w) { return w.expiresMs > 0 && w.expiresMs < nowMs; }),
                        s.watched.end());
        return changed || reactionsBefore != s.ownReactions.size() || watchedBefore != s.watched.size();
    }

    // ===== 12. Graph addresses and the sign-in forms ========================================================================

    /// <summary>The one host a Graph call goes to (§4): the owner's bearer token is never sent to any other host.</summary>
    inline constexpr std::wstring_view kGraphHost = L"graph.microsoft.com";
    /// <summary>The Microsoft identity platform's host: the device-code sign-in and the token refresh (§4.1).</summary>
    inline constexpr std::wstring_view kLoginHost = L"login.microsoftonline.com";

    namespace detail
    {
        /// <summary>An ASCII letter in lower case; any other character as it is.</summary>
        /// <param name="c">The character.</param>
        /// <returns>The character, an ASCII capital lowered.</returns>
        inline wchar_t AsciiLower(wchar_t c) noexcept
        {
            return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        }

        /// <summary>Do two texts match with ASCII letters compared without case (a URL's scheme and host)?</summary>
        /// <param name="a">One text.</param>
        /// <param name="b">The other.</param>
        /// <returns>True when they match.</returns>
        inline bool EqualsAsciiNoCase(std::wstring_view a, std::wstring_view b) noexcept
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (AsciiLower(a[i]) != AsciiLower(b[i]))
                {
                    return false;
                }
            }
            return true;
        }

        /// <summary>Does a text start with a prefix, ASCII letters compared without case?</summary>
        /// <param name="text">The text.</param>
        /// <param name="prefix">The prefix.</param>
        /// <returns>True when it does.</returns>
        inline bool StartsWithAsciiNoCase(std::wstring_view text, std::wstring_view prefix) noexcept
        {
            return text.size() >= prefix.size() && EqualsAsciiNoCase(text.substr(0, prefix.size()), prefix);
        }

        /// <summary>
        /// A member that Microsoft sends as a number in one answer and as a string of digits in another (`expires_in` and
        /// `interval` of the sign-in, `expires_on` of the Azure CLI).
        /// </summary>
        /// <param name="o">The object.</param>
        /// <param name="key">The member.</param>
        /// <param name="def">The value for a missing member, or one that is neither a number nor up to 18 digits.</param>
        /// <returns>The value.</returns>
        /// <exception cref="std::bad_alloc">Only from Json.h's lookup observer, which runs during a state document's round trip
        /// (PERSISTENCE.md §17) and never around the sign-in.</exception>
        inline int64_t NumberOrDigits(const json::Value& o, std::wstring_view key, int64_t def)
        {
            const json::Value* v = o.type == json::Value::Type::Obj ? o.Find(key) : nullptr;
            if (!v)
            {
                return def;
            }
            if (v->type == json::Value::Type::Num)
            {
                return static_cast<int64_t>(v->num);
            }
            if (v->type != json::Value::Type::Str || v->str.empty() || v->str.size() > 18)
            {
                return def;
            }
            int64_t n = 0;
            for (const wchar_t c : v->str)
            {
                if (c < L'0' || c > L'9')
                {
                    return def;
                }
                n = n * 10 + (c - L'0');
            }
            return n;
        }

        /// <summary>
        /// The first line of a text, trimmed and cut: the sign-in's `error_description` runs over several lines of trace and
        /// correlation ids, and only its first line says what went wrong.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <param name="maxChars">The most characters kept; a longer line ends in "…".</param>
        /// <returns>The line.</returns>
        /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
        inline std::wstring FirstLine(std::wstring_view text, size_t maxChars)
        {
            std::wstring line = Trim(text.substr(0, text.find_first_of(L"\r\n")));
            if (line.size() > maxChars)
            {
                line.resize(maxChars);
                // Never end on half a surrogate pair: the status line would show a broken character.
                if (!line.empty() && line.back() >= 0xD800 && line.back() <= 0xDBFF)
                {
                    line.pop_back();
                }
                line += L"…";
            }
            return line;
        }

        /// <summary>Appends one byte as an RFC 3986 percent escape ("%2F").</summary>
        /// <typeparam name="Text">std::string or std::wstring.</typeparam>
        /// <param name="out">The text to append to.</param>
        /// <param name="c">The byte.</param>
        /// <exception cref="std::bad_alloc">If the text cannot grow.</exception>
        template<typename Text>
        void AppendPercentByte(Text& out, unsigned char c)
        {
            static constexpr char kHex[] = "0123456789ABCDEF";
            out.push_back(static_cast<typename Text::value_type>('%'));
            out.push_back(static_cast<typename Text::value_type>(kHex[c >> 4]));
            out.push_back(static_cast<typename Text::value_type>(kHex[c & 0xF]));
        }

        /// <summary>Is a byte an RFC 3986 unreserved character (A–Z a–z 0–9 - . _ ~)?</summary>
        /// <param name="c">The byte.</param>
        /// <returns>True when it is.</returns>
        inline bool IsUnreservedByte(unsigned char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        }
    }

    /// <summary>
    /// The path (with its query) of a Graph call, from what the caller holds: a path the service builds ("/v1.0/teams/…") or a
    /// link Graph handed back (a delta link "https://graph.microsoft.com/v1.0/…?$deltatoken=…", an @odata.nextLink). A link to
    /// ANY other host is refused, so the owner's bearer token only ever travels to Graph: a next link read from the network
    /// cannot send it elsewhere. User info ("x@host"), a port, another scheme, a dot segment, a backslash, a fragment, a space
    /// or a control character is refused too: WinHTTP or a proxy might read them differently than this check does.
    /// </summary>
    /// <param name="pathOrUrl">A path under "/v1.0/" or "/beta/", or an https URL on graph.microsoft.com.</param>
    /// <returns>The path to request, or nothing when the address is not a plain Graph address.</returns>
    /// <exception cref="std::bad_alloc">If the path cannot be allocated.</exception>
    inline std::optional<std::wstring> GraphRequestPath(std::wstring_view pathOrUrl)
    {
        std::wstring_view rest = pathOrUrl;
        constexpr std::wstring_view kScheme = L"https://";
        if (detail::StartsWithAsciiNoCase(rest, kScheme))
        {
            rest.remove_prefix(kScheme.size());
            const size_t slash = rest.find(L'/');
            // The whole authority must be the host: "graph.microsoft.com:443", "x@graph.microsoft.com" and
            // "graph.microsoft.com.example.com" all differ from it.
            if (slash == std::wstring_view::npos || !detail::EqualsAsciiNoCase(rest.substr(0, slash), kGraphHost))
            {
                return std::nullopt;
            }
            rest.remove_prefix(slash);
        }
        // A path of the API's two versions only: this also refuses "//host/…" (a scheme-relative link) and "http://…".
        if (rest.substr(0, 6) != L"/v1.0/" && rest.substr(0, 6) != L"/beta/")
        {
            return std::nullopt;
        }
        for (const wchar_t c : rest)
        {
            if (c <= L' ' || c == 0x7F || c == L'\\' || c == L'#')
            {
                return std::nullopt;
            }
        }
        if (rest.find(L"/../") != std::wstring_view::npos || rest.find(L"/./") != std::wstring_view::npos ||
            (rest.size() >= 3 && rest.substr(rest.size() - 3) == L"/.."))
        {
            return std::nullopt;
        }
        return std::wstring{ rest };
    }

    /// <summary>
    /// A text as one application/x-www-form-urlencoded value (the sign-in forms, §4.1): its UTF-8 bytes, every byte that is not
    /// an unreserved character written as %XX. A space becomes %20, which the identity platform reads like "+".
    /// </summary>
    /// <param name="value">The value.</param>
    /// <returns>The encoded value.</returns>
    /// <exception cref="std::bad_alloc">If the value cannot be allocated.</exception>
    inline std::string FormUrlEncode(std::wstring_view value)
    {
        const std::string bytes = Utf8FromUtf16(value);
        std::string out;
        out.reserve(bytes.size() * 3);
        for (const char ch : bytes)
        {
            const auto c = static_cast<unsigned char>(ch);
            if (detail::IsUnreservedByte(c))
            {
                out.push_back(static_cast<char>(c));
            }
            else
            {
                detail::AppendPercentByte(out, c);
            }
        }
        return out;
    }

    /// <summary>
    /// One id as a URL path segment: RFC 3986's pchar characters stay (the unreserved ones, ':' and '@' — a channel id is
    /// "19:…@thread.tacv2"), every other byte of its UTF-8 becomes %XX, so no id can add a segment or a query.
    /// </summary>
    /// <param name="id">The id.</param>
    /// <returns>The segment.</returns>
    /// <exception cref="std::bad_alloc">If the segment cannot be allocated.</exception>
    inline std::wstring PathSegment(std::wstring_view id)
    {
        std::wstring out;
        out.reserve(id.size());
        for (const char ch : Utf8FromUtf16(id))
        {
            const auto c = static_cast<unsigned char>(ch);
            if (detail::IsUnreservedByte(c) || c == ':' || c == '@')
            {
                out.push_back(static_cast<wchar_t>(c));
            }
            else
            {
                detail::AppendPercentByte(out, c);
            }
        }
        return out;
    }

    /// <summary>The channel's messages: "/v1.0/teams/{team}/channels/{channel}/messages" (posts go here, and its /delta is the poll).</summary>
    /// <param name="teamId">The team (teamsTeamId).</param>
    /// <param name="channelId">The channel (teamsChannelId).</param>
    /// <returns>The path.</returns>
    /// <exception cref="std::bad_alloc">If the path cannot be allocated.</exception>
    inline std::wstring ChannelMessagesPath(std::wstring_view teamId, std::wstring_view channelId)
    {
        return L"/v1.0/teams/" + PathSegment(teamId) + L"/channels/" + PathSegment(channelId) + L"/messages";
    }

    /// <summary>One message of the channel: a root ("…/messages/{root}") or a reply ("…/messages/{root}/replies/{reply}").</summary>
    /// <param name="teamId">The team.</param>
    /// <param name="channelId">The channel.</param>
    /// <param name="rootId">The root.</param>
    /// <param name="replyId">The reply ("" = the root itself).</param>
    /// <returns>The path (an edit PATCHes it; "/setReaction" after it reacts).</returns>
    /// <exception cref="std::bad_alloc">If the path cannot be allocated.</exception>
    inline std::wstring MessagePath(std::wstring_view teamId, std::wstring_view channelId, std::wstring_view rootId, std::wstring_view replyId = L"")
    {
        std::wstring path = ChannelMessagesPath(teamId, channelId) + L"/" + PathSegment(rootId);
        if (!replyId.empty())
        {
            path += L"/replies/" + PathSegment(replyId);
        }
        return path;
    }

    /// <summary>A root's replies: "…/messages/{root}/replies" (a reply is posted here; its GET lists them newest first).</summary>
    /// <param name="teamId">The team.</param>
    /// <param name="channelId">The channel.</param>
    /// <param name="rootId">The root.</param>
    /// <returns>The path.</returns>
    /// <exception cref="std::bad_alloc">If the path cannot be allocated.</exception>
    inline std::wstring RepliesPath(std::wstring_view teamId, std::wstring_view channelId, std::wstring_view rootId)
    {
        return MessagePath(teamId, channelId, rootId) + L"/replies";
    }

    /// <summary>
    /// Is a tenant safe in the sign-in's path ("/{tenant}/oauth2/v2.0/…")? A tenant id, "organizations", "common" or a domain:
    /// letters, digits, '.' and '-', not starting with '.' or '-', at most 100 characters. Anything else (a slash, a '?', a
    /// space) is refused, so the setting can never reshape the request.
    /// </summary>
    /// <param name="tenant">The tenant (teamsTenantId).</param>
    /// <returns>True when it is safe.</returns>
    inline bool IsTenantWord(std::wstring_view tenant) noexcept
    {
        if (tenant.empty() || tenant.size() > 100 || tenant.front() == L'.' || tenant.front() == L'-')
        {
            return false;
        }
        for (const wchar_t c : tenant)
        {
            const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>Is a client id an application id's form (hex digits and '-', 8 to 64 characters)? Anything else is a typo.</summary>
    /// <param name="clientId">The client id (teamsClientId).</param>
    /// <returns>True when it has the form.</returns>
    inline bool IsClientIdWord(std::wstring_view clientId) noexcept
    {
        if (clientId.size() < 8 || clientId.size() > 64)
        {
            return false;
        }
        for (const wchar_t c : clientId)
        {
            const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F') || c == L'-';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>A Graph or sign-in refusal, as its answer's body names it.</summary>
    struct GraphError
    {
        /// <summary>Graph's `error.code` ("Forbidden"), or the sign-in's `error` ("authorization_pending"); "" = none.</summary>
        std::wstring code;
        /// <summary>Graph's `error.message`, or the first line of the sign-in's `error_description`.</summary>
        std::wstring message;
    };

    /// <summary>
    /// Reads the refusal in an answer's body: Graph's form `{"error": {"code", "message"}}` or the identity platform's
    /// `{"error": "…", "error_description": "…"}`.
    /// </summary>
    /// <param name="body">The parsed body (any value).</param>
    /// <returns>The refusal; empty for a body that carries none.</returns>
    /// <exception cref="std::bad_alloc">If the texts cannot be allocated.</exception>
    inline GraphError ReadGraphError(const json::Value& body)
    {
        GraphError e;
        const json::Value* err = body.type == json::Value::Type::Obj ? body.Find(L"error") : nullptr;
        if (!err)
        {
            return e;
        }
        if (err->type == json::Value::Type::Obj)
        {
            e.code = err->StrAt(L"code");
            e.message = detail::FirstLine(err->StrAt(L"message"), 300);
        }
        else if (err->type == json::Value::Type::Str)
        {
            e.code = err->str;
            e.message = detail::FirstLine(body.StrAt(L"error_description"), 300);
        }
        return e;
    }

    // ===== 13. The sign-in's answers ========================================================================================

    /// <summary>The scope every token of the app is asked for: everything the tenant let the app have, and a refresh token.</summary>
    inline constexpr std::wstring_view kAppScope = L"https://graph.microsoft.com/.default offline_access";

    /// <summary>The device-code answer (POST /{tenant}/oauth2/v2.0/devicecode, §4.1).</summary>
    struct DeviceCodeAnswer
    {
        /// <summary>⚠ A secret: it redeems the tokens once the owner signed in. Never logged or shown.</summary>
        std::wstring deviceCode;
        /// <summary>What the owner types at the address ("ABCD1234").</summary>
        std::wstring userCode;
        /// <summary>The address ("https://microsoft.com/devicelogin").</summary>
        std::wstring verificationUri;
        /// <summary>How long the code is good for (seconds; 900 when the answer names none).</summary>
        int64_t expiresInSec{ 900 };
        /// <summary>How often the token endpoint may be asked (seconds, 1–60; 5 when the answer names none).</summary>
        int64_t intervalSec{ 5 };
    };

    /// <summary>Reads the device-code answer.</summary>
    /// <param name="body">The parsed body.</param>
    /// <returns>The answer, or nothing when a code, the user code or the address is missing.</returns>
    /// <exception cref="std::bad_alloc">If the answer cannot be allocated.</exception>
    inline std::optional<DeviceCodeAnswer> ParseDeviceCodeAnswer(const json::Value& body)
    {
        if (body.type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        DeviceCodeAnswer a;
        a.deviceCode = body.StrAt(L"device_code");
        a.userCode = body.StrAt(L"user_code");
        a.verificationUri = body.StrAt(L"verification_uri");
        if (a.verificationUri.empty())
        {
            a.verificationUri = body.StrAt(L"verification_url"); // the older endpoint's spelling
        }
        if (a.deviceCode.empty() || a.userCode.empty() || a.verificationUri.empty())
        {
            return std::nullopt;
        }
        a.expiresInSec = detail::NumberOrDigits(body, L"expires_in", 900);
        if (a.expiresInSec <= 0)
        {
            a.expiresInSec = 900;
        }
        a.intervalSec = detail::NumberOrDigits(body, L"interval", 5);
        a.intervalSec = a.intervalSec < 1 ? 5 : (a.intervalSec > 60 ? 60 : a.intervalSec);
        return a;
    }

    /// <summary>The token endpoint's answer (a device-code redemption or a refresh, §4.1).</summary>
    struct TokenAnswer
    {
        /// <summary>⚠ A secret: the access token ("" = refused).</summary>
        std::wstring accessToken;
        /// <summary>⚠ A secret: the new refresh token ("" = none sent; the stored one stays good).</summary>
        std::wstring refreshToken;
        /// <summary>The access token's lifetime (seconds; 0 = not named).</summary>
        int64_t expiresInSec{ 0 };
        /// <summary>The refusal, when there is no access token.</summary>
        GraphError error;
    };

    /// <summary>Reads the token endpoint's answer.</summary>
    /// <param name="body">The parsed body.</param>
    /// <returns>The answer (no access token and an error for a refusal or a body that is no answer).</returns>
    /// <exception cref="std::bad_alloc">If the answer cannot be allocated.</exception>
    inline TokenAnswer ParseTokenAnswer(const json::Value& body)
    {
        TokenAnswer a;
        if (body.type != json::Value::Type::Obj)
        {
            a.error.code = L"no_answer";
            return a;
        }
        a.accessToken = body.StrAt(L"access_token");
        a.refreshToken = body.StrAt(L"refresh_token");
        a.expiresInSec = detail::NumberOrDigits(body, L"expires_in", 0);
        if (a.accessToken.empty())
        {
            a.error = ReadGraphError(body);
            if (a.error.code.empty())
            {
                a.error.code = L"no_token";
            }
        }
        return a;
    }

    /// <summary>Where a device-code sign-in stands after one ask of the token endpoint (RFC 8628 §3.5).</summary>
    enum class SignInPoll
    {
        /// <summary>The owner has not finished yet: ask again after the interval.</summary>
        Pending,
        /// <summary>Asked too often: add 5 s to the interval, then ask again.</summary>
        SlowDown,
        /// <summary>Signed in: the answer carries the tokens.</summary>
        Done,
        /// <summary>The owner declined.</summary>
        Declined,
        /// <summary>The code expired: a new sign-in is needed.</summary>
        Expired,
        /// <summary>Any other refusal (a wrong client id, a tenant that does not allow the app, …).</summary>
        Failed
    };

    /// <summary>Classifies a token answer during a device-code sign-in.</summary>
    /// <param name="a">The answer.</param>
    /// <returns>Where the sign-in stands.</returns>
    inline SignInPoll ClassifySignInPoll(const TokenAnswer& a) noexcept
    {
        if (!a.accessToken.empty())
        {
            return SignInPoll::Done;
        }
        const std::wstring& c = a.error.code;
        if (c == L"authorization_pending")
        {
            return SignInPoll::Pending;
        }
        if (c == L"slow_down")
        {
            return SignInPoll::SlowDown;
        }
        if (c == L"authorization_declined" || c == L"access_denied")
        {
            return SignInPoll::Declined;
        }
        if (c == L"expired_token" || c == L"code_expired")
        {
            return SignInPoll::Expired;
        }
        return SignInPoll::Failed;
    }

    /// <summary>The Azure CLI's answer to `az account get-access-token --output json` (§4.1).</summary>
    struct AzCliTokenAnswer
    {
        /// <summary>⚠ A secret: the access token.</summary>
        std::wstring accessToken;
        /// <summary>When it expires (Unix ms, from `expires_on`; 0 = not named: the caller reads the token's own `exp`).</summary>
        int64_t expiresMs{ 0 };
    };

    /// <summary>
    /// Reads the Azure CLI's output: one JSON object with `accessToken` and `expires_on` (Unix seconds, a number or a string of
    /// digits, by the CLI's version). Text before the first '{' (a warning an older CLI printed to its output) is skipped.
    /// </summary>
    /// <param name="out">The command's standard output.</param>
    /// <returns>The answer, or nothing when the output holds no token.</returns>
    /// <exception cref="std::bad_alloc">If the answer cannot be allocated.</exception>
    inline std::optional<AzCliTokenAnswer> ParseAzCliTokenAnswer(std::wstring_view out)
    {
        const size_t brace = out.find(L'{');
        if (brace == std::wstring_view::npos)
        {
            return std::nullopt;
        }
        const auto v = json::Parse(out.substr(brace));
        if (!v || v->type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        AzCliTokenAnswer a;
        a.accessToken = v->StrAt(L"accessToken");
        if (a.accessToken.empty())
        {
            return std::nullopt;
        }
        const int64_t seconds = detail::NumberOrDigits(*v, L"expires_on", 0);
        a.expiresMs = seconds > 0 ? seconds * 1000 : 0;
        return a;
    }

    // ===== 14. The refresh token's parts in Credential Manager ==============================================================

    /// <summary>The most bytes one Windows credential holds (wincred.h CRED_MAX_CREDENTIAL_BLOB_SIZE, 5 × 512).</summary>
    inline constexpr size_t kCredentialPartBytes = 5 * 512;
    /// <summary>The most parts a stored secret may take (20 KB; a refresh token is 1–3 KB, so more is not a token).</summary>
    inline constexpr size_t kCredentialMaxParts = 8;

    /// <summary>
    /// A secret cut into Credential Manager's parts (§4.1): a Microsoft refresh token, even as UTF-8, can be longer than one
    /// credential holds, so it is stored over several. Every part but the last is full.
    /// </summary>
    /// <param name="secret">The secret's bytes.</param>
    /// <returns>The parts in order; empty for an empty secret, or for one that needs more than kCredentialMaxParts (refused).</returns>
    /// <exception cref="std::bad_alloc">If the parts cannot be allocated.</exception>
    inline std::vector<std::string> SplitSecretParts(std::string_view secret)
    {
        std::vector<std::string> parts;
        if (secret.empty() || secret.size() > kCredentialPartBytes * kCredentialMaxParts)
        {
            return parts;
        }
        for (size_t at = 0; at < secret.size(); at += kCredentialPartBytes)
        {
            parts.emplace_back(secret.substr(at, kCredentialPartBytes));
        }
        return parts;
    }

    /// <summary>A part's credential target: the first is the base itself, the others "base#2", "base#3", ….</summary>
    /// <param name="base">The first part's target ("Agentmaster:messaging:teams:&lt;profile leaf&gt;").</param>
    /// <param name="index">The part's index (0 = the first).</param>
    /// <returns>The target.</returns>
    /// <exception cref="std::bad_alloc">If the target cannot be allocated.</exception>
    inline std::wstring CredentialPartTarget(std::wstring_view base, size_t index)
    {
        std::wstring target{ base };
        if (index > 0)
        {
            target += L"#" + std::to_wstring(index + 1);
        }
        return target;
    }

    /// <summary>
    /// The first part's user name: "teams:&lt;parts&gt;:&lt;client id&gt;" — how many parts to read, and the app the token
    /// belongs to (a refresh token works only for the client that got it, so a changed client id asks for a new sign-in).
    /// </summary>
    /// <param name="parts">The number of parts.</param>
    /// <param name="clientId">The app's client id.</param>
    /// <returns>The user name.</returns>
    /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
    inline std::wstring CredentialUserWord(size_t parts, std::wstring_view clientId)
    {
        return L"teams:" + std::to_wstring(parts) + L":" + std::wstring{ clientId };
    }

    /// <summary>CredentialUserWord's inverse.</summary>
    /// <param name="user">The first part's user name.</param>
    /// <returns>The number of parts (1..kCredentialMaxParts) and the client id, or nothing for any other name.</returns>
    /// <exception cref="std::bad_alloc">If the client id cannot be allocated.</exception>
    inline std::optional<std::pair<size_t, std::wstring>> ParseCredentialUserWord(std::wstring_view user)
    {
        constexpr std::wstring_view kPrefix = L"teams:";
        if (user.substr(0, kPrefix.size()) != kPrefix)
        {
            return std::nullopt;
        }
        user.remove_prefix(kPrefix.size());
        const size_t colon = user.find(L':');
        if (colon == std::wstring_view::npos || colon == 0 || colon > 2 || colon + 1 >= user.size())
        {
            return std::nullopt;
        }
        size_t parts = 0;
        for (const wchar_t c : user.substr(0, colon))
        {
            if (c < L'0' || c > L'9')
            {
                return std::nullopt;
            }
            parts = parts * 10 + static_cast<size_t>(c - L'0');
        }
        if (parts < 1 || parts > kCredentialMaxParts)
        {
            return std::nullopt;
        }
        return std::make_pair(parts, std::wstring{ user.substr(colon + 1) });
    }

    // ===== 15. Pacing, the state file's name, the status line ===============================================================

    /// <summary>The send gap (§6.5): one call every 1.2 s for the whole channel (Graph allows one a second, ~83 % sustained).</summary>
    inline constexpr int64_t kSendGapMs = 1200;
    /// <summary>How long a 403 keeps a (token source, kind of call) pair out of the router (§4.2).</summary>
    inline constexpr int64_t kRefusedForMs = 30LL * 60 * 1000;
    /// <summary>A token is renewed this long before it expires, so no call goes out with one that dies on the way.</summary>
    inline constexpr int64_t kTokenEarlyRefreshMs = 5LL * 60 * 1000;
    /// <summary>The legend's fleet board is edited at most this often (§5).</summary>
    inline constexpr int64_t kLegendEditGapMs = 60'000;
    /// <summary>The legend post's subject (§5): the line the owner pins.</summary>
    inline constexpr std::wstring_view kLegendSubject = L"\U0001F4CC Agentmaster · legend and fleet";

    /// <summary>The wait before a retry (§6.5): 1 s, 2 s, 4 s … then 60 s for every later attempt.</summary>
    /// <param name="attempt">The attempt that failed (1 = the first).</param>
    /// <returns>The wait in ms.</returns>
    inline int64_t RetryBackoffMs(int attempt) noexcept
    {
        int64_t ms = 1000;
        for (int k = 1; k < attempt && ms < 60'000; ++k)
        {
            ms *= 2;
        }
        return ms > 60'000 ? 60'000 : ms;
    }

    /// <summary>The wait after a 429 (§6.5): its Retry-After (at most 10 minutes), else 5 s.</summary>
    /// <param name="retryAfterSec">The answer's Retry-After (0 = none).</param>
    /// <returns>The wait in ms.</returns>
    inline int64_t ThrottleWaitMs(int retryAfterSec) noexcept
    {
        if (retryAfterSec <= 0)
        {
            return 5000;
        }
        return (retryAfterSec > 600 ? 600 : retryAfterSec) * 1000LL;
    }

    /// <summary>
    /// The state file's name for a channel: "state-&lt;8 hex&gt;.json" from the team and the channel. One file per channel, so a
    /// channel switched away from and back to finds its threads again; the ids are also inside the file, which settles a clash.
    /// </summary>
    /// <param name="teamId">The team.</param>
    /// <param name="channelId">The channel.</param>
    /// <returns>The file name (no folder).</returns>
    /// <exception cref="std::bad_alloc">If the name cannot be allocated.</exception>
    inline std::wstring ChannelStateFileLeaf(std::wstring_view teamId, std::wstring_view channelId)
    {
        std::wstring key{ teamId };
        key += L'|';
        key += channelId;
        return L"state-" + detail::HexU64(Fnv1a64(key)).substr(0, 8) + L".json";
    }

    /// <summary>What the cog's status line says (§10); the service fills it, BuildStatusLine words it.</summary>
    struct StatusFacts
    {
        /// <summary>teamsEnabled.</summary>
        bool enabled{ false };
        /// <summary>A team and a channel are set.</summary>
        bool channelSet{ false };
        /// <summary>The service's threads run.</summary>
        bool running{ false };
        /// <summary>A device-code sign-in waits for the owner: the address ("" = no sign-in waits).</summary>
        std::wstring signInUri;
        /// <summary>…its code.</summary>
        std::wstring signInCode;
        /// <summary>…until when, as the owner reads it ("14:32"; "" = not shown).</summary>
        std::wstring signInUntil;
        /// <summary>The owner's name, from a token ("" = not known yet).</summary>
        std::wstring ownerName;
        /// <summary>The source that posts ("the app", "the Azure CLI"; "" = none).</summary>
        std::wstring postsBy;
        /// <summary>The source that edits ("" = none).</summary>
        std::wstring editsBy;
        /// <summary>The source that reads ("" = none).</summary>
        std::wstring readsBy;
        /// <summary>The source that reacts ("" = none).</summary>
        std::wstring reactsBy;
        /// <summary>How many sessions have a thread.</summary>
        size_t threads{ 0 };
        /// <summary>The legend post exists.</summary>
        bool legendPosted{ false };
        /// <summary>The newest problem, one line ("" = none).</summary>
        std::wstring problem;
    };

    /// <summary>
    /// The cog's status line (§10): off, what to set, the sign-in to finish, or "on" with who makes which call — the degraded
    /// features named (§4.2: no edit, no read = a mirror only, no reactions) — and the newest problem.
    /// </summary>
    /// <param name="f">The facts.</param>
    /// <returns>The line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring BuildStatusLine(const StatusFacts& f)
    {
        if (!f.enabled)
        {
            return L"Teams: off";
        }
        if (!f.channelSet)
        {
            return L"Teams: set the team and the channel (teamsTeamId, teamsChannelId)";
        }
        std::wstring s = L"Teams: ";
        if (!f.running)
        {
            s += L"stopped";
        }
        else
        {
            if (!f.signInCode.empty())
            {
                s += L"sign in: open " + f.signInUri + L" and enter " + f.signInCode;
                if (!f.signInUntil.empty())
                {
                    s += L" (until " + f.signInUntil + L")";
                }
                s += L" · ";
            }
            if (f.postsBy.empty())
            {
                s += L"nothing can post yet (sign in to the app, or run az login)";
            }
            else
            {
                s += L"on";
                if (!f.ownerName.empty())
                {
                    s += L" as " + f.ownerName;
                }
                s += L" · " + std::to_wstring(f.threads) + (f.threads == 1 ? L" thread" : L" threads");
                s += L" · posts by " + f.postsBy;
                if (f.editsBy.empty())
                {
                    s += L", no edits";
                }
                else
                {
                    s += L", edits by " + f.editsBy;
                }
                if (f.readsBy.empty())
                {
                    s += L", no reads (a mirror only)";
                }
                else
                {
                    s += L", reads by " + f.readsBy;
                }
                if (f.reactsBy.empty())
                {
                    s += L", no reactions";
                }
                else
                {
                    s += L", reactions by " + f.reactsBy;
                }
                if (!f.legendPosted)
                {
                    s += L" · the legend is not posted yet";
                }
            }
        }
        if (!f.problem.empty())
        {
            s += L" · " + f.problem;
        }
        return s;
    }

    // ===== 16. The service's settings =======================================================================================

    namespace detail
    {
        /// <summary>
        /// Decodes the %XX escapes of ASCII characters: a channel id copied from a channel's link comes encoded
        /// (19%3a…%40thread.tacv2). An escape of a byte above 0x7F, or a malformed one ("%4", "%zz"), stays as it is, so a
        /// text without escapes comes back unchanged.
        /// </summary>
        /// <param name="s">The text.</param>
        /// <returns>The decoded text.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring PercentDecodeAscii(std::wstring_view s)
        {
            const auto hexValue = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9')
                {
                    return c - L'0';
                }
                if (c >= L'a' && c <= L'f')
                {
                    return c - L'a' + 10;
                }
                if (c >= L'A' && c <= L'F')
                {
                    return c - L'A' + 10;
                }
                return -1;
            };
            std::wstring out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == L'%' && i + 2 < s.size())
                {
                    const int high = hexValue(s[i + 1]);
                    const int low = hexValue(s[i + 2]);
                    // ASCII only (a first digit below 8): an encoded UTF-8 byte would need the whole sequence decoded together.
                    if (high >= 0 && high < 8 && low >= 0)
                    {
                        out.push_back(static_cast<wchar_t>(high * 16 + low));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(s[i]);
            }
            return out;
        }
    }

    /// <summary>The settings the service runs with (§10): AppSettings' teams* members, read at each Apply.</summary>
    struct TeamsConfig
    {
        /// <summary>teamsEnabled.</summary>
        bool enabled{ false };
        /// <summary>teamsTeamId, trimmed.</summary>
        std::wstring teamId;
        /// <summary>teamsChannelId, trimmed.</summary>
        std::wstring channelId;
        /// <summary>teamsClientId, trimmed ("" = no app).</summary>
        std::wstring clientId;
        /// <summary>teamsTenantId, trimmed ("organizations" when empty).</summary>
        std::wstring tenant{ L"organizations" };
        /// <summary>teamsUseAzureCli.</summary>
        bool useAzureCli{ true };
        /// <summary>teamsVerbose.</summary>
        bool verbose{ false };
        /// <summary>teamsForeignMarkers.</summary>
        std::wstring foreignMarkers;
    };

    /// <summary>
    /// The service's settings from AppSettings. The ids are trimmed: a channel id pasted from a link often carries a space or
    /// a line break, and Graph would answer it 404.
    /// </summary>
    /// <param name="s">The settings.</param>
    /// <returns>The config.</returns>
    /// <exception cref="std::bad_alloc">If the texts cannot be copied.</exception>
    inline TeamsConfig TeamsConfigFrom(const AppSettings& s)
    {
        TeamsConfig c;
        c.enabled = s.teamsEnabled;
        c.teamId = detail::Trim(s.teamsTeamId);
        // A channel id copied from a channel's link is percent-encoded (19%3a…%40thread.tacv2): decoded here, since the Graph
        // paths encode it again (PathSegment) and a doubly encoded id is answered 404.
        c.channelId = detail::PercentDecodeAscii(detail::Trim(s.teamsChannelId));
        c.clientId = detail::Trim(s.teamsClientId);
        c.tenant = detail::Trim(s.teamsTenantId);
        if (c.tenant.empty())
        {
            c.tenant = L"organizations";
        }
        c.useAzureCli = s.teamsUseAzureCli;
        c.verbose = s.teamsVerbose;
        c.foreignMarkers = s.teamsForeignMarkers;
        return c;
    }

    /// <summary>Should the service run with this config? Switched on, with a team and a channel.</summary>
    /// <param name="c">The config.</param>
    /// <returns>True when the threads should run.</returns>
    inline bool WantsToRun(const TeamsConfig& c) noexcept
    {
        return c.enabled && !c.teamId.empty() && !c.channelId.empty();
    }

    /// <summary>
    /// Does a running service have to stop and start again for a new config? Another channel (another state file and other
    /// threads), another app or tenant (another identity), or the Azure CLI switched (the router's sources). Verbose and the
    /// foreign markers apply while it runs.
    /// </summary>
    /// <param name="running">The config the service runs with.</param>
    /// <param name="next">The new config.</param>
    /// <returns>True when it must restart.</returns>
    inline bool RestartNeeded(const TeamsConfig& running, const TeamsConfig& next) noexcept
    {
        return running.teamId != next.teamId || running.channelId != next.channelId || running.clientId != next.clientId || running.tenant != next.tenant ||
               running.useAzureCli != next.useAzureCli;
    }

    // ===== 17. Threads, the card and the mirror =============================================================================

    /// <summary>
    /// How fresh an item must be to alert the owner (§6.4): an older one — a backlog read after a restart, the newest turn
    /// replayed when a thread is made — is posted silently, so a restart never rings for turns that ended long ago.
    /// </summary>
    inline constexpr int64_t kAlertFreshMs = 10LL * 60 * 1000;
    /// <summary>A card is edited at most this often (§5); the changes in between are merged into the next edit.</summary>
    inline constexpr int64_t kCardEditGapMs = 10'000;
    /// <summary>
    /// A question or a plan posted this recently already told the owner that Claude needs them: the approval notice of the
    /// same moment is not posted (one alert, §6.4).
    /// </summary>
    inline constexpr int64_t kNeedsYouQuietMs = 30'000;

    /// <summary>
    /// Should a session get a thread (§5)? Telegram's rule (Msg::QualifiesForTopic): a Claude Code session with a tab that is
    /// not Idle, a pinned one, or one used within the idle grace (a new or resumed tab is listed before its first message). A
    /// thread, once made, stays: this only decides when one is MADE.
    /// </summary>
    /// <param name="s">The session (a lean record is enough: no queue or text is read).</param>
    /// <param name="nowMs">The clock (Unix ms).</param>
    /// <param name="idleGraceMs">The idle grace (AppSettings::messagingIdleGraceMinutes, shared with Telegram).</param>
    /// <returns>True when the session should have a thread.</returns>
    inline bool QualifiesForThread(const SessionInfo& s, int64_t nowMs, int64_t idleGraceMs) noexcept
    {
        Msg::TabFact f;
        f.state = s.state;
        f.live = s.live;
        f.inTab = !s.tabToken.empty();
        f.claude = s.kind == AgentKind::Claude;
        f.pinned = s.pinned;
        f.lastActivityMs = s.lastActivityUnixMs;
        return Msg::QualifiesForTopic(f, nowMs, idleGraceMs);
    }

    /// <summary>
    /// The card's facts from a session (§2, §5), read as Telegram's card reads them (Msg::BuildCardText): the state and since
    /// when (a clock word from persisted stamps, so a restart changes nothing), the title, the effective folder's last two
    /// segments, the branch, the model's short label, the effort, the context size, the runner and its pending prompts,
    /// open or closed, and the actions the session offers now.
    /// </summary>
    /// <param name="s">The session (a record whose queue holds at least its open prompts: SnapshotLiveForScan, Get).</param>
    /// <param name="colorHex">The tab's color ("" = none).</param>
    /// <param name="nowMs">The clock (the since-word drops its date on the same day).</param>
    /// <param name="firstPrompt">The owner's post this card replaced (§9.1; "" = none).</param>
    /// <returns>The facts.</returns>
    /// <exception cref="std::bad_alloc">If the texts cannot be allocated.</exception>
    inline CardFacts CardFactsFromSession(const SessionInfo& s, std::wstring_view colorHex, int64_t nowMs, std::wstring_view firstPrompt = {})
    {
        CardFacts f;
        f.state = s.state;
        if (const int64_t since = Msg::CardStateSinceMs(s); since > 0 && nowMs >= since)
        {
            f.sinceWord = L"since " + Msg::TgClockText(since, nowMs);
        }
        f.title = s.title.empty() ? s.id.substr(0, 8) : s.title;
        f.colorHex = std::wstring{ colorHex };
        const std::wstring& dir = s.inferredWorkingDir.empty() ? s.workingDir : s.inferredWorkingDir;
        f.folder = Msg::detail::ShortPath(dir, 2, false);
        f.branch = s.branch;
        f.model = Msg::CardModelLabel(s.currentModel.empty() ? s.model : s.currentModel);
        f.effort = s.effort;
        if (s.contextTokens > 0)
        {
            f.contextWord = std::to_wstring(s.contextTokens / 1000) + L"K";
        }
        f.runnerAuto = s.autorunner.mode == AutorunnerMode::Full;
        for (const auto& q : s.queue)
        {
            if (q.status == PromptStatus::Pending)
            {
                ++f.queued;
            }
        }
        f.live = s.live;
        f.firstPrompt = std::wstring{ firstPrompt };
        f.buttons = Msg::CardButtonFactsFor(s);
        return f;
    }

    /// <summary>The turn's final reply as a post (§6.1): "🤖 Claude · 14:05", then the reply's Markdown as Teams HTML.</summary>
    /// <param name="it">The reply (a Text item).</param>
    /// <param name="nowMs">The clock (the time drops its date on the same day; a 0 item time shows none).</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the post cannot be allocated.</exception>
    inline std::wstring ReplyPostHtml(const Msg::TopicItem& it, int64_t nowMs)
    {
        std::wstring h = L"<b>\U0001F916 Claude</b>";
        if (it.tsMs > 0)
        {
            h += L" · " + Msg::TgClockText(it.tsMs, nowMs);
        }
        return h + L"<br>" + TeamsHtmlFromTelegramHtml(Msg::TgMarkdownToHtml(it.text));
    }

    /// <summary>A remark (a text that a tool call followed) for the verbose narrative: the Markdown as Teams HTML, in italics' place a 💬.</summary>
    /// <param name="it">The remark (a Text item).</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring RemarkHtml(const Msg::TopicItem& it)
    {
        return L"\U0001F4AC " + TeamsHtmlFromTelegramHtml(Msg::TgMarkdownToHtml(it.text));
    }

    /// <summary>
    /// A question (AskUserQuestion) as a post (§6.1, §8): "❓ the question" and its options numbered, as Telegram's history
    /// shows it; M5's emoji answer it (1️⃣ … 🔟).
    /// </summary>
    /// <param name="it">The question item.</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the post cannot be allocated.</exception>
    inline std::wstring QuestionPostHtml(const Msg::TopicItem& it)
    {
        std::wstring h = L"❓ <b>" + Msg::TgEscapeHtml(it.text) + L"</b>";
        for (size_t o = 0; o < it.options.size(); ++o)
        {
            h += L"<br>" + std::to_wstring(o + 1) + L". " + Msg::TgEscapeHtml(it.options[o]);
        }
        return h;
    }

    /// <summary>A plan waiting for approval (ExitPlanMode) as a post: "📋 Plan" and the plan's Markdown (cut at 20,000 characters).</summary>
    /// <param name="it">The plan item.</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the post cannot be allocated.</exception>
    inline std::wstring PlanPostHtml(const Msg::TopicItem& it)
    {
        const std::wstring text = it.text.size() > 20000 ? it.text.substr(0, 20000) + L"…" : it.text;
        return L"\U0001F4CB <b>Plan</b><br>" + TeamsHtmlFromTelegramHtml(Msg::TgMarkdownToHtml(text));
    }

    /// <summary>An API error that ended a turn, as a post: "⚠ API error" and the message (cut at 2,000 characters).</summary>
    /// <param name="it">The error item.</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the post cannot be allocated.</exception>
    inline std::wstring ApiErrorPostHtml(const Msg::TopicItem& it)
    {
        const std::wstring text = it.text.size() > 2000 ? it.text.substr(0, 2000) : it.text;
        return L"⚠ <b>API error</b><br>" + TeamsHtmlFromTelegramHtml(Msg::TgEscapeHtml(text));
    }

    /// <summary>
    /// The notice that Claude waits for an approval at the PC (§6.4): a permission prompt is in no transcript, so the state
    /// change itself is posted (a NeedsYou alert). The screen (🖼️) shows the prompt.
    /// </summary>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the post cannot be allocated.</exception>
    inline std::wstring ApprovalPostHtml()
    {
        return L"\U0001F7E0 <b>Claude needs your approval</b> at the PC (react " + std::wstring{ kEmojiScreen } + L" on the card for the screen)";
    }

    /// <summary>One transcript line as the live mirror reads it (§6.1): where it starts and ends in the file, and its items.</summary>
    struct MirrorLine
    {
        /// <summary>The line's first byte.</summary>
        int64_t at{ 0 };
        /// <summary>The byte after its newline.</summary>
        int64_t end{ 0 };
        /// <summary>Msg::ClassifyTranscriptLine's items (live mode).</summary>
        std::vector<Msg::TopicItem> items;
    };

    /// <summary>One post of the live mirror (§6.1).</summary>
    struct MirrorPost
    {
        /// <summary>Its Teams HTML.</summary>
        std::wstring html;
        /// <summary>Whether it alerts the owner (§6.4).</summary>
        AlertKind alert{ AlertKind::None };
        /// <summary>
        /// Where the thread's transcript cursor may move once this post is answered: the end of the lines it covers, never past
        /// a text still held (0 = no move). The service commits on the answer, so a restart re-reads what was not posted.
        /// </summary>
        int64_t commitTo{ 0 };
    };

    /// <summary>
    /// What the live mirror of one thread carries between reads (§6.1). In memory only: a restart reads again from the
    /// committed cursor, which never passes a held text, so nothing it held is lost.
    /// </summary>
    struct MirrorState
    {
        /// <summary>A text of the running turn waits: a REMARK if a tool call, a question or another text follows, the FINAL
        /// reply if the turn ends after it.</summary>
        bool holding{ false };
        /// <summary>The held text.</summary>
        Msg::TopicItem held;
        /// <summary>The held text's line start: no commit passes it while it is held.</summary>
        int64_t heldAt{ 0 };
        /// <summary>The held text's line end.</summary>
        int64_t heldEnd{ 0 };
        /// <summary>Verbose only: remarks and tool lines not posted yet (Teams HTML), posted as one post at the next stop.</summary>
        std::wstring narrative;
        /// <summary>The end of the last line folded into the narrative.</summary>
        int64_t narrativeTo{ 0 };
    };

    /// <summary>
    /// The live mirror's decisions for newly read lines (§6.1, §6.4), pure. A turn's prompt (and a message typed while it
    /// ran, an answer, a command) is posted at once, silently; a question or a plan at once with a NeedsYou alert; an API
    /// error with an Error alert; an interrupt silently. A text is HELD until what follows tells: a tool call, a question, an
    /// error or another text makes it a remark (posted only when verbose, in the narrative with the tool lines); the turn's
    /// end makes it the FINAL reply, posted with a TurnEnd alert — the turn ends when the text's own stop reason ends it,
    /// when the next turn's prompt arrives, or when the session rests and the reader has read everything (`turnAtRest`).
    /// Only items newer than `alertSinceMs` alert.
    /// </summary>
    /// <param name="st">The thread's mirror state (in/out).</param>
    /// <param name="lines">The lines read, in file order.</param>
    /// <param name="readTo">Where the read ended (the end of the last complete line).</param>
    /// <param name="turnAtRest">The session is not working (not Running, not NeedsApproval) and the read reached the file's end:
    /// a held text is the final reply.</param>
    /// <param name="verbose">teamsVerbose: remarks and tool lines are posted too.</param>
    /// <param name="alertSinceMs">Items older than this post silently (the service passes now − kAlertFreshMs).</param>
    /// <param name="nowMs">The clock, for the reply's time word.</param>
    /// <param name="promptSource">Where a prompt came from (§7.5: "typed at the PC", the runner); empty = the PC.</param>
    /// <param name="postedInTeams">True for a prompt the owner wrote IN the thread (§7.5): their own reply shows it already, so
    /// the mirror posts no "👤 You" line for it (the turn still starts there). Called once per prompt; empty = none.</param>
    /// <returns>The posts, in the order they are to be sent.</returns>
    /// <exception cref="std::bad_alloc">If the posts cannot be allocated.</exception>
    inline std::vector<MirrorPost> MirrorLines(MirrorState& st, const std::vector<MirrorLine>& lines, int64_t readTo, bool turnAtRest, bool verbose,
                                               int64_t alertSinceMs, int64_t nowMs, const std::function<Msg::PromptSource(const std::wstring&)>& promptSource = {},
                                               const std::function<bool(const std::wstring&)>& postedInTeams = {})
    {
        std::vector<MirrorPost> out;
        const auto alertFor = [alertSinceMs](const Msg::TopicItem& it, AlertKind kind) { return it.tsMs >= alertSinceMs ? kind : AlertKind::None; };
        // Every post commits at most to the held text's line start: a restart must read the held text again.
        const auto post = [&](std::wstring html, AlertKind alert, int64_t to) {
            MirrorPost p;
            p.html = std::move(html);
            p.alert = alert;
            p.commitTo = st.holding ? (std::min)(to, st.heldAt) : to;
            out.push_back(std::move(p));
        };
        const auto flushNarrative = [&]() {
            if (!st.narrative.empty())
            {
                post(std::move(st.narrative), AlertKind::None, st.narrativeTo);
                st.narrative.clear();
            }
        };
        const auto addNarrative = [&](const std::wstring& html, int64_t to) {
            if (!st.narrative.empty())
            {
                st.narrative += L"<br>";
            }
            st.narrative += html;
            st.narrativeTo = (std::max)(st.narrativeTo, to);
        };
        // The held text was a remark: shown in the verbose narrative only.
        const auto releaseAsRemark = [&]() {
            if (!st.holding)
            {
                return;
            }
            st.holding = false;
            if (verbose)
            {
                addNarrative(RemarkHtml(st.held), st.heldEnd);
            }
        };
        // The held text was the turn's final reply: the narrative goes first (its commit still limited by the held text),
        // then the reply.
        const auto finalize = [&](int64_t to) {
            if (!st.holding)
            {
                return;
            }
            flushNarrative();
            st.holding = false;
            post(ReplyPostHtml(st.held, nowMs), alertFor(st.held, AlertKind::TurnEnd), to);
        };
        const auto source = [&](const std::wstring& text) { return promptSource ? promptSource(text) : Msg::PromptSource::Pc; };
        for (const MirrorLine& line : lines)
        {
            for (const Msg::TopicItem& it : line.items)
            {
                switch (it.kind)
                {
                case Msg::TopicItemKind::Prompt:
                    if (!it.midTurn)
                    {
                        // A new turn: what the last one held was its final reply (everything before this line is done).
                        finalize(line.at);
                        flushNarrative();
                    }
                    if (!(postedInTeams && postedInTeams(it.text)))
                    {
                        post(TeamsHtmlFromTelegramHtml(Msg::BuildPromptLineHtml(it.tsMs, source(it.text), it.text, Msg::PromptTurnStatus::None, it.midTurn)),
                             AlertKind::None, line.end);
                    }
                    break;
                case Msg::TopicItemKind::Answer:
                    post(TeamsHtmlFromTelegramHtml(Msg::BuildAnswerLineHtml(it.tsMs, it.text)), AlertKind::None, line.end);
                    break;
                case Msg::TopicItemKind::Command:
                    post(TeamsHtmlFromTelegramHtml(Msg::BuildCommandLineHtml(it.tsMs, it.text)), AlertKind::None, line.end);
                    break;
                case Msg::TopicItemKind::Text:
                    releaseAsRemark(); // a second text: the first was a remark
                    st.holding = true;
                    st.held = it;
                    st.heldAt = line.at;
                    st.heldEnd = line.end;
                    if (it.turnEnds)
                    {
                        finalize(line.end); // its own stop reason ends the turn
                    }
                    break;
                case Msg::TopicItemKind::ToolCall:
                    releaseAsRemark();
                    if (verbose)
                    {
                        addNarrative(L"\U0001F527 " + Msg::TgEscapeHtml(it.summary.empty() ? it.toolName : it.summary), line.end);
                    }
                    break;
                case Msg::TopicItemKind::ToolResult:
                    if (verbose && it.isError)
                    {
                        addNarrative(L"✗ " + Msg::TgEscapeHtml(it.toolName.empty() ? std::wstring{ L"a tool" } : it.toolName) + L" failed: " +
                                         Msg::TgEscapeHtml(Msg::TgFirstLine(it.text, 160)),
                                     line.end);
                    }
                    break;
                case Msg::TopicItemKind::Question:
                case Msg::TopicItemKind::Plan:
                    releaseAsRemark();
                    flushNarrative();
                    post(it.kind == Msg::TopicItemKind::Question ? QuestionPostHtml(it) : PlanPostHtml(it), alertFor(it, AlertKind::NeedsYou), line.end);
                    break;
                case Msg::TopicItemKind::ApiError:
                    releaseAsRemark();
                    flushNarrative();
                    post(ApiErrorPostHtml(it), alertFor(it, AlertKind::Error), line.end);
                    break;
                case Msg::TopicItemKind::Interrupt:
                    releaseAsRemark();
                    flushNarrative();
                    post(L"■ <i>interrupted</i>", AlertKind::None, line.end);
                    break;
                case Msg::TopicItemKind::Compaction:
                    if (verbose)
                    {
                        addNarrative(L"<i>the conversation was compacted</i>", line.end);
                    }
                    break;
                default:
                    break; // a recap, bookkeeping
                }
            }
        }
        if (turnAtRest)
        {
            finalize(readTo);
            flushNarrative();
        }
        return out;
    }

    /// <summary>One post of a thread's history replay (§6.3): history entries packed up to the post limit, and the cursor it commits.</summary>
    struct HistoryChunk
    {
        /// <summary>Teams HTML ("" = nothing to post: only the commit, for turns that rendered nothing).</summary>
        std::wstring html;
        /// <summary>The newest turn end it holds (0 = none: the cursor stays).</summary>
        int64_t commitTo{ 0 };
    };

    /// <summary>
    /// Packs a history window's posts (Msg::PlanHistoryPosts, turn by turn) into Teams posts of at most `maxBytes` (§6.3):
    /// Telegram's per-message limit does not apply, so consecutive entries share a post, a blank line between them — a long
    /// session's history becomes a few dozen posts instead of hundreds. A post commits the newest turn end among its entries;
    /// one entry larger than the limit is a post of its own (the sender splits it, SplitTeamsHtml). Nothing in a history
    /// alerts.
    /// </summary>
    /// <param name="plan">The planned posts, in order.</param>
    /// <param name="maxBytes">The size limit of one post (UTF-8 bytes).</param>
    /// <returns>The chunks in order; a last chunk with no HTML carries a commit only.</returns>
    /// <exception cref="std::bad_alloc">If the chunks cannot be allocated.</exception>
    inline std::vector<HistoryChunk> PackHistoryPosts(const std::vector<Msg::HistoryPlanPost>& plan, size_t maxBytes = kTeamsPostMaxBytes)
    {
        constexpr std::wstring_view kSeparator = L"<br><br>";
        std::vector<HistoryChunk> out;
        HistoryChunk cur;
        size_t curBytes = 0;
        for (const Msg::HistoryPlanPost& p : plan)
        {
            const std::wstring html = TeamsHtmlFromTelegramHtml(p.post.html);
            if (!html.empty())
            {
                const size_t bytes = Utf8Length(html);
                if (!cur.html.empty() && curBytes + kSeparator.size() + bytes > maxBytes)
                {
                    out.push_back(std::move(cur));
                    cur = HistoryChunk{};
                    curBytes = 0;
                }
                if (!cur.html.empty())
                {
                    cur.html += kSeparator;
                    curBytes += kSeparator.size();
                }
                cur.html += html;
                curBytes += bytes;
            }
            if (p.commitTo > 0)
            {
                cur.commitTo = (std::max)(cur.commitTo, p.commitTo);
            }
        }
        if (!cur.html.empty() || cur.commitTo > 0)
        {
            out.push_back(std::move(cur));
        }
        return out;
    }

    /// <summary>
    /// Where a thread's history replay stops (§6.3): before the window's NEWEST turn, which the live mirror takes — its final
    /// reply, when the turn ended recently, then alerts like any other. Drops that turn's items from the window (a window
    /// that is the replay's last one) and returns its line start.
    /// </summary>
    /// <param name="w">The window (Msg::SplitHistoryWindow with `reachedEnd`), its `items` and `itemAt` cut in place.</param>
    /// <returns>The offset where the live mirror starts (the newest turn's first line); the window's `consumedTo` when it holds no
    /// turn start.</returns>
    inline int64_t CutNewestTurn(Msg::HistoryWindow& w)
    {
        for (size_t k = w.items.size(); k-- > 0;)
        {
            if (w.items[k].kind == Msg::TopicItemKind::Prompt && !w.items[k].midTurn && k < w.itemAt.size())
            {
                const int64_t at = w.itemAt[k];
                // Every item of that line and after it goes to the live mirror (a line can hold several items).
                size_t first = k;
                while (first > 0 && first - 1 < w.itemAt.size() && w.itemAt[first - 1] == at)
                {
                    --first;
                }
                w.items.resize(first);
                w.itemAt.resize(first);
                w.consumedTo = at;
                w.turnContinues = false;
                return at;
            }
        }
        return w.consumedTo;
    }

    // ===== 18. Inbound: the poll's pages, the owner's replies, prompts from Teams ===========================================

    /// <summary>The newest replies a changed thread's read asks for (`replies?$top=`, newest first, §7.2).</summary>
    inline constexpr int kRepliesPerRead = 20;
    /// <summary>A prompt's status reactions are followed this long at most (a turn that never ends does not keep it forever).</summary>
    inline constexpr int64_t kPromptTrackMaxMs = 6LL * 3'600'000;
    /// <summary>A backlog offer waits this long for the owner's ✅ or ❌ (§7.4).</summary>
    inline constexpr int64_t kBacklogOfferMs = 2LL * 3'600'000;

    /// <summary>One page of the channel's delta (§7.1).</summary>
    struct DeltaPage
    {
        /// <summary>The roots that changed (a new or edited root, a new reply, a reaction on any of its messages).</summary>
        std::vector<InboundMessage> roots;
        /// <summary>The next page ("" = this is the last one).</summary>
        std::wstring nextLink;
        /// <summary>The link of the next poll (only on the last page).</summary>
        std::wstring deltaLink;
    };

    /// <summary>Reads one page of `GET …/messages/delta` (or of its links): the changed roots and the links.</summary>
    /// <param name="body">The answer's body.</param>
    /// <returns>The page (empty for a body that is not one).</returns>
    /// <exception cref="std::bad_alloc">If the page cannot be allocated.</exception>
    inline DeltaPage ParseDeltaPage(const json::Value& body)
    {
        DeltaPage page;
        if (body.type != json::Value::Type::Obj)
        {
            return page;
        }
        if (const json::Value* value = body.Find(L"value"); value && value->type == json::Value::Type::Arr)
        {
            for (const json::Value& m : value->arr)
            {
                InboundMessage msg = ParseInboundMessage(m);
                if (!msg.id.empty())
                {
                    page.roots.push_back(std::move(msg));
                }
            }
        }
        page.nextLink = body.StrAt(L"@odata.nextLink");
        page.deltaLink = body.StrAt(L"@odata.deltaLink");
        return page;
    }

    /// <summary>
    /// The owner's NEW replies in a thread, oldest first (§7.2): from a `replies?$top=N` answer (newest first), the replies
    /// the owner wrote (IsOwnerMessage: the owner's id and not in Agentmaster's journal — both write as the owner) that are
    /// plain messages, not deleted, and newer than the last one read.
    /// </summary>
    /// <param name="body">The answer's body (`value`: the replies).</param>
    /// <param name="ownerId">The owner.</param>
    /// <param name="ownPosts">Agentmaster's journal of its own posts.</param>
    /// <param name="lastReplyMs">The newest reply already read (Thread::lastReplyMs).</param>
    /// <returns>The new replies, oldest first.</returns>
    /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
    inline std::vector<InboundMessage> NewOwnerReplies(const json::Value& body, std::wstring_view ownerId, const std::map<std::wstring, int64_t>& ownPosts,
                                                       int64_t lastReplyMs)
    {
        std::vector<InboundMessage> out;
        const json::Value* value = body.type == json::Value::Type::Obj ? body.Find(L"value") : nullptr;
        if (!value || value->type != json::Value::Type::Arr)
        {
            return out;
        }
        for (const json::Value& r : value->arr)
        {
            InboundMessage m = ParseInboundMessage(r);
            if (m.id.empty() || m.deleted || m.createdMs <= lastReplyMs || (!m.messageType.empty() && m.messageType != L"message"))
            {
                continue;
            }
            if (!IsOwnerMessage(m, ownerId, ownPosts))
            {
                continue;
            }
            out.push_back(std::move(m));
        }
        std::stable_sort(out.begin(), out.end(), [](const InboundMessage& a, const InboundMessage& b) { return a.createdMs < b.createdMs; });
        return out;
    }

    /// <summary>
    /// The status a prompt from Teams shows on the owner's reply (§2: 👀 → 👨‍💻 → 👌): 👀 while it waits in the queue (or was
    /// sent and not echoed yet), 👨‍💻 while the turn it started runs, 👌 when that turn is over, ⚠️ when it failed.
    /// </summary>
    /// <param name="s">The session (a FULL record: the queue's sent prompts are read — Get, not a lean snapshot).</param>
    /// <param name="promptId">The prompt.</param>
    /// <returns>The emoji, or "" when the queue no longer holds the prompt (the follow ends without a change).</returns>
    /// <exception cref="std::bad_alloc">If the emoji cannot be allocated.</exception>
    inline std::wstring PromptReactionFor(const SessionInfo& s, std::wstring_view promptId)
    {
        for (const QueuedPrompt& q : s.queue)
        {
            if (q.id != promptId)
            {
                continue;
            }
            switch (q.status)
            {
            case PromptStatus::Pending:
            case PromptStatus::Held:
                return std::wstring{ kEmojiSeen };
            case PromptStatus::Failed:
            case PromptStatus::Skipped:
                return std::wstring{ kEmojiWarn };
            case PromptStatus::Sent:
            default:
                if (!q.echoed)
                {
                    return std::wstring{ kEmojiSeen };
                }
                return (s.live && (s.state == SessionState::Running || s.state == SessionState::NeedsApproval)) ? std::wstring{ kEmojiWorking } :
                                                                                                                    std::wstring{ kEmojiDone };
            }
        }
        return {};
    }

    /// <summary>Is a status emoji the last one of a prompt's follow (👌 or ⚠️: nothing changes after it)?</summary>
    /// <param name="emoji">The emoji shown.</param>
    /// <returns>True for the final ones.</returns>
    inline bool IsFinalPromptReaction(std::wstring_view emoji)
    {
        return SameEmoji(emoji, kEmojiDone) || SameEmoji(emoji, kEmojiWarn);
    }

    /// <summary>
    /// The answer to a reply the owner wrote while Agentmaster was closed (§7.4, Telegram's backlog rule): it is not run; it is
    /// offered, quoted, to send now (✅) or drop (❌). The service watches the answer (MessageKind::Backlog).
    /// </summary>
    /// <param name="text">The reply's text.</param>
    /// <param name="ageWord">How old it is ("2 h"), as the owner reads it.</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the answer cannot be allocated.</exception>
    inline std::wstring BacklogOfferHtml(std::wstring_view text, std::wstring_view ageWord)
    {
        return L"\U0001F4E5 <b>Written while Agentmaster was closed</b> (" + Msg::TgEscapeHtml(ageWord) + L" ago): “" + Msg::TgEscapeHtml(Msg::TgFirstLine(text, 160)) +
               L"” — react " + std::wstring{ kEmojiYes } + L" to send it now, " + std::wstring{ kEmojiNo } + L" to drop it.";
    }

    /// <summary>The answer to a Claude Code command that cannot be sent from Teams (Msg::ParseRemoteCommand's refusals, §7.5).</summary>
    /// <param name="cmd">The command.</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the answer cannot be allocated.</exception>
    inline std::wstring RefusedCommandHtml(const Msg::RemoteCommand& cmd)
    {
        const std::wstring name = L"<code>/" + Msg::TgEscapeHtml(cmd.name) + L"</code>";
        if (cmd.kind == Msg::RemoteKind::RefusedLeavesTab)
        {
            return name + L" would take the conversation out of its tab: not sent.";
        }
        return name + L" runs at the PC only (it leaves nothing in the conversation): not sent.";
    }

    /// <summary>The commands a session's thread reads (§7.5), for /help and for a command Teams does not run yet.</summary>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring TeamsCommandsHelpHtml()
    {
        return L"<b>In a session's thread</b><br>"
               L"a plain reply: queued, sent when the session is ready (next before the runner's own prompts)<br>"
               L"<code>/steer &lt;words&gt;</code>: send the words now, into a running turn too<br>"
               L"<code>/enter</code>: press Enter on the prompt waiting in the box<br>"
               L"<code>/interrupt</code>: one Esc (stops the running turn)<br>"
               L"<code>/deactivate</code>: park the tab<br>"
               L"<code>/status</code>: Claude Code's status, account and plan usage<br>"
               L"<code>/screen</code>: the terminal's screen as a picture<br>"
               L"<code>/allscreen [level] [diffs] [last]</code>: the whole conversation as pictures (drawn from the transcript)<br>"
               L"<code>/forksession [model]</code>: fork the session into a new tab (it may ask interrupted or uninterrupted first)<br>"
               L"<code>/newsession [model]</code>: a new session in this session's folder (its own thread comes up)<br>"
               L"<code>/cc /command</code>: send a Claude Code command whose name is also Agentmaster's<br>"
               L"The card's emoji do the same (the legend post lists them).<br>"
               L"<b>In the channel</b>: a NEW post opens a session in a folder you pick; its words are the first prompt.";
    }

    /// <summary>How long a post Agentmaster sent is remembered by its fingerprint (PostFingerprint).</summary>
    inline constexpr int64_t kOwnPostFingerprintMs = 10LL * 60 * 1000;

    /// <summary>
    /// A post's FINGERPRINT (§7.2): an FNV-1a hash of its plain text's ASCII letters and digits, lowercased — nothing else. Both
    /// Agentmaster and the owner write as the owner, and the poll can read a post of Agentmaster's own BEFORE the send's answer
    /// has journaled its id; read as the owner's, Claude's reply would come back as a prompt. So every post is fingerprinted
    /// when it is queued, and a reply whose fingerprint matches one of the thread's recent posts is Agentmaster's. Only letters
    /// and digits count, because Teams rewrites what it stores (emoji become &lt;emoji&gt; elements, spacing and tags change).
    /// </summary>
    /// <param name="plainText">The post's plain text (PlainTextFromTeamsHtml of what was sent, or of what was read).</param>
    /// <returns>The fingerprint; 0 when the text holds no letter or digit (such a post is never matched).</returns>
    inline uint64_t PostFingerprint(std::wstring_view plainText) noexcept
    {
        uint64_t h = 14695981039346656037ull;
        bool any = false;
        for (const wchar_t c : plainText)
        {
            wchar_t x = c;
            if (x >= L'A' && x <= L'Z')
            {
                x = static_cast<wchar_t>(x - L'A' + L'a');
            }
            else if (!((x >= L'a' && x <= L'z') || (x >= L'0' && x <= L'9')))
            {
                continue;
            }
            h ^= static_cast<uint16_t>(x);
            h *= 1099511628211ull;
            any = true;
        }
        return any ? h : 0;
    }

    /// <summary>A span the owner reads ("45 s", "12 min", "3 h", "2 d"): how old a backlog reply is.</summary>
    /// <param name="ms">The span (ms).</param>
    /// <returns>The words.</returns>
    /// <exception cref="std::bad_alloc">If the words cannot be allocated.</exception>
    inline std::wstring SpanWord(int64_t ms)
    {
        const int64_t s = ms < 0 ? 0 : ms / 1000;
        if (s < 60)
        {
            return std::to_wstring(s) + L" s";
        }
        if (s < 3600)
        {
            return std::to_wstring(s / 60) + L" min";
        }
        if (s < 86'400)
        {
            return std::to_wstring(s / 3600) + L" h";
        }
        return std::to_wstring(s / 86'400) + L" d";
    }

    // ===== 19. Emoji actions: confirms, the screen's text, what is not here yet =============================================

    /// <summary>How long a confirm waits for its ✅ (§8).</summary>
    inline constexpr int64_t kConfirmWindowMs = 60'000;
    /// <summary>The most characters of a screen's text posted (its END is kept: the box and the newest lines are there).</summary>
    inline constexpr size_t kScreenTextMaxChars = 40'000;

    /// <summary>
    /// The confirm a restart (always) or a park during a turn asks (§8, ActionNeedsConfirm): what it ends, and "react ✅
    /// within 60 s". The service watches it (MessageKind::Confirm) and acts on the owner's ✅.
    /// </summary>
    /// <param name="a">Action::Restart or Action::Park.</param>
    /// <param name="title">The session's title ("" = "this session").</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the post cannot be allocated.</exception>
    inline std::wstring ConfirmPostHtml(Action a, std::wstring_view title)
    {
        const std::wstring name = Msg::TgEscapeHtml(Msg::TgOneLineTitle(title.empty() ? std::wstring_view{ L"this session" } : title));
        if (a == Action::Restart)
        {
            return std::wstring{ kEmojiRestart } + L" <b>Restart " + name + L"?</b> Its claude ends and starts again (the conversation goes on). React " +
                   std::wstring{ kEmojiYes } + L" within 60 s.";
        }
        return std::wstring{ kEmojiPark } + L" <b>Park " + name + L" now?</b> A turn is running: parking cuts it off. React " + std::wstring{ kEmojiYes } +
               L" within 60 s.";
    }

    /// <summary>The word a confirm's watch carries (Watched::payload) for its action.</summary>
    /// <param name="a">The action.</param>
    /// <returns>"restart", "park", or "" for an action that is never confirmed.</returns>
    inline const wchar_t* ConfirmWord(Action a) noexcept
    {
        return a == Action::Restart ? L"restart" : (a == Action::Park ? L"park" : L"");
    }

    /// <summary>ConfirmWord's inverse.</summary>
    /// <param name="w">The word.</param>
    /// <returns>The action; Action::None for any other word.</returns>
    inline Action ConfirmActionFromWord(std::wstring_view w) noexcept
    {
        if (w == L"restart")
        {
            return Action::Restart;
        }
        if (w == L"park")
        {
            return Action::Park;
        }
        return Action::None;
    }

    /// <summary>
    /// The terminal's visible text as a post (🖼️ until the picture comes, §8): a code block of the text, trailing blank lines
    /// dropped, its END kept past kScreenTextMaxChars (never starting on half a surrogate pair).
    /// </summary>
    /// <param name="text">The window's answer to the "screen" action.</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the post cannot be allocated.</exception>
    inline std::wstring ScreenTextPostHtml(std::wstring_view text)
    {
        std::wstring t{ text };
        while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' '))
        {
            t.pop_back();
        }
        if (t.size() > kScreenTextMaxChars)
        {
            t = t.substr(t.size() - kScreenTextMaxChars);
            if (!t.empty() && t.front() >= 0xDC00 && t.front() <= 0xDFFF)
            {
                t.erase(0, 1);
            }
        }
        if (t.empty())
        {
            return L"<i>The screen is empty.</i>";
        }
        return L"<b>\U0001F5A5 Screen</b><pre>" + Msg::TgEscapeHtml(t) + L"</pre>";
    }

    /// <summary>The answer for an emoji whose action this build does not run from Teams yet (§8).</summary>
    /// <param name="a">The action.</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the answer cannot be allocated.</exception>
    inline std::wstring NotYetHtml(Action a)
    {
        return L"<i>" + std::wstring{ ActionWord(a) } + L" from Teams comes with a later update: use the PC (or the Telegram bot) for it now.</i>";
    }

    // ===== 20. Pictures in a post ===========================================================================================

    /// <summary>
    /// The largest picture one post carries (its PNG bytes): the post is one JSON request, the picture in it as Base64 (4/3 of
    /// its size), and Graph refuses a request past ~4 MB.
    /// </summary>
    inline constexpr size_t kImagePostMaxBytes = 2'800'000;

    /// <summary>Standard Base64 (RFC 4648 §4, with padding): a picture's bytes in a post's `hostedContents`.</summary>
    /// <param name="bytes">The bytes.</param>
    /// <returns>The text ("" for no bytes).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::string Base64Encode(std::string_view bytes)
    {
        static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 3 <= bytes.size(); i += 3)
        {
            const uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                               (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) | static_cast<unsigned char>(bytes[i + 2]);
            out.push_back(kAlphabet[(v >> 18) & 0x3F]);
            out.push_back(kAlphabet[(v >> 12) & 0x3F]);
            out.push_back(kAlphabet[(v >> 6) & 0x3F]);
            out.push_back(kAlphabet[v & 0x3F]);
        }
        const size_t rest = bytes.size() - i;
        if (rest > 0)
        {
            uint32_t v = static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
            if (rest == 2)
            {
                v |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8;
            }
            out.push_back(kAlphabet[(v >> 18) & 0x3F]);
            out.push_back(kAlphabet[(v >> 12) & 0x3F]);
            out.push_back(rest == 2 ? kAlphabet[(v >> 6) & 0x3F] : '=');
            out.push_back('=');
        }
        return out;
    }

    /// <summary>
    /// A reply that shows a PNG inline (§8's 🖼️, M6's whole conversation): the caption, then the picture as the post's own
    /// hosted content (`<img src="../hostedContents/1/$value">` and a `hostedContents` entry with temporary id 1 — measured to
    /// work, TEAMS.md §15.1). The caller keeps the PNG under kImagePostMaxBytes.
    /// </summary>
    /// <param name="captionHtml">The caption (Teams HTML; "" = none).</param>
    /// <param name="png">The picture's bytes.</param>
    /// <returns>The post's JSON body.</returns>
    /// <exception cref="std::bad_alloc">If the body cannot be allocated.</exception>
    inline json::Value BuildImagePostJson(std::wstring_view captionHtml, std::string_view png)
    {
        std::wstring content{ captionHtml };
        if (!content.empty())
        {
            content += L"<br>";
        }
        content += L"<img src=\"../hostedContents/1/$value\">";
        json::Value body = json::Value::MkObj();
        body.Set(L"contentType", json::Value::MkStr(L"html"));
        body.Set(L"content", json::Value::MkStr(std::move(content)));
        json::Value hosted = json::Value::MkObj();
        hosted.Set(L"@microsoft.graph.temporaryId", json::Value::MkStr(L"1"));
        hosted.Set(L"contentBytes", json::Value::MkStr(Utf16FromUtf8(Base64Encode(png))));
        hosted.Set(L"contentType", json::Value::MkStr(L"image/png"));
        json::Value list = json::Value::MkArr();
        list.Push(std::move(hosted));
        json::Value post = json::Value::MkObj();
        post.Set(L"body", std::move(body));
        post.Set(L"hostedContents", std::move(list));
        return post;
    }

    // ===== 21. A new session from a new post (§9.1) =========================================================================

    /// <summary>How long the folder step of a new post waits for the owner's pick.</summary>
    inline constexpr int64_t kWizardStepMs = 60LL * 60 * 1000;
    /// <summary>The most folders the step lists (one keycap each: 1️⃣ … 🔟).</summary>
    inline constexpr size_t kWizardMaxFolders = 10;

    /// <summary>
    /// What a new post's folder step carries in its watch (Watched::payload), so a restart in the middle still knows it: the
    /// first prompt (the post's words), the post's deep link, and the folders the step listed, in their order.
    /// </summary>
    struct WizardData
    {
        /// <summary>The owner's words: the session's first prompt.</summary>
        std::wstring prompt;
        /// <summary>The post's deep link (the thread's webUrl once the session is live).</summary>
        std::wstring webUrl;
        /// <summary>The folders listed, 1️⃣ first.</summary>
        std::vector<std::wstring> folders;
    };

    /// <summary>The wizard's watch payload: its fields joined by U+001F (a character no prompt, link or folder path holds).</summary>
    /// <param name="d">The data.</param>
    /// <returns>The payload.</returns>
    /// <exception cref="std::bad_alloc">If the payload cannot be allocated.</exception>
    inline std::wstring WizardPayload(const WizardData& d)
    {
        std::wstring p = d.prompt;
        // A U+001F inside the prompt would cut it short when read back: it is replaced (it shows nothing anyway).
        std::replace(p.begin(), p.end(), L'\x1F', L' ');
        p += L'\x1F';
        p += d.webUrl;
        for (const std::wstring& f : d.folders)
        {
            p += L'\x1F';
            p += f;
        }
        return p;
    }

    /// <summary>WizardPayload's inverse.</summary>
    /// <param name="payload">The payload.</param>
    /// <returns>The data (a payload without a separator is all prompt).</returns>
    /// <exception cref="std::bad_alloc">If the data cannot be allocated.</exception>
    inline WizardData ParseWizardPayload(std::wstring_view payload)
    {
        WizardData d;
        std::vector<std::wstring> parts;
        size_t at = 0;
        for (;;)
        {
            const size_t sep = payload.find(L'\x1F', at);
            parts.emplace_back(payload.substr(at, sep == std::wstring_view::npos ? std::wstring_view::npos : sep - at));
            if (sep == std::wstring_view::npos)
            {
                break;
            }
            at = sep + 1;
        }
        d.prompt = parts[0];
        if (parts.size() > 1)
        {
            d.webUrl = parts[1];
        }
        for (size_t k = 2; k < parts.size(); ++k)
        {
            if (!parts[k].empty())
            {
                d.folders.push_back(parts[k]);
            }
        }
        return d;
    }

    /// <summary>
    /// The folder step of a new session (§9.1): the post's first line quoted, then "📁 Which folder? React 1️⃣ … or reply with
    /// a folder's path", the folders numbered with their keycaps. With no folder to list, only the typed path is asked for.
    /// </summary>
    /// <param name="prompt">The post's words.</param>
    /// <param name="folders">The folders (at most kWizardMaxFolders are listed).</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the post cannot be allocated.</exception>
    inline std::wstring WizardStepHtml(std::wstring_view prompt, const std::vector<std::wstring>& folders)
    {
        std::wstring h = L"\U0001F195 <b>New session</b> for “" + Msg::TgEscapeHtml(Msg::TgFirstLine(prompt, 120)) + L"”<br>\U0001F4C1 Which folder? ";
        if (folders.empty())
        {
            return h + L"Reply with the folder's full path.";
        }
        h += L"React with its number, or reply with a folder's full path:";
        const size_t n = (std::min)(folders.size(), kWizardMaxFolders);
        for (size_t k = 0; k < n; ++k)
        {
            h += L"<br>" + KeycapEmoji(static_cast<int>(k + 1)) + L" <code>" + Msg::TgEscapeHtml(folders[k]) + L"</code>";
        }
        return h;
    }

    // ===== 22. The whole conversation as pictures (📜, /allscreen) ==========================================================

    /// <summary>
    /// The tallest picture of a conversation in Teams (§8 📜), in pixels with its padding. A part is a post's own hosted content
    /// and must stay under kImagePostMaxBytes; Telegram's 32,000 px parts go as documents, which a Teams post here cannot
    /// carry. 8,000 px (~330 rows of text) is an ESTIMATE, not measured in Teams: a part past the bytes cap is told in words
    /// (TeamsService::_postImage). Ten parts hold ~3,300 rows; a longer conversation keeps its most recent rows.
    /// </summary>
    inline constexpr int kAllScreenPartHeightPx = 8000;

    /// <summary>
    /// A conversation picture's caption (§8 📜; Telegram's §21.39 caption, as Teams HTML): what was drawn, which part, and which
    /// turns it shows — "📜 <title> · conversation (full + diffs) · 2/4 · turns 12–31", "📜 <title> · last turn (max) · turn 31".
    /// </summary>
    /// <param name="titleLine">The session's one-line title (escaped here).</param>
    /// <param name="lastTurnOnly">Last Turn: only the newest turn was drawn.</param>
    /// <param name="levelName">The level's name (TurnsLevelName).</param>
    /// <param name="diffs">File Diffs: every file change drawn as a diff.</param>
    /// <param name="index">The part, 0-based.</param>
    /// <param name="count">How many parts the conversation has (the part is named only when there are several).</param>
    /// <param name="firstTurn">The turn of the part's first row (0 or less = the rows above the first turn: read as turn 1).</param>
    /// <param name="lastTurn">The turn of its last row (0 = no turn: no turns named).</param>
    /// <returns>The caption, Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the caption cannot be allocated.</exception>
    inline std::wstring AllScreenCaptionHtml(std::wstring_view titleLine, bool lastTurnOnly, std::wstring_view levelName, bool diffs, size_t index, size_t count, int firstTurn, int lastTurn)
    {
        std::wstring caption = L"\U0001F4DC <b>" + Msg::TgEscapeHtml(titleLine) + L"</b> \x00B7 " + (lastTurnOnly ? L"last turn" : L"conversation") + L" (" +
                               std::wstring{ levelName } + (diffs ? L" + diffs" : L"") + L")";
        if (count > 1)
        {
            caption += L" \x00B7 " + std::to_wstring(index + 1) + L"/" + std::to_wstring(count);
        }
        if (lastTurn > 0)
        {
            // The sheet counts the rows above the first turn heading (the path legend, a Last Turn's note) as that turn; the 1
            // only guards a sheet with none.
            const int first = (std::max)(1, firstTurn);
            caption += first == lastTurn ? L" \x00B7 turn " + std::to_wstring(first) : L" \x00B7 turns " + std::to_wstring(first) + L"\x2013" + std::to_wstring(lastTurn);
        }
        return caption;
    }

    /// <summary>
    /// The line after pictures that left the START of a long conversation out (§8 📜; Telegram's §21.39 notice): how many rows,
    /// through which turn, and what draws more of it — a shorter level (typed, Teams has no level picker), or no diffs.
    /// </summary>
    /// <param name="lastTurnOnly">Last Turn was asked for (its rows are all one turn's: no "through turn").</param>
    /// <param name="parts">How many pictures were drawn.</param>
    /// <param name="droppedRows">The rows left out at the top.</param>
    /// <param name="droppedThroughTurn">The last turn that lost rows (0 = not known).</param>
    /// <param name="level">The level drawn.</param>
    /// <param name="diffs">File Diffs was asked for.</param>
    /// <returns>The line, Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring AllScreenCutNoticeHtml(bool lastTurnOnly, size_t parts, int droppedRows, int droppedThroughTurn, TurnsLevel level, bool diffs)
    {
        const std::wstring typedLast = lastTurnOnly ? L" last" : L"";
        std::wstring h = std::wstring{ lastTurnOnly ? L"The last turn" : L"The conversation" } + L" is longer than " + std::to_wstring(parts) + L" pictures hold: its first " +
                         std::to_wstring(droppedRows) + L" rows";
        if (droppedThroughTurn > 0 && !lastTurnOnly)
        {
            h += L" (through turn " + std::to_wstring(droppedThroughTurn) + L")";
        }
        h += L" are left out.";
        // Full (the default) keeps the most per turn, so a long session reaches the cap there first.
        if (level >= TurnsLevel::Full)
        {
            h += L" A shorter level draws more of it: <code>/allscreen default" + typedLast + L"</code> or <code>/allscreen compact" + typedLast + L"</code>.";
        }
        else if (level == TurnsLevel::Default)
        {
            h += L" Compact draws more of it: <code>/allscreen compact" + typedLast + L"</code>.";
        }
        if (diffs)
        {
            h += L" Without diffs the pictures are shorter too.";
        }
        return h;
    }

    /// <summary>The answer to a /allscreen whose words are not a level, "diffs" or "last" (Telegram's §21.39 usage, as Teams HTML).</summary>
    /// <param name="defaultLevel">The level a bare /allscreen draws (the cog's Messaging tab: one setting for both connectors).</param>
    /// <returns>The usage, Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring AllScreenUsageHtml(TurnsLevel defaultLevel)
    {
        return L"Usage: <code>/allscreen [minimal|compact|default|full|max] [diffs] [last]</code>. Bare, it draws " + std::wstring{ Msg::AllScreenLevelWord(defaultLevel) } +
               L" (the default level, set in the cog's Messaging tab); <code>diffs</code> adds every file change as a diff, whatever the level; <code>last</code> "
               L"draws the last turn only.";
    }

    // ===== 23. A fork from Teams (🔀, /forksession) =========================================================================

    /// <summary>How long a fork question waits for the owner's pick (Telegram's single-use buttons live an hour).</summary>
    inline constexpr int64_t kForkQuestionMs = 60LL * 60 * 1000;

    /// <summary>
    /// Why a fork is asked about (HOOKS.md "Interrupted turns"): AssessForkContinuation's three situations, each with its own
    /// meaning of "Fork interrupted" and "Fork uninterrupted" (ForkChoiceFor). Persisted in a question's payload by word.
    /// </summary>
    enum class ForkSituation
    {
        /// <summary>A: the fork would continue the source's turn by itself (the source runs, or its tail is one Claude Code continues).</summary>
        Continues,
        /// <summary>B: the source's turn died on an API error after output began: the fork would wait at the error.</summary>
        ApiError,
        /// <summary>C: the owner interrupted the source's turn: the fork would wait at the interrupt.</summary>
        UserInterrupt
    };

    /// <summary>A situation's word in a question's payload ("continues", "api-error", "user-interrupt").</summary>
    /// <param name="s">The situation.</param>
    /// <returns>The word (a static string; "continues" for a value outside the enum).</returns>
    inline const wchar_t* ForkSituationWord(ForkSituation s) noexcept
    {
        switch (s)
        {
        case ForkSituation::ApiError:
            return L"api-error";
        case ForkSituation::UserInterrupt:
            return L"user-interrupt";
        case ForkSituation::Continues:
        default:
            return L"continues";
        }
    }

    /// <summary>ForkSituationWord's inverse.</summary>
    /// <param name="w">The word.</param>
    /// <returns>The situation; nothing for an unknown word (a payload this build cannot read asks nothing: no fork).</returns>
    inline std::optional<ForkSituation> ForkSituationFromWord(std::wstring_view w) noexcept
    {
        for (const ForkSituation s : { ForkSituation::Continues, ForkSituation::ApiError, ForkSituation::UserInterrupt })
        {
            if (w == ForkSituationWord(s))
            {
                return s;
            }
        }
        return std::nullopt;
    }

    /// <summary>
    /// What an answer of a fork question launches (Telegram's tokens, Messaging.Inbound.cpp): 1️⃣ "Fork interrupted" or 2️⃣
    /// "Fork uninterrupted", in the question's situation. A: Interrupted (the stop watch) / Uninterrupted. B: TakeBackAndInterrupt
    /// (the error taken back AND the stop watch: a regular interrupt replaces the error) / TakeBackAndContinue. C: Uninterrupted
    /// (a plain fork that waits at the interrupt, which is a regular one already) / TakeBackAndContinue (the interrupt taken back).
    /// </summary>
    /// <param name="s">The question's situation.</param>
    /// <param name="uninterrupted">True for 2️⃣ "Fork uninterrupted", false for 1️⃣ "Fork interrupted".</param>
    /// <returns>The fork's turn choice (RemoteLaunchRequest::forkTurn).</returns>
    inline ForkTurnChoice ForkChoiceFor(ForkSituation s, bool uninterrupted) noexcept
    {
        switch (s)
        {
        case ForkSituation::ApiError:
            return uninterrupted ? ForkTurnChoice::TakeBackAndContinue : ForkTurnChoice::TakeBackAndInterrupt;
        case ForkSituation::UserInterrupt:
            return uninterrupted ? ForkTurnChoice::TakeBackAndContinue : ForkTurnChoice::Uninterrupted;
        case ForkSituation::Continues:
        default:
            return uninterrupted ? ForkTurnChoice::Uninterrupted : ForkTurnChoice::Interrupted;
        }
    }

    /// <summary>What a fork question carries in its watch (Watched::payload), so its answer forks exactly what was asked.</summary>
    struct ForkQuestionData
    {
        /// <summary>The situation the question was asked in (what its two answers mean).</summary>
        ForkSituation situation{ ForkSituation::Continues };
        /// <summary>The source's working folder (the fork runs there).</summary>
        std::wstring dir;
        /// <summary>The source's title (the window derives the fork's own: "(fork)", "(fork 2)" …).</summary>
        std::wstring title;
        /// <summary>A per-launch model (/forksession's argument; "" = the settings' model).</summary>
        std::wstring model;
    };

    /// <summary>A fork question's payload: the situation's word, the folder, the title and the model, joined by U+001F.</summary>
    /// <param name="d">The data.</param>
    /// <returns>The payload (a U+001F inside a field is replaced by a space: it would cut the field short).</returns>
    /// <exception cref="std::bad_alloc">If the payload cannot be allocated.</exception>
    inline std::wstring ForkQuestionPayload(const ForkQuestionData& d)
    {
        const auto field = [](std::wstring_view v) {
            std::wstring f{ v };
            std::replace(f.begin(), f.end(), L'\x1F', L' ');
            return f;
        };
        return std::wstring{ ForkSituationWord(d.situation) } + L'\x1F' + field(d.dir) + L'\x1F' + field(d.title) + L'\x1F' + field(d.model);
    }

    /// <summary>ForkQuestionPayload's inverse.</summary>
    /// <param name="payload">The payload.</param>
    /// <returns>The data; nothing for a payload this build cannot read (an unknown situation, a missing folder): no fork.</returns>
    /// <exception cref="std::bad_alloc">If the data cannot be allocated.</exception>
    inline std::optional<ForkQuestionData> ParseForkQuestionPayload(std::wstring_view payload)
    {
        std::vector<std::wstring_view> parts;
        size_t at = 0;
        for (;;)
        {
            const size_t sep = payload.find(L'\x1F', at);
            parts.push_back(payload.substr(at, sep == std::wstring_view::npos ? std::wstring_view::npos : sep - at));
            if (sep == std::wstring_view::npos)
            {
                break;
            }
            at = sep + 1;
        }
        const auto situation = ForkSituationFromWord(parts[0]);
        if (!situation || parts.size() < 2 || parts[1].empty())
        {
            return std::nullopt;
        }
        ForkQuestionData d;
        d.situation = *situation;
        d.dir = std::wstring{ parts[1] };
        d.title = parts.size() > 2 ? std::wstring{ parts[2] } : std::wstring{};
        d.model = parts.size() > 3 ? std::wstring{ parts[3] } : std::wstring{};
        return d;
    }

    /// <summary>The facts a fork question says (AssessForkContinuation's outlook, read by the service), so its words are built purely.</summary>
    struct ForkQuestionFacts
    {
        /// <summary>The situation.</summary>
        ForkSituation situation{ ForkSituation::Continues };
        /// <summary>A: the source runs a turn right now (else it stopped in the middle of one).</summary>
        bool sourceRunning{ false };
        /// <summary>A: the tool call the source was running, which runs again in the fork ("" = none).</summary>
        std::wstring droppedTool;
        /// <summary>B: the API error's words ("" = not known).</summary>
        std::wstring errorText;
        /// <summary>C: the owner interrupted a tool call (else the middle of a reply).</summary>
        bool duringToolUse{ false };
        /// <summary>C: "Fork uninterrupted" sends the interrupted prompt again (the interrupt plan's target is a prompt).</summary>
        bool sendsPromptAgain{ false };
        /// <summary>C: that prompt's words ("" = not shown).</summary>
        std::wstring promptText;
        /// <summary>B, C: CLAUDE_CODE_RESUME_INTERRUPTED_TURN is off in a settings file, so the fork cannot carry on by itself.</summary>
        bool switchOffInSettings{ false };
    };

    /// <summary>
    /// The fork question (§8 🔀; Telegram's _askForkTurnBeforeLaunch words, as Teams HTML with numbered answers instead of
    /// buttons): what the source's turn is, then 1️⃣ "Fork interrupted" and 2️⃣ "Fork uninterrupted" with what each does in the
    /// situation, and how to answer (react 1️⃣ or 2️⃣ within an hour, ❌ for no fork).
    /// </summary>
    /// <param name="title">The source's title (escaped here; "" = "the session").</param>
    /// <param name="f">The facts.</param>
    /// <returns>The question, Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring ForkQuestionHtml(std::wstring_view title, const ForkQuestionFacts& f)
    {
        const std::wstring one = KeycapEmoji(1);
        const std::wstring two = KeycapEmoji(2);
        std::wstring h = L"\U0001F500 <b>" + Msg::TgEscapeHtml(title.empty() ? std::wstring_view{ L"the session" } : title) + L"</b> ";
        // The take-back's own words (B and C): one line in the SOURCE's transcript, which the fork reads once at its start.
        const std::wstring takeBackTail = f.switchOffInSettings ?
                                              std::wstring{ L"; CLAUDE_CODE_RESUME_INTERRUPTED_TURN is off in your settings, so your next message in the fork continues the turn." } :
                                              std::wstring{};
        switch (f.situation)
        {
        case ForkSituation::ApiError:
            h += L"stopped on an API error";
            if (!f.errorText.empty())
            {
                h += L": <i>" + Msg::TgEscapeHtml(f.errorText) + L"</i>";
            }
            h += L". A fork starts from the same conversation, so it would wait at that error too.<br><br>";
            h += one + (f.switchOffInSettings ? L" <b>Fork interrupted</b>: Agentmaster takes the error back for the fork too, and the fork waits for you." :
                                                L" <b>Fork interrupted</b>: Agentmaster takes the error back for the fork too, then presses Esc the moment the fork starts the turn, "
                                                L"so it waits for you with a regular interrupt in place of the error.");
            h += L"<br>" + two + L" <b>Fork uninterrupted</b>: Agentmaster takes the error back for the fork (one line added to the source's transcript; the source stays where it is)";
            h += f.switchOffInSettings ? takeBackTail : std::wstring{ L", and the fork starts at once and carries on with the turn by itself." };
            break;
        case ForkSituation::UserInterrupt:
            h += f.duringToolUse ? L"was interrupted by you during a tool call." : L"was interrupted by you in the middle of a reply.";
            h += L" A fork starts from the same conversation, so it would wait at your interrupt too.<br><br>";
            h += one + L" <b>Fork interrupted</b>: the fork waits for you, interrupted like its source.";
            h += L"<br>" + two + L" <b>Fork uninterrupted</b>: Agentmaster takes the interrupt back for the fork (one line added to the source's transcript; the source stays where it is)";
            if (f.switchOffInSettings)
            {
                h += takeBackTail;
            }
            else if (f.sendsPromptAgain)
            {
                h += L", and the fork starts at once and sends your prompt";
                if (!f.promptText.empty())
                {
                    h += L" \x201C" + Msg::TgEscapeHtml(f.promptText) + L"\x201D";
                }
                h += L" again.";
            }
            else
            {
                h += L", and the fork starts at once and carries on from the last step that finished.";
            }
            break;
        case ForkSituation::Continues:
        default:
            h += f.sourceRunning ? L"is in the middle of a turn." : L"stopped in the middle of a turn.";
            h += L" Claude Code continues that turn in the fork by itself as soon as the fork's Claude starts";
            if (!f.droppedTool.empty())
            {
                h += L" \x2014 the <code>" + Msg::TgEscapeHtml(f.droppedTool) + L"</code> call it was running runs again in the fork";
            }
            h += L".<br><br>" + one + L" <b>Fork interrupted</b>: Agentmaster locks the fork's tab and presses Esc the moment it starts running, so the fork waits for you.";
            h += L"<br>" + two + L" <b>Fork uninterrupted</b>: the fork runs the turn on its own.";
            break;
        }
        h += L"<br>React " + one + L" or " + two + L" within an hour, " + std::wstring{ kEmojiNo } + L" for no fork.";
        return h;
    }

    // ===== 24. The 🔎 Search thread (§9.3) ==================================================================================

    /// <summary>The Search thread root's subject (the channel's thread list shows it).</summary>
    inline constexpr std::wstring_view kSearchSubject = L"\U0001F50E Agentmaster \x00B7 search your sessions";
    /// <summary>The most sessions one answer lists; the rest are counted ("… and 4 more").</summary>
    inline constexpr size_t kSearchListMax = 30;

    /// <summary>One session a search answer lists: the agent's row (Msg::SearchRow) as the service copies it, with where it stands.</summary>
    struct SearchListing
    {
        /// <summary>The session.</summary>
        std::wstring sessionId;
        /// <summary>Its title ("" = its id's start is shown).</summary>
        std::wstring title;
        /// <summary>Its working folder ("" = not known).</summary>
        std::wstring dir;
        /// <summary>Its last activity (Unix ms; 0 = not known).</summary>
        int64_t lastActivityMs{ 0 };
        /// <summary>The prompts the owner typed in it.</summary>
        int prompts{ 0 };
        /// <summary>The model's one line of evidence for it ("" = none).</summary>
        std::wstring why;
        /// <summary>A tab hosts it now (a pick tells where it is; a closed one is resumed).</summary>
        bool open{ false };
    };

    /// <summary>The Search thread's root post: what it does and how to use it (the answers come as replies under it).</summary>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring SearchRootHtml()
    {
        return L"\U0001F50E <b>Search your sessions</b><br>"
               L"Reply here in plain words (\x201Cthe drag crash fix last week\x201D): Claude searches every Claude Code session on the PC "
               L"and lists the ones that fit, numbered, with why.<br>"
               L"Reply with a number to open one: an open session is named with its thread; a closed one is resumed into a tab and gets "
               L"a thread.<br><code>/clear</code> starts the search conversation over.";
    }

    /// <summary>
    /// A search answer (§9.3; Telegram's evidence cards, §21.10, as lines): the model's sentence, then one entry per listed
    /// session — its number, its title (▶ when a tab hosts it), its folder, how long ago it was used, its prompts — with the
    /// model's why under it; past kSearchListMax, a count; then how to pick.
    /// </summary>
    /// <param name="answer">The model's sentence ("" = none).</param>
    /// <param name="rows">The sessions it showed, in its order.</param>
    /// <param name="nowMs">The clock (Unix ms), for "2h ago".</param>
    /// <returns>Teams HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring SearchAnswerHtml(std::wstring_view answer, const std::vector<SearchListing>& rows, int64_t nowMs)
    {
        std::wstring h = L"\U0001F50E " + (answer.empty() ? std::wstring{ L"<i>No answer text.</i>" } : Msg::TgEscapeHtml(answer));
        const size_t listed = (std::min)(rows.size(), kSearchListMax);
        for (size_t k = 0; k < listed; ++k)
        {
            const SearchListing& r = rows[k];
            const std::wstring title = r.title.empty() ? (r.sessionId.size() > 8 ? r.sessionId.substr(0, 8) : r.sessionId) : Msg::TgOneLineTitle(r.title);
            h += L"<br><b>" + std::to_wstring(k + 1) + L".</b> " + (r.open ? L"\x25B6 " : L"") + L"<b>" + Msg::TgEscapeHtml(title) + L"</b>";
            if (!r.dir.empty())
            {
                h += L" \x00B7 <code>" + Msg::TgEscapeHtml(r.dir) + L"</code>";
            }
            if (r.lastActivityMs > 0)
            {
                h += L" \x00B7 " + Msg::TgShortSpan(nowMs - r.lastActivityMs) + L" ago";
            }
            if (r.prompts > 0)
            {
                h += L" \x00B7 " + std::to_wstring(r.prompts) + (r.prompts == 1 ? L" prompt" : L" prompts");
            }
            if (!r.why.empty())
            {
                h += L"<br>&nbsp;&nbsp;<i>" + Msg::TgEscapeHtml(r.why) + L"</i>";
            }
        }
        if (rows.size() > listed)
        {
            h += L"<br><i>\x2026 and " + std::to_wstring(rows.size() - listed) + L" more: ask more narrowly to see them.</i>";
        }
        if (listed > 0)
        {
            h += L"<br>Reply with a number to open it (\x25B6 = open in a tab; a closed one is resumed into a tab).";
        }
        return h;
    }

    /// <summary>What a reply in the Search thread is (§9.3).</summary>
    enum class SearchReplyKind
    {
        /// <summary>Words: a search request to the agent.</summary>
        Request,
        /// <summary>A number (or a keycap emoji typed): the session with that number in the last answer.</summary>
        Pick,
        /// <summary>/clear or /reset: the search conversation starts over.</summary>
        Clear,
        /// <summary>/help or /start: the root's own text again.</summary>
        Help
    };

    /// <summary>A reply in the Search thread, classified (ClassifySearchReply).</summary>
    struct SearchReply
    {
        /// <summary>What it is.</summary>
        SearchReplyKind kind{ SearchReplyKind::Request };
        /// <summary>The number of a Pick (1 or more); 0 otherwise.</summary>
        int pick{ 0 };
        /// <summary>The request's words (trimmed); "" for the other kinds.</summary>
        std::wstring text;
    };

    /// <summary>
    /// Classifies a reply in the Search thread: "/clear" and "/reset" clear, "/help" and "/start" show help, a lone number 1-999
    /// (a "#" before it allowed) or a keycap emoji picks, anything else is a request. A reply that is only another slash
    /// command is still a request: the model reads it as words.
    /// </summary>
    /// <param name="text">The reply's plain text.</param>
    /// <returns>The classification.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be copied.</exception>
    inline SearchReply ClassifySearchReply(std::wstring_view text)
    {
        SearchReply r;
        const std::wstring t = detail::Trim(text);
        const auto isCommand = [&t](std::wstring_view name) {
            if (t.size() < name.size() + 1 || t[0] != L'/' || t.compare(1, name.size(), name) != 0)
            {
                return false;
            }
            // The word ends there: "/clear", "/clear now", "/clear@bot" — not "/clearly".
            return t.size() == name.size() + 1 || t[name.size() + 1] == L' ' || t[name.size() + 1] == L'@';
        };
        if (isCommand(L"clear") || isCommand(L"reset"))
        {
            r.kind = SearchReplyKind::Clear;
            return r;
        }
        if (isCommand(L"help") || isCommand(L"start"))
        {
            r.kind = SearchReplyKind::Help;
            return r;
        }
        std::wstring_view digits = t;
        if (!digits.empty() && digits.front() == L'#')
        {
            digits.remove_prefix(1);
        }
        if (!digits.empty() && digits.size() <= 3 && std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        {
            const int n = std::stoi(std::wstring{ digits });
            if (n >= 1)
            {
                r.kind = SearchReplyKind::Pick;
                r.pick = n;
                return r;
            }
        }
        if (const auto keycap = KeycapNumber(t))
        {
            r.kind = SearchReplyKind::Pick;
            r.pick = *keycap;
            return r;
        }
        r.text = t;
        return r;
    }
}
