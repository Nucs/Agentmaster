// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — ClaudeInvisibleChars.cpp: the ICU-backed half of ClaudeInvisibleChars.h (the
// header carries the WHY; this file carries the HOW).
//
// READING GUIDE. CleanClaudeInputText is a line-by-line port of Claude Code 2.1.280's minified
// cleanup. Each helper names the bundle identifier it mirrors in a trailing comment (`wZe`, `yn`,
// `Qt`, `oa`, `ta`, `tu`, ...) so a future re-extraction can be diffed against this file one helper
// at a time. The JS semantics that matter and are easy to get wrong:
//   * the RING is the last 16 code points the cleanup KEPT (removed characters never enter it), each
//     flagged "hidden" when it was a kept candidate; `lastWasHidden` is the flag of the newest entry.
//   * "undefined" neighbors (start/end of text, a ring with fewer entries) are std::nullopt here, and
//     every JS `Ce(regex, undefined)` is false — so each OptCp test below is written `x && pred(*x)`.
//   * the direction-mark LINE test scans the WHOLE line of the ORIGINAL (well-formed) text, before and
//     after the mark, and is cached per line (reset at every line break the loop passes).
//   * `\p{Script=X}` is uscript_getScript == X; `\p{Script_Extensions=X}` is uscript_hasScript(c, X)
//     (which falls back to Script when a code point has no explicit extensions — the regex semantics).

#include "ClaudeInvisibleChars.h"

// sdkddkver.h FIRST: icu.h gates whole sections on NTDDI_VERSION but never includes the header that
// defines it — without it, the gates (and the enumerator fallbacks below) would depend on whatever
// the including TU happened to pull in first. This TU uses no PCH, so it states its own baseline.
#include <sdkddkver.h>
#include <icu.h>

#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Windows SDK's ICU import library (icu.dll ships in System32 since Windows 10 1903). TerminalApp.dll
// already resolves these imports through WindowsApp.lib (its fzf / til::regex ICU use); the directive is
// for the standalone builds that link this TU directly — the m5 test harness and agentmaster-cli.
#pragma comment(lib, "icu.lib")

namespace Agentmaster
{
    namespace
    {
        using Cp = char32_t;
        using OptCp = std::optional<Cp>;

        // The SDK's icu.h HIDES a few newer enumerators behind NTDDI_VERSION (Hanifi Rohingya and
        // Extended_Pictographic need 19H1, Yezidi needs Windows 11 "CO"), while the values themselves are
        // ICU's stable, append-only C API. The default NTDDI (WDK_NTDDI_VERSION) exposes all three, but a
        // project that pins an older target must still compile — hence the numeric fallbacks. At RUNTIME
        // an older icu.dll that predates a value simply never reports it (uscript_getScript never returns
        // Yezidi; u_hasBinaryProperty answers false for an unknown property), so the membership test
        // reads "not in the set" — the conservative, remove-the-candidate direction.
#if defined(NTDDI_WIN10_19H1) && (NTDDI_VERSION >= NTDDI_WIN10_19H1)
        constexpr UScriptCode kScriptHanifiRohingya = USCRIPT_HANIFI_ROHINGYA;
        constexpr UProperty kPropExtendedPictographic = UCHAR_EXTENDED_PICTOGRAPHIC;
#else
        constexpr UScriptCode kScriptHanifiRohingya = static_cast<UScriptCode>(182); // USCRIPT_HANIFI_ROHINGYA
        constexpr UProperty kPropExtendedPictographic = static_cast<UProperty>(64); // UCHAR_EXTENDED_PICTOGRAPHIC
#endif
#if defined(NTDDI_WIN10_CO) && (NTDDI_VERSION >= NTDDI_WIN10_CO)
        constexpr UScriptCode kScriptYezidi = USCRIPT_YEZIDI;
#else
        constexpr UScriptCode kScriptYezidi = static_cast<UScriptCode>(192); // USCRIPT_YEZIDI
#endif

        // One code point decoded out of UTF-16: its value and how many units it spans.
        struct Decoded
        {
            Cp cp;
            size_t units;
        };

        // JS `codePointAt(i)`: a valid surrogate PAIR decodes to its supplementary code point; anything
        // else — including a lone surrogate — decodes to the single unit's value.
        //   s       — the UTF-16 text; i — an index < s.size().
        //   returns — the code point at i and its width in units (1 or 2).
        //   throws  — never.
        Decoded DecodeAt(std::wstring_view s, size_t i) noexcept
        {
            const auto hi = static_cast<char16_t>(s[i]);
            if (hi >= 0xD800 && hi <= 0xDBFF && i + 1 < s.size())
            {
                const auto lo = static_cast<char16_t>(s[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    return { 0x10000 + ((static_cast<Cp>(hi) - 0xD800) << 10) + (static_cast<Cp>(lo) - 0xDC00), 2 };
                }
            }
            return { static_cast<Cp>(hi), 1 };
        }

        // Is the unit at `i` a LONE surrogate (a high one with no low partner, or an orphan low one)?
        // The only ill-formed UTF-16 shape; both the cleanup and the compare key rewrite it to U+FFFD.
        //   s — the text; i — an index < s.size().  returns — true for a lone surrogate.  throws — never.
        bool IsLoneSurrogateAt(std::wstring_view s, size_t i) noexcept
        {
            const auto u = static_cast<char16_t>(s[i]);
            if (u >= 0xD800 && u <= 0xDBFF)
            {
                return !(i + 1 < s.size() && static_cast<char16_t>(s[i + 1]) >= 0xDC00 && static_cast<char16_t>(s[i + 1]) <= 0xDFFF);
            }
            return u >= 0xDC00 && u <= 0xDFFF;
        }

        // ---- Unicode property probes (ICU) — each mirrors one of the bundle's `\p{...}` regexes ----

        // ICU's code-point type is a SIGNED int32 (UChar32); every scalar fits, so this is a plain cast.
        UChar32 AsUChar32(Cp c) noexcept
        {
            return static_cast<UChar32>(c);
        }

        // `Wn` = \p{L}. Also the "letter" half of every letter-in-script test (`Vr`).
        bool IsLetter(Cp c) noexcept
        {
            return (U_GET_GC_MASK(AsUChar32(c)) & U_GC_L_MASK) != 0;
        }

        // `Pt` = \p{M}: combining marks — skipped over (or refused) by most look-behind rules.
        bool IsMark(Cp c) noexcept
        {
            return (U_GET_GC_MASK(AsUChar32(c)) & U_GC_M_MASK) != 0;
        }

        // `Gc` = \p{White_Space}: a ZWNJ after whitespace is never a word-internal joiner.
        bool IsWhiteSpaceProp(Cp c) noexcept
        {
            return u_hasBinaryProperty(AsUChar32(c), UCHAR_WHITE_SPACE) != 0;
        }

        // `Wc` = [\p{Emoji}\p{Extended_Pictographic}]: a base a VS15/VS16 may legitimately follow.
        bool IsEmojiOrPictographic(Cp c) noexcept
        {
            return u_hasBinaryProperty(AsUChar32(c), UCHAR_EMOJI) != 0 || u_hasBinaryProperty(AsUChar32(c), kPropExtendedPictographic) != 0;
        }

        // `qs` = \p{Extended_Pictographic}: both ends of an emoji ZWJ sequence.
        bool IsExtendedPictographic(Cp c) noexcept
        {
            return u_hasBinaryProperty(AsUChar32(c), kPropExtendedPictographic) != 0;
        }

        // `Yc` = \p{Sm}: the math symbols a VS1 may select a variant of.
        bool IsMathSymbol(Cp c) noexcept
        {
            return u_charType(AsUChar32(c)) == U_MATH_SYMBOL;
        }

        // `Br` = \p{Nd}: decimal digits of ANY script (the redundant-direction-mark digit rule).
        bool IsDecimalDigit(Cp c) noexcept
        {
            return u_charType(AsUChar32(c)) == U_DECIMAL_DIGIT_NUMBER;
        }

        // `Js` = /^[0-9]$/: ASCII digits only (the ZWSP-beside-a-number rule).
        bool IsAsciiDigit(Cp c) noexcept
        {
            return c >= U'0' && c <= U'9';
        }

        // `ra`: the five emoji skin-tone modifiers, skipped when looking behind a ZWJ for its emoji.
        bool IsSkinToneModifier(Cp c) noexcept
        {
            return c >= 0x1F3FB && c <= 0x1F3FF;
        }

        // `nu`: a keycap base (digit, '#', '*') — VS16 then U+20E3 makes the keycap emoji.
        bool IsKeycapBase(Cp c) noexcept
        {
            return (c >= U'0' && c <= U'9') || c == U'#' || c == U'*';
        }

        // The RTL blocks the direction-mark LINE test scans for letters in (`ta`'s range filter):
        // Hebrew..NKo/Samaritan/Mandaic/Syriac-Supplement/Arabic-Extended, the Hebrew/Arabic
        // presentation forms, and the SMP RTL blocks (Old Hungarian .. Adlam and friends).
        bool InRtlBlock(Cp c) noexcept
        {
            return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) ||
                   (c >= 0x10800 && c <= 0x10FFF) || (c >= 0x1E800 && c <= 0x1EFFF);
        }

