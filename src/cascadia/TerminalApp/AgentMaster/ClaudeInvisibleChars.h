// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — ClaudeInvisibleChars: Claude Code's INVISIBLE-CHARACTER rule, mirrored so a prompt we
// deliver can never be held back by it (DELIVERY.md §15, HOOKS.md "Encoding").
//
// WHY THIS EXISTS. Since Claude Code 2.1.277 (verified byte-for-byte in the 2.1.280 bundle — the
// minified function `wZe` and its helpers), pressing Enter first runs the input box through a cleanup
// that deletes "invisible" characters: zero-width spaces/joiners, bidi controls and direction marks,
// tag characters, variation selectors, C0/C1 controls, fillers. When it deletes ANYTHING
// (removedTotal > 0) the submit is CANCELLED — the cleaned text goes back into the box with
// "Removed N invisible characters · review and press Enter to send". For a human that is a review
// prompt. For a pasted delivery it is a silent stall: the prompt sits in the box un-submitted, the
// Enter-retry watchdog's draft guard compared the box to the prompt EXACTLY and refused to press
// (the box no longer "read as" our prompt), and fifteen seconds later the lost-send verdict marked
// it Failed and paused the autorunner. It hit every paste channel — autorunner, Send-now, the
// /handover paste tier, the standby fill — while a short handover riding the launch commandline was
// cleaned and sent normally (claude never cancels a launch prompt).
//
// THE FIX HAS TWO HALVES, both routed through this file:
//   1. BEFORE INJECTING — SanitizeForClaudeInput runs Claude's OWN cleanup on the text first, so the
//      Enter that submits it finds nothing to remove and never cancels. It is an EXACT mirror, not a
//      blanket strip: the same rule deliberately KEEPS a character where it carries meaning — the
//      emoji variation selector of a warning sign (U+26A0 U+FE0F), the joiners inside a ZWJ emoji
//      (woman-cook, U+1F469 U+200D U+1F373) or a Devanagari conjunct, the ZWNJ of Persian word forms,
//      and (the Hebrew case) an RLM/LRM on a line that holds Hebrew or Arabic text where it actually
//      changes the rendering. A superset strip would damage exactly that text; a subset would still
//      trip the cancel. (This source spells such characters as code points on purpose: a literal
//      invisible character in a source file is the very thing this file exists to handle.)
//   2. ON BOTH SIDES OF EVERY PROMPT COMPARISON — NormalizePromptTextForCompare folds line breaks,
//      drops EVERY candidate character (kept-or-removed alike) and applies NFC, so a prompt compares
//      equal to its delivered form however the cleanup, the terminal render, or claude's editor
//      (which NFC-normalizes everything it holds — its MeasuredText constructor) reshaped the
//      invisible parts. Hebrew with niqqud typed in non-canonical mark order is the live case for
//      the NFC half: the box renders it reordered, and an exact compare called that "foreign".
//
// PURE vs ICU. The candidate / line-break / class predicates below are pure constexpr code-point
// tables (header-only, no dependencies — the PromptAnchor.h idiom). Everything that needs Unicode
// PROPERTIES (General_Category, Script, Script_Extensions, Emoji, Extended_Pictographic, White_Space),
// normalization, or grapheme segmentation lives in ClaudeInvisibleChars.cpp on the Windows SDK's ICU
// (the same icu.dll WT's own search + fzf use), deliberately OUT of this header so the hot
// SessionModels.h include chain never drags <icu.h> into every TU. Any binary that calls the
// functions declared here must compile ClaudeInvisibleChars.cpp: TerminalAppLib, the m5 test harness
// (run-m5-tests.bat) and agentmaster-cli (its vcxproj + _compile.bat) all do.
//
// FIDELITY CONTRACT. The mirror is pinned by tests_invisible.cpp to outputs captured by running
// Claude's own extracted function under node (93 vectors — Hebrew, Arabic, Persian, Devanagari, Thai,
// emoji ZWJ/keycap/flag sequences, every control class). When a future Claude Code version changes
// the rule, re-extract it (DELIVERY.md §15 has the recipe), regenerate the table, and update both.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Agentmaster
{
    // ---- the PURE code-point tables -----------------------------------------------------------------

    // Is `cp` a character Claude Code's Enter-time cleanup even LOOKS at (the bundle's `Sn`)? A
    // candidate is either removed outright or kept only when a context rule proves it meaningful (see
    // CleanClaudeInputText); everything else always passes through untouched. TAB and LF are NOT
    // candidates; every other C0 control, DEL and the C1 range are. The set: C0/C1/DEL, SOFT HYPHEN,
    // CGJ, ARABIC LETTER MARK, the Hangul fillers (U+115F/1160/3164/FFA0), the Khmer inherent vowels,
    // the Mongolian free variation selectors + vowel separator, U+200B–200F (ZWSP ZWNJ ZWJ LRM RLM),
    // U+2028–202E (LS PS + the bidi embeddings/overrides), U+2060–206F (word joiner, invisible
    // operators, isolates, deprecated format controls), VS1–VS16, BOM/ZWNBSP, the U+FFF0–FFFB
    // specials, the Brahmi number joiner, the Egyptian hieroglyph format controls, the Khitan filler,
    // the shorthand format controls, the musical-symbol format controls, and U+E0000–E0FFF (tags,
    // the variation-selector supplement, and the reserved remainder of that plane block).
    //   cp      — a Unicode scalar (a lone surrogate value is simply "not a candidate" here; the
    //             cleanup replaces lone surrogates separately, before this table is consulted).
    //   returns — true when the cleanup would consider `cp` for removal.
    //   throws  — never (noexcept, pure arithmetic).
    constexpr bool IsClaudeInvisibleCandidate(char32_t cp) noexcept
    {
        if (cp < 0xA0)
        {
            return (cp < 0x20 && cp != 0x09 && cp != 0x0A) || cp >= 0x7F; // C0 (minus TAB/LF), DEL, C1
        }
        if (cp < 0x2000)
        {
            return cp == 0x00AD || cp == 0x034F || cp == 0x061C || cp == 0x115F || cp == 0x1160 ||
                   cp == 0x17B4 || cp == 0x17B5 || (cp >= 0x180B && cp <= 0x180F);
        }
        if (cp < 0x10000)
        {
            return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) ||
                   cp == 0x3164 || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0xFFA0 ||
                   (cp >= 0xFFF0 && cp <= 0xFFFB);
        }
        return cp == 0x1107F || (cp >= 0x13430 && cp <= 0x1343F) || cp == 0x16FE4 || (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
               (cp >= 0x1D173 && cp <= 0x1D17A) || (cp >= 0xE0000 && cp <= 0xE0FFF);
    }

    // Is `cp` a LINE BREAK in the cleanup's sense (the bundle's `Hr`)? LF passes untouched; every other
    // one (VT, FF, a lone CR, NEL, LS, PS) is REWRITTEN to LF and counted as a removal — which is why a
    // lone CR in a pasted prompt would cancel the submit on its own. CR immediately followed by LF is the
    // single silent case: the CR is dropped and nothing is counted. Also the line boundary for the
    // direction-mark rule ("does this LINE hold Hebrew/Arabic text?").
    //   cp      — the code point to classify.
    //   returns — true for LF, VT, FF, CR, NEL (U+0085), LS (U+2028) and PS (U+2029).
    //   throws  — never (noexcept, pure arithmetic).
    constexpr bool IsClaudeLineBreak(char32_t cp) noexcept
    {
        return cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x85 || cp == 0x2028 || cp == 0x2029;
    }

    // The bucket a REMOVED character is counted under (the bundle's `iu` — the same five classes
    // Claude's "Removed N invisible characters" accounting keeps). Only diagnostic for us: the send
    // gate is the total, never the class.
    enum class ClaudeInvisibleClass : uint8_t
    {
        Tags, // U+E0000–E007F: the tag block (the "ASCII smuggling" channel)
        Bidi, // ALM, LRM, RLM, the embeddings/overrides U+202A–202E, the isolates U+2066–2069
        ZeroWidth, // ZWSP, ZWNJ, ZWJ, WORD JOINER, BOM/ZWNBSP
        Selectors, // VS1–VS16, the supplement U+E0100–E01EF, Mongolian FVS1–3 + FVS4
        Other, // everything else: controls, SHY, CGJ, fillers, specials, line-break rewrites, lone surrogates
    };

    // Classify a removed candidate into its counting bucket (the bundle's `iu`).
    //   cp      — a code point the cleanup removed (any value is accepted; non-candidates land in Other).
    //   returns — the bucket; Other for anything not named by the four specific classes.
    //   throws  — never (noexcept, pure arithmetic).
    constexpr ClaudeInvisibleClass ClassifyClaudeInvisible(char32_t cp) noexcept
    {
        if (cp >= 0xE0000 && cp <= 0xE007F)
        {
            return ClaudeInvisibleClass::Tags;
        }
        if (cp == 0x061C || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069))
        {
            return ClaudeInvisibleClass::Bidi;
        }
        if ((cp >= 0x200B && cp <= 0x200D) || cp == 0x2060 || cp == 0xFEFF)
        {
            return ClaudeInvisibleClass::ZeroWidth;
        }
        if ((cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF) || (cp >= 0x180B && cp <= 0x180D) || cp == 0x180F)
        {
            return ClaudeInvisibleClass::Selectors;
        }
        return ClaudeInvisibleClass::Other;
    }

    // The cleanup's FAST PATH test (the bundle's `Vc`): text made only of TAB, LF and printable ASCII
    // (U+0020–007E) can hold nothing to clean, so the whole scan — and every ICU property lookup — is
    // skipped. Note CR is NOT in the safe set: a CRLF text takes the full path (its CRs are rewritten).
    //   text    — UTF-16 text to probe.
    //   returns — true when at least one UTF-16 unit falls outside {TAB, LF, U+0020–007E}.
    //   throws  — never (noexcept, a read-only linear scan).
    constexpr bool TextNeedsClaudeInvisibleScan(std::wstring_view text) noexcept
    {
        for (const wchar_t c : text)
        {
            if (!(c == L'\t' || c == L'\n' || (c >= 0x20 && c <= 0x7E)))
            {
                return true;
            }
        }
        return false;
    }

    // ---- the ICU-backed operations (ClaudeInvisibleChars.cpp) --------------------------------------

    // The result of one CleanClaudeInputText pass — the same shape Claude's own function returns
    // ({text, removedTotal, removedByClass, keptConditional}), so a test can compare field for field.
    struct ClaudeInputCleanup
    {
        std::wstring text; // the cleaned text (== the input when nothing was removed or rewritten)
        size_t removedTotal{ 0 }; // what Claude's submit gate checks: > 0 CANCELS an Enter-time submit
        size_t removedTags{ 0 }; // removed, bucketed per ClassifyClaudeInvisible
        size_t removedBidi{ 0 };
        size_t removedZeroWidth{ 0 };
        size_t removedSelectors{ 0 };
        size_t removedOther{ 0 }; // incl. line-break rewrites and lone surrogates (-> U+FFFD)
        size_t keptConditional{ 0 }; // candidates a context rule proved meaningful and KEPT (not a removal)
    };

    // Claude Code's Enter-time invisible-character cleanup, EXACTLY (2.1.280's `wZe`): lone surrogates
    // become U+FFFD; line breaks other than LF are rewritten to LF (a CR directly before an LF is
    // dropped silently, the one uncounted rewrite); every other candidate is removed UNLESS its context
    // rule keeps it — ZWJ/ZWNJ inside Indic/Arabic-family words and emoji ZWJ sequences, ZWSP between
    // Khmer/Thai/Lao/Myanmar words, VS15/VS16 after an emoji (or a keycap base), VS1–3 in the few
    // standardized variation sequences, LRM/RLM (ALM) on a line containing Hebrew/Arabic-family letters
    // (Arabic-family for ALM) when not redundant and not before a combining mark, CGJ between marks,
    // and a few script-specific format controls — plus valid England/Scotland/Wales subdivision flags.
    // Does NOT normalize (no NFC): use SanitizeForClaudeInput for text about to be pasted.
    //   text    — the raw UTF-16 text (lone surrogates allowed).
    //   returns — the cleaned text and Claude's own counters; removedTotal == 0 is the "an Enter here
    //             would submit" signal.
    //   throws  — std::bad_alloc when the output string cannot be allocated; nothing else (ICU is a C
    //             API that reports through UErrorCode, and a failed property lookup reads as "no
    //             match", which only ever makes a conditional candidate REMOVED — never a crash).
    ClaudeInputCleanup CleanClaudeInputText(std::wstring_view text);

    // The text to PASTE into Claude's input box: the FIXED POINT of "NFC, then CleanClaudeInputText".
    // NFC because claude's editor normalizes whatever it holds (so that is the form its Enter-time
    // cleanup actually sees); repeated because one pass is not always stable — a removal can leave
    // combinable characters adjacent, the editor composes them, and a CGJ kept beside the old mark is
    // then removable (rare: 8 of 50,099 fuzzed strings, at most 2 extra rounds). The result is NFC and
    // clean, so the Enter that follows finds nothing to remove and submits — the invariant the whole
    // delivery pipeline relies on. Idempotent (a second call returns its input unchanged).
    //   text    — the prompt/draft as queued or read (any line endings; lone surrogates allowed).
    //   returns — the sanitized text; pure ASCII (+ TAB/LF) comes back unchanged via the fast path.
    //   throws  — std::bad_alloc on allocation failure only. An ICU normalization failure falls back
    //             to cleaning the un-normalized text (still cancel-free for everything but a
    //             normalization-dependent edge, which the Enter-retry rescue then covers).
    std::wstring SanitizeForClaudeInput(std::wstring_view text);

    // The COMPARE KEY for prompt text: CRLF/CR and every other line break -> LF, every invisible
    // candidate DROPPED (whether Claude keeps it or not — one side of a comparison may carry it and
    // the other not: our sanitized paste, claude's cleaned submit, the terminal's render), lone
    // surrogates -> U+FFFD, then NFC. Two texts that differ only in invisible characters, line-break
    // flavor or canonical composition/order produce the same key. Deliberately NOT trimmed — callers
    // that want the trailing-whitespace tolerance use FoldTrimPromptText (SessionModels.h).
    //   text    — any prompt-like text (a queued prompt, a hook/transcript echo, a box read-back).
    //   returns — the normalized key; printable-ASCII-only text (+ TAB/LF) comes back unchanged.
    //   throws  — std::bad_alloc on allocation failure only; an ICU failure skips the NFC step (the
    //             comparison then degrades to the pre-NFC fold, never to an exception).
    std::wstring NormalizePromptTextForCompare(std::wstring_view text);

    // How many EDITOR CHARACTERS `text` is to claude — extended grapheme clusters (UAX #29, ICU's
    // character break iterator; claude's editor steps and deletes by Intl.Segmenter graphemes over its
    // NFC text). One Backspace removes one cluster: a Hebrew letter with its niqqud, a whole emoji ZWJ
    // family, a flag. The Foreign-undo budget counts with this so it can never press more Backspaces
    // than the characters we typed — over-pressing would eat the user's own text before ours.
    //   text    — the text as pasted (pass PromptFillBody's output, not the queued prompt).
    //   returns — the cluster count; on an ICU failure, the CODE-POINT count (never below the true
    //             cluster count — still no worse than the historical UTF-16-unit budget).
    //   throws  — never in practice (noexcept is not declared only because the fallback walks a
    //             std::wstring_view; no allocation happens on any path).
    size_t CountGraphemeClusters(std::wstring_view text);

    // ===== AskUserQuestion's option PREVIEWS (doc/agentmaster/INTERACTIVE_TOOLS.md §1.2) =====
    // Claude Code 2.1.291 maps every option's `preview` before its question dialog shows it (`G5`), and the PC dialog echoes the
    // picked option's mapped preview back to the model as `annotations[question].preview` (`nJ`). An answer composed elsewhere
    // (Agentmaster's Telegram question card) must apply the SAME two rules, or the model reads a different text than a PC answer
    // gives it — and a preview the dialog withholds must not be echoed at all.

    /// <summary>The longest preview the dialog shows, in UTF-16 units (JavaScript's String.length): `FXe` = 2,000. A longer one is withheld.</summary>
    inline constexpr size_t kClaudeQuestionPreviewMaxChars = 2000;

    /// <summary>How the question dialog treats one option's preview (2.1.291's `G5`).</summary>
    enum class ClaudePreviewKind
    {
        /// <summary>No preview: the option has none, or nothing in it is visible (only whitespace and invisible characters).</summary>
        None,
        /// <summary>
        /// Withheld: longer than kClaudeQuestionPreviewMaxChars, or holding Claude Code's own truncation marker ("… [+N
        /// graphemes]") or a run of more than 8 stacked combining / zero-width marks. The dialog shows "(preview cannot be shown
        /// in full — compare the option labels and descriptions instead)", and a pick of the option echoes NO preview.
        /// </summary>
        Withheld,
        /// <summary>Full: shown, and echoed (as ClaudeQuestionPreviewEcho) when the option is picked.</summary>
        Full,
    };

    /// <summary>
    /// The kind of one option's preview, exactly as the question dialog decides it (`G5`: the length test, `zz`'s visible-text
    /// test, `e0`'s truncation-marker and long-mark-run tests). Ordinary previews — ASCII mockups, box drawing, code — are Full.
    /// </summary>
    /// <param name="preview">The option's `preview` as the model wrote it (an empty string is None).</param>
    /// <returns>None, Withheld or Full.</returns>
    /// <exception cref="std::bad_alloc">If the scratch copy cannot be allocated.</exception>
    ClaudePreviewKind ClassifyClaudeQuestionPreview(std::wstring_view preview);

    /// <summary>
    /// A Full preview as the PC dialog echoes it into `annotations[question].preview` (`zet` = `Nl`): every invisible or unsafe
    /// character (a default-ignorable code point, a format character, LS / PS, a surrogate half, U+10EEEE) becomes U+FFFD except
    /// ZWNJ, ZWJ, LRM, RLM, ALM, the variation selectors, the Mongolian selectors and U+2800; then VS15 / VS16 are DROPPED, every
    /// control character but TAB and LF (C0, DEL, C1 — a CR included), ALM and the bidi embeddings and isolates become U+FFFD, and
    /// TABs expand to the next multiple of 8 display columns. Plain text comes back unchanged: on this machine 101 of 101 echoes
    /// of picked previews were byte-identical to the model's preview.
    /// </summary>
    /// <remarks>
    /// Not ported: the per-line grapheme-width repair (`Ba`), which replaces a grapheme cluster whose terminal width is not
    /// additive with U+FFFD — a few exotic emoji and combining sequences only. The over-budget line truncation (`wR`) cannot
    /// apply: it starts beyond 4,096 characters a line, and a Full preview is at most 2,000. Call it for a Full preview only (a
    /// Withheld one is never echoed).
    /// </remarks>
    /// <param name="preview">The option's preview as the model wrote it.</param>
    /// <returns>The echoed text.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    std::wstring ClaudeQuestionPreviewEcho(std::wstring_view preview);

    /// <summary>
    /// How many terminal columns one code point takes in Claude Code's TUI and in Agentmaster's drawn previews: 0 for a combining
    /// mark, a format character and a default-ignorable code point (they ride on the cell before them), 2 for an East Asian Wide
    /// or Fullwidth character (CJK, most emoji), 1 for everything else. An approximation of the TUI's string width per code point
    /// (it measures grapheme clusters), exact for every character an ASCII or box-drawing mockup holds.
    /// </summary>
    /// <param name="cp">The code point (a lone surrogate counts 1, as the U+FFFD it is drawn as).</param>
    /// <returns>0, 1 or 2.</returns>
    int ClaudeCellWidth(char32_t cp) noexcept;
}
