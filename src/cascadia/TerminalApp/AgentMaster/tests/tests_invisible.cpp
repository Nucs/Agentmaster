// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster M5 engine test harness - a themed partial file (see m5_tests.h for the list).
// * tests_invisible.cpp - DELIVERY.md section 15: Claude Code's invisible-character rule, mirrored
//                         (ClaudeInvisibleChars.{h,cpp}) - the wZe oracle table, the paste
//                         sanitize, the compare key on both sides of every prompt compare, the
//                         Hebrew cases, the registry echo/pull-echo/dedupe, the Enter-retry
//                         draft guard, and the hook forwarder's UTF-8 decoding + BOM.
// ======================================================================================
//
// WHY A GENERATED TABLE. The mirror is only worth anything if it agrees with claude byte for byte -
// a character we keep that claude removes cancels the Enter (the 2.1.277+ review prompt), and one we
// remove that claude keeps damages the text (a Hebrew RLM, an emoji joiner). So the table below is not
// written by hand: each row is the output of Claude Code 2.1.280's OWN cleanup function (`wZe`,
// extracted verbatim from the claude.exe bundle) run under node, escaped to pure-ASCII C++ literals.
// Regenerating it is DELIVERY.md section 15's "re-extracting the rule" recipe.
//
// This source stays pure ASCII on purpose (comments included): every non-ASCII character is written as
// an escape, since a literal invisible character in a test file is exactly the kind of thing that
// silently changes under an editor.

#include "m5_tests.h"