        // A set of scripts tested either by the Script property (`\p{Script=X}`) or by
        // Script_Extensions (`\p{Script_Extensions=X}`) — the two regex families the bundle builds.
        struct ScriptSet
        {
            const UScriptCode* codes;
            size_t count;
            bool extensions; // false: Script (uscript_getScript); true: Script_Extensions (uscript_hasScript)
        };

        // Is `c` in `set`? A failed ICU lookup reads as "not in the set" — for a KEEP rule that means the
        // candidate is removed, the conservative direction (a removed invisible costs nothing visible).
        //   c — the code point; set — the scripts + which property to test.  returns — membership.
        //   throws — never.
        bool InScriptSet(Cp c, const ScriptSet& set) noexcept
        {
            if (set.extensions)
            {
                for (size_t i = 0; i < set.count; ++i)
                {
                    if (uscript_hasScript(AsUChar32(c), set.codes[i]))
                    {
                        return true;
                    }
                }
                return false;
            }
            UErrorCode err = U_ZERO_ERROR;
            const UScriptCode sc = uscript_getScript(AsUChar32(c), &err);
            if (U_FAILURE(err))
            {
                return false;
            }
            for (size_t i = 0; i < set.count; ++i)
            {
                if (sc == set.codes[i])
                {
                    return true;
                }
            }
            return false;
        }

        // `Vr(set, c)`: a LETTER of one of the scripts (marks, digits and punctuation never qualify).
        bool LetterInScripts(Cp c, const ScriptSet& set) noexcept
        {
            return IsLetter(c) && InScriptSet(c, set);
        }

        // The bundle's `{base, mark}` pairs: `Ur(names)` builds base = Script, mark = Script_Extensions
        // over the same names; `En(regex)` uses one regex for both (Script only).
        struct ScriptPair
        {
            ScriptSet base;
            ScriptSet mark;
        };

        // `ia` — the RTL scripts whose letters make an LRM/RLM on the same line meaningful. Hebrew is
        // here, which is what keeps a Hebrew line's direction marks through the cleanup.
        constexpr UScriptCode kRtlLineScripts[] = { USCRIPT_ARABIC, USCRIPT_HEBREW, USCRIPT_SYRIAC, USCRIPT_THAANA, USCRIPT_NKO,
                                                    USCRIPT_SAMARITAN, USCRIPT_MANDAIC, USCRIPT_ADLAM, kScriptHanifiRohingya, kScriptYezidi };
        constexpr ScriptSet kRtlLine{ kRtlLineScripts, std::size(kRtlLineScripts), false };

        // `qc` — the narrower set for the ARABIC LETTER MARK (no Hebrew: an ALM on a Hebrew-only line is removed).
        constexpr UScriptCode kAlmLineScripts[] = { USCRIPT_ARABIC, USCRIPT_SYRIAC, USCRIPT_THAANA, kScriptHanifiRohingya };
        constexpr ScriptSet kAlmLine{ kAlmLineScripts, std::size(kAlmLineScripts), false };

        // `Xs` — scripts where ZWNJ is word-internal (look-behind AND look-ahead rules).
        constexpr UScriptCode kZwnjScripts[] = { USCRIPT_ARABIC, USCRIPT_SYRIAC, USCRIPT_NKO, USCRIPT_MONGOLIAN, USCRIPT_DEVANAGARI,
                                                 USCRIPT_BENGALI, USCRIPT_GURMUKHI, USCRIPT_GUJARATI, USCRIPT_ORIYA, USCRIPT_TAMIL,
                                                 USCRIPT_TELUGU, USCRIPT_KANNADA, USCRIPT_MALAYALAM, USCRIPT_SINHALA, USCRIPT_MYANMAR,
                                                 USCRIPT_KHMER, USCRIPT_TIBETAN };
        constexpr ScriptPair kZwnjPair{ { kZwnjScripts, std::size(kZwnjScripts), false }, { kZwnjScripts, std::size(kZwnjScripts), true } };

