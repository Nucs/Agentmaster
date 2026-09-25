// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the DRAFT CARRY -- copy an UNSENT input-box draft into another claude session's box
// with every "[Pasted text #N +M lines]" placeholder backed by its REAL pasted content
// (PENDING_INPUT.md section 11 -- the fork carry -- and section 2c -- the paste journal).
//
// WHY THIS EXISTS. Claude Code renders a large paste in its input box as a one-line PLACEHOLDER and
// keeps the content only in its own process memory until the prompt is SUBMITTED -- the
// paste-cache/<hash>.txt files the section-2b resolver reads are written at SUBMIT time (measured
// 2026-09-25 on 2.1.280: every cache file's mtime equals its history.jsonl submit timestamp to the
// second; today's unsent drafts had no file at all). So an unsent draft's paste is invisible on disk,
// and re-typing its placeholder into another box submits the literal label with the content lost.
//
// THE FIX: Agentmaster IS the terminal, so every paste passes through it on its way to claude. The
// PASTE JOURNAL records what was pasted into each session (the clipboard handler + our own box fills),
// normalized EXACTLY as claude stores it, and this header resolves a draft's placeholders against it
// (then against the paste-cache as the fallback), and plans a PIECEWISE fill that reproduces the
// source box's rendering in the target box -- typed text inline, each pasted block as its own paste.
//
// Claude's own rules this mirrors (read from the 2.1.280 bundle, the prompt input's paste handler):
//   * normalize:  Bun.stripANSI(text).replace(/\r\n|\r/g,"\n").replaceAll("\t","    ")
//   * count:      M = (text.match(/\r\n|\r|\n/g) || []).length          -- the NEWLINE count
//   * collapse:   text.length > 800 || M > min(rows-10, 2)                -- 2 on any usable terminal
//   * render:     M == 0 ? "[Pasted text #N]" : "[Pasted text #N +M lines]"
//   * truncate:   an INPUT over 10,000 chars keeps its first/last 500 chars around
//                 "[...Truncated text #N +M lines...]" (M = newlines in the elided middle)
//   * ids:        N is minted past every id already referenced in the input, so ids grow with time
//                 within one draft (a typed QUOTE of a placeholder therefore never shares a real id).
//
// PURE + HEADER-ONLY (the PendingPaste.h idiom): no filesystem, no WinRT; the impure adapter
// (ClaudeSpawn.cpp: ResolvePendingDraftCarry) reads the paste-cache only when the journal is not
// enough. Pure-ASCII source: non-ASCII code points appear only as numeric constants (never \u text).

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "PendingPaste.h" // PasteMarker / PasteFileText / ValidatePasteFile / paste_detail::SplitSegments
#include "SessionModels.h" // FoldTrimPromptText + verify_detail::WsStripped -- the compare key every fill verify shares

namespace Agentmaster
{
    // ---- The PASTE JOURNAL entry ------------------------------------------------------------------

    // One paste Agentmaster itself handed a claude session (a user clipboard paste, or one of our own
    // box fills), recorded by SessionRegistry::NotePastedText. The journal is what lets a draft's
    // placeholder be resolved at all on current Claude Code (see the header comment); it is TRANSIENT
    // (process memory only, never persisted) and bounded (the caps below), so an app restart forgets it
    // and a restored draft falls back to the paste-cache tier -- exactly the pre-journal behavior.
    struct PasteJournalEntry
    {
        std::wstring text; // the paste as CLAUDE stores it (NormalizePastedTextLikeClaude) -- what a placeholder expands to
        int64_t unixMs{ 0 }; // when it was pasted (system clock); journal order is arrival order, this is for diagnostics/eviction
    };

    // Claude's inline-vs-collapse threshold, CHARACTERS (JS UTF-16 length == wchar_t count). A paste
    // longer than this is always collapsed into a placeholder. A piecewise fill keeps every TYPED
    // chunk at or below it so claude inserts the chunk as visible text, never as a placeholder.
    inline constexpr size_t kClaudePasteInlineMaxChars = 800;
    // Claude's inline-vs-collapse threshold, NEWLINES: min(terminal rows - 10, 2). A Claude tab shorter
    // than 12 rows would collapse even a 2-newline chunk -- not a realistic working setup, and the
    // failure is cosmetic (the content still lands, just collapsed), so the common value is used.
    inline constexpr size_t kClaudePasteInlineMaxNewlines = 2;

    // Journal caps. A session keeps its most recent pastes only (placeholders resolve against recent
    // pastes -- the journal is pruned when the draft clears, so older entries are dead weight anyway);
    // one pathological paste can never pin memory (an entry over the per-entry cap is not recorded);
    // and the whole process stays bounded however many sessions paste (the global cap evicts the
    // oldest entries fleet-wide). Numbers are wchar_t counts.
    inline constexpr size_t kPasteJournalMaxEntriesPerSession = 16;
    inline constexpr size_t kPasteJournalMaxEntryChars = 4u * 1024u * 1024u;
    inline constexpr size_t kPasteJournalMaxSessionChars = 16u * 1024u * 1024u;
    inline constexpr size_t kPasteJournalMaxTotalChars = 64u * 1024u * 1024u;