#include "../ClaudeInvisibleChars.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    // One captured oracle row: the input, claude's cleaned text and its counters.
    struct InvisibleOracleCase
    {
        const char* name;
        const wchar_t* input;
        const wchar_t* expected;
        size_t removedTotal;
        size_t tags;
        size_t bidi;
        size_t zeroWidth;
        size_t selectors;
        size_t other;
        size_t keptConditional;
        bool idempotent;
    };

    // GENERATED - do not hand-edit (DELIVERY.md section 15). Columns: name, input, claude's cleaned text,
    // removedTotal, removed tags / bidi / zeroWidth / selectors / other, keptConditional, idempotent.
    const InvisibleOracleCase kInvisibleOracle[] = {
        { "ascii", L"hello world", L"hello world", 0, 0, 0, 0, 0, 0, 0, true },
        { "tab-kept", L"a\x0009" L"b", L"a\x0009" L"b", 0, 0, 0, 0, 0, 0, 0, true },
        { "lf-kept", L"a\x000A" L"b", L"a\x000A" L"b", 0, 0, 0, 0, 0, 0, 0, true },
        { "crlf", L"a\x000D\x000A" L"b", L"a\x000A" L"b", 0, 0, 0, 0, 0, 0, 0, true },
        { "lone-cr", L"a\x000D" L"b", L"a\x000A" L"b", 1, 0, 0, 0, 0, 1, 0, true },
        { "ls", L"a\u2028b", L"a\x000A" L"b", 1, 0, 0, 0, 0, 1, 0, true },
        { "ps", L"a\u2029b", L"a\x000A" L"b", 1, 0, 0, 0, 0, 1, 0, true },
        { "vt", L"a\x000B" L"b", L"a\x000A" L"b", 1, 0, 0, 0, 0, 1, 0, true },
        { "ff", L"a\x000C" L"b", L"a\x000A" L"b", 1, 0, 0, 0, 0, 1, 0, true },
        { "nel", L"a\x0085" L"b", L"a\x000A" L"b", 1, 0, 0, 0, 0, 1, 0, true },
        { "bel", L"a\x0007" L"b", L"ab", 1, 0, 0, 0, 0, 1, 0, true },
        { "esc", L"a\x001B[31mb", L"a[31mb", 1, 0, 0, 0, 0, 1, 0, true },
        { "del", L"a\x007F" L"b", L"ab", 1, 0, 0, 0, 0, 1, 0, true },
        { "c1", L"a\x0090" L"b", L"ab", 1, 0, 0, 0, 0, 1, 0, true },
        { "shy", L"co\u00ADop", L"coop", 1, 0, 0, 0, 0, 1, 0, true },
        { "hangul-filler", L"a\u3164b", L"ab", 1, 0, 0, 0, 0, 1, 0, true },
        { "halfwidth-filler", L"a\uFFA0b", L"ab", 1, 0, 0, 0, 0, 1, 0, true },
        { "specials", L"a\uFFF9b\uFFFBc", L"abc", 2, 0, 0, 0, 0, 2, 0, true },
        { "objrepl-kept", L"a\uFFFCb", L"a\uFFFCb", 0, 0, 0, 0, 0, 0, 0, true },
        { "lone-surrogate", L"a\xD800" L"b", L"a\uFFFDb", 1, 0, 0, 0, 0, 1, 0, true },
        { "bom-lead", L"\uFEFFhello", L"hello", 1, 0, 0, 1, 0, 0, 0, true },
        { "wj", L"a\u2060b", L"ab", 1, 0, 0, 1, 0, 0, 0, true },
        { "invisible-times", L"a\u2062b", L"ab", 1, 0, 0, 0, 0, 1, 0, true },
        { "zwsp-latin", L"a\u200Bb", L"ab", 1, 0, 0, 1, 0, 0, 0, true },
        { "zwsp-thai", L"\u0E01\u200B\u0E02", L"\u0E01\u200B\u0E02", 0, 0, 0, 0, 0, 0, 1, true },
        { "zwsp-thai-digit", L"\u0E01\u200B5", L"\u0E01\u200B5", 0, 0, 0, 0, 0, 0, 1, true },
        { "zwsp-digit-thai", L"5\u200B\u0E01", L"5\u200B\u0E01", 0, 0, 0, 0, 0, 0, 1, true },
        { "zwsp-lead", L"\u200Bword", L"word", 1, 0, 0, 1, 0, 0, 0, true },
        { "zwj-latin", L"a\u200Db", L"ab", 1, 0, 0, 1, 0, 0, 0, true },
        { "zwnj-latin", L"a\u200Cb", L"ab", 1, 0, 0, 1, 0, 0, 0, true },
        { "vs16-warning", L"\u26A0\uFE0F done", L"\u26A0\uFE0F done", 0, 0, 0, 0, 0, 0, 1, true },
        { "vs16-check", L"\u2705 ok \u2714\uFE0F", L"\u2705 ok \u2714\uFE0F", 0, 0, 0, 0, 0, 0, 1, true },
        { "vs16-heart", L"\u2764\uFE0F", L"\u2764\uFE0F", 0, 0, 0, 0, 0, 0, 1, true },
        { "vs16-after-letter", L"a\uFE0Fb", L"ab", 1, 0, 0, 0, 1, 0, 0, true },
        { "vs15-text", L"\u26A0\uFE0E", L"\u26A0\uFE0E", 0, 0, 0, 0, 0, 0, 1, true },
        { "keycap", L"1\uFE0F\u20E3", L"1\uFE0F\u20E3", 0, 0, 0, 0, 0, 0, 1, true },
        { "keycap-hash", L"#\uFE0F\u20E3", L"#\uFE0F\u20E3", 0, 0, 0, 0, 0, 0, 1, true },
        { "zwj-cook", L"\U0001F469\u200D\U0001F373", L"\U0001F469\u200D\U0001F373", 0, 0, 0, 0, 0, 0, 1, true },
        { "zwj-family", L"\U0001F468\u200D\U0001F469\u200D\U0001F467", L"\U0001F468\u200D\U0001F469\u200D\U0001F467", 0, 0, 0, 0, 0, 0, 2, true },
        { "zwj-skin-dev", L"\U0001F9D1\U0001F3FD\u200D\U0001F4BB", L"\U0001F9D1\U0001F3FD\u200D\U0001F4BB", 0, 0, 0, 0, 0, 0, 1, true },
        { "zwj-vs16-rainbow", L"\U0001F3F3\uFE0F\u200D\U0001F308", L"\U0001F3F3\uFE0F\u200D\U0001F308", 0, 0, 0, 0, 0, 0, 2, true },
        { "zwj-trailing", L"\U0001F469\u200D", L"\U0001F469", 1, 0, 0, 1, 0, 0, 0, true },
        { "flag-england", L"\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007F", L"\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007F", 0, 0, 0, 0, 0, 0, 6, true },
        { "flag-bogus-tags", L"\U0001F3F4\U000E0078\U000E0078\U000E007F", L"\U0001F3F4", 3, 3, 0, 0, 0, 0, 0, true },
        { "tag-smuggle", L"hi\U000E0041\U000E0042", L"hi", 2, 2, 0, 0, 0, 0, 0, true },
        { "vs-supplement", L"a\U000E0100b", L"ab", 1, 0, 0, 0, 1, 0, 0, true },
        { "vs1-math", L"\u2229\uFE00", L"\u2229\uFE00", 0, 0, 0, 0, 0, 0, 1, true },
        { "vs1-latin", L"a\uFE00", L"a", 1, 0, 0, 0, 1, 0, 0, true },
        { "rle-pdf-hebrew", L"\u202B\u05E9\u05DC\u05D5\u05DD\u202C", L"\u05E9\u05DC\u05D5\u05DD", 2, 0, 2, 0, 0, 0, 0, true },
        { "rli-pdi-hebrew", L"\u2067\u05E9\u05DC\u05D5\u05DD\u2069", L"\u05E9\u05DC\u05D5\u05DD", 2, 0, 2, 0, 0, 0, 0, true },
        { "lro-latin", L"\u202Dabc\u202C", L"abc", 2, 0, 2, 0, 0, 0, 0, true },
        { "fsi", L"x\u2068y\u2069", L"xy", 2, 0, 2, 0, 0, 0, 0, true },
        { "he-rlm-before-period", L"\u05E9\u05DC\u05D5\u05DD\u200F.", L"\u05E9\u05DC\u05D5\u05DD\u200F.", 0, 0, 0, 0, 0, 0, 1, true },
        { "he-rlm-between-letters", L"\u05E9\u05DC\u200F\u05D5\u05DD", L"\u05E9\u05DC\u05D5\u05DD", 1, 0, 1, 0, 0, 0, 0, true },
        { "he-rlm-end-of-text", L"\u05E9\u05DC\u05D5\u05DD\u200F", L"\u05E9\u05DC\u05D5\u05DD\u200F", 0, 0, 0, 0, 0, 0, 1, true },
        { "he-rlm-latin-to-hebrew", L"abc\u200F\u05E9\u05DC\u05D5\u05DD", L"abc\u200F\u05E9\u05DC\u05D5\u05DD", 0, 0, 0, 0, 0, 0, 1, true },
        { "he-rlm-before-space", L"Hello\u200F \u05E9\u05DC\u05D5\u05DD", L"Hello\u200F \u05E9\u05DC\u05D5\u05DD", 0, 0, 0, 0, 0, 0, 1, true },
        { "he-lrm-before-space", L"\u05E9\u05DC\u05D5\u05DD\u200E v2", L"\u05E9\u05DC\u05D5\u05DD\u200E v2", 0, 0, 0, 0, 0, 0, 1, true },
        { "he-rlm-between-digits", L"2\u200F3 \u05E9", L"23 \u05E9", 1, 0, 1, 0, 0, 0, 0, true },
        { "he-rlm-before-mark", L"\u05E9\u200F\u05B8x", L"\u05E9\u05B8x", 1, 0, 1, 0, 0, 0, 0, true },
        { "he-rlm-double", L"\u05E9\u200F\u200F.", L"\u05E9\u200F.", 1, 0, 1, 0, 0, 0, 1, true },
        { "he-rlm-at-line-start", L"\u200F\u05E9\u05DC\u05D5\u05DD", L"\u200F\u05E9\u05DC\u05D5\u05DD", 0, 0, 0, 0, 0, 0, 1, true },
        { "lrm-english-only", L"Hello\u200E world", L"Hello world", 1, 0, 1, 0, 0, 0, 0, true },
        { "rlm-alone", L"\u200F", L"", 1, 0, 1, 0, 0, 0, 0, true },
        { "he-rlm-other-line", L"\u05E9\u05DC\u05D5\u05DD\x000A" L"abc\u200F.", L"\u05E9\u05DC\u05D5\u05DD\x000A" L"abc.", 1, 0, 1, 0, 0, 0, 0, true },
        { "he-rlm-line-below-hebrew", L"abc\u200F.\x000A\u05E9", L"abc.\x000A\u05E9", 1, 0, 1, 0, 0, 0, 0, true },
        { "he-rlm-same-line-later-hebrew", L"PDF\u200F: \u05E9\u05DC\u05D5\u05DD", L"PDF\u200F: \u05E9\u05DC\u05D5\u05DD", 0, 0, 0, 0, 0, 0, 1, true },
        { "he-niqqud", L"\u05E9\u05B8\u05C1\u05DC\u05D5\u05B9\u05DD", L"\u05E9\u05B8\u05C1\u05DC\u05D5\u05B9\u05DD", 0, 0, 0, 0, 0, 0, 0, true },
        { "he-zwnj", L"\u05E9\u05DC\u05D5\u05DD\u200C\u05E2\u05D5\u05DC\u05DD", L"\u05E9\u05DC\u05D5\u05DD\u05E2\u05D5\u05DC\u05DD", 1, 0, 0, 1, 0, 0, 0, true },
        { "he-zwj", L"\u05E9\u200D\u05DC", L"\u05E9\u05DC", 1, 0, 0, 1, 0, 0, 0, true },
        { "he-mixed-sentence", L"\u05EA\u05DB\u05EA\u05D5\u05D1 \u05EA\u05D5\u05DB\u05E0\u05D9\u05EA - PDF \u2014 OK?", L"\u05EA\u05DB\u05EA\u05D5\u05D1 \u05EA\u05D5\u05DB\u05E0\u05D9\u05EA - PDF \u2014 OK?", 0, 0, 0, 0, 0, 0, 0, true },
        { "he-tatweel-rlm", L"\u0640\u200F\u05E9", L"\u0640\u05E9", 1, 0, 1, 0, 0, 0, 0, true },
        { "he-crlf-rlm", L"\u05E9\u05DC\u05D5\u05DD\u200F.\x000D\x000A\u05E9", L"\u05E9\u05DC\u05D5\u05DD\u200F.\x000A\u05E9", 0, 0, 0, 0, 0, 0, 1, true },
        { "alm-arabic", L"\u0645\u0631\u062D\u0628\u0627\u061C.", L"\u0645\u0631\u062D\u0628\u0627\u061C.", 0, 0, 0, 0, 0, 0, 1, true },
        { "alm-hebrew-line", L"\u05E9\u061C.", L"\u05E9.", 1, 0, 1, 0, 0, 0, 0, true },
        { "fa-zwnj-latin-suffix", L"PDF\u200C\u0647\u0627", L"PDF\u200C\u0647\u0627", 0, 0, 0, 0, 0, 0, 1, true },
        { "fa-zwnj-between", L"\u0645\u06CC\u200C\u062E\u0648\u0627\u0647\u0645", L"\u0645\u06CC\u200C\u062E\u0648\u0627\u0647\u0645", 0, 0, 0, 0, 0, 0, 1, true },
        { "ar-zwj", L"\u0644\u200D\u0627", L"\u0644\u200D\u0627", 0, 0, 0, 0, 0, 0, 1, true },
        { "deva-zwj", L"\u0915\u094D\u200D\u0937", L"\u0915\u094D\u200D\u0937", 0, 0, 0, 0, 0, 0, 1, true },
        { "deva-zwnj", L"\u0915\u094D\u200C\u0937", L"\u0915\u094D\u200C\u0937", 0, 0, 0, 0, 0, 0, 1, true },
        { "cgj-between-marks", L"a\u0301\u034F\u0302", L"a\u0301\u034F\u0302", 0, 0, 0, 0, 0, 0, 1, true },
        { "cgj-latin", L"a\u034Fb", L"ab", 1, 0, 0, 0, 0, 1, 0, true },
        { "khmer-inherent", L"\u1780\u17B4", L"\u1780\u17B4", 0, 0, 0, 0, 0, 0, 1, true },
        { "mongolian-fvs", L"\u1820\u180B", L"\u1820\u180B", 0, 0, 0, 0, 0, 0, 1, true },
        { "mongolian-fvs-latin", L"a\u180Bb", L"ab", 1, 0, 0, 0, 1, 0, 0, true },
        { "brahmi-njoiner", L"\U00011066\U0001107F\U00011067", L"\U00011066\U0001107F\U00011067", 0, 0, 0, 0, 0, 0, 1, true },
        { "egyptian-format", L"\U00013000\U00013430\U00013001", L"\U00013000\U00013430\U00013001", 0, 0, 0, 0, 0, 0, 1, true },
        { "shorthand-format", L"\U0001BC00\U0001BCA0\U0001BC01", L"\U0001BC00\U0001BCA0\U0001BC01", 0, 0, 0, 0, 0, 0, 1, true },
        { "musical-format", L"a\U0001D173b", L"ab", 1, 0, 0, 0, 0, 1, 0, true },
        { "khitan-filler", L"a\U00016FE4b", L"ab", 1, 0, 0, 0, 0, 1, 0, true },
        { "prompt-warning-handover", L"# Handover\x000A\x000A\u26A0\uFE0F ONE CAVEAT \u2014 do not deploy \u2192 run tests\x000D\x000A\u2502 col \u2502", L"# Handover\x000A\x000A\u26A0\uFE0F ONE CAVEAT \u2014 do not deploy \u2192 run tests\x000A\u2502 col \u2502", 0, 0, 0, 0, 0, 0, 1, true },
        { "prompt-hebrew-rlm-and-bom", L"\uFEFF\u05D1\u05D3\u05D5\u05E7 \u05D0\u05EA PR #12\u200F \u05D1\u05D1\u05E7\u05E9\u05D4.", L"\u05D1\u05D3\u05D5\u05E7 \u05D0\u05EA PR #12\u200F \u05D1\u05D1\u05E7\u05E9\u05D4.", 1, 0, 0, 1, 0, 0, 1, true },
        { "prompt-web-zwsp", L"see\u200Bhttps://example.com/a\u200Bb for details", L"seehttps://example.com/ab for details", 2, 0, 0, 2, 0, 0, 0, true },
    };

    // A CHECK message naming the oracle row it is about ("<what> [<row>]").
    //   what - the assertion; name - the row name.  returns - the combined message.  throws - std::bad_alloc.
    std::string Tagged(const char* what, const char* name)
    {
        return std::string{ what } + " [" + name + "]";
    }

    // A queued prompt carrying the report's exact hazards: the warning-sign emoji WITH its VS16 (claude
    // keeps that one) and a trailing web-pasted ZWSP (claude removes that one - and cancels the Enter).
    const std::wstring kQueuedWithInvisibles = L"deploy \u26A0\uFE0F now\u200B";
}