        // `Kc` — scripts where a ZWJ after a letter is meaningful (conjunct / half-form control).
        constexpr UScriptCode kZwjScripts[] = { USCRIPT_DEVANAGARI, USCRIPT_BENGALI, USCRIPT_GURMUKHI, USCRIPT_GUJARATI, USCRIPT_ORIYA,
                                                USCRIPT_TAMIL, USCRIPT_TELUGU, USCRIPT_KANNADA, USCRIPT_MALAYALAM, USCRIPT_SINHALA,
                                                USCRIPT_MYANMAR, USCRIPT_KHMER, USCRIPT_TIBETAN, USCRIPT_ARABIC, USCRIPT_SYRIAC, USCRIPT_TIFINAGH };
        constexpr ScriptPair kZwjPair{ { kZwjScripts, std::size(kZwjScripts), false }, { kZwjScripts, std::size(kZwjScripts), true } };

        // `Xc` — joining scripts where a ZWJ BEFORE a letter (after a non-letter) forces a joining form.
        constexpr UScriptCode kZwjForwardScripts[] = { USCRIPT_ARABIC, USCRIPT_SYRIAC, USCRIPT_MONGOLIAN, USCRIPT_NKO };
        constexpr ScriptPair kZwjForwardPair{ { kZwjForwardScripts, std::size(kZwjForwardScripts), false },
                                              { kZwjForwardScripts, std::size(kZwjForwardScripts), true } };

        // `sa` / `Jc` — the scripts written without spaces, where ZWSP is the word-break hint.
        constexpr UScriptCode kSeaScripts[] = { USCRIPT_KHMER, USCRIPT_THAI, USCRIPT_LAO, USCRIPT_MYANMAR };
        constexpr ScriptSet kSea{ kSeaScripts, std::size(kSeaScripts), false };
        constexpr ScriptPair kSeaPair{ kSea, kSea };

        // `Zs` / `Zc` / `Dr` / `Qs` / `ea` — the single-script format-control rules (all `En`: Script only).
        constexpr UScriptCode kMongolianScripts[] = { USCRIPT_MONGOLIAN };
        constexpr ScriptPair kMongolianPair{ { kMongolianScripts, 1, false }, { kMongolianScripts, 1, false } };
        constexpr UScriptCode kKhmerScripts[] = { USCRIPT_KHMER };
        constexpr ScriptPair kKhmerPair{ { kKhmerScripts, 1, false }, { kKhmerScripts, 1, false } };
        constexpr UScriptCode kEgyptianScripts[] = { USCRIPT_EGYPTIAN_HIEROGLYPHS };
        constexpr ScriptSet kEgyptian{ kEgyptianScripts, 1, false };
        constexpr ScriptPair kEgyptianPair{ kEgyptian, kEgyptian };
        constexpr UScriptCode kDuployanScripts[] = { USCRIPT_DUPLOYAN };
        constexpr ScriptPair kDuployanPair{ { kDuployanScripts, 1, false }, { kDuployanScripts, 1, false } };
        constexpr UScriptCode kBrahmiScripts[] = { USCRIPT_BRAHMI };
        constexpr ScriptSet kBrahmi{ kBrahmiScripts, 1, false };

        // `Qc` — the scripts whose standardized variation sequences use VS1.
        constexpr UScriptCode kVs1Scripts[] = { USCRIPT_MYANMAR, USCRIPT_PHAGS_PA, USCRIPT_MANICHAEAN };
        constexpr ScriptSet kVs1{ kVs1Scripts, std::size(kVs1Scripts), false };

        // ---- the look-behind ring (`g`) ----

        constexpr int kRingSize = 16; // `en`

        // The last kRingSize code points the cleanup KEPT, newest last, each remembered with whether it
        // was a conditionally-kept ("hidden") candidate. Removed characters never enter it — that is
        // what makes "the character before" mean "the character before, in the OUTPUT".
        struct Ring
        {
            std::array<Cp, kRingSize> ring{};
            int next = 0;
            int size = 0;
            bool lastWasHidden = false;
            size_t keptConditional = 0;

            // `yn`: record a kept code point (hidden == a conditionally-kept candidate, which is also
            // what the keptConditional counter counts).
            //   cp — the kept code point; hidden — true for a conditionally-kept candidate.
            //   throws — never.
            void Push(Cp cp, bool hidden) noexcept
            {
                ring[static_cast<size_t>(next)] = cp;
                next = (next + 1) % kRingSize;
                if (size < kRingSize)
                {
                    ++size;
                }
                lastWasHidden = hidden;
                if (hidden)
                {
                    ++keptConditional;
                }
            }

            // `Qt`: the n-th newest kept code point (0 == the one right before the cursor), or nullopt
            // past what the ring holds (the start of the text, for short prefixes).
            //   n — 0-based age (0 == newest).  returns — the code point, or nullopt past the ring's size.
            //   throws — never.
            OptCp At(int n) const noexcept
            {
                if (n >= size)
                {
                    return std::nullopt;
                }
                return ring[static_cast<size_t>((next - 1 - n + 2 * kRingSize) % kRingSize)];
            }
        };

        // `oa(g, pair, baseOnly)`: walking back over combining marks (each must belong to pair.mark),
        // is the first non-mark character in pair.base (as a LETTER unless baseOnly)? False whenever
        // the newest ring entry was itself a kept candidate — two conditional keeps never chain.
        bool PrecedingBaseInPair(const Ring& g, const ScriptPair& pair, bool baseOnly) noexcept
        {
            if (g.lastWasHidden)
            {
                return false;
            }
            for (int i = 0; i < g.size; ++i)
            {
                const Cp s = *g.At(i);
                if (IsMark(s))
                {
                    if (!InScriptSet(s, pair.mark))
                    {
                        return false;
                    }
                    continue;
                }
                return baseOnly ? InScriptSet(s, pair.base) : LetterInScripts(s, pair.base);
            }
            return false;
        }

        // `vn(g, pair)`: PrecedingBaseInPair requiring a LETTER.
        bool PrecedingLetterInPair(const Ring& g, const ScriptPair& pair) noexcept
        {
            return PrecedingBaseInPair(g, pair, false);
        }

        // `jr(g, pred)`: the IMMEDIATELY previous kept character (no mark skipping) satisfies pred, and
        // it was not itself a conditional keep.
        template<typename Pred>
        bool PreviousIs(const Ring& g, Pred pred) noexcept
        {
            return g.size > 0 && !g.lastWasHidden && pred(*g.At(0));
        }

        // `wn(next, pred)`: the NEXT character exists, is not a candidate itself, and satisfies pred.
        template<typename Pred>
        bool NextIs(OptCp next, Pred pred) noexcept
        {
            return next.has_value() && !IsClaudeInvisibleCandidate(*next) && pred(*next);
        }

        // `xn(next, pair)`: the NEXT character is a non-candidate LETTER of pair.base.
        bool NextLetterInPair(OptCp next, const ScriptPair& pair) noexcept
        {
            return next.has_value() && !IsClaudeInvisibleCandidate(*next) && LetterInScripts(*next, pair.base);
        }

