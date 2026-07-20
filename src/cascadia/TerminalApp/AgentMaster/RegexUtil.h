// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — RegexUtil: the ONE reusable, GUARDED wide-regex component (COMMANDS.md §6b).
//
// Everywhere Agentmaster lets the USER type a regular expression (the Commands tab's successor
// TITLE rewrite and HANDOVER file-match today; any future pattern box), the consumers share these
// helpers instead of touching std::wregex directly, because raw std::wregex is the wrong shape for
// user input three ways:
//   * CONSTRUCTION THROWS on an invalid pattern — and a pattern being edited is invalid on most
//     keystrokes. These helpers never throw: invalid reads as "no match" / "input unchanged", and
//     RegexIsValid is the cog's live-validation probe.
//   * UNBOUNDED COST — a pathological pattern over a long input can backtrack for seconds. Inputs
//     and patterns are length-capped here (titles and file leaves are tiny; anything past the caps
//     is not a use case, it's a hazard), so a hostile settings.json can't wedge a scanner pass.
//   * SCATTERED SEMANTICS — one flavor everywhere: ECMAScript, regex_search semantics (a pattern
//     matches anywhere unless anchored with ^/$), optional case-insensitivity, $1-style backrefs
//     in replacements, replace-ALL-occurrences.
//
// Header-only + pure (the PromptAnchor.h idiom) so the engine (CommandWatch), the UI layer
// (TerminalPage / the Settings cog), and the standalone harness share one definition. Plain C++,
// no WinRT, no I/O.
//
// Rule #18 note: the catch blocks below are the documented "expected control flow" exemption —
// an invalid user pattern is the NORMAL state mid-edit (the cog probes per keystroke), so logging
// each would flood hooks.log with non-events. The cog surfaces invalidity to the user instead.

#pragma once

#include <regex>
#include <string>
#include <string_view>

namespace Agentmaster
{
    // Bounds (explicit + static_assert-able, the CommandWatch tunables idiom). A pattern past
    // kRegexMaxPatternChars or an input past kRegexMaxInputChars is refused outright (no match /
    // unchanged) — real titles and file leaves are tens of chars; the caps exist so a hand-edited
    // settings.json or an adversarial transcript field can never buy unbounded backtracking.
    inline constexpr size_t kRegexMaxPatternChars = 512;
    inline constexpr size_t kRegexMaxInputChars = 4096;

    namespace detail
    {
        // Compile `pattern` into `out`. False on empty / over-cap / syntactically invalid.
        inline bool BuildRegex(std::wstring_view pattern, bool caseInsensitive, std::wregex& out)
        {
            if (pattern.empty() || pattern.size() > kRegexMaxPatternChars)
            {
                return false;
            }
            try
            {
                auto flags = std::regex_constants::ECMAScript;
                if (caseInsensitive)
                {
                    flags |= std::regex_constants::icase;
                }
                out.assign(pattern.begin(), pattern.end(), flags);
                return true;
            }
            catch (...)
            {
                // Expected control flow (Rule #18 exemption, see header): a user pattern mid-edit
                // is invalid most keystrokes — the cog's live validation is the reporting channel.
                return false;
            }
        }
    }

    // Is `pattern` a compilable, in-bounds regex? The cog's live-validation probe (and the
    // consumers' "configured but broken -> fall back" test). Empty => false (nothing configured).
    inline bool RegexIsValid(std::wstring_view pattern)
    {
        std::wregex re;
        return detail::BuildRegex(pattern, /*caseInsensitive*/ false, re);
    }

    // Does `pattern` match anywhere in `text` (regex_search semantics — anchor with ^/$ for a
    // whole-string match)? False on an empty/invalid/over-cap pattern or an over-cap input —
    // callers that need "invalid pattern" told apart from "no match" probe RegexIsValid.
    inline bool RegexSearch(std::wstring_view text, std::wstring_view pattern, bool caseInsensitive = false)
    {
        if (text.size() > kRegexMaxInputChars)
        {
            return false;
        }
        std::wregex re;
        if (!detail::BuildRegex(pattern, caseInsensitive, re))
        {
            return false;
        }
        try
        {
            return std::regex_search(text.begin(), text.end(), re);
        }
        catch (...)
        {
            // Expected control flow (Rule #18 exemption): regex_error{error_complexity/error_stack}
            // on a pathological pattern — read as "no match", never unwind into a scanner pass.
            return false;
        }
    }

    // Replace EVERY occurrence of `pattern` in `text` with `replacement` ($1-style backrefs
    // honored — ECMAScript regex_replace). Returns the rewritten text; on an empty/invalid/
    // over-cap pattern, an over-cap input, or NO match, returns `text` unchanged. `applied`
    // (optional) reports whether a replacement actually happened — the "configured and it DID
    // something" signal (the title-rewrite fallback keys on it: no match => default naming).
    inline std::wstring RegexReplace(std::wstring_view text, std::wstring_view pattern, std::wstring_view replacement, bool caseInsensitive = false, bool* applied = nullptr)
    {
        if (applied)
        {
            *applied = false;
        }
        std::wstring in{ text };
        if (in.size() > kRegexMaxInputChars)
        {
            return in;
        }
        std::wregex re;
        if (!detail::BuildRegex(pattern, caseInsensitive, re))
        {
            return in;
        }
        try
        {
            if (!std::regex_search(in, re))
            {
                return in; // no match — unchanged, applied stays false
            }
            std::wstring out = std::regex_replace(in, re, std::wstring{ replacement });
            if (applied)
            {
                *applied = true;
            }
            return out;
        }
        catch (...)
        {
            // Expected control flow (Rule #18 exemption): a pathological pattern/replacement —
            // degrade to "unchanged" rather than unwind (the caller's fallback naming applies).
            return in;
        }
    }
}