// The mirror against claude's own outputs - every row, field for field - plus the two invariants the
// delivery path relies on: the cleanup is idempotent, and what SanitizeForClaudeInput produces for
// pasting can never make claude's Enter-time cleanup remove anything (== the Enter always submits).
void TestClaudeInvisibleOracle()
{
    std::wprintf(L"Claude invisible-character cleanup (DELIVERY.md 15 - the wZe mirror vs Claude's own outputs):\n");
    for (const auto& c : kInvisibleOracle)
    {
        const std::wstring input{ c.input };
        const auto r = CleanClaudeInputText(input);
        CHECK(r.text == c.expected, Tagged("oracle: cleaned text == claude's", c.name).c_str());
        CHECK(r.removedTotal == c.removedTotal, Tagged("oracle: removedTotal == claude's", c.name).c_str());
        CHECK(r.removedTags == c.tags && r.removedBidi == c.bidi && r.removedZeroWidth == c.zeroWidth &&
                  r.removedSelectors == c.selectors && r.removedOther == c.other,
              Tagged("oracle: removed-by-class == claude's", c.name).c_str());
        CHECK(r.keptConditional == c.keptConditional, Tagged("oracle: keptConditional == claude's", c.name).c_str());
        const auto again = CleanClaudeInputText(r.text);
        CHECK((again.text == r.text && again.removedTotal == 0) == c.idempotent, Tagged("oracle: idempotence matches claude's", c.name).c_str());
        CHECK(CleanClaudeInputText(SanitizeForClaudeInput(input)).removedTotal == 0,
              Tagged("DELIVERY INVARIANT: a sanitized paste never trips claude's review cancel", c.name).c_str());
    }
}