        // `ta(set, r, lineStart)`: does the LINE starting at lineStart hold a letter of `set` inside the
        // RTL blocks? Scans the whole line (so a direction mark BEFORE the Hebrew word counts too) and
        // stops at the first line break, returning false at a break or the end of the text.
        bool LineHoldsRtlLetter(std::wstring_view r, size_t lineStart, const ScriptSet& set) noexcept
        {
            for (size_t i = lineStart; i < r.size();)
            {
                const Decoded d = DecodeAt(r, i);
                if (IsClaudeLineBreak(d.cp))
                {
                    return false;
                }
                if (InRtlBlock(d.cp) && LetterInScripts(d.cp, set))
                {
                    return true;
                }
                i += d.units;
            }
            return false;
        }

        // `na`: the direction family a letter belongs to for the redundancy test — RTL (an `ia`-script
        // letter, or the Arabic TATWEEL, which is Script=Common but joins RTL text) vs everything else.
        bool IsRtlDirectional(Cp c) noexcept
        {
            return c == 0x0640 || InScriptSet(c, kRtlLine);
        }

        // `tu(g, next)`: is a direction mark REDUNDANT here — sitting between two letters of the same
        // direction family, or between two digits (where it changes nothing)? Walks back over marks;
        // a candidate in the look-behind (only reachable after a kept candidate) reads as redundant;
        // a ring that is ALL marks (16 of them) is treated as redundant too, the bundle's cap.
        bool DirectionMarkRedundant(const Ring& g, OptCp next) noexcept
        {
            const bool nextIsLetter = next.has_value() && IsLetter(*next);
            if (!nextIsLetter && !(next.has_value() && IsDecimalDigit(*next)))
            {
                return false;
            }
            for (int i = 0; i < g.size; ++i)
            {
                const Cp s = *g.At(i);
                if (IsClaudeInvisibleCandidate(s))
                {
                    return true;
                }
                if (IsMark(s))
                {
                    continue;
                }
                return nextIsLetter ? (IsLetter(s) && IsRtlDirectional(s) == IsRtlDirectional(*next)) : IsDecimalDigit(s);
            }
            return g.size >= kRingSize;
        }

        // `zr(cps, 0)`: when cps[0] is the WAVING BLACK FLAG, is it followed by the tag spelling of a
        // RECOGNIZED subdivision flag (gbeng / gbsct / gbwls — England, Scotland, Wales) closed by the
        // CANCEL TAG? Returns the index of that cancel tag (so cps[1..result] are the tag characters to
        // keep), or -1. Any other tag run is not a flag and its tags fall to the normal removal rule.
        int SubdivisionFlagEnd(const std::vector<Decoded>& cps)
        {
            constexpr size_t kMaxTagLetters = 5; // `aa`
            std::string letters;
            for (size_t s = 1; s <= kMaxTagLetters + 1 && s < cps.size(); ++s)
            {
                const Cp h = cps[s].cp;
                if (h == 0xE007F) // CANCEL TAG: the sequence is complete — accept only the three flags
                {
                    return (letters == "gbeng" || letters == "gbsct" || letters == "gbwls") ? static_cast<int>(s) : -1;
                }
                if (h < 0xE0061 || h > 0xE007A) // not a TAG LATIN SMALL LETTER
                {
                    return -1;
                }
                letters.push_back(static_cast<char>(h - 0xE0000));
            }
            return -1;
        }

        // Bump the bucket counter a removed candidate belongs to (`n[iu(N)]++`).
        void CountRemovedByClass(ClaudeInputCleanup& out, Cp cp) noexcept
        {
            switch (ClassifyClaudeInvisible(cp))
            {
            case ClaudeInvisibleClass::Tags:
                ++out.removedTags;
                break;
            case ClaudeInvisibleClass::Bidi:
                ++out.removedBidi;
                break;
            case ClaudeInvisibleClass::ZeroWidth:
                ++out.removedZeroWidth;
                break;
            case ClaudeInvisibleClass::Selectors:
                ++out.removedSelectors;
                break;
            case ClaudeInvisibleClass::Other:
            default:
                ++out.removedOther;
                break;
            }
        }

        // NFC, or `text` itself when it already is NFC / ICU cannot help. Claude's editor NFC-normalizes
        // everything it holds (its MeasuredText constructor), so NFC is the form both its render and its
        // submit carry.
        //   text    — any UTF-16 text (lone surrogates pass through ICU untouched).
        //   returns — the NFC form; the input unchanged on any ICU failure or a > INT32_MAX length.
        //   throws  — std::bad_alloc on allocation failure only.
        std::wstring NfcOrSame(std::wstring_view text)
        {
            if (text.empty() || text.size() > static_cast<size_t>(INT32_MAX))
            {
                return std::wstring{ text };
            }
            UErrorCode err = U_ZERO_ERROR;
            const UNormalizer2* nfc = unorm2_getNFCInstance(&err);
            if (U_FAILURE(err) || nfc == nullptr)
            {
                return std::wstring{ text };
            }
            const auto* src = reinterpret_cast<const UChar*>(text.data());
            const auto len = static_cast<int32_t>(text.size());
            err = U_ZERO_ERROR;
            const UBool already = unorm2_isNormalized(nfc, src, len, &err);
            if (U_SUCCESS(err) && already != 0)
            {
                return std::wstring{ text }; // the common case: no second buffer, no second pass
            }
            // Preflight for the exact length (composition can shrink; reordering keeps it), then fill.
            err = U_ZERO_ERROR;
            const int32_t need = unorm2_normalize(nfc, src, len, nullptr, 0, &err);
            if (err != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(err))
            {
                return std::wstring{ text };
            }
            std::wstring out(static_cast<size_t>(need), L'\0');
            err = U_ZERO_ERROR;
            unorm2_normalize(nfc, src, len, reinterpret_cast<UChar*>(out.data()), need, &err);
            if (U_FAILURE(err))
            {
                return std::wstring{ text };
            }
            return out;
        }

        // The code-point count — CountGraphemeClusters' fallback when ICU cannot segment (an upper
        // bound on clusters, and still never above the UTF-16-unit count the budget used before).
        size_t CountCodePoints(std::wstring_view text) noexcept
        {
            size_t n = 0;
            for (size_t i = 0; i < text.size();)
            {
                i += DecodeAt(text, i).units;
                ++n;
            }
            return n;
        }
    }

