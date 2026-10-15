// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's TOKENIZER (SESSIONS_DB.md §6.3): the one rule that turns a
// block of conversation text into the terms of the inverted index, and a query term into the
// tokens the planner looks up. Index side and query side run THIS code, so a word can never be
// tokenized one way when stored and another when searched.
//
// The rule:
//   - A token is a maximal run of WORD characters: a Unicode letter (with its combining marks), a
//     digit, or '_'. Everything else separates (spaces, punctuation, path separators, quotes ...).
//   - Tokens are folded to lower case (simple per-code-point case mapping).
//   - An identifier also contributes its PARTS as extra tokens: split on '_' and at camelCase
//     boundaries (lower->Upper, and the last Upper of an acronym before a lower: "HTMLParser" ->
//     "html", "parser"). "_RenderSessionsTable" -> "_rendersessionstable", "render", "sessions",
//     "table". Parts shorter than 2 characters are dropped (noise). The whole token is always kept,
//     so a substring of an identifier is found through the dictionary scan (§6.3 rule 1) either way;
//     the parts make whole-word matches rank (BM25) and let a separated query term match.
//   - A token longer than kMaxTokenChars is indexed as its prefix (a base64 run or a hash: kept
//     findable by its start, never ballooning the dictionary).
//   - Only the first kMaxIndexedChars characters of a block are tokenized (D4: the full text is
//     stored; the per-block index cap is 64 KB). A match past the cap is found by verification
//     scans only (the hot tier, `grep`).
//
// PURE and header-only (no ICU, no Windows): the character classes come from a caller-supplied
// classifier, so the m5 harness tests the rules with a plain one and the service plugs in ICU
// (u_isalpha / u_charType / u_tolower — SessionDbIndex.cpp).

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Agentmaster::SessionDbTokenizer
{
    // The longest token kept whole (longer ones are cut to this prefix).
    inline constexpr size_t kMaxTokenChars = 256;
    // The per-block tokenization cap (D4: "per-block index cap 64 KB").
    inline constexpr size_t kMaxIndexedChars = 64 * 1024;
    // Identifier parts shorter than this are not emitted (a lone "a" from "aValue" is noise).
    inline constexpr size_t kMinPartChars = 2;

    // What the tokenizer needs to know about one code point.
    struct CharClass
    {
        bool word{ false }; // a letter, a combining mark, a digit, or '_'
        bool upper{ false }; // an upper-case (or title-case) letter — camelCase boundaries
        bool lower{ false }; // a lower-case letter — camelCase boundaries
        char32_t folded{ 0 }; // the code point's simple lower-case mapping (itself when none)
    };

    // The classifier the tokenizer calls for every NON-ASCII code point (ASCII is classified inline).
    using Classifier = std::function<CharClass(char32_t)>;

    // A classifier that knows ASCII only: every non-ASCII code point >= U+0080 that is not a
    // separator-looking symbol counts as a word character with no case. Good enough for tests and
    // as a fallback; the service uses ICU. Pure.
    inline CharClass BasicClassify(char32_t cp) noexcept
    {
        CharClass c{};
        c.folded = cp;
        // General punctuation, the Latin-1 punctuation/symbol block, and a few common symbol ranges
        // separate; everything else above ASCII is treated as a word character (letters of every
        // script, CJK, combining marks).
        const bool symbol = (cp >= 0x80 && cp <= 0xBF && cp != 0xAA && cp != 0xB5 && cp != 0xBA) || cp == 0xD7 || cp == 0xF7 ||
                            (cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
                            (cp >= 0xFF00 && cp <= 0xFF0F) || cp == 0xFFFD;
        c.word = !symbol;
        return c;
    }

    // Decode the next code point of UTF-16 `s` at `i` (advancing i). A lone surrogate decodes as itself
    // (classified by the classifier, normally a separator). Pure.
    inline char32_t NextCodePoint(std::wstring_view s, size_t& i) noexcept
    {
        const char32_t c = s[i++];
        if (c >= 0xD800 && c <= 0xDBFF && i < s.size() && s[i] >= 0xDC00 && s[i] <= 0xDFFF)
        {
            const char32_t lo = s[i++];
            return 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
        return c;
    }

    // Append code point `cp` to UTF-16 `out`. Pure.
    inline void AppendCodePoint(std::wstring& out, char32_t cp)
    {
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
        else
        {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }

    // Classify one code point: ASCII inline, the rest through `classify`. Pure.
    inline CharClass Classify(char32_t cp, const Classifier& classify) noexcept
    {
        if (cp < 0x80)
        {
            CharClass c{};
            const bool upper = cp >= U'A' && cp <= U'Z';
            const bool lower = cp >= U'a' && cp <= U'z';
            const bool digit = cp >= U'0' && cp <= U'9';
            c.word = upper || lower || digit || cp == U'_';
            c.upper = upper;
            c.lower = lower;
            c.folded = upper ? cp + 32 : cp;
            return c;
        }
        return classify ? classify(cp) : BasicClassify(cp);
    }

    // The per-character facts the identifier split reads, one byte per raw character of a token.
    enum : uint8_t
    {
        kTokUpper = 1, // an upper-case (or title-case) letter
        kTokLower = 2, // a lower-case letter
        kTokUnderscore = 4, // '_'
    };

    // Split ONE folded token into its identifier parts (snake_case and camelCase) WITHOUT allocating:
    // `kinds[i]` is raw character i's case byte, `ends[i]` the folded length after raw character i. Each
    // part is reported as (offset, length) into the folded token, through `part`. Parts shorter than
    // kMinPartChars are dropped; when fewer than two remain, nothing is reported (one part IS the token).
    //   folded — the token, folded.
    //   kinds  — per raw character: kTokUpper / kTokLower / kTokUnderscore (0 for a digit, a mark, an uncased letter).
    //   ends   — per raw character: folded.size() after appending it.
    //   part   — called with (offset, length) for each part, in order.
    // Pure.
    template<typename Part>
    void ForEachIdentifierPart(std::wstring_view folded, const std::vector<uint8_t>& kinds, const std::vector<uint32_t>& ends, Part&& part)
    {
        // At most 64 parts are reported (an identifier with more is noise past that point).
        uint32_t offs[64];
        uint32_t lens[64];
        size_t count = 0;
        uint32_t partStart = 0; // folded offset where the current part began
        uint32_t partLen = 0; // folded length of the current part so far (underscores excluded)
        const auto flush = [&](uint32_t endOffset) {
            if (partLen >= kMinPartChars && count < 64)
            {
                offs[count] = partStart;
                lens[count] = endOffset - partStart;
                ++count;
            }
            partLen = 0;
        };
        const size_t n = kinds.size();
        for (size_t i = 0; i < n; ++i)
        {
            const uint32_t begin = i == 0 ? 0 : ends[i - 1];
            if (kinds[i] & kTokUnderscore)
            {
                flush(begin);
                partStart = ends[i]; // the next part starts after the underscore
                continue;
            }
            if (partLen != 0 && i > 0)
            {
                // lower -> Upper ("renderSessions": the S starts a part); Upper Upper lower: the last Upper of an
                // acronym starts the next part ("HTMLParser" -> "html", "parser").
                const bool lowerToUpper = (kinds[i - 1] & kTokLower) && (kinds[i] & kTokUpper);
                const bool acronymEnd = (kinds[i - 1] & kTokUpper) && (kinds[i] & kTokUpper) && i + 1 < n && (kinds[i + 1] & kTokLower);
                if (lowerToUpper || acronymEnd)
                {
                    flush(begin);
                    partStart = begin;
                }
            }
            if (partLen == 0)
            {
                partStart = begin;
            }
            partLen += ends[i] - begin;
        }
        flush(static_cast<uint32_t>(folded.size()));
        if (count < 2)
        {
            return; // one part == the token itself: nothing extra to add
        }
        for (size_t k = 0; k < count; ++k)
        {
            part(offs[k], lens[k]);
        }
    }

    // Tokenize `text` (only its first kMaxIndexedChars characters) and call `emit(token)` for every
    // token occurrence AND every identifier part occurrence, in text order. Tokens are folded and
    // capped at kMaxTokenChars. The views passed to `emit` are valid only during the call. Allocation-free
    // in the steady state (the buffers are reused from token to token). Pure (beyond the callbacks).
    template<typename Emit>
    void ForEachToken(std::wstring_view text, const Classifier& classify, Emit&& emit)
    {
        if (text.size() > kMaxIndexedChars)
        {
            text = text.substr(0, kMaxIndexedChars);
        }
        std::wstring folded;
        std::vector<uint8_t> kinds;
        std::vector<uint32_t> ends;
        folded.reserve(64);
        kinds.reserve(64);
        ends.reserve(64);
        bool hasCaseOrUnderscore = false;
        const auto finish = [&]() {
            if (kinds.empty())
            {
                return;
            }
            const std::wstring_view whole{ folded };
            emit(whole);
            // Parts only matter when the token mixes case or carries '_' (most tokens do not: skip the split).
            if (hasCaseOrUnderscore)
            {
                ForEachIdentifierPart(whole, kinds, ends, [&](uint32_t off, uint32_t len) {
                    const std::wstring_view p = whole.substr(off, len);
                    if (p != whole)
                    {
                        emit(p);
                    }
                });
            }
            kinds.clear();
            ends.clear();
            folded.clear();
            hasCaseOrUnderscore = false;
        };
        size_t i = 0;
        while (i < text.size())
        {
            const char32_t cp = NextCodePoint(text, i);
            const CharClass cls = Classify(cp, classify);
            if (!cls.word)
            {
                finish();
                continue;
            }
            if (kinds.size() < kMaxTokenChars)
            {
                const uint8_t kind = static_cast<uint8_t>((cls.upper ? kTokUpper : 0) | (cls.lower ? kTokLower : 0) | (cp == U'_' ? kTokUnderscore : 0));
                kinds.push_back(kind);
                AppendCodePoint(folded, cls.folded);
                ends.push_back(static_cast<uint32_t>(folded.size()));
                if (cls.upper || cp == U'_')
                {
                    hasCaseOrUnderscore = true;
                }
            }
            // Past the cap the token keeps consuming its characters (they are not separators) but
            // indexes only its prefix.
        }
        finish();
    }

    // The identifier parts of one token (the allocating form, for tests and diagnostics): tokenize `token`
    // alone and return the parts ForEachToken would emit beside it. Pure.
    inline std::vector<std::wstring> IdentifierParts(std::wstring_view token, const Classifier& classify)
    {
        std::vector<std::wstring> out;
        bool first = true;
        ForEachToken(token, classify, [&](std::wstring_view t) {
            if (first)
            {
                first = false; // the whole token
                return;
            }
            out.emplace_back(t);
        });
        return out;
    }
    // Term frequencies of one block: token -> occurrences (tokens and parts), for the postings.
    // Pure.
    inline std::unordered_map<std::wstring, uint32_t> TermFrequencies(std::wstring_view text, const Classifier& classify)
    {
        std::unordered_map<std::wstring, uint32_t> tf;
        ForEachToken(text, classify, [&tf](std::wstring_view t) { ++tf[std::wstring{ t }]; });
        return tf;
    }

    // Fold a string the way tokens are folded (every code point through the classifier's mapping,
    // separators kept as they are). The query side uses it for verification scans (the hot tier,
    // candidate checks), which compare against a block's folded text. Pure.
    inline std::wstring FoldText(std::wstring_view text, const Classifier& classify)
    {
        std::wstring out;
        out.reserve(text.size());
        size_t i = 0;
        while (i < text.size())
        {
            const char32_t cp = NextCodePoint(text, i);
            AppendCodePoint(out, Classify(cp, classify).folded);
        }
        return out;
    }

    // The query side of a term (§6.3): its tokens WITHOUT identifier parts (a query word is matched as
    // typed), folded, in order. One token and no separator anywhere => rule 1 (dictionary substring,
    // exact). Otherwise rule 2 (first token a suffix of a term, middle tokens whole terms, last a
    // prefix; candidates verified). Pure.
    struct QueryTokens
    {
        std::vector<std::wstring> tokens;
        bool hasSeparator{ false }; // the term holds a non-word character (or nothing but them)
    };
    inline QueryTokens TokenizeQueryTerm(std::wstring_view term, const Classifier& classify)
    {
        QueryTokens q;
        std::wstring cur;
        size_t i = 0;
        while (i < term.size())
        {
            const char32_t cp = NextCodePoint(term, i);
            const CharClass cls = Classify(cp, classify);
            if (!cls.word)
            {
                q.hasSeparator = true;
                if (!cur.empty())
                {
                    q.tokens.push_back(std::move(cur));
                    cur.clear();
                }
                continue;
            }
            if (cur.size() < kMaxTokenChars)
            {
                AppendCodePoint(cur, cls.folded);
            }
        }
        if (!cur.empty())
        {
            q.tokens.push_back(std::move(cur));
        }
        return q;
    }
}