// The pure tables, the paste sanitize, the compare key, every prompt compare that consumes it, and the
// grapheme budget - with the Hebrew shapes the user asked to be covered.
void TestClaudeInputSanitize()
{
    std::wprintf(L"Claude input sanitize + prompt compare key (DELIVERY.md 15):\n");

    // ---- the pure tables ----
    CHECK(!IsClaudeInvisibleCandidate(U'\t') && !IsClaudeInvisibleCandidate(U'\n'), "table: TAB and LF are never candidates");
    CHECK(IsClaudeInvisibleCandidate(U'\r') && IsClaudeInvisibleCandidate(0x1B) && IsClaudeInvisibleCandidate(0x7F) && IsClaudeInvisibleCandidate(0x9F),
          "table: CR, ESC, DEL and C1 are candidates");
    CHECK(IsClaudeInvisibleCandidate(0x200E) && IsClaudeInvisibleCandidate(0x200F) && IsClaudeInvisibleCandidate(0x061C),
          "table: LRM, RLM and ALM are candidates (kept only by context)");
    CHECK(!IsClaudeInvisibleCandidate(0x05D0) && !IsClaudeInvisibleCandidate(0x05B8) && !IsClaudeInvisibleCandidate(0x05C1) && !IsClaudeInvisibleCandidate(0x0640),
          "table HEBREW: letters, niqqud and the tatweel are never candidates");
    CHECK(IsClaudeInvisibleCandidate(0xE0041) && IsClaudeInvisibleCandidate(0xE0100) && IsClaudeInvisibleCandidate(0xE0FFF) && !IsClaudeInvisibleCandidate(0xE1000),
          "table: U+E0000-E0FFF are candidates, the block end is exclusive");
    CHECK(!IsClaudeInvisibleCandidate(0xFFFC) && !IsClaudeInvisibleCandidate(0xFFFD) && IsClaudeInvisibleCandidate(0xFFFB),
          "table: FFFC/FFFD pass, the interlinear-annotation controls do not");
    CHECK(IsClaudeLineBreak(0x2028) && IsClaudeLineBreak(0x85) && IsClaudeLineBreak(U'\n') && IsClaudeLineBreak(U'\r') && !IsClaudeLineBreak(U'\t'),
          "table: claude's line breaks");
    CHECK(ClassifyClaudeInvisible(0x200F) == ClaudeInvisibleClass::Bidi && ClassifyClaudeInvisible(0x200B) == ClaudeInvisibleClass::ZeroWidth &&
              ClassifyClaudeInvisible(0xFE0F) == ClaudeInvisibleClass::Selectors && ClassifyClaudeInvisible(0xE0041) == ClaudeInvisibleClass::Tags &&
              ClassifyClaudeInvisible(0x00AD) == ClaudeInvisibleClass::Other && ClassifyClaudeInvisible(0x180E) == ClaudeInvisibleClass::Other,
          "table: removal classes (MVS U+180E is Other, not a selector)");
    CHECK(!TextNeedsClaudeInvisibleScan(L"plain ascii\twith a tab\nand an LF") && TextNeedsClaudeInvisibleScan(L"crlf\r\n") &&
              TextNeedsClaudeInvisibleScan(L"\u05E9"),
          "table: the fast-path test (CR and any non-ASCII take the full path)");

    // ---- SanitizeForClaudeInput: the text we paste ----
    CHECK(SanitizeForClaudeInput(L"fix the tests") == L"fix the tests", "sanitize: ASCII passes untouched");
    CHECK(SanitizeForClaudeInput(L"\u26A0\uFE0F ONE CAVEAT") == L"\u26A0\uFE0F ONE CAVEAT", "sanitize: the warning-sign emoji KEEPS its VS16 (claude keeps it)");
    CHECK(SanitizeForClaudeInput(kQueuedWithInvisibles) == L"deploy \u26A0\uFE0F now", "sanitize: the trailing ZWSP goes, the VS16 stays");
    CHECK(SanitizeForClaudeInput(L"see\u200Bhttps://x.y") == L"seehttps://x.y", "sanitize: a web-pasted ZWSP between Latin words is removed");
    CHECK(SanitizeForClaudeInput(L"a\x1B[201~b") == L"a[201~b", "sanitize: ESC is removed - no body byte can close the bracketed paste early");
    CHECK(SanitizeForClaudeInput(L"line\r\nnext\rlast") == L"line\nnext\nlast", "sanitize: CRLF and a lone CR become LF");
    CHECK(SanitizeForClaudeInput(L"\u05E9\u05DC\u05D5\u05DD\u200F.") == L"\u05E9\u05DC\u05D5\u05DD\u200F.",
          "sanitize HEBREW: an RLM before punctuation on a Hebrew line is KEPT");
    CHECK(SanitizeForClaudeInput(L"PR #12\u200F \u05D1\u05D1\u05E7\u05E9\u05D4") == L"PR #12\u200F \u05D1\u05D1\u05E7\u05E9\u05D4",
          "sanitize HEBREW: an RLM after Latin/digits on a line that holds Hebrew is KEPT");
    CHECK(SanitizeForClaudeInput(L"\u05E9\u05DC\u200F\u05D5\u05DD") == L"\u05E9\u05DC\u05D5\u05DD",
          "sanitize HEBREW: a REDUNDANT RLM between two Hebrew letters is removed");
    CHECK(SanitizeForClaudeInput(L"Hello\u200E world") == L"Hello world", "sanitize: an LRM on an English-only line is removed");
    CHECK(SanitizeForClaudeInput(L"\u202B\u05E9\u05DC\u05D5\u05DD\u202C") == L"\u05E9\u05DC\u05D5\u05DD",
          "sanitize HEBREW: RLE/PDF embeddings are always removed, the letters stay");
    CHECK(SanitizeForClaudeInput(L"\u05D1\u05BC\u05B8") == L"\u05D1\u05B8\u05BC",
          "sanitize HEBREW: niqqud typed in non-canonical order is NFC-reordered (dagesh ccc 21 after qamats ccc 18)");
    CHECK(SanitizeForClaudeInput(L"\u05E9\u05B8\u05C1\u05DC\u05D5\u05B9\u05DD") == L"\u05E9\u05B8\u05C1\u05DC\u05D5\u05B9\u05DD",
          "sanitize HEBREW: canonical niqqud passes untouched");
    CHECK(SanitizeForClaudeInput(L"\U0001F469\u200D\U0001F373") == L"\U0001F469\u200D\U0001F373", "sanitize: an emoji ZWJ sequence keeps its joiner");
    CHECK(SanitizeForClaudeInput(L"hi\U000E0041\U000E0042") == L"hi", "sanitize: tag characters (ASCII smuggling) are removed");
    {
        const std::wstring once = SanitizeForClaudeInput(kQueuedWithInvisibles);
        CHECK(SanitizeForClaudeInput(once) == once, "sanitize: idempotent");
    }
    {
        // The ONE-PASS TRAP (found by the differential fuzz): the Egyptian format control between "a"
        // and COMBINING ACUTE is removed, the editor's NFC then composes a+acute into U+00E1, and the
        // CGJ that was kept because it followed the mark now follows a precomposed LETTER - claude's
        // next Enter-time cleanup would remove it and cancel. The sanitize iterates to the fixed point.
        const std::wstring trap = L"a\U00013430\u0301\u034F\U0001BC00";
        const std::wstring fixedPoint = SanitizeForClaudeInput(trap);
        CHECK(fixedPoint == L"\u00E1\U0001BC00", "sanitize: iterates NFC + cleanup to a FIXED POINT (the CGJ orphaned by composition goes too)");
        CHECK(CleanClaudeInputText(fixedPoint).removedTotal == 0 && SanitizeForClaudeInput(fixedPoint) == fixedPoint,
              "sanitize: the fixed point is clean and stable");
    }

    // ---- PromptFillBody / BuildPromptFill: the ONE paste chokepoint ----
    CHECK(BuildPromptFill(L"a\u200Bb") == L"\x1b[200~ab\x1b[201~", "fill: the pasted body is sanitized");
    CHECK(PromptFillBody(L"\u26A0\uFE0F x\r\ny") == L"\u26A0\uFE0F x\ny", "fill body: sanitized + CRLF folded, VS16 kept");
    CHECK(BuildPromptSubmission(L"\u05E9\u05DC\u05D5\u05DD\u200F.") == L"\x1b[200~\u05E9\u05DC\u05D5\u05DD\u200F.\x1b[201~\r",
          "submit HEBREW: the kept RLM rides the paste, one CR outside it");
    CHECK(BuildPromptFill(L"x\x1b[201~y").find(L"\x1b[201~y") == std::wstring::npos, "fill: an embedded paste terminator can no longer end the paste early");

    // ---- NormalizePromptTextForCompare: the compare key ----
    CHECK(NormalizePromptTextForCompare(L"plain") == L"plain", "key: ASCII is its own key");
    CHECK(NormalizePromptTextForCompare(L"\u26A0\uFE0F done") == L"\u26A0 done", "key: VS16 dropped (kept-or-removed alike)");
    CHECK(NormalizePromptTextForCompare(L"a\r\nb\u2028c\rd\u000Be") == L"a\nb\nc\nd\ne", "key: every line break folds to one LF");
    CHECK(NormalizePromptTextForCompare(std::wstring{ L'a', static_cast<wchar_t>(0xD800), L'b' }) == L"a\uFFFDb", "key: a lone surrogate reads as U+FFFD");
    CHECK(NormalizePromptTextForCompare(L"\u05D1\u05BC\u05B8") == NormalizePromptTextForCompare(L"\u05D1\u05B8\u05BC"), "key HEBREW: niqqud order-insensitive (NFC)");
    CHECK(NormalizePromptTextForCompare(L"\u05E9\u05DC\u200F\u05D5\u05DD\u200E") == L"\u05E9\u05DC\u05D5\u05DD", "key HEBREW: direction marks dropped");

    // ---- the compares that consume the key ----
    CHECK(PromptTextsEquivalent(L"\u05E9\u05DC\u05D5\u05DD \u200B\u05E2\u05D5\u05DC\u05DD", L"\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD"),
          "equivalent HEBREW: an invisible-only difference is no difference");
    CHECK(!PromptTextsEquivalent(L"fix the tests", L"fix the test"), "equivalent: a visible difference still is one");
    CHECK(!PromptTextsEquivalent(L"go ", L"go"), "equivalent: whitespace stays exact (claude submits it verbatim)");
    {
        // THE REPORTED FAILURE, end to end on the pure half: claude's 2.1.277+ review cancel leaves its
        // CLEANED text in the box; the draft guard must still read that as OUR prompt (press = rescue).
        const std::wstring cleaned = CleanClaudeInputText(kQueuedWithInvisibles).text;
        CHECK(cleaned != kQueuedWithInvisibles, "report shape: claude's cleanup does change this prompt (its Enter would cancel)");
        CHECK(DraftMatchesPromptText(cleaned, kQueuedWithInvisibles), "draft guard: claude's cleaned box still reads as OUR prompt (the rescue press is allowed)");
        CHECK(!DraftMatchesPromptText(L"deploy now please", kQueuedWithInvisibles), "draft guard: a human draft is still foreign");
    }
    CHECK(PromptSwallowedByMessage(L"FOREIGN deploy \u26A0\uFE0F now", kQueuedWithInvisibles), "merge: the suffix test is invisible-insensitive");
    CHECK(VerifyFillAgainstPrompt(PromptFillBody(kQueuedWithInvisibles), kQueuedWithInvisibles) == FillVerify::Verified,
          "verify: the SANITIZED fill verifies against the raw queued prompt");
    CHECK(VerifyFillAgainstPrompt(L"\u05D1\u05B8\u05BC!", L"\u05D1\u05BC\u05B8!") == FillVerify::Verified, "verify HEBREW: the NFC-reordered render verifies");
    CHECK(VerifyFillAgainstPrompt(L"X deploy \u26A0 now", kQueuedWithInvisibles) == FillVerify::Foreign, "verify: visible foreign content still reads Foreign");
    {
        const auto plan = DecideSendUndo(L"USER TEXT " + PromptFillBody(kQueuedWithInvisibles), kQueuedWithInvisibles);
        CHECK(plan.action == SendUndoAction::Press, "undo: our sanitized tail is still recognized as ours");
    }

    // ---- CountGraphemeClusters: the Foreign-undo budget's unit (claude deletes by grapheme) ----
    CHECK(CountGraphemeClusters(L"") == 0 && CountGraphemeClusters(L"abc") == 3, "graphemes: ASCII counts one per character");
    CHECK(CountGraphemeClusters(L"\u05E9\u05B8\u05C1") == 1, "graphemes HEBREW: a letter with its niqqud is ONE backspace");
    CHECK(CountGraphemeClusters(L"\u05E9\u05DC\u05D5\u05DD") == 4, "graphemes HEBREW: plain letters count one each");
    CHECK(CountGraphemeClusters(L"\U0001F44B") == 1, "graphemes: a surrogate pair is one cluster");
    CHECK(CountGraphemeClusters(L"\U0001F469\u200D\U0001F373") == 1, "graphemes: an emoji ZWJ sequence is one cluster");
    CHECK(CountGraphemeClusters(L"\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007F") == 1, "graphemes: a subdivision flag is one cluster");
    CHECK(CountGraphemeClusters(L"1\uFE0F\u20E3") == 1, "graphemes: a keycap is one cluster");
    CHECK(CountGraphemeClusters(L"a\r\nb") == 3, "graphemes: CRLF is one cluster");
    CHECK(CountGraphemeClusters(PromptFillBody(kQueuedWithInvisibles)) < kQueuedWithInvisibles.size(),
          "graphemes: the undo budget counts what we typed, never the queued text's UTF-16 units");
}