    ClaudeInputCleanup CleanClaudeInputText(std::wstring_view input)
    {
        ClaudeInputCleanup out;

        // `Vc` fast path: TAB/LF/printable-ASCII text holds no candidate — return it untouched,
        // without a single property lookup (the overwhelmingly common English prompt).
        if (!TextNeedsClaudeInvisibleScan(input))
        {
            out.text.assign(input);
            return out;
        }

        // `ep` + `ru`: make the text well-formed (every lone surrogate -> U+FFFD) and count those
        // replacements as removals — claude would count them too, so a lone surrogate alone cancels.
        std::wstring wellFormed; // `r` — every index below (line starts, the kept-run start) points into THIS
        wellFormed.reserve(input.size());
        size_t loneSurrogates = 0;
        for (size_t k = 0; k < input.size(); ++k)
        {
            if (IsLoneSurrogateAt(input, k))
            {
                wellFormed.push_back(static_cast<wchar_t>(0xFFFD));
                ++loneSurrogates;
                continue;
            }
            wellFormed.push_back(input[k]);
            const auto unit = static_cast<char16_t>(input[k]);
            if (unit >= 0xD800 && unit <= 0xDBFF)
            {
                wellFormed.push_back(input[++k]); // the valid pair's low half (IsLoneSurrogateAt proved it is there)
            }
        }
        size_t removed = loneSurrogates; // `h` — the running removedTotal
        out.removedOther += loneSurrogates;

        Ring ring; // `g`
        size_t lineStart = 0; // `y` — where the current line begins in wellFormed
        int lineRtl = -1; // `w` — cached ia-line test for LRM/RLM: -1 unknown, 0/1 answer
        int lineAlm = -1; // `E` — cached qc-line test for ALM
        size_t keptFrom = 0; // `s` — start of the not-yet-flushed kept run
        std::wstring built; // `i.join("")` — the flushed output
        built.reserve(wellFormed.size());

        for (size_t pos = 0; pos < wellFormed.size();) // `C`
        {
            const Decoded here = DecodeAt(wellFormed, pos);
            const Cp cp = here.cp; // `N`
            const size_t after = pos + here.units; // `M`

            if (!IsClaudeInvisibleCandidate(cp))
            {
                if (cp == 0x0A)
                {
                    // A real LF starts a new line: the direction-mark line tests must be re-asked.
                    lineStart = after;
                    lineRtl = -1;
                    lineAlm = -1;
                }
                else if (cp == 0x1F3F4)
                {
                    // WAVING BLACK FLAG: a recognized subdivision flag keeps its tag characters
                    // (hidden keeps); anything else leaves the tags to be removed one by one below.
                    // The bundle looks at the next 14 UTF-16 units (flag + up to 6 tags).
                    const size_t sliceEnd = (wellFormed.size() - pos > 14) ? pos + 14 : wellFormed.size();
                    const std::wstring_view slice = std::wstring_view{ wellFormed }.substr(0, sliceEnd);
                    std::vector<Decoded> flagCps;
                    for (size_t k = pos; k < sliceEnd;)
                    {
                        const Decoded d = DecodeAt(slice, k); // a pair split by the slice edge decodes lone, as in JS
                        flagCps.push_back(d);
                        k += d.units;
                    }
                    const int flagEnd = SubdivisionFlagEnd(flagCps);
                    if (flagEnd != -1)
                    {
                        ring.Push(cp, false);
                        pos = after;
                        for (int tag = 1; tag <= flagEnd; ++tag)
                        {
                            ring.Push(flagCps[static_cast<size_t>(tag)].cp, true);
                            pos += flagCps[static_cast<size_t>(tag)].units;
                        }
                        continue;
                    }
                }
                ring.Push(cp, false);
                pos = after;
                continue;
            }

            if (IsClaudeLineBreak(cp))
            {
                // Every candidate line break becomes LF (counted) — except a CR directly followed by an
                // LF, which is simply dropped (not counted): that LF comes through on the next step.
                const bool crBeforeLf = cp == 0x0D && after < wellFormed.size() && wellFormed[after] == L'\n';
                if (pos > keptFrom)
                {
                    built.append(wellFormed, keptFrom, pos - keptFrom);
                }
                keptFrom = after;
                if (!crBeforeLf)
                {
                    built.push_back(L'\n');
                    ++removed;
                    ++out.removedOther;
                    ring.Push(0x0A, false);
                    lineStart = after;
                    lineRtl = -1;
                    lineAlm = -1;
                }
                pos = after;
                continue;
            }

            const OptCp nextCp = (after < wellFormed.size()) ? OptCp{ DecodeAt(wellFormed, after).cp } : std::nullopt; // `L`
            const OptCp prevCp = ring.At(0); // `j` — the previous KEPT code point
            const bool prevWasHidden = ring.lastWasHidden; // `P` — was that previous one itself a conditional keep?
            bool keep = false; // `B`

            switch (cp)
            {
            case 0x200C: // ZWNJ
            case 0x200D: // ZWJ
            {
                keep = PrecedingLetterInPair(ring, cp == 0x200C ? kZwnjPair : kZwjPair);
                if (!keep && cp == 0x200D)
                {
                    // An emoji ZWJ sequence: the base before the joiner (skipping up to two VS16 /
                    // skin-tone modifiers) and the character after must both be pictographic.
                    OptCp base = prevCp;
                    if (base && (*base == 0xFE0F || IsSkinToneModifier(*base)))
                    {
                        base = ring.At(1);
                        if (base && (*base == 0xFE0F || IsSkinToneModifier(*base)))
                        {
                            base = ring.At(2);
                        }
                    }
                    keep = prevCp && *prevCp != 0x200D && base && IsExtendedPictographic(*base) && NextIs(nextCp, IsExtendedPictographic);
                }
                if (!keep && prevCp && !prevWasHidden)
                {
                    // Look-ahead: a joiner right before a letter of a joining script, after a character
                    // that is not whitespace/mark (ZWNJ) or not a letter/digit/mark (ZWJ).
                    keep = (cp == 0x200C) ? (NextLetterInPair(nextCp, kZwnjPair) && !IsWhiteSpaceProp(*prevCp) && !IsMark(*prevCp)) :
                                            (NextLetterInPair(nextCp, kZwjForwardPair) && !IsLetter(*prevCp) && !IsDecimalDigit(*prevCp) && !IsMark(*prevCp));
                }
                break;
            }
            case 0x200B: // ZWSP: a word-break hint only between Khmer/Thai/Lao/Myanmar words (or beside ASCII digits there)
            {
                const bool afterSea = PrecedingBaseInPair(ring, kSeaPair, true);
                const bool beforeSea = NextIs(nextCp, [](Cp c) noexcept { return InScriptSet(c, kSea); }) && !(nextCp && IsMark(*nextCp));
                keep = (afterSea && (beforeSea || NextIs(nextCp, IsAsciiDigit))) || (beforeSea && PreviousIs(ring, IsAsciiDigit));
                break;
            }
            case 0xFE0E: // VS15 (text presentation)
            case 0xFE0F: // VS16 (emoji presentation) — the one inside a warning-sign emoji is KEPT here
                keep = prevCp && !prevWasHidden &&
                       ((*prevCp >= 0xA9 && IsEmojiOrPictographic(*prevCp)) || (IsKeycapBase(*prevCp) && nextCp && *nextCp == 0x20E3));
                break;
            case 0xFE00: // VS1
            case 0xFE01: // VS2
            case 0xFE02: // VS3
                keep = prevCp && !prevWasHidden &&
                       (InScriptSet(*prevCp, kEgyptian) ||
                        (cp == 0xFE00 && ((*prevCp >= 0x2200 && *prevCp <= 0x2AFF && IsMathSymbol(*prevCp)) || InScriptSet(*prevCp, kVs1))));
                break;
            case 0x200E: // LRM
            case 0x200F: // RLM
            case 0x061C: // ALM
                // THE HEBREW RULE: a direction mark survives only on a line that holds Hebrew/Arabic-
                // family letters (Arabic-family for ALM), not right after another conditional keep,
                // before the end / a line break / a visible non-mark character, and only where it is
                // not redundant between two same-direction letters (or two digits).
                if (cp == 0x061C)
                {
                    if (lineAlm < 0)
                    {
                        lineAlm = LineHoldsRtlLetter(wellFormed, lineStart, kAlmLine) ? 1 : 0;
                    }
                    keep = lineAlm == 1;
                }
                else
                {
                    if (lineRtl < 0)
                    {
                        lineRtl = LineHoldsRtlLetter(wellFormed, lineStart, kRtlLine) ? 1 : 0;
                    }
                    keep = lineRtl == 1;
                }
                keep = keep && !prevWasHidden &&
                       (!nextCp || IsClaudeLineBreak(*nextCp) || (!IsClaudeInvisibleCandidate(*nextCp) && !IsMark(*nextCp))) &&
                       !DirectionMarkRedundant(ring, nextCp);
                break;
            case 0x034F: // CGJ: meaningful only between combining marks
                keep = PreviousIs(ring, IsMark) || (!prevWasHidden && NextIs(nextCp, IsMark));
                break;
            case 0x17B4: // KHMER VOWEL INHERENT AQ
            case 0x17B5: // KHMER VOWEL INHERENT AA
                keep = PrecedingLetterInPair(ring, kKhmerPair);
                break;
            case 0x180B: // MONGOLIAN FVS1
            case 0x180C: // FVS2
            case 0x180D: // FVS3
            case 0x180E: // MONGOLIAN VOWEL SEPARATOR
            case 0x180F: // FVS4
                keep = PrecedingLetterInPair(ring, kMongolianPair) || (!prevWasHidden && NextLetterInPair(nextCp, kMongolianPair));
                break;
            case 0x1107F: // BRAHMI NUMBER JOINER
                keep = PreviousIs(ring, [](Cp c) noexcept { return InScriptSet(c, kBrahmi); }) &&
                       NextIs(nextCp, [](Cp c) noexcept { return InScriptSet(c, kBrahmi); });
                break;
            default:
                if (cp >= 0x13430 && cp <= 0x1343F) // Egyptian hieroglyph format controls
                {
                    keep = PrecedingLetterInPair(ring, kEgyptianPair) || (!prevWasHidden && NextLetterInPair(nextCp, kEgyptianPair));
                }
                else if (cp >= 0x1BCA0 && cp <= 0x1BCA3) // shorthand format controls (Duployan)
                {
                    keep = PrecedingLetterInPair(ring, kDuployanPair) || (!prevWasHidden && NextLetterInPair(nextCp, kDuployanPair));
                }
                break; // every other candidate has no keep rule: removed
            }

            if (keep)
            {
                ring.Push(cp, true);
            }
            else
            {
                if (pos > keptFrom)
                {
                    built.append(wellFormed, keptFrom, pos - keptFrom);
                }
                keptFrom = after;
                ++removed;
                CountRemovedByClass(out, cp);
            }
            pos = after;
        }

        out.removedTotal = removed;
        out.keptConditional = ring.keptConditional;
        if (keptFrom == 0)
        {
            out.text = std::move(wellFormed); // nothing removed or rewritten (lone surrogates may still have been replaced)
            return out;
        }
        built.append(wellFormed, keptFrom, wellFormed.size() - keptFrom);
        out.text = std::move(built);
        return out;
    }