    // Normalize a pasted text EXACTLY as it ends up in claude's pastedContents: Windows Terminal's
    // paste filter first (FilterStringForPaste with CarriageReturnNewline|ControlCodes -- CRLF and a
    // lone LF become CR, every C0/C1 control except TAB/CR/LF is dropped), then claude's paste handler
    // (stripANSI -- a no-op once ESC is gone -- CRLF/CR -> LF, TAB -> four spaces). Journal entries are
    // stored in this form so a placeholder's "+M lines" and the entry's newline count agree by
    // construction, and so the expansion is byte-identical to what claude itself would submit.
    //   raw     -- the text as it was pasted (the clipboard text, or the body of one of our fills).
    //   returns -- the normalized text ("" for an all-control input).
    //   throws  -- std::bad_alloc on allocation failure only.
    inline std::wstring NormalizePastedTextLikeClaude(std::wstring_view raw)
    {
        std::wstring out;
        out.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i)
        {
            const wchar_t c = raw[i];
            if (c == L'\r')
            {
                out.push_back(L'\n');
                if (i + 1 < raw.size() && raw[i + 1] == L'\n')
                {
                    ++i; // CRLF -> one LF (WT folds it to CR, claude folds that CR to LF)
                }
                continue;
            }
            if (c == L'\n')
            {
                out.push_back(L'\n');
                continue;
            }
            if (c == L'\t')
            {
                out.append(4, L' '); // claude's replaceAll("\t", "    ")
                continue;
            }
            if (c < 0x20 || (c >= 0x7F && c <= 0x9F))
            {
                continue; // WT's ControlCodes filter: C0 + DEL + C1 never reach claude
            }
            out.push_back(c);
        }
        return out;
    }

    // Claude's "+M lines" arithmetic: the number of newline SEQUENCES (CRLF counts once, a lone CR or
    // LF counts once) -- claude's own gM(). On a normalized text this is simply the LF count.
    //   text    -- any text (normalized or raw).
    //   returns -- the newline-sequence count.
    //   throws  -- nothing.
    inline size_t ClaudePasteNewlineCount(std::wstring_view text) noexcept
    {
        size_t n = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == L'\r')
            {
                ++n;
                if (i + 1 < text.size() && text[i + 1] == L'\n')
                {
                    ++i;
                }
            }
            else if (text[i] == L'\n')
            {
                ++n;
            }
        }
        return n;
    }

    // Would claude collapse this (already normalized) paste into a placeholder rather than insert it
    // as visible text? Only such pastes can ever back a placeholder, so the journal records only these
    // and the resolver only matches against these (a small inline paste can never be what a
    // "[Pasted text ...]" stands for).
    //   normalized -- a NormalizePastedTextLikeClaude output.
    //   returns    -- true when claude renders it as a placeholder.
    //   throws     -- nothing.
    inline bool ClaudeWouldCollapsePaste(std::wstring_view normalized) noexcept
    {
        return normalized.size() > kClaudePasteInlineMaxChars || ClaudePasteNewlineCount(normalized) > kClaudePasteInlineMaxNewlines;
    }

    // ---- Placeholders (wrap-tolerant) -------------------------------------------------------------

    // Which of claude's placeholder forms a draft span is.
    enum class DraftPlaceholderKind : uint8_t
    {
        PastedText, // "[Pasted text #N]" / "[Pasted text #N +M lines]" -- a collapsed paste
        TruncatedText, // "[...Truncated text #N +M lines...]" -- the elided middle of an over-long input
        Image, // "[Image #N]" -- a pasted image; its bytes are not carryable, so it REFUSES a carry
        Audio, // "[Audio #N]" -- same as Image
    };

    // One placeholder found in a draft. Offsets are into the WHOLE draft string (not per line), and the
    // span may contain a soft-wrap newline (the detector joins the input box's visual rows with '\n', so
    // a placeholder the TUI wrapped at one of its internal spaces reads "[Pasted\ntext #1 +31 lines]").
    struct DraftPlaceholder
    {
        DraftPlaceholderKind kind{ DraftPlaceholderKind::PastedText };
        int index{ 0 }; // N -- claude's paste id (grows with time within one draft)
        int lines{ -1 }; // M -- the "+M lines" value; -1 for the bare "[Pasted text #N]" / image / audio forms
        size_t begin{ 0 }; // the '[' offset (inclusive)
        size_t end{ 0 }; // one past the ']' (exclusive)
    };

    namespace draft_carry_detail
    {
        inline constexpr size_t kNpos = static_cast<size_t>(-1);
        // The widest whitespace run one GRAMMAR space may absorb: a soft wrap is "\n" (the wrapped
        // row's trailing space already trimmed) or " \n"; anything wider is not a wrapped placeholder.
        inline constexpr size_t kMaxFlexRun = 4;

        // Consume ONE grammar space at `p`: 1..kMaxFlexRun chars of ' ' / '\n', at most one '\n'.
        //   s, p    -- the text and the position.
        //   returns -- the position after the run, or kNpos when there is no space there.
        //   throws  -- nothing.
        inline size_t MatchFlexSpace(std::wstring_view s, size_t p) noexcept
        {
            if (p == kNpos)
            {
                return kNpos;
            }
            size_t q = p;
            size_t newlines = 0;
            while (q < s.size() && q - p < kMaxFlexRun && (s[q] == L' ' || s[q] == L'\n'))
            {
                if (s[q] == L'\n' && ++newlines > 1)
                {
                    break; // a second newline is a real line break, never one wrapped space
                }
                ++q;
            }
            return q == p ? kNpos : q;
        }

        // Match the grammar literal `lit` at `p`, where every ' ' in `lit` is a flexible space
        // (MatchFlexSpace) and every other character must match exactly.
        //   s, p, lit -- the text, the position, the literal.
        //   returns   -- the position after the literal, or kNpos on a mismatch (or a kNpos input).
        //   throws    -- nothing.
        inline size_t MatchLiteral(std::wstring_view s, size_t p, std::wstring_view lit) noexcept
        {
            for (const wchar_t c : lit)
            {
                if (p == kNpos)
                {
                    return kNpos;
                }
                if (c == L' ')
                {
                    p = MatchFlexSpace(s, p);
                    continue;
                }
                if (p >= s.size() || s[p] != c)
                {
                    return kNpos;
                }
                ++p;
            }
            return p;
        }

        // Match a run of 1..maxDigits ASCII digits at `p` (a longer run is not the grammar).
        //   s, p      -- the text and the position (kNpos passes through).
        //   maxDigits -- the widest accepted run.
        //   value     -- receives the parsed number on success; untouched otherwise.
        //   returns   -- the position after the digits, or kNpos.
        //   throws    -- nothing.
        inline size_t MatchDigits(std::wstring_view s, size_t p, size_t maxDigits, int& value) noexcept
        {
            if (p == kNpos)
            {
                return kNpos;
            }
            size_t q = p;
            long long v = 0;
            while (q < s.size() && s[q] >= L'0' && s[q] <= L'9' && q - p < maxDigits)
            {
                v = v * 10 + (s[q] - L'0');
                ++q;
            }
            if (q == p || (q < s.size() && s[q] >= L'0' && s[q] <= L'9'))
            {
                return kNpos;
            }
            value = static_cast<int>(v);
            return q;
        }

        // The " +M lines" / " +M line" tail shared by both counted forms.
        //   s, p  -- the text and the position right after N's digits.
        //   lines -- receives M.
        //   returns -- the position right after "line"/"lines", or kNpos.
        //   throws  -- nothing.
        inline size_t MatchLinesSuffix(std::wstring_view s, size_t p, int& lines) noexcept
        {
            p = MatchLiteral(s, p, L" +");
            p = MatchDigits(s, p, 8, lines);
            p = MatchLiteral(s, p, L" line");
            if (p != kNpos && p < s.size() && s[p] == L's')
            {
                ++p;
            }
            return p;
        }

        // Is `c` a UTF-16 low surrogate (the second half of an astral code point)?
        //   c       -- the code unit.
        //   returns -- true for 0xDC00..0xDFFF.
        //   throws  -- nothing.
        inline constexpr bool IsLowSurrogate(wchar_t c) noexcept
        {
            return c >= 0xDC00 && c <= 0xDFFF;
        }
    }

    // Parse ONE placeholder at `pos` (which must hold '['), tolerating a soft-wrap newline wherever the
    // grammar has a space. Accepted: "[Pasted text #N]", "[Pasted text #N +M lines]" (or "line"),
    // "[...Truncated text #N +M lines...]", "[Image #N]", "[Audio #N]" -- claude's own placeholder set.
    //   s   -- the draft.
    //   pos -- the candidate '[' offset.
    //   out -- receives the placeholder on success; untouched otherwise.
    //   returns -- true on an exact grammar match.
    //   throws  -- nothing.
    inline bool ParseDraftPlaceholderAt(std::wstring_view s, size_t pos, DraftPlaceholder& out) noexcept
    {
        using namespace draft_carry_detail;
        if (pos >= s.size() || s[pos] != L'[')
        {
            return false;
        }
        const size_t p0 = pos + 1;
        int idx = 0;
        int lines = -1;
        if (size_t p = MatchLiteral(s, p0, L"Pasted text #"); p != kNpos)
        {
            p = MatchDigits(s, p, 7, idx);
            if (p == kNpos)
            {
                return false;
            }
            if (p < s.size() && s[p] == L']')
            {
                out = DraftPlaceholder{ DraftPlaceholderKind::PastedText, idx, -1, pos, p + 1 };
                return true;
            }
            p = MatchLinesSuffix(s, p, lines);
            if (p == kNpos || p >= s.size() || s[p] != L']')
            {
                return false;
            }
            out = DraftPlaceholder{ DraftPlaceholderKind::PastedText, idx, lines, pos, p + 1 };
            return true;
        }
        if (size_t p = MatchLiteral(s, p0, L"...Truncated text #"); p != kNpos)
        {
            p = MatchDigits(s, p, 7, idx);
            p = MatchLinesSuffix(s, p, lines);
            p = MatchLiteral(s, p, L"...]");
            if (p == kNpos)
            {
                return false;
            }
            out = DraftPlaceholder{ DraftPlaceholderKind::TruncatedText, idx, lines, pos, p };
            return true;
        }
        const auto attachment = [&](std::wstring_view word, DraftPlaceholderKind kind) -> bool {
            size_t p = MatchLiteral(s, p0, word);
            p = MatchDigits(s, p, 7, idx);
            if (p == kNpos || p >= s.size() || s[p] != L']')
            {
                return false;
            }
            out = DraftPlaceholder{ kind, idx, -1, pos, p + 1 };
            return true;
        };
        return attachment(L"Image #", DraftPlaceholderKind::Image) || attachment(L"Audio #", DraftPlaceholderKind::Audio);
    }

    // Every placeholder in a draft, in text order (non-overlapping).
    //   draft   -- the draft ('\n'-joined, as DetectPendingInput emits it; any other text works too).
    //   returns -- the placeholders found (empty for a placeholder-free text).
    //   throws  -- std::bad_alloc on allocation failure only.
    inline std::vector<DraftPlaceholder> FindDraftPlaceholders(std::wstring_view draft)
    {
        std::vector<DraftPlaceholder> out;
        for (size_t i = 0; i < draft.size(); ++i)
        {
            if (draft[i] != L'[')
            {
                continue;
            }
            DraftPlaceholder ph;
            if (ParseDraftPlaceholderAt(draft, i, ph))
            {
                out.push_back(ph);
                i = ph.end - 1; // resume after the span (the loop's ++i steps past its ']')
            }
        }
        return out;
    }

    // Is the placeholder the only visible content of its line? The resolver's evidence that a span is
    // a REAL paste rather than typed text QUOTING one: claude inserts a paste at the cursor, and every
    // quote observed sits mid-sentence. Used two ways: the paste-cache tier (a line-count match is weak,
    // so it is trusted only for a whole-line placeholder -- the section-2b rule), and the carry's
    // unresolved-placeholder policy (whole-line refuses, mid-line may be kept as literal text).
    //   s, ph   -- the draft and one of its placeholders.
    //   returns -- true when everything else on the placeholder's line is whitespace.
    //   throws  -- nothing.
    inline bool DraftPlaceholderIsWholeLine(std::wstring_view s, const DraftPlaceholder& ph) noexcept
    {
        for (size_t a = ph.begin; a > 0 && s[a - 1] != L'\n';)
        {
            --a;
            if (!pending_detail::IsWs(s[a]))
            {
                return false;
            }
        }
        for (size_t b = ph.end; b < s.size() && s[b] != L'\n'; ++b)
        {
            if (!pending_detail::IsWs(s[b]))
            {
                return false;
            }
        }
        return true;
    }

    // The placeholder's text with every flexible-space run (a soft wrap included) collapsed back to the
    // single space claude rendered -- what a placeholder kept as LITERAL typed text is re-typed as.
    //   s, ph   -- the draft and one of its placeholders.
    //   returns -- e.g. "[Pasted text #1 +31 lines]" for a span that read "[Pasted\ntext #1 +31 lines]".
    //   throws  -- std::bad_alloc on allocation failure only.
    inline std::wstring CanonicalDraftPlaceholderText(std::wstring_view s, const DraftPlaceholder& ph)
    {
        std::wstring out;
        bool inRun = false;
        for (size_t i = ph.begin; i < ph.end && i < s.size(); ++i)
        {
            const wchar_t c = s[i];
            if (c == L' ' || c == L'\n')
            {
                if (!inRun)
                {
                    out.push_back(L' ');
                }
                inRun = true;
                continue;
            }
            out.push_back(c);
            inRun = false;
        }
        return out;
    }

    // ---- Resolution --------------------------------------------------------------------------------

    // One piece of a resolved draft, in draft order.
    enum class DraftSegmentKind : uint8_t
    {
        Typed, // visible text (what the user typed, plus any placeholder KEPT as literal text)
        Block, // a placeholder's real pasted content
    };

    // Where a Block's content came from -- the honesty tier the logs report.
    enum class DraftBlockSource : uint8_t
    {
        None, // a Typed segment
        Journal, // this session's paste journal (a paste Agentmaster itself delivered -- strong)
        Cache, // Claude's paste-cache (a previously SUBMITTED identical paste -- line-count or anchor matched)
    };

    struct DraftSegment
    {
        DraftSegmentKind kind{ DraftSegmentKind::Typed };
        std::wstring text; // Typed: the visible text; Block: the pasted content (normalized like claude)
        DraftBlockSource source{ DraftBlockSource::None }; // Block only
    };

    // The resolver's verdict. complete == true means `segments` reproduces the whole draft with every
    // pasted block backed by real content; complete == false means the carry is REFUSED as a whole
    // (segments empty, `refusal` says why) -- the caller must NOT re-type the draft, because a literal
    // placeholder label would submit as junk text with the pasted content silently lost.
    struct DraftCarryPlan
    {
        bool complete{ false };
        std::vector<DraftSegment> segments;
        int placeholders{ 0 }; // placeholders found in the draft (diagnostic)
        int fromJournal{ 0 }; // blocks resolved from the paste journal
        int fromCache{ 0 }; // blocks resolved from the paste-cache
        int keptLiteral{ 0 }; // mid-line placeholders kept as literal typed text (a quote -- see ResolveDraftCarry)
        // A refusal the paste-cache could still fix (some placeholder is unresolved and the cache was
        // not consulted) -- the impure adapter's cue to read the cache and resolve once more.
        bool cacheMightHelp{ false };
        std::wstring refusal; // why complete == false ("" when complete)
    };

    namespace draft_carry_detail
    {
        // The section-2b marker for a TRUNCATED placeholder: its head/tail fragments are the visible text
        // on the placeholder's line before/after the span (the content anchors ValidatePasteFile proves).
        //   draft, ph -- the draft and a TruncatedText placeholder.
        //   returns   -- the PasteMarker ValidatePasteFile consumes.
        //   throws    -- std::bad_alloc on allocation failure only.
        inline PasteMarker TruncatedMarkerFor(std::wstring_view draft, const DraftPlaceholder& ph)
        {
            PasteMarker m;
            m.truncated = true;
            m.index = ph.index;
            m.lines = ph.lines;
            size_t a = ph.begin;
            while (a > 0 && draft[a - 1] != L'\n')
            {
                --a;
            }
            size_t b = ph.end;
            while (b < draft.size() && draft[b] != L'\n')
            {
                ++b;
            }
            m.headFragment.assign(draft.substr(a, ph.begin - a));
            m.tailFragment.assign(draft.substr(ph.end, b - ph.end));
            return m;
        }

        // The ELIDED MIDDLE a truncated placeholder stands for, cut out of `text` (a journal entry or a
        // cache file holding the whole paste): segment i minus the head fragment, the segments between,
        // and segment i+M minus the tail fragment -- exactly M newlines, which is what claude counted.
        // The visible head/tail stay in the surrounding Typed segments, so head + middle + tail is the
        // original text.
        //   m      -- the truncated marker (TruncatedMarkerFor).
        //   text   -- the candidate full paste.
        //   middle -- receives the middle on success.
        //   returns -- true when the content anchors + the line arithmetic validate (ValidatePasteFile).
        //   throws  -- std::bad_alloc on allocation failure only.
        inline bool ExtractTruncatedMiddle(const PasteMarker& m, std::wstring_view text, std::wstring& middle)
        {
            size_t headSeg = 0;
            if (!ValidatePasteFile(m, text, &headSeg) || headSeg == 0)
            {
                return false;
            }
            const auto segs = paste_detail::SplitSegments(text);
            const size_t i = headSeg - 1;
            const size_t j = i + static_cast<size_t>(m.lines);
            if (j >= segs.size() || segs[i].size() < m.headFragment.size() || segs[j].size() < m.tailFragment.size())
            {
                return false;
            }
            std::wstring out = segs[i].substr(m.headFragment.size());
            for (size_t k = i + 1; k <= j; ++k)
            {
                out.push_back(L'\n');
                out += (k == j) ? segs[j].substr(0, segs[j].size() - m.tailFragment.size()) : segs[k];
            }
            middle = std::move(out);
            return true;
        }

        // The collapsed form's comparable count: a bare "[Pasted text #N]" is a zero-newline paste.
        //   ph      -- a PastedText placeholder.
        //   returns -- M, or 0 for the bare form.
        //   throws  -- nothing.
        inline size_t CollapsedNewlines(const DraftPlaceholder& ph) noexcept
        {
            return ph.lines < 0 ? 0u : static_cast<size_t>(ph.lines);
        }
    }

    // Resolve a draft's placeholders to real content and split it into Typed/Block segments.
    //
    // Tiers, strongest first:
    //   1. the session's PASTE JOURNAL. Collapsed placeholders are grouped by M; within a group the
    //      placeholders ordered by claude's id N (ids grow with time) take the most recent same-M
    //      journal entries in time order -- so "#1 +31" and "#2 +31" get the older and the newer
    //      31-line paste respectively. A truncated placeholder takes the newest entry its content
    //      anchors validate against.
    //   2. the PASTE-CACHE (previously submitted identical content): a collapsed placeholder only when
    //      it is WHOLE-LINE and exactly one cache file has that line count (the section-2b rule); a
    //      truncated one by its anchors, again uniquely.
    //   3. otherwise UNRESOLVED: when `unresolvedMidLineIsLiteral` and the placeholder is a mid-line
    //      "[Pasted text ...]", it is kept as LITERAL typed text (a quote -- "make sure the [Pasted text
    //      #1 +31 lines] are expanded" is prose, not a paste); anything else refuses the whole carry.
    //
    // Always refused: an image/audio placeholder (the bytes can't be re-attached), and two placeholders
    // sharing one paste id (claude never mints a duplicate, so one of them is a quote and nothing tells
    // which).
    //
    //   draft      -- the rendered draft (the source box as DetectPendingInput read it).
    //   journal    -- the source session's paste journal, oldest first (SessionRegistry::PasteJournal).
    //   cacheFiles -- the paste-cache files (ReadPasteCacheFileTexts), or {} to try the journal alone.
    //   unresolvedMidLineIsLiteral -- the tier-3 policy: true for a LIVE carry (a fork -- the journal
    //                  covers every real clipboard paste, so an unresolved mid-line placeholder is a
    //                  quote); false for a restore across an app restart (the journal is gone, so an
    //                  unresolved placeholder is most likely a real paste whose content would be lost).
    //   returns    -- the plan (complete, or refused with the reason + cacheMightHelp).
    //   throws     -- std::bad_alloc on allocation failure only.
    inline DraftCarryPlan ResolveDraftCarry(std::wstring_view draft,
                                            const std::vector<PasteJournalEntry>& journal,
                                            const std::vector<PasteFileText>& cacheFiles,
                                            bool unresolvedMidLineIsLiteral)
    {
        using namespace draft_carry_detail;
        DraftCarryPlan plan;
        const auto phs = FindDraftPlaceholders(draft);
        plan.placeholders = static_cast<int>(phs.size());
        const size_t n = phs.size();

        for (const auto& ph : phs)
        {
            if (ph.kind == DraftPlaceholderKind::Image || ph.kind == DraftPlaceholderKind::Audio)
            {
                plan.refusal = L"the draft holds an image/audio attachment ([Image #" + std::to_wstring(ph.index) + L"]-style) that cannot be re-attached";
                return plan;
            }
        }
        for (size_t i = 0; i < n; ++i)
        {
            for (size_t j = i + 1; j < n; ++j)
            {
                if (phs[i].index == phs[j].index)
                {
                    plan.refusal = L"two placeholders share paste id #" + std::to_wstring(phs[i].index) + L" (one is typed text quoting the other) - cannot tell which is the real paste";
                    return plan;
                }
            }
        }

        std::vector<std::wstring> content(n);
        std::vector<DraftBlockSource> source(n, DraftBlockSource::None);
        std::vector<bool> literal(n, false);

        // Tier 1a -- collapsed placeholders vs the journal, grouped by M, assigned chronologically.
        std::map<size_t, std::vector<size_t>> collapsedByLines;
        for (size_t i = 0; i < n; ++i)
        {
            if (phs[i].kind == DraftPlaceholderKind::PastedText)
            {
                collapsedByLines[CollapsedNewlines(phs[i])].push_back(i);
            }
        }
        for (auto& [lines, group] : collapsedByLines)
        {
            std::stable_sort(group.begin(), group.end(), [&](size_t a, size_t b) { return phs[a].index < phs[b].index; });
            std::vector<size_t> candidates; // journal indices, oldest first
            for (size_t j = 0; j < journal.size(); ++j)
            {
                if (ClaudePasteNewlineCount(journal[j].text) == lines && ClaudeWouldCollapsePaste(journal[j].text))
                {
                    candidates.push_back(j);
                }
            }
            const size_t take = (std::min)(group.size(), candidates.size());
            for (size_t t = 0; t < take; ++t)
            {
                // The newest `take` candidates go to the highest-id `take` placeholders, both ascending.
                const size_t ph = group[group.size() - take + t];
                content[ph] = journal[candidates[candidates.size() - take + t]].text;
                source[ph] = DraftBlockSource::Journal;
            }
        }

        // Tier 1b -- truncated placeholders vs the journal, newest validating entry first.
        for (size_t i = 0; i < n; ++i)
        {
            if (phs[i].kind != DraftPlaceholderKind::TruncatedText)
            {
                continue;
            }
            const auto marker = TruncatedMarkerFor(draft, phs[i]);
            for (size_t j = journal.size(); j-- > 0;)
            {
                std::wstring middle;
                if (ExtractTruncatedMiddle(marker, journal[j].text, middle))
                {
                    content[i] = std::move(middle);
                    source[i] = DraftBlockSource::Journal;
                    break;
                }
            }
        }

        // Tier 2 -- the paste-cache, UNIQUE matches only (never guess between files).
        if (!cacheFiles.empty())
        {
            for (size_t i = 0; i < n; ++i)
            {
                if (source[i] != DraftBlockSource::None)
                {
                    continue;
                }
                const auto& ph = phs[i];
                const PasteFileText* hit = nullptr;
                size_t hits = 0;
                std::wstring middle;
                if (ph.kind == DraftPlaceholderKind::PastedText)
                {
                    if (ph.lines <= 0 || !DraftPlaceholderIsWholeLine(draft, ph))
                    {
                        continue; // the count tier is weak: whole-line + a positive count only (section 2b)
                    }
                    PasteMarker m;
                    m.truncated = false;
                    m.index = ph.index;
                    m.lines = ph.lines;
                    for (const auto& f : cacheFiles)
                    {
                        if (ValidatePasteFile(m, f.text) && ++hits == 1)
                        {
                            hit = &f;
                        }
                    }
                    if (hits == 1 && hit)
                    {
                        content[i] = NormalizePastedTextLikeClaude(hit->text);
                        source[i] = DraftBlockSource::Cache;
                    }
                }
                else if (ph.kind == DraftPlaceholderKind::TruncatedText)
                {
                    const auto marker = TruncatedMarkerFor(draft, ph);
                    for (const auto& f : cacheFiles)
                    {
                        std::wstring candidate;
                        if (ExtractTruncatedMiddle(marker, f.text, candidate) && ++hits == 1)
                        {
                            middle = std::move(candidate);
                        }
                    }
                    if (hits == 1)
                    {
                        content[i] = NormalizePastedTextLikeClaude(middle);
                        source[i] = DraftBlockSource::Cache;
                    }
                }
            }
        }

        // Tier 3 -- whatever is still unresolved.
        for (size_t i = 0; i < n; ++i)
        {
            if (source[i] != DraftBlockSource::None)
            {
                continue;
            }
            const auto& ph = phs[i];
            if (unresolvedMidLineIsLiteral && ph.kind == DraftPlaceholderKind::PastedText && !DraftPlaceholderIsWholeLine(draft, ph))
            {
                literal[i] = true;
                ++plan.keptLiteral;
                continue;
            }
            const std::wstring what = ph.kind == DraftPlaceholderKind::TruncatedText ? L"[...Truncated text #" : L"[Pasted text #";
            plan.refusal = L"placeholder " + what + std::to_wstring(ph.index) + (ph.lines >= 0 ? (L" +" + std::to_wstring(ph.lines) + L" lines") : std::wstring{}) +
                           (ph.kind == DraftPlaceholderKind::TruncatedText ? L"...]" : L"]") +
                           L" matches no paste in this session's journal" + (cacheFiles.empty() ? L"" : L" or Claude's paste-cache");
            // Only a placeholder tier 2 COULD answer makes a cache read worthwhile: a truncated one (anchors),
            // or a whole-line collapsed one with a count. A mid-line or bare-form placeholder would be refused
            // again after reading up to 64 MiB of cache for nothing.
            const bool cacheCouldAnswer = ph.kind == DraftPlaceholderKind::TruncatedText ||
                                          (ph.kind == DraftPlaceholderKind::PastedText && ph.lines > 0 && DraftPlaceholderIsWholeLine(draft, ph));
            plan.cacheMightHelp = cacheFiles.empty() && cacheCouldAnswer;
            return plan;
        }

        // Build the ordered segments (adjacent Typed runs merged -- a literal placeholder joins its text).
        const auto pushTyped = [&](std::wstring text) {
            if (text.empty())
            {
                return;
            }
            if (!plan.segments.empty() && plan.segments.back().kind == DraftSegmentKind::Typed)
            {
                plan.segments.back().text += text;
                return;
            }
            plan.segments.push_back(DraftSegment{ DraftSegmentKind::Typed, std::move(text), DraftBlockSource::None });
        };
        size_t pos = 0;
        for (size_t i = 0; i < n; ++i)
        {
            pushTyped(std::wstring{ draft.substr(pos, phs[i].begin - pos) });
            if (literal[i])
            {
                pushTyped(CanonicalDraftPlaceholderText(draft, phs[i]));
            }
            else
            {
                plan.segments.push_back(DraftSegment{ DraftSegmentKind::Block, std::move(content[i]), source[i] });
                if (source[i] == DraftBlockSource::Journal)
                {
                    ++plan.fromJournal;
                }
                else
                {
                    ++plan.fromCache;
                }
            }
            pos = phs[i].end;
        }
        pushTyped(std::wstring{ draft.substr(pos) });
        plan.complete = true;
        return plan;
    }

    // The resolved draft as ONE text -- every placeholder replaced by its content (literal ones kept).
    // What a single-paste consumer (the section-10 re-fill, a copy, the section-8c store) uses.
    //   plan    -- a COMPLETE plan (a refused plan yields "").
    //   returns -- the concatenated segment texts.
    //   throws  -- std::bad_alloc on allocation failure only.
    inline std::wstring DraftCarryExpandedText(const DraftCarryPlan& plan)
    {
        std::wstring out;
        for (const auto& seg : plan.segments)
        {
            out += seg.text;
        }
        return out;
    }

    // ---- The PIECEWISE fill -----------------------------------------------------------------------

    // One bracketed paste of a piecewise fill (BuildPromptFill(text), no submit CR).
    struct DraftFillPaste
    {
        std::wstring text; // what is pasted
        bool block{ false }; // true: a pasted block claude will collapse into its own placeholder
        size_t blockNewlines{ 0 }; // block only: the placeholder's expected "+M" (0 == the bare form)
    };

    namespace draft_carry_detail
    {
        // Split visible (Typed) text into pastes claude will insert INLINE: each at most
        // kClaudePasteInlineMaxChars characters and kClaudePasteInlineMaxNewlines newlines, a cut never
        // inside a surrogate pair nor inside a literal placeholder (which must stay contiguous so it
        // re-renders -- and verifies -- as the same quote).
        //   text -- the Typed segment.
        //   out  -- receives the chunks, in order.
        //   returns -- nothing.
        //   throws  -- std::bad_alloc on allocation failure only.
        inline void AppendInlineChunks(std::wstring_view text, std::vector<DraftFillPaste>& out)
        {
            const auto quotes = FindDraftPlaceholders(text);
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = start;
                size_t newlines = 0;
                while (end < text.size() && end - start < kClaudePasteInlineMaxChars)
                {
                    const wchar_t c = text[end++];
                    if (c == L'\n' && ++newlines == kClaudePasteInlineMaxNewlines)
                    {
                        break;
                    }
                }
                if (end < text.size() && end > start + 1 && IsLowSurrogate(text[end]))
                {
                    --end; // keep the astral pair together
                }
                for (const auto& q : quotes)
                {
                    if (q.begin > start && q.begin < end && end < q.end)
                    {
                        end = q.begin; // cut before the quote instead of through it
                        break;
                    }
                }
                out.push_back(DraftFillPaste{ std::wstring{ text.substr(start, end - start) }, false, 0 });
                start = end;
            }
        }
    }

    // Plan the bracketed pastes that rebuild a resolved draft in an EMPTY claude input box so it renders
    // like the source did: Typed text chunked to stay inline, every Block pasted alone (so claude mints
    // its own bound placeholder for it -- "[Pasted text #K +M lines]", backed by the real content).
    // Pasting the whole expanded draft as ONE block would make claude collapse the typed words into the
    // same placeholder -- correct content, but a box that no longer shows what the user wrote.
    //   plan    -- a COMPLETE plan.
    //   returns -- the pastes, in order ({} for a refused/empty plan).
    //   throws  -- std::bad_alloc on allocation failure only.
    inline std::vector<DraftFillPaste> BuildDraftFillPastes(const DraftCarryPlan& plan)
    {
        std::vector<DraftFillPaste> out;
        if (!plan.complete)
        {
            return out;
        }
        for (const auto& seg : plan.segments)
        {
            if (seg.kind == DraftSegmentKind::Block)
            {
                out.push_back(DraftFillPaste{ seg.text, true, ClaudePasteNewlineCount(seg.text) });
                continue;
            }
            draft_carry_detail::AppendInlineChunks(seg.text, out);
        }
        return out;
    }

    // ---- Verification -------------------------------------------------------------------------------

    enum class DraftFillVerdict : uint8_t
    {
        Verified, // the box reads exactly the planned pastes (whitespace/invisibles-tolerant, placeholder ids free)
        Partial, // the box holds a clean PREFIX of the plan ending at a paste boundary -- resume from `landed`
        Eaten, // the box is empty -- nothing landed (the TUI ate it pre-raw-mode); a full re-fill is safe
        Foreign, // anything else: text we did not plan, a paste cut mid-way, a placeholder of the wrong size
    };

    struct DraftFillCheck
    {
        DraftFillVerdict verdict{ DraftFillVerdict::Foreign };
        size_t landed{ 0 }; // Partial: how many leading pastes are confirmed in the box (the resume index)
    };

    namespace draft_carry_detail
    {
        // One comparable unit of a box read or of the expected fill.
        struct FillAtom
        {
            bool isPlaceholder{ false };
            std::wstring text; // text atom: the compare key (whitespace + invisibles out, NFC); unused for a block (its inline form is derived lazily from the plan)
            DraftPlaceholderKind kind{ DraftPlaceholderKind::PastedText };
            int index{ 0 };
            int lines{ -1 };
            bool anyIndex{ false }; // a Block: claude mints its own id, so any N matches
            size_t paste{ 0 }; // expected atoms: which paste this atom belongs to
        };

        // The compare key shared with VerifyFillAgainstPrompt: FoldTrimPromptText (line breaks folded,
        // invisible characters dropped, NFC) then every whitespace character stripped -- soft wrap,
        // TAB->spaces and trailing-space trimming all live in whitespace, so none of them can matter.
        //   s       -- any text.
        //   returns -- the key.
        //   throws  -- std::bad_alloc on allocation failure only.
        inline std::wstring CompareKey(std::wstring_view s)
        {
            return verify_detail::WsStripped(FoldTrimPromptText(s));
        }

        // Split `s` into text/placeholder atoms (empty text atoms dropped).
        //   s     -- a box read or a typed chunk.
        //   paste -- the owning paste index (expected side; 0 for the box).
        //   out   -- appended to.
        //   returns -- nothing.
        //   throws  -- std::bad_alloc on allocation failure only.
        inline void AppendFillAtoms(std::wstring_view s, size_t paste, std::vector<FillAtom>& out)
        {
            size_t pos = 0;
            for (const auto& ph : FindDraftPlaceholders(s))
            {
                if (auto key = CompareKey(s.substr(pos, ph.begin - pos)); !key.empty())
                {
                    FillAtom a;
                    a.text = std::move(key);
                    a.paste = paste;
                    out.push_back(std::move(a));
                }
                FillAtom m;
                m.isPlaceholder = true;
                m.kind = ph.kind;
                m.index = ph.index;
                m.lines = ph.lines;
                m.paste = paste;
                out.push_back(std::move(m));
                pos = ph.end;
            }
            if (auto key = CompareKey(s.substr(pos)); !key.empty())
            {
                FillAtom a;
                a.text = std::move(key);
                a.paste = paste;
                out.push_back(std::move(a));
            }
        }

        // Does a placeholder seen in the box satisfy an expected placeholder atom?
        //   seen, want -- the box atom and the expected atom (both placeholders).
        //   returns    -- true on a match (a Block: any id, the collapsed form with the planned count;
        //                 a literal quote: the exact kind/id/count re-typed).
        //   throws     -- nothing.
        inline bool PlaceholderAtomMatches(const FillAtom& seen, const FillAtom& want) noexcept
        {
            if (want.anyIndex)
            {
                if (seen.kind != DraftPlaceholderKind::PastedText)
                {
                    return false;
                }
                const int wantLines = want.lines;
                return wantLines == 0 ? (seen.lines <= 0) : (seen.lines == wantLines);
            }
            return seen.kind == want.kind && seen.index == want.index && seen.lines == want.lines;
        }
    }

    // Judge a box read against a piecewise fill plan -- or against its first `count` pastes (the delivery's
    // "has piece i landed?" signal: the box must read as EXACTLY pieces 1..i, so a mid-repaint frame that
    // shows an empty or half-drawn box never counts as a landing). Pure; the pump acts on the verdict:
    // Verified -> done; Eaten -> the one safe retry (nothing ever landed); Partial / Foreign -> leave the
    // box alone (never paste onto text we can't account for) -- Partial is reported at a clean paste
    // boundary only, a paste cut mid-way reads Foreign (re-typing it would duplicate its visible part).
    // Whitespace-only pastes carry no comparable content, so they count as landed when reached -- a
    // lost newline between two pastes is the one thing this cannot see (cosmetic, never content).
    // A block's inline text (claude INLINES a block only below its collapse threshold -- in practice
    // never) is normalized only on the path that needs it, so a multi-MB block costs nothing per poll.
    //   box     -- the target box as DetectPendingInput / the deep probe read it.
    //   pastes  -- the plan (BuildDraftFillPastes).
    //   count   -- verify against pastes[0, count) only (clamped; the default is the whole plan).
    //   returns -- the verdict (+ the resume index for Partial).
    //   throws  -- std::bad_alloc on allocation failure only.
    inline DraftFillCheck VerifyDraftFillPastes(std::wstring_view box, const std::vector<DraftFillPaste>& pastes, size_t count = static_cast<size_t>(-1))
    {
        using namespace draft_carry_detail;
        const size_t planned = (std::min)(count, pastes.size());
        std::vector<FillAtom> seen;
        AppendFillAtoms(box, 0, seen);
        if (seen.empty())
        {
            return DraftFillCheck{ DraftFillVerdict::Eaten, 0 };
        }
        std::vector<FillAtom> want;
        for (size_t i = 0; i < planned; ++i)
        {
            if (pastes[i].block)
            {
                FillAtom b;
                b.isPlaceholder = true;
                b.anyIndex = true;
                b.lines = static_cast<int>(pastes[i].blockNewlines);
                b.paste = i; // the inline alternative is computed lazily from pastes[i] (see the loop)
                want.push_back(std::move(b));
                continue;
            }
            AppendFillAtoms(pastes[i].text, i, want);
        }

        size_t si = 0; // box atom index
        size_t off = 0; // offset inside the current box TEXT atom
        size_t landed = 0; // pastes fully confirmed so far
        size_t currentPaste = 0;
        bool currentPasteTouched = false; // some atom of currentPaste already matched
        // Pastes before the first expected atom's paste (or between atoms) with no atoms are whitespace-only.
        const auto finishPastesBefore = [&](size_t paste) {
            if (paste > currentPaste)
            {
                landed = paste; // every paste before `paste` has had all its atoms matched
                currentPaste = paste;
                currentPasteTouched = false;
            }
        };
        // Partial only on a clean paste boundary; a paste cut mid-way is Foreign (re-delivering it would duplicate).
        const auto stopHere = [&](size_t paste) -> DraftFillCheck {
            finishPastesBefore(paste);
            if (currentPasteTouched)
            {
                return DraftFillCheck{ DraftFillVerdict::Foreign, landed };
            }
            return DraftFillCheck{ DraftFillVerdict::Partial, landed };
        };
        const auto consumeText = [&](const std::wstring& need) -> int {
            // 1 = consumed, 0 = box ran out cleanly (a prefix of need), -1 = mismatch
            size_t done = 0;
            while (done < need.size())
            {
                if (si >= seen.size())
                {
                    return 0;
                }
                if (seen[si].isPlaceholder)
                {
                    return -1;
                }
                const std::wstring& have = seen[si].text;
                const size_t take = (std::min)(have.size() - off, need.size() - done);
                if (have.compare(off, take, need, done, take) != 0)
                {
                    return -1;
                }
                done += take;
                off += take;
                if (off == have.size())
                {
                    ++si;
                    off = 0;
                }
                else if (done < need.size())
                {
                    return -1; // unreachable: take was bounded by need
                }
            }
            return 1;
        };

        // Match `need` as TEXT at the cursor; `ok` receives whether to continue, the return value is the
        // verdict to stop with otherwise. A box that ends inside `need` is a clean stop only if nothing of
        // it had landed (stopHere); a partial one marks the paste touched first (-> Foreign).
        const auto matchText = [&](const std::wstring& need, size_t paste, bool& ok) -> DraftFillCheck {
            ok = false;
            const size_t siBefore = si;
            const size_t offBefore = off;
            const int r = consumeText(need);
            if (r == 1)
            {
                currentPasteTouched = true;
                ok = true;
                return DraftFillCheck{};
            }
            if (r == 0)
            {
                if (!(si == siBefore && off == offBefore))
                {
                    currentPasteTouched = true;
                }
                return stopHere(paste);
            }
            return DraftFillCheck{ DraftFillVerdict::Foreign, landed };
        };

        for (const auto& atom : want)
        {
            finishPastesBefore(atom.paste);
            if (atom.isPlaceholder && atom.anyIndex)
            {
                // A BLOCK: claude's own placeholder for it (any id, the planned count)...
                if (si >= seen.size())
                {
                    return stopHere(atom.paste);
                }
                if (off == 0 && seen[si].isPlaceholder)
                {
                    if (!PlaceholderAtomMatches(seen[si], atom))
                    {
                        return DraftFillCheck{ DraftFillVerdict::Foreign, landed };
                    }
                    ++si;
                    currentPasteTouched = true;
                    continue;
                }
                // ...or, only if claude INLINED it, its text (normalized on this path alone).
                bool ok = false;
                const auto verdict = matchText(CompareKey(pastes[atom.paste].text), atom.paste, ok);
                if (!ok)
                {
                    return verdict;
                }
                continue;
            }
            if (!atom.isPlaceholder)
            {
                if (atom.text.empty())
                {
                    continue;
                }
                bool ok = false;
                const auto verdict = matchText(atom.text, atom.paste, ok);
                if (!ok)
                {
                    return verdict;
                }
                continue;
            }
            // A LITERAL placeholder (a kept quote re-typed as text): the exact kind / id / count.
            if (si >= seen.size())
            {
                return stopHere(atom.paste);
            }
            if (off != 0 || !seen[si].isPlaceholder || !PlaceholderAtomMatches(seen[si], atom))
            {
                return DraftFillCheck{ DraftFillVerdict::Foreign, landed };
            }
            ++si;
            currentPasteTouched = true;
        }
        if (si < seen.size())
        {
            return DraftFillCheck{ DraftFillVerdict::Foreign, landed }; // box content beyond the (prefix of the) plan
        }
        return DraftFillCheck{ DraftFillVerdict::Verified, planned };
    }
}