// The registry half of "both sides of every comparison": the push echo, the pull echo, the pull twin's
// dedupe against a push Typed row, the merge classifier - and the scheduler's Enter-retry draft guard.
void TestInvisibleEchoAndDedupe()
{
    std::wprintf(L"Invisible/NFC-insensitive echo, pull-echo, dedupe and draft guard (DELIVERY.md 15):\n");
    const int64_t now = NowMsTest();
    const auto addSent = [&](SessionRegistry& reg, const std::wstring& sid, const std::wstring& pid, const std::wstring& text) {
        reg.Update(sid, [&](SessionInfo& ss) {
            QueuedPrompt p;
            p.id = pid;
            p.text = text;
            p.status = PromptStatus::Sent;
            p.origin = PromptOrigin::Autorun;
            p.echoed = false;
            p.sentAtUnixMs = now;
            ss.queue.push_back(p);
        });
    };
    // A Hebrew autorun prompt with a stray ZWSP (removed by claude) and an RLM after "#12" (KEPT by claude).
    const std::wstring hebrew = L"\u05D1\u05D3\u05D5\u05E7 \u05D0\u05EA \u200BPR #12\u200F \u05D1\u05D1\u05E7\u05E9\u05D4.";

    // ---- push: claude's UserPromptSubmit carries the CLEANED (and NFC) text ----
    {
        SessionRegistry reg;
        auto s = MakeSession(L"he", SessionState::WaitingForInput);
        s.live = true;
        reg.Upsert(s);
        addSent(reg, L"he", L"hp", hebrew);
        reg.OnHookEvent(UPS(L"he", SanitizeForClaudeInput(hebrew)));
        const auto after = reg.Get(L"he");
        CHECK(after.has_value() && after->queue.size() == 1, "echo HEBREW: the sanitized echo records NO duplicate Typed row");
        CHECK(after.has_value() && !after->queue.empty() && after->queue[0].echoed, "echo HEBREW: the sanitized echo is consumed (no lost-send verdict)");
    }
    // ---- push: NFC-reordered niqqud ----
    {
        SessionRegistry reg;
        auto s = MakeSession(L"nq", SessionState::WaitingForInput);
        s.live = true;
        reg.Upsert(s);
        addSent(reg, L"nq", L"np", L"\u05D1\u05BC\u05B8\u05E8\u05D5\u05BC\u05DA \u05D4\u05B7\u05D1\u05BC\u05B8\u05D0"); // typed order: dagesh before the vowel
        reg.OnHookEvent(UPS(L"nq", L"\u05D1\u05B8\u05BC\u05E8\u05D5\u05BC\u05DA \u05D4\u05B7\u05D1\u05B8\u05BC\u05D0")); // claude's NFC submit
        const auto after = reg.Get(L"nq");
        CHECK(after.has_value() && after->queue.size() == 1 && after->queue[0].echoed, "echo HEBREW: an NFC-reordered niqqud echo is consumed, no dup row");
    }
    // ---- pull: the transcript line vouches for the sanitized delivery ----
    {
        SessionRegistry reg;
        auto s = MakeSession(L"pl", SessionState::WaitingForInput);
        s.live = true;
        reg.Upsert(s);
        addSent(reg, L"pl", L"pp", kQueuedWithInvisibles);
        reg.NoteExternalPrompt(L"pl", SanitizeForClaudeInput(kQueuedWithInvisibles), now + 5);
        const auto after = reg.Get(L"pl");
        CHECK(after.has_value() && after->queue.size() == 1 && after->queue[0].echoed, "pull-echo: the cleaned transcript line marks the sanitized prompt echoed, no dup row");
    }
    // ---- pull twin dedupes against the push Typed row (the Hebrew double-recording, now closed) ----
    {
        SessionRegistry reg;
        auto s = MakeSession(L"tw", SessionState::WaitingForInput);
        s.live = true;
        reg.Upsert(s);
        reg.OnHookEvent(UPS(L"tw", L"\u05EA\u05EA\u05E7\u05DF \u05D0\u05EA \u05D4\u05D1\u05D3\u05D9\u05E7\u05D4 \u05D1\u05BC\u05B8")); // a human-typed Hebrew prompt (push)
        reg.NoteExternalPrompt(L"tw", L"\u05EA\u05EA\u05E7\u05DF \u05D0\u05EA \u05D4\u05D1\u05D3\u05D9\u05E7\u05D4 \u05D1\u05B8\u05BC", now + 5); // its transcript twin (NFC)
        const auto after = reg.Get(L"tw");
        CHECK(after.has_value() && after->queue.size() == 1, "dedupe HEBREW: the pull twin of a push Typed row is not recorded twice");
    }
    // ---- the merge classifier sees through invisibles too ----
    {
        SessionRegistry reg;
        auto s = MakeSession(L"mg", SessionState::WaitingForInput);
        s.live = true;
        reg.Upsert(s);
        addSent(reg, L"mg", L"mp", kQueuedWithInvisibles);
        reg.OnHookEvent(UPS(L"mg", L"user draft " + SanitizeForClaudeInput(kQueuedWithInvisibles)));
        const auto after = reg.Get(L"mg");
        CHECK(after.has_value() && !after->queue.empty() && after->queue[0].status == PromptStatus::Failed,
              "merge: a merged submit swallowing the SANITIZED prompt is still detected (Failed + paused)");
    }
    // ---- the scheduler's Enter-retry draft guard: claude's cleaned box is still OUR prompt ----
    {
        const int64_t T = 1000000;
        SessionInfo s;
        s.id = L"er";
        s.live = true;
        s.started = true;
        s.state = SessionState::WaitingForInput;
        QueuedPrompt p;
        p.id = L"erp";
        p.text = kQueuedWithInvisibles;
        p.status = PromptStatus::Sent;
        p.origin = PromptOrigin::Autorun;
        p.sentAtUnixMs = T;
        p.echoed = false;
        s.queue.push_back(p);
        s.pendingInput = CleanClaudeInputText(kQueuedWithInvisibles).text; // what the 2.1.277+ review cancel leaves in the box
        CHECK(DecideEnterRetry(s, T + kEnterRetryFirstMs).action == EnterRetryAction::Retry,
              "enter-retry: a box holding claude's CLEANED text of our prompt gets the rescue press (was: Waiting -> lost-send -> Failed + paused)");
        s.pendingInput = L"a human draft";
        CHECK(DecideEnterRetry(s, T + kEnterRetryFirstMs).action == EnterRetryAction::Waiting, "enter-retry: a foreign draft still blocks the press");
    }
}