    std::wstring SanitizeForClaudeInput(std::wstring_view text)
    {
        if (!TextNeedsClaudeInvisibleScan(text))
        {
            return std::wstring{ text }; // printable ASCII + TAB/LF: already exactly what claude would hold and send
        }
        // NFC first — the form claude's editor will hold and its Enter-time cleanup will see — then
        // the cleanup, then NFC again (claude's editor re-normalizes whatever we paste), REPEATED until
        // nothing changes. One pass is not always a fixed point: a removal can leave combinable
        // characters adjacent (a dropped format control between "a" and a combining acute), the
        // re-normalization composes them, and a CGJ that was kept because it FOLLOWED that mark now
        // follows a precomposed letter — which the cleanup removes. Pasting the one-pass result would
        // therefore still cancel claude's Enter for that input (measured: 8 of 50,099 fuzzed strings;
        // the loop needs at most 2 extra rounds and fixes all of them — tests_invisible.cpp pins the
        // shape). Every round that continues removed at least one character, so the loop is bounded by
        // the text length; the cap is a belt against an ICU oddity, never a practical limit.
        constexpr int kMaxRounds = 16;
        std::wstring current = NfcOrSame(text);
        for (int round = 0; round < kMaxRounds; ++round)
        {
            ClaudeInputCleanup pass = CleanClaudeInputText(current);
            if (pass.removedTotal == 0 && pass.text == current)
            {
                return current; // NFC (by construction) and clean: exactly what an Enter submits as-is
            }
            current = NfcOrSame(pass.text);
        }
        return current;
    }

    std::wstring NormalizePromptTextForCompare(std::wstring_view text)
    {
        if (!TextNeedsClaudeInvisibleScan(text))
        {
            return std::wstring{ text }; // no CR, no candidate, nothing to compose: the key is the text
        }
        std::wstring folded;
        folded.reserve(text.size());
        for (size_t i = 0; i < text.size();)
        {
            if (IsLoneSurrogateAt(text, i))
            {
                folded.push_back(static_cast<wchar_t>(0xFFFD)); // claude's toWellFormed
                ++i;
                continue;
            }
            const Decoded d = DecodeAt(text, i);
            if (d.cp == 0x0D)
            {
                folded.push_back(L'\n'); // CR and CRLF both read as ONE LF (the FoldCrToLf rule)
                i += (i + 1 < text.size() && text[i + 1] == L'\n') ? 2 : 1;
                continue;
            }
            if (IsClaudeLineBreak(d.cp))
            {
                folded.push_back(L'\n'); // LF stays; VT/FF/NEL/LS/PS are claude's LF rewrites
                i += d.units;
                continue;
            }
            if (IsClaudeInvisibleCandidate(d.cp))
            {
                i += d.units; // kept-or-removed by claude, dropped on BOTH sides here
                continue;
            }
            folded.append(text.substr(i, d.units));
            i += d.units;
        }
        return NfcOrSame(folded);
    }