// The hook forwarder's decoding (HOOKS.md "Encoding"): the payload is read as UTF-8 from the raw stdin
// stream (never the console code page - the Hebrew mojibake), bridge.json and the transcript tail as
// UTF-8, the script body stays pure ASCII, and the file on disk carries a UTF-8 BOM so Windows
// PowerShell parses a non-ASCII profile path.
void TestForwarderEncoding()
{
    std::wprintf(L"Hook forwarder encoding (HOOKS.md Encoding - the Hebrew-prompt mojibake fix):\n");
    const auto fwd = BuildForwarderScript(L"C:\\Users\\x\\.agentmaster-dev");
    const auto utf8At = fwd.find(L"[Console]::OpenStandardInput()");
    const auto legacyAt = fwd.find(L"[Console]::In.ReadToEnd()");
    CHECK(utf8At != std::wstring::npos && fwd.find(L"New-Object System.Text.UTF8Encoding($false)") != std::wstring::npos,
          "forwarder: the payload is decoded as UTF-8 from the raw stdin stream");
    CHECK(legacyAt != std::wstring::npos && utf8At < legacyAt && fwd.find(L"if (-not $stdinOk)") != std::wstring::npos,
          "forwarder: the console-code-page read survives only as the fallback, after the UTF-8 read");
    CHECK(fwd.find(L"[Console]::InputEncoding =") == std::wstring::npos, "forwarder: never re-code-pages the SHARED console");
    CHECK(fwd.find(L"-Tail 60 -Encoding UTF8") != std::wstring::npos, "forwarder: the transcript tail is read as UTF-8");
    CHECK(fwd.find(L"-Raw -Encoding UTF8") != std::wstring::npos, "forwarder: bridge.json is read as UTF-8");
    CHECK(fwd.find(L"[System.Text.Encoding]::UTF8.GetBytes($line") != std::wstring::npos, "forwarder: the wire line is still written as UTF-8 bytes");
    bool ascii = true;
    for (const wchar_t c : fwd)
    {
        if (c >= 0x80)
        {
            ascii = false;
            break;
        }
    }
    CHECK(ascii, "forwarder: the script body stays pure ASCII (the BOM is added at write time)");

    // The file on disk: BOM + the same body. A scratch folder under %TEMP% (never the live profile).
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / L"agentmaster-m5-invisible-forwarder";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto files = MaterializeSharedHookFiles(dir.wstring(), AppSettings{});
    CHECK(!files.second.empty(), "forwarder: MaterializeSharedHookFiles wrote the script");
    if (!files.second.empty())
    {
        std::ifstream f(std::filesystem::path{ files.second }, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(bytes.size() > 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
                  static_cast<unsigned char>(bytes[2]) == 0xBF,
              "forwarder: the file starts with a UTF-8 BOM (Windows PowerShell 5.1 then decodes a non-ASCII profile path correctly)");
        CHECK(bytes.find("OpenStandardInput") != std::string::npos, "forwarder: the file carries the UTF-8 stdin read");
    }
    std::filesystem::remove_all(dir, ec);
}