    size_t CountGraphemeClusters(std::wstring_view text)
    {
        if (text.empty())
        {
            return 0;
        }
        if (text.size() > static_cast<size_t>(INT32_MAX))
        {
            return CountCodePoints(text);
        }
        UErrorCode err = U_ZERO_ERROR;
        UBreakIterator* it = ubrk_open(UBRK_CHARACTER, "", reinterpret_cast<const UChar*>(text.data()), static_cast<int32_t>(text.size()), &err);
        if (U_FAILURE(err) || it == nullptr)
        {
            if (it != nullptr)
            {
                ubrk_close(it);
            }
            return CountCodePoints(text);
        }
        size_t clusters = 0;
        for (int32_t boundary = ubrk_next(it); boundary != UBRK_DONE; boundary = ubrk_next(it))
        {
            ++clusters; // one boundary after position 0 per cluster
        }
        ubrk_close(it);
        return clusters;
    }

    // ===== AskUserQuestion's option PREVIEWS (INTERACTIVE_TOOLS.md §1.2; Claude Code 2.1.291) =====
    // Each helper names the bundle identifier it mirrors. The module of `Nl` / `zz` / `e0` / `zet` starts at byte 210,378,231 of
    // the 2.1.291 claude.exe; `Vs` / `Wa` / `Rae` / `Ha` / `Ka` / `mR` / `gR` live in the module imported from chunk-bbhdeztd
    // (byte ~208,072,600–208,075,800); `G5` (which calls them) at 237,369,819.

    namespace
    {
        // `D`: the characters a terminal cannot be trusted to draw — a default-ignorable code point, LS, PS, U+2800, a surrogate
        // half, U+10EEEE, or a format character (\p{Cf}).
        //   c — a code point (a lone surrogate arrives as its own value).  returns — true when unsafe.  throws — never.
        bool IsPreviewUnsafe(Cp c) noexcept
        {
            if (c == 0x2028 || c == 0x2029 || c == 0x2800 || (c >= 0xD800 && c <= 0xDFFF) || c == 0x10EEEE)
            {
                return true;
            }
            const UChar32 u = AsUChar32(c);
            return u_hasBinaryProperty(u, UCHAR_DEFAULT_IGNORABLE_CODE_POINT) != 0 || u_charType(u) == U_FORMAT_CHAR;
        }

        // `an`: the unsafe characters `Nl` KEEPS (its predicate `ln` is `D && !an`) — ZWNJ, ZWJ, LRM, RLM (U+200C–U+200F), ALM
        // (U+061C), the variation selectors (U+FE00–U+FE0F), the Mongolian free variation selectors (U+180B–U+180F), U+2800.
        //   c — a code point.  returns — true when `Nl` leaves it in place.  throws — never.
        bool IsPreviewKeptUnsafe(Cp c) noexcept
        {
            return (c >= 0x200C && c <= 0x200F) || c == 0x061C || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x180B && c <= 0x180F) || c == 0x2800;
        }

        // `Ha`: a control character the TUI never draws — every C0 control but TAB and LF (a CR too), DEL and the C1 controls.
        //   c — a code point.  returns — true for such a control.  throws — never.
        bool IsPreviewControl(Cp c) noexcept
        {
            if (c == 0x09 || c == 0x0A)
            {
                return false;
            }
            return c < 0x20 || (c >= 0x7F && c <= 0x9F);
        }

        // `Ka`: ALM, the bidi embeddings and overrides (U+202A–U+202E) and the bidi isolates (U+2066–U+2069).
        //   c — a code point.  returns — true for such a control.  throws — never.
        bool IsPreviewBidiControl(Cp c) noexcept
        {
            return c == 0x061C || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
        }

        // JavaScript's `\s` (the WhiteSpace and LineTerminator sets): what `zz` folds away before it asks whether anything
        // visible is left.
        //   c — a code point.  returns — true for JS whitespace.  throws — never.
        bool IsJsWhiteSpace(Cp c) noexcept
        {
            switch (c)
            {
            case 0x09:
            case 0x0A:
            case 0x0B:
            case 0x0C:
            case 0x0D:
            case 0x20:
            case 0xA0:
            case 0x1680:
            case 0x2028:
            case 0x2029:
            case 0x202F:
            case 0x205F:
            case 0x3000:
            case 0xFEFF:
                return true;
            default:
                return c >= 0x2000 && c <= 0x200A;
            }
        }

        // `Vs` without its grapheme-width repair `Ba` (see ClaudeQuestionPreviewEcho's remarks): `Wa` drops VS15 / VS16 (`mR`)
        // and turns every `Ha` / `Ka` character and every surrogate half (`gR`) into U+FFFD; then `Rae` → `iur` expands each TAB
        // to the next multiple of 8 display columns (`Ykt` = 8), counting each other piece by its terminal width and starting the
        // count again after a LF. `Rae` also rewrites ANSI escape sequences, but `Wa` has already turned their ESC / CSI / C1
        // introducers into U+FFFD, so only the TABs are left for it.
        //   text    — the text after `Nl`'s first step (or any text).
        //   returns — the terminal-safe text.
        //   throws  — std::bad_alloc if the result cannot be allocated.
        std::wstring PreviewTerminalSafe(std::wstring_view text)
        {
            std::wstring out;
            out.reserve(text.size());
            bool hasTab = false;
            for (size_t i = 0; i < text.size();)
            {
                const Decoded d = DecodeAt(text, i);
                const bool loneSurrogate = d.units == 1 && d.cp >= 0xD800 && d.cp <= 0xDFFF;
                if (d.cp == 0xFE0E || d.cp == 0xFE0F)
                {
                    i += d.units; // `mR`: dropped, not replaced
                    continue;
                }
                if (IsPreviewControl(d.cp) || IsPreviewBidiControl(d.cp) || loneSurrogate)
                {
                    out.push_back(static_cast<wchar_t>(0xFFFD));
                }
                else
                {
                    hasTab = hasTab || d.cp == 0x09;
                    out.append(text.substr(i, d.units));
                }
                i += d.units;
            }
            if (!hasTab)
            {
                return out; // `Rae` returns its input when it holds no TAB (and no escape sequence is left)
            }
            std::wstring expanded;
            expanded.reserve(out.size() + 16);
            size_t column = 0;
            for (size_t i = 0; i < out.size();)
            {
                const Decoded d = DecodeAt(out, i);
                if (d.cp == 0x09)
                {
                    const size_t pad = 8 - column % 8;
                    expanded.append(pad, L' ');
                    column += pad;
                }
                else if (d.cp == 0x0A)
                {
                    expanded.push_back(L'\n');
                    column = 0;
                }
                else
                {
                    expanded.append(out, i, d.units);
                    column += static_cast<size_t>(ClaudeCellWidth(d.cp));
                }
                i += d.units;
            }
            return expanded;
        }

        // `zz`: is anything VISIBLE in the preview? `k2e` turns every `D` character (the kept ones too) into U+FFFD and applies
        // `Vs` and `Ym`; `zz` then removes every U+FFFD, folds whitespace and trims, and asks for a non-empty text of non-zero
        // width. Equivalent per code point: some character that is not `D`, `Ha`, `Ka`, a surrogate half, VS15 / VS16 or JS
        // whitespace, and whose width is not 0 (a mark alone draws nothing).
        //   preview — the option's preview.  returns — true when something is visible.  throws — never.
        bool PreviewHasVisibleText(std::wstring_view preview) noexcept
        {
            for (size_t i = 0; i < preview.size();)
            {
                const Decoded d = DecodeAt(preview, i);
                i += d.units;
                const bool loneSurrogate = d.units == 1 && d.cp >= 0xD800 && d.cp <= 0xDFFF;
                if (loneSurrogate || IsPreviewUnsafe(d.cp) || IsPreviewControl(d.cp) || IsPreviewBidiControl(d.cp) || d.cp == 0xFE0E || d.cp == 0xFE0F ||
                    IsJsWhiteSpace(d.cp))
                {
                    continue;
                }
                if (ClaudeCellWidth(d.cp) > 0)
                {
                    return true;
                }
            }
            return false;
        }

        // `Ske` = /… \[\+\d+ graphemes\]/: Claude Code's own truncation marker — a preview that carries one was cut by an
        // earlier pass and is withheld rather than shown as if whole.
        //   text — the echoed preview.  returns — true when the marker occurs.  throws — never.
        bool HasPreviewTruncationMarker(std::wstring_view text) noexcept
        {
            constexpr std::wstring_view kHead{ L"\x2026 [+" }; // an escape, not the literal character: the literal's bytes depend on /utf-8
            constexpr std::wstring_view kTail{ L" graphemes]" };
            for (size_t at = text.find(kHead); at != std::wstring_view::npos; at = text.find(kHead, at + 1))
            {
                size_t p = at + kHead.size();
                const size_t digitsFrom = p;
                while (p < text.size() && text[p] >= L'0' && text[p] <= L'9')
                {
                    ++p;
                }
                if (p > digitsFrom && text.substr(p, kTail.size()) == kTail)
                {
                    return true;
                }
            }
            return false;
        }

        // `Ee` = `Ym(x) !== x`: does the text hold a run of MORE than 8 (`pn`) consecutive marks or zero-width characters
        // (`qQr`: \p{M}, or a character of no visible width once VS15 / VS16 / the Mongolian selectors are gone)? `Ym` replaces
        // such a run's tail with U+FFFD — the "Zalgo" guard — and a preview it would change is withheld. A LF or any visible
        // character ends a run.
        //   text — the echoed preview.  returns — true when a run longer than 8 occurs.  throws — never.
        bool HasPreviewLongMarkRun(std::wstring_view text) noexcept
        {
            size_t run = 0;
            for (size_t i = 0; i < text.size();)
            {
                const Decoded d = DecodeAt(text, i);
                i += d.units;
                if (d.cp == 0x0A)
                {
                    run = 0;
                    continue;
                }
                const bool zeroWidth = d.cp == 0xFE0E || d.cp == 0xFE0F || (d.cp >= 0x180B && d.cp <= 0x180F) || ClaudeCellWidth(d.cp) == 0;
                if (!(IsMark(d.cp) || zeroWidth))
                {
                    run = 0;
                    continue;
                }
                if (++run > 8)
                {
                    return true;
                }
            }
            return false;
        }
    }

    ClaudePreviewKind ClassifyClaudeQuestionPreview(std::wstring_view preview)
    {
        // `G5`, in its own order: the length (in UTF-16 units, JavaScript's String.length) first, then `zz`, then `e0` on `Nl`.
        if (preview.size() > kClaudeQuestionPreviewMaxChars)
        {
            return ClaudePreviewKind::Withheld;
        }
        if (!PreviewHasVisibleText(preview))
        {
            return ClaudePreviewKind::None;
        }
        const std::wstring echo = ClaudeQuestionPreviewEcho(preview);
        if (HasPreviewTruncationMarker(echo) || HasPreviewLongMarkRun(echo))
        {
            return ClaudePreviewKind::Withheld;
        }
        return ClaudePreviewKind::Full;
    }

    std::wstring ClaudeQuestionPreviewEcho(std::wstring_view preview)
    {
        // `Nl` = `Vs(Ce(e, ln))`: first every unsafe character but the kept ones becomes U+FFFD (`Ce` walks code points, so a
        // valid surrogate PAIR is one character and only a lone half is a surrogate here), then the terminal-safe pass.
        std::wstring step;
        step.reserve(preview.size());
        for (size_t i = 0; i < preview.size();)
        {
            const Decoded d = DecodeAt(preview, i);
            if (IsPreviewUnsafe(d.cp) && !IsPreviewKeptUnsafe(d.cp))
            {
                step.push_back(static_cast<wchar_t>(0xFFFD));
            }
            else
            {
                step.append(preview.substr(i, d.units));
            }
            i += d.units;
        }
        return PreviewTerminalSafe(step);
    }

    int ClaudeCellWidth(char32_t cp) noexcept
    {
        if (cp >= 0xD800 && cp <= 0xDFFF)
        {
            return 1; // a lone surrogate half is drawn as the one-column U+FFFD
        }
        const UChar32 u = AsUChar32(cp);
        const int8_t type = u_charType(u);
        // Marks ride on the cell before them; format characters, controls and default-ignorable code points draw nothing.
        if (type == U_NON_SPACING_MARK || type == U_ENCLOSING_MARK || type == U_FORMAT_CHAR || type == U_CONTROL_CHAR)
        {
            return 0;
        }
        if (u_hasBinaryProperty(u, UCHAR_DEFAULT_IGNORABLE_CODE_POINT) != 0)
        {
            return 0;
        }
        const int32_t ea = u_getIntPropertyValue(u, UCHAR_EAST_ASIAN_WIDTH);
        return (ea == U_EA_WIDE || ea == U_EA_FULLWIDTH) ? 2 : 1;
    }
}
