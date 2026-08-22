// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster M5 engine test harness (7 partial files)
// Standalone engine test harness (NOT in the msbuild) -- run-m5-tests.bat compiles every TU
// unity-style without the WinRT PCH and links the engine .cpp. The 38 tests + 2 benches were
// split out of the former 6006-line m5_tests.cpp into themed TUs that share m5_tests.h.
//
// Partial files in this group (★ marks THIS file):
//   m5_tests.cpp              - the RUNNER: wmain (calls every entry point, in order) + the g_checks/g_failures defs
//   m5_tests.h                - shared header: the CHECK macro, the extern counters, the fixtures (MakeSession/Msg/UPS/NowMsTest), and all 40 test entry-point declarations
//   tests_state.cpp           - state machine / ordered-state / wire / registry / fanout / fork-echo / typed-capture / ObserveClaude / supersede
//   tests_spawn_sched.cpp     - spawn builders / profile bootstrap / bridge round-trip / scheduler / enter-retry / build-prompt / scheduler integration
//   tests_persistence.cpp     - persistence / manager layout / window record / app settings / tab naming + color
//   tests_transcript.cpp      - transcript scan + reconcilers / ProcessInspect tree+parse / transcript resolve / Codex / store / lineage / search / live / bring-to-front
// ★ tests_summary_anchor.cpp  - summary table-trim + user-msg noise / PromptAnchor (+ edge/corpus/benches) / pending-input
// ======================================================================================
//
// Agentmaster - M5 standalone test harness: summary_anchor tests. Shared CHECK/fixtures/decls
// live in m5_tests.h; the runner (m5_tests.cpp) calls each entry point. See run-m5-tests.bat.
#include "m5_tests.h"

#include <fstream> // TestPendingPaste: write the temp paste-cache fixture files

void TestSummaryTableTrim()
{
    std::wprintf(L"[TestSummaryTableTrim]\n");
    std::wstring D = L" "; D += static_cast<wchar_t>(0x00B7); D += L" "; // " · " de-framed cell separator

    // 1) Box-drawing table: the top/sep/bottom rules (┌──┬──┐ / ├──┼──┤ / └──┴──┘) drop; the two data
    //    rows de-frame to "Name · Age" / "Bob · 30", joined by '\n'.
    {
        const std::wstring in =
            L"\x250C\x2500\x2500\x252C\x2500\x2500\x2510\n" // ┌──┬──┐
            L"\x2502 Name \x2502 Age \x2502\n" // │ Name │ Age │
            L"\x251C\x2500\x2500\x253C\x2500\x2500\x2524\n" // ├──┼──┤
            L"\x2502 Bob \x2502 30 \x2502\n" // │ Bob │ 30 │
            L"\x2514\x2500\x2500\x2534\x2500\x2500\x2518"; // └──┴──┘
        const std::wstring want = L"Name" + D + L"Age\nBob" + D + L"30";
        CHECK(StripSummaryTableRules(in) == want, "box-drawing table: rule rows dropped, data rows de-framed to cell · cell");
    }

    // 2) The user's actual long box-drawing separator row, between two data rows -> rule drops, data de-frames.
    {
        const std::wstring rule =
            L"\x251C\x2500\x2500\x2500\x2500\x2500\x253C\x2500\x2500\x2500\x2500\x253C\x2500\x2500\x2500\x2524"; // ├────┼───┼──┤
        const std::wstring in = L"\x2502 a \x2502 b \x2502 c \x2502\n" + rule + L"\n\x2502 1 \x2502 2 \x2502 3 \x2502";
        const std::wstring want = L"a" + D + L"b" + D + L"c\n1" + D + L"2" + D + L"3";
        CHECK(StripSummaryTableRules(in) == want, "long ├────┼────┤ rule row dropped; flanking rows de-framed");
        CHECK(StripSummaryTableRules(rule) == rule, "a lone all-rule message is returned UNCHANGED (never blanked)");
    }

    // 3) Markdown table: the |---|---| separator drops; header + data de-frame.
    {
        const std::wstring in = L"| Name | Age |\n|------|-----|\n| Bob | 30 |";
        const std::wstring want = L"Name" + D + L"Age\nBob" + D + L"30";
        CHECK(StripSummaryTableRules(in) == want, "markdown table: |---| separator dropped, header+data de-framed");
        CHECK(StripSummaryTableRules(L"|:---|:---:|---:|") == L"|:---|:---:|---:|",
              "a markdown alignment rule alone -> unchanged (all-rules guard)");
    }

    // 4) Data rows de-frame to their CELL content (the bars are UI, the cells are the data).
    {
        CHECK(StripSummaryTableRules(L"\x2502 :) \x2502 :( \x2502") == L":)" + D + L":(",
              "a box row with punctuation cells de-frames to ':) · :('");
        CHECK(StripSummaryTableRules(L"| -1 | +2 |") == L"-1" + D + L"+2",
              "a markdown data row of signed numbers de-frames (digits kept)");
        CHECK(StripSummaryTableRules(L"| - | + |") == L"-" + D + L"+",
              "a markdown row of single -/+ cells de-frames (it is bar-framed)");
    }

    // 5) A fully-EMPTY box row (│   │) is a contentless rule -> dropped; flanking rows de-frame.
    {
        const std::wstring in = L"\x2502 a \x2502\n\x2502   \x2502\n\x2502 b \x2502";
        CHECK(StripSummaryTableRules(in) == L"a\nb", "an empty │   │ row drops; 'a' / 'b' single-cell rows de-frame");
    }

    // 5b) De-frame mechanics: any │-bearing row (even border-less), single-cell, and internal empty cells.
    {
        CHECK(StripSummaryTableRules(L"Name \x2502 Age") == L"Name" + D + L"Age", "a border-less box row (│ present) de-frames");
        CHECK(StripSummaryTableRules(L"\x2502 MENU \x2502") == L"MENU", "a single-cell box row de-frames to just the cell");
        CHECK(StripSummaryTableRules(L"\x2502 a \x2502   \x2502 b \x2502") == L"a" + D + L"b", "an internal empty cell is dropped");
    }

    // 5c) SAFETY: a stray prose/code pipe is NOT a table row (markdown needs a leading AND trailing bar).
    {
        CHECK(StripSummaryTableRules(L"run foo | grep bar") == L"run foo | grep bar", "an un-framed '|' (shell pipe) is left verbatim");
        CHECK(StripSummaryTableRules(L"| head -5") == L"| head -5", "a leading-only '|' is not a framed row -> verbatim");
        CHECK(StripSummaryTableRules(L"a | b | c") == L"a | b | c", "a border-less '|' row is left verbatim (ambiguous with prose)");
    }

    // 6) ASCII thematic break (>=3 of the fill set -/=/~/#/*/_) drops; a short 2-char run does NOT.
    //    The fill set was widened to # * _ after a 1.4 GB corpus scan turned up bare "######" rule
    //    lines (the only separator shape the original -/=/~ set missed).
    {
        CHECK(StripSummaryTableRules(L"intro\n---\noutro") == L"intro\noutro", "a --- thematic break row drops");
        CHECK(StripSummaryTableRules(L"intro\n--\noutro") == L"intro\n--\noutro", "a 2-dash line is kept (run < 3, no box char)");
        CHECK(StripSummaryTableRules(L"a\n====\nb") == L"a\nb", "a ==== underline rule drops");
        CHECK(StripSummaryTableRules(L"a\n######\nb") == L"a\nb", "a ###### rule line drops (corpus: the real miss)");
        CHECK(StripSummaryTableRules(L"a\n###\nb") == L"a\nb", "a bare ### (run==3) drops");
        CHECK(StripSummaryTableRules(L"a\n***\nb") == L"a\nb", "a *** thematic break drops");
        CHECK(StripSummaryTableRules(L"a\n___\nb") == L"a\nb", "a ___ thematic break drops");
    }

    // 6b) The all-structural GUARD protects real content built from the same fill chars: a markdown
    //     heading, a bullet item, an emphasis marker, a titled separator (corpus had 80 of these), and
    //     a "::: fence" all carry letters / a sub-3 run, so they are KEPT verbatim.
    {
        CHECK(StripSummaryTableRules(L"### Heading") == L"### Heading", "### Heading kept (has letters)");
        CHECK(StripSummaryTableRules(L"* item") == L"* item", "a '* item' bullet kept (has letters)");
        CHECK(StripSummaryTableRules(L"**") == L"**", "a lone ** kept (run < 3, no box)");
        CHECK(StripSummaryTableRules(L"=== first GET ===") == L"=== first GET ===", "a titled separator keeps its label");
        CHECK(StripSummaryTableRules(L":::") == L":::", "a ::: fence kept (colon is glue, not fill -> no run)");
        CHECK(StripSummaryTableRules(L"1K \x2588\x2588\x2588 1.96x") == L"1K \x2588\x2588\x2588 1.96x",
              "a bar-chart data row (block chars + numbers) is kept as data");
    }

    // 7) No table at all -> returned unchanged (cheap no-op path).
    {
        const std::wstring in = L"just some prose\nwith two lines";
        CHECK(StripSummaryTableRules(in) == in, "no rule rows -> message unchanged");
        CHECK(StripSummaryTableRules(L"") == L"", "empty message -> empty");
        CHECK(StripSummaryTableRules(L"single line") == L"single line", "single non-rule line -> unchanged");
    }

    // 8) CRLF on kept lines is normalized to LF; the rule row (with its CR) still drops; data de-frames.
    {
        const std::wstring in = L"| a | b |\r\n|---|---|\r\n| 1 | 2 |";
        const std::wstring want = L"a" + D + L"b\n1" + D + L"2";
        CHECK(StripSummaryTableRules(in) == want, "CRLF data rows de-frame (-> LF), the |---| rule (with CR) dropped");
    }
}

// ---- Summary-panel JUMP resolver (PromptAnchor.h / SUMMARY_JUMP.md) ----

static bool SpanHolds(const std::wstring& hay, const Agentmaster::AnchorMatch& m, const std::wstring& wantSub)
{
    if (!m.found || m.offset + m.length > hay.size())
    {
        return false;
    }
    const auto slice = hay.substr(m.offset, m.length);
    return Agentmaster::NormalizeForMatch(slice).find(Agentmaster::NormalizeForMatch(wantSub)) != std::wstring::npos;
}

void TestPromptAnchor()
{
    using namespace Agentmaster;
    std::wprintf(L"Summary-panel jump resolver (SUMMARY_JUMP.md):\n");

    // Normalize: ASCII-lower, collapse whitespace runs, trim.
    CHECK(NormalizeForMatch(L"  Fix   The\tBuild \n") == L"fix the build", "normalize: lower+collapse+trim");
    CHECK(NormalizeForMatch(L"\n\n  ") == L"", "normalize: all-whitespace -> empty");

    // Needle: first non-trivial line, capped to maxLen.
    CHECK(PickAnchorNeedle(L"\n\n  Run the tests\nsecond line", 64) == L"run the tests", "needle: first non-trivial line");
    CHECK(PickAnchorNeedle(L"abcdefghij", 4) == L"abcd", "needle: capped to maxLen");
    CHECK(PickAnchorNeedle(L"   \n  ", 64) == L"", "needle: empty when no content");

    // Basic in-order resolve over a synthetic scrollback ('>' prompt + assistant filler).
    {
        const std::wstring hay = L"> fix the build\nassistant: working on it\n> run the tests\nassistant: done\n";
        auto r = ResolvePromptAnchors(hay, { L"fix the build", L"run the tests" });
        CHECK(r.size() == 2 && r[0].found && r[1].found, "resolve: both prompts found");
        CHECK(r[0].offset < r[1].offset, "resolve: in buffer order");
        CHECK(!r[0].partial && r[0].quality > 0.99, "resolve: full match quality ~1");
        CHECK(SpanHolds(hay, r[0], L"fix the build") && SpanHolds(hay, r[1], L"run the tests"), "resolve: spans land on the text");
        CHECK(!r[0].outOfOrder && !r[1].outOfOrder, "resolve: in-order, not flagged");
    }

    // Whitespace-tolerant: a re-indent / odd transcript whitespace still matches the rendered line
    // (this is also what makes a SOFT-WRAPPED prompt — continuous in the haystack — resolve).
    {
        const std::wstring hay = L"> fix the build now\n";
        auto m = ResolveOnePromptAnchor(hay, L"fix   the\tbuild now", 0);
        CHECK(m.found && m.quality > 0.99, "resolve: whitespace differences tolerated");
    }

    // Render prefix ("> ") is skipped naturally (substring search).
    {
        const std::wstring hay = L"  > implement the centering primitive\n";
        auto m = ResolveOnePromptAnchor(hay, L"implement the centering primitive", 0);
        CHECK(m.found && !m.partial, "resolve: '> ' render prefix skipped");
    }

    // Duplicates: the i-th send maps to the i-th surviving occurrence (order-preserving greedy).
    {
        const std::wstring hay = L"> deploy now\nout1\n> deploy now\nout2\n";
        auto r = ResolvePromptAnchors(hay, { L"deploy now", L"deploy now" });
        CHECK(r[0].found && r[1].found && r[0].offset < r[1].offset, "duplicates: first then second occurrence");
        CHECK(!r[0].outOfOrder && !r[1].outOfOrder, "duplicates: both in order");
    }

    // Partial: a truncated/reflowed render resolves at quality < 1 and is flagged partial.
    {
        const std::wstring full = L"please refactor the entire authentication subsystem to use tokens";
        const std::wstring hay = L"> please refactor the entire authentication subsy\nok\n"; // render cut short
        auto m = ResolveOnePromptAnchor(hay, full, 0);
        CHECK(m.found && m.partial && m.quality < 1.0 && m.quality > 0.5, "partial: truncated render -> partial match");
    }

    // Not found: a prompt not on screen resolves to nothing.
    {
        const std::wstring hay = L"> something else entirely\n";
        auto m = ResolveOnePromptAnchor(hay, L"this prompt is absent from the buffer xyzzy", 0);
        CHECK(!m.found, "not found: absent prompt -> no match");
    }

    // Out-of-order fallback: no in-order candidate -> last global occurrence, flagged outOfOrder.
    {
        const std::wstring hay = L"> beta task here\n> alpha task here\n";
        auto r = ResolvePromptAnchors(hay, { L"alpha task here", L"beta task here" });
        CHECK(r[0].found, "outOfOrder: first prompt still found");
        CHECK(r[1].found && r[1].outOfOrder, "outOfOrder: second prompt falls back, flagged");
    }

    // RTL (Hebrew/Arabic): a terminal renders an RTL line CHARACTER-REVERSED (visual order) while the
    // transcript stores it logical, so the prompt appears reversed in the buffer. The resolver must still
    // find it via the reversed orientation (the matched ROW is the same, so centering works). The fix is
    // gated on ContainsRtl so LTR matching is never perturbed. (SUMMARY_JUMP.md §5.)
    {
        // logical "write a program" in Hebrew, built from code points so the source stays pure ASCII
        // (cl without /utf-8 would mis-decode a raw Hebrew literal). U+05D0..U+05EA = Hebrew letters.
        std::wstring logical;
        for (const int c : { 0x05DB, 0x05EA, 0x05D5, 0x05D1, 0x0020, 0x05EA, 0x05D5, 0x05DB, 0x05E0, 0x05D9, 0x05EA })
        {
            logical.push_back(static_cast<wchar_t>(c));
        }
        const std::wstring visual(logical.rbegin(), logical.rend()); // what the terminal buffer holds
        CHECK(ResolveOnePromptAnchor(L"> " + visual + L"\n", logical, 0).found, "RTL: reversed-in-buffer Hebrew prompt resolves");
        CHECK(ResolveOnePromptAnchor(L"> " + logical + L"\n", logical, 0).found, "RTL: forward orientation still resolves");
        // An LTR prompt must NOT be matched by its own reversal (no RTL char => no reversed pass).
        CHECK(!ResolveOnePromptAnchor(L"> dlrow olleh now\n", L"hello world now is the prompt", 0).found, "LTR: reversed text is NOT spuriously matched");
        // Batch: a reversed-in-buffer RTL prompt resolves in the greedy pass too.
        auto rb = ResolvePromptAnchors(L"> " + visual + L"\nout\n", { logical });
        CHECK(rb.size() == 1 && rb[0].found, "RTL: batch resolve finds the reversed prompt");
    }

    // Cheap validate: true at the resolved offset, false at a bogus one / past end.
    {
        const std::wstring hay = L"> fix the build\nout\n";
        auto m = ResolveOnePromptAnchor(hay, L"fix the build", 0);
        CHECK(m.found && ValidatePromptAnchor(hay, L"fix the build", m.offset), "validate: true at resolved offset");
        CHECK(!ValidatePromptAnchor(hay, L"fix the build", m.offset + 8), "validate: false at a wrong offset");
        CHECK(!ValidatePromptAnchor(hay, L"fix the build", hay.size() + 100), "validate: false past end");
    }

    // Agentmaster (SUMMARY_JUMP.md §5): prompt-MARKER validation. Claude Code prefixes a SENT prompt's
    // rendered line with a marker glyph (U+276F); requiring a match to sit right after one binds it to the
    // real user-prompt render instead of an assistant ECHO of the same words. The glyph is built from its
    // code point so this TU stays pure-ASCII (it compiles without /utf-8); the marker SET passed to the
    // resolver is the shipping constant kClaudePromptMarkers (already \u-escaped in the header).
    {
        const std::wstring caret(1, static_cast<wchar_t>(0x276F)); // the heavy right-angle prompt ornament
        AnchorOptions mopts;
        mopts.promptMarkers = std::wstring{ kClaudePromptMarkers };

        // (1) An echo PRECEDES the real render. Legacy (no markers) binds to the earlier echo; marker
        //     validation binds to the caret-marked render instead. This is the core win.
        {
            const std::wstring hay = L"assistant: ill fix the bug now\n" + caret + L" fix the bug\nout\n";
            const size_t echoPos = hay.find(L"fix the bug");
            const size_t markedPos = hay.find(L"fix the bug", hay.find(caret));
            CHECK(echoPos != std::wstring::npos && markedPos != std::wstring::npos && echoPos < markedPos, "marker: fixture sane (echo before render)");

            auto noMark = ResolvePromptAnchors(hay, { L"fix the bug" });
            CHECK(noMark[0].found && noMark[0].offset == echoPos, "marker: legacy binds the earlier echo");

            auto withMark = ResolvePromptAnchors(hay, { L"fix the bug" }, mopts);
            CHECK(withMark[0].found && withMark[0].offset == markedPos, "marker: validation binds the caret-marked render, not the echo");
        }

        // (2) Soft fallback (regression-proofing): when markers ARE in use (present for another prompt) but
        //     a prompt's text appears ONLY unmarked (no caret render — its own scrolled off, or the marker
        //     glyph isn't on sent lines in this build), that prompt STILL resolves via a legacy fallback.
        //     Marker enforcement never makes a prompt that legacy would resolve disappear.
        {
            const std::wstring hay = caret + L" unrelated heading line\nassistant: please refactor the auth module now\n";
            auto r = ResolvePromptAnchors(hay, { L"unrelated heading line", L"refactor the auth module now" }, mopts);
            CHECK(r[0].found, "marker: the caret-marked prompt resolves");
            CHECK(r[1].found, "marker(soft fallback): an unmarked-only prompt still resolves (never regresses)");
        }

        // (3) Self-adapting: markers requested but NONE present (a '>'-rendering build) => enforcement
        //     auto-disables and falls back to legacy text matching (never regresses).
        {
            const std::wstring hay = L"> fix the build\nout\n"; // '>' is not a configured marker; no U+276F anywhere
            auto r = ResolvePromptAnchors(hay, { L"fix the build" }, mopts);
            CHECK(r[0].found, "marker: no marker in buffer => enforcement disabled, legacy match still resolves");
        }

        // (4) Duplicate disambiguation: two caret-marked sends with an assistant echo between them resolve
        //     to the TWO real renders, in order (the unmarked echo is skipped by the gate).
        {
            const std::wstring hay = caret + L" deploy now\nassistant: ok i will deploy now\n" + caret + L" deploy now\nout\n";
            const size_t firstMarked = hay.find(L"deploy now");
            const size_t lastMarked = hay.rfind(L"deploy now");
            auto r = ResolvePromptAnchors(hay, { L"deploy now", L"deploy now" }, mopts);
            CHECK(r[0].found && r[1].found && r[0].offset == firstMarked && r[1].offset == lastMarked, "marker: duplicates map to the two caret renders, echo skipped");
            CHECK(!r[0].outOfOrder && !r[1].outOfOrder, "marker: duplicates resolved in order");
        }

        // (5) Proximity, not just same-line: with TWO caret lines — one where the text appears FAR down a
        //     long marked line (beyond markerLookback), another where it starts right AT the marker — the
        //     match binds to the proximate (real-render) one, skipping the distant in-line occurrence. (A
        //     real prompt's needle is its first line and so starts right at the marker; an incidental
        //     occurrence lands mid-line, too far to validate.)
        {
            const std::wstring hay = caret + L" heading XXXXXXXXXXXXXXXXXXXXXX run the migration\n" + caret + L" run the migration\nout\n";
            const size_t proximate = hay.rfind(L"run the migration"); // second caret line: text right after the marker
            auto r = ResolvePromptAnchors(hay, { L"run the migration" }, mopts);
            CHECK(r[0].found && r[0].offset == proximate, "marker: binds the occurrence right after the marker, not a distant in-line one");
        }

        // (6) PREFIX COLLISION (SUMMARY_JUMP.md §5b — the "(4) and (5) both jump to (5)" report): prompt A's
        //     text is a strict PREFIX of prompt B's. A's needle also matches the START of B's render, so when
        //     A's OWN render has scrolled off the greedy binds A to B's render -> both resolve to B and A's
        //     jump wrongly lands on B. The second pass gives the offset to the LONGEST match (B) and unresolves
        //     the shorter prefix (A).
        {
            // Only B's render is on screen (A's scrolled off): A and B's prefixes start at the SAME offset.
            const std::wstring hay = L"... lots of old output ...\n" + caret + L" deploy dev please, fast mode if possible\nreply\n";
            auto r = ResolvePromptAnchors(hay, { L"deploy dev please", L"deploy dev please, fast mode if possible" }, mopts);
            CHECK(r[1].found, "prefix-collision: the longer prompt (B) keeps its render");
            CHECK(!r[0].found, "prefix-collision: the shorter prefix (A), render off-screen, does NOT steal B's render");
        }
        {
            // Both renders ON screen: the order-preserving greedy already binds each to its OWN render
            // (distinct offsets), so the second pass must NOT unresolve the shorter one.
            const std::wstring hay = caret + L" deploy dev please\nout\n" + caret + L" deploy dev please, fast mode if possible\nout\n";
            auto r = ResolvePromptAnchors(hay, { L"deploy dev please", L"deploy dev please, fast mode if possible" }, mopts);
            CHECK(r[0].found && r[1].found && r[0].offset != r[1].offset, "prefix-collision: both on screen -> each binds its OWN render (no false unresolve)");
        }
        {
            // EXACT duplicates (equal length) are NOT unresolved by the second pass: two identical sends with
            // two on-screen renders map to the two renders, in order (the "handle exact same properly" case).
            const std::wstring hay = caret + L" deploy dev\nout1\n" + caret + L" deploy dev\nout2\n";
            auto r = ResolvePromptAnchors(hay, { L"deploy dev", L"deploy dev" }, mopts);
            CHECK(r[0].found && r[1].found && r[0].offset < r[1].offset, "prefix-collision: exact duplicates keep their two distinct renders");
        }
        {
            // Three-way prefix chain at one offset (only the longest render on screen): A < B < C all start at
            // C's render offset; only C (the longest) survives, A and B unresolve.
            const std::wstring hay = L"old...\n" + caret + L" alpha beta gamma delta epsilon\ndone\n";
            auto r = ResolvePromptAnchors(hay, { L"alpha beta", L"alpha beta gamma", L"alpha beta gamma delta epsilon" }, mopts);
            CHECK(r[2].found && !r[0].found && !r[1].found, "prefix-collision: 3-way chain -> only the longest (C) keeps the render");
        }
    }
}

// Agentmaster (SUMMARY_JUMP.md §5): every found span must be in-bounds of the haystack it was resolved
// against — an OOB offset/length would AV when the adapter maps it back to a buffer row.
static bool AnchorSpansValid(const std::wstring& hay, const std::vector<Agentmaster::AnchorMatch>& r)
{
    for (const auto& m : r)
    {
        if (!m.found)
        {
            continue;
        }
        if (m.offset > hay.size() || m.offset + m.length > hay.size())
        {
            return false;
        }
    }
    return true;
}

// Agentmaster (SUMMARY_JUMP.md §5b): COLLISION tests. A prompt's needle is its first line (a PREFIX of its
// text) and matching is a substring search, so several prompts can land on ONE rendered line — by prefix,
// suffix, substring, or a longer prompt backing off to a shorter sibling's render. The second pass resolves
// these by region containment (+ a quality tiebreak for an identical region). These cases pin every shape:
// each prompt is a MARKED `<U+276F> <text>` render; off-screen renders are simulated by omitting them; the
// glyph is built from its code point so the TU stays pure-ASCII (compiles without /utf-8).
namespace
{
    const std::wstring kCaret(1, static_cast<wchar_t>(0x276F));
    std::wstring CMk(const std::wstring& t) { return kCaret + L" " + t + L"\n"; }                 // a marked render
    std::wstring CEcho(const std::wstring& t) { return L"assistant: " + t + L"\n"; }               // an unmarked echo
    const std::wstring kFill = L"...filler reply output line...\n";
    std::vector<Agentmaster::AnchorMatch> CRes(const std::wstring& hay, const std::vector<std::wstring>& p)
    {
        Agentmaster::AnchorOptions mk;
        mk.promptMarkers = std::wstring{ Agentmaster::kClaudePromptMarkers };
        return Agentmaster::ResolvePromptAnchors(hay, p, mk);
    }
    bool CDistinct(const std::vector<Agentmaster::AnchorMatch>& r) // all FOUND offsets distinct (no two on one render-offset)
    {
        for (size_t i = 0; i < r.size(); ++i)
            for (size_t j = i + 1; j < r.size(); ++j)
                if (r[i].found && r[j].found && r[i].offset == r[j].offset)
                    return false;
        return true;
    }
}

void TestPromptAnchorCollisions()
{
    using namespace Agentmaster;
    std::wprintf(L"Prompt resolver COLLISIONS (prefix / suffix / substring / backoff / exact-dup, SUMMARY_JUMP.md §5b):\n");

    // ---- PREFIX (A is a strict prefix of B) ----
    {
        auto r = CRes(CMk(L"deploy dev please") + kFill + CMk(L"deploy dev please, fast mode if possible") + kFill,
                      { L"deploy dev please", L"deploy dev please, fast mode if possible" });
        CHECK(r[0].found && r[1].found && CDistinct(r), "collide/prefix: both on screen -> distinct renders");
    }
    {
        auto r = CRes(L"...old output...\n" + CMk(L"deploy dev please, fast mode if possible") + kFill,
                      { L"deploy dev please", L"deploy dev please, fast mode if possible" });
        CHECK(!r[0].found && r[1].found, "collide/prefix: shorter's render off-screen -> shorter DIM, longer keeps (the report)");
    }
    {
        auto r = CRes(L"...old output...\n" + CMk(L"deploy dev please") + kFill,
                      { L"deploy dev please", L"deploy dev please, fast mode if possible" });
        CHECK(r[0].found && !r[1].found, "collide/prefix: LONGER's render off-screen -> longer DIM (backoff tie broken by quality)");
    }
    {
        auto r = CRes(CMk(L"alpha beta") + kFill + CMk(L"alpha beta gamma") + kFill + CMk(L"alpha beta gamma delta") + kFill,
                      { L"alpha beta", L"alpha beta gamma", L"alpha beta gamma delta" });
        CHECK(r[0].found && r[1].found && r[2].found && CDistinct(r), "collide/prefix: 3-way chain ALL on screen -> all distinct");
    }
    {
        auto r = CRes(L"...old...\n" + CMk(L"alpha beta gamma delta") + kFill,
                      { L"alpha beta", L"alpha beta gamma", L"alpha beta gamma delta" });
        CHECK(!r[0].found && !r[1].found && r[2].found, "collide/prefix: 3-way chain, only longest render -> only longest survives");
    }
    {
        // Renders in REVERSE order vs prompts (rare): we only require no same-offset collision; the exact
        // assignment is a documented limitation (SUMMARY_JUMP.md §5b).
        auto r = CRes(CMk(L"deploy dev please, fast mode if possible") + kFill + CMk(L"deploy dev please") + kFill,
                      { L"deploy dev please", L"deploy dev please, fast mode if possible" });
        CHECK(r[0].found && r[1].found && CDistinct(r), "collide/prefix: reversed render order -> both found, distinct (assignment is a known limitation)");
    }
    {
        // Two prompts sharing a >maxNeedle (64-char) prefix => identical needles after truncation.
        const std::wstring base = L"please carefully perform the entire staged deployment sequence right now today";
        auto on = CRes(CMk(base + L" using ALPHA") + kFill + CMk(base + L" using BETA") + kFill,
                       { base + L" using ALPHA", base + L" using BETA" });
        CHECK(on[0].found && on[1].found && CDistinct(on), "collide/prefix: shared >64-char needle, both on screen -> distinct");
        auto off = CRes(L"...old...\n" + CMk(base + L" using BETA") + kFill,
                        { base + L" using ALPHA", base + L" using BETA" });
        CHECK(!off[0].found && off[1].found, "collide/prefix: shared >64-char needle, first off-screen -> first DIM, second keeps");
    }

    // ---- SUFFIX / SUBSTRING (B is contained in A's render but not at its start) ----
    {
        auto r = CRes(CMk(L"Please deploy dev") + kFill + CMk(L"deploy dev") + kFill,
                      { L"Please deploy dev", L"deploy dev" });
        CHECK(r[0].found && r[1].found && CDistinct(r), "collide/suffix: both on screen -> each its own render");
    }
    {
        auto r = CRes(L"...old...\n" + CMk(L"Please deploy dev") + kFill, { L"Please deploy dev", L"deploy dev" });
        CHECK(r[0].found && !r[1].found, "collide/suffix: shorter off-screen -> shorter DIM (was a mid-line same-row collision)");
    }
    {
        auto r = CRes(L"...old...\n" + CMk(L"the long line about deploying the dev build") + kFill,
                      { L"the long line about deploying the dev build", L"deploying the dev" });
        CHECK(r[0].found && !r[1].found, "collide/substring: mid-line substring, shorter off-screen -> shorter DIM");
    }
    {
        // Two SHORTER prompts off-screen + a single longer CONTAINER render on screen -> both shorter DIM.
        auto r = CRes(L"...old...\n" + CMk(L"deploy the app server now") + kFill,
                      { L"deploy", L"deploy the app", L"deploy the app server now" });
        CHECK(!r[0].found && !r[1].found && r[2].found, "collide/container: two shorter off-screen + container render -> only container survives");
    }

    // ---- NON-collisions that must NOT be over-unresolved ----
    {
        auto r = CRes(CMk(L"fix the build now") + kFill + CMk(L"fix the build later") + kFill,
                      { L"fix the build now", L"fix the build later" });
        CHECK(r[0].found && r[1].found && CDistinct(r), "no-collide/divergent: shared lead, divergent tail, both on screen -> distinct");
    }
    {
        auto r = CRes(CMk(L"deploy dev") + kFill + CMk(L"deploy app") + kFill, { L"deploy dev", L"deploy app" });
        CHECK(r[0].found && r[1].found && CDistinct(r), "no-collide/leading-word: two renders sharing a leading word -> each its own");
    }
    {
        // Same first line, different body (multi-line) — distinct renders on screen.
        auto r = CRes(CMk(L"do the thing\nfast") + kFill + CMk(L"do the thing\nslow") + kFill,
                      { L"do the thing\nfast", L"do the thing\nslow" });
        CHECK(r[0].found && r[1].found && CDistinct(r), "no-collide/same-first-line: both on screen -> distinct");
    }

    // ---- EXACT DUPLICATES (the 'handle exact same properly' case — equal region+quality kept) ----
    {
        auto r = CRes(CMk(L"run it") + kFill + CMk(L"run it") + kFill + CMk(L"run it") + kFill,
                      { L"run it", L"run it", L"run it" });
        CHECK(r[0].found && r[1].found && r[2].found && CDistinct(r), "collide/exact-dup: 3 sends, 3 renders -> 3 distinct, none unresolved");
    }
    {
        auto r = CRes(L"...old...\n" + CMk(L"run it") + kFill, { L"run it", L"run it", L"run it" });
        CHECK(r[0].found && r[1].found && r[2].found, "collide/exact-dup: 3 sends, 1 surviving render -> all kept (identical region+quality)");
    }

    // ---- MARKER + ECHO + PREFIX combined ----
    {
        auto r = CRes(CEcho(L"deploy dev please") + CMk(L"deploy dev please, fast mode if possible") + kFill,
                      { L"deploy dev please", L"deploy dev please, fast mode if possible" });
        CHECK(!r[0].found && r[1].found, "collide/echo+prefix: unmarked echo of A before B's render -> A DIM, B keeps");
    }

    // ---- The literal user 1..6 list (the report), two scroll states ----
    {
        std::vector<std::wstring> six = { L"Please deploy dev", L"deploy dev", L"again", L"deploy dev please",
                                          L"deploy dev please, fast mode if possible",
                                          L"build and deploy dev please, fast mode if possible" };
        std::wstring all;
        for (const auto& p : six)
            all += CMk(p) + kFill;
        auto r = CRes(all, six);
        bool allFound = true;
        for (const auto& m : r)
            allFound = allFound && m.found;
        CHECK(allFound && CDistinct(r), "collide/list-1to6: ALL on screen -> all 6 found, distinct (4 and 5 do NOT collide)");

        std::wstring tail = L"...scrollback elided...\n" + CMk(six[4]) + kFill + CMk(six[5]) + kFill;
        auto t = CRes(tail, six);
        CHECK(!t[0].found && !t[1].found && !t[2].found && !t[3].found && t[4].found && t[5].found && t[4].offset != t[5].offset,
              "collide/list-1to6: only 5 and 6 on screen -> 1-4 DIM, 5 and 6 land on distinct rows");
    }
}

// Agentmaster (SUMMARY_JUMP.md §4): the lazy floor-prefix candidate index that caps the MISS-CASCADE
// cost (the release-0.6.8 UI-freeze unit cost: a prompt whose floor prefix occurs in the haystack but
// whose longer prefixes don't pays in-order + global-rfind x every backoff length x the marker
// soft-fallback — each a full O(haystack) scan). The index must be INVISIBLE: bit-identical results to
// the legacy scans on every shape — overlapping occurrences, the marker echo-storm, and the dense
// (> kFloorIndexMaxHits candidates) fallback. Single-prompt equality pits ResolveOnePromptAnchor
// (legacy scans, never indexed) against the 1-element batch (the index engages after the first miss)
// on identical inputs, so the two paths cross-check each other.
void TestPromptAnchorFloorIndex()
{
    using namespace Agentmaster;
    std::wprintf(L"Prompt resolver floor-hit index (miss-cascade cost cap, SUMMARY_JUMP.md §4):\n");
    const std::wstring caret(1, static_cast<wchar_t>(0x276F)); // the U+276F prompt ornament
    AnchorOptions mk;
    mk.promptMarkers = std::wstring{ kClaudePromptMarkers };

    const auto sameMatch = [](const AnchorMatch& a, const AnchorMatch& b) {
        const double dq = a.quality - b.quality;
        return a.found == b.found &&
               (!a.found || (a.offset == b.offset && a.length == b.length &&
                             a.outOfOrder == b.outOfOrder && a.partial == b.partial &&
                             a.needleLen == b.needleLen && dq < 1e-9 && dq > -1e-9));
    };
    const auto batchEqualsSingle = [&](const std::wstring& hay, const std::wstring& msg, const AnchorOptions& o) {
        const auto one = ResolveOnePromptAnchor(hay, msg, 0, o);
        const auto batch = ResolvePromptAnchors(hay, { msg }, o);
        return batch.size() == 1 && sameMatch(one, batch[0]);
    };

    // Overlapping floor occurrences, happy path: the longest needle hits in-order immediately (no
    // index ever builds) — the common case must stay byte-identical.
    {
        const std::wstring hay = L"filler line before\nnow aaaaaaaaaaaa tail\nfiller after\n"; // 12 a's
        const std::wstring msg = L"aaaaaaaaaa"; // 10 a's
        CHECK(batchEqualsSingle(hay, msg, {}), "floor-index: overlapping-run happy path == legacy");
        const auto r = ResolvePromptAnchors(hay, { msg });
        CHECK(r[0].found && !r[0].partial && hay.substr(r[0].offset, msg.size()) == msg,
              "floor-index: overlapping-run lands at the run start, full quality");
    }
    // Overlapping floor occurrences THROUGH the index: the full needle misses (absent suffix), the
    // floor (8 a's) has overlapping candidates at every offset of the run — collection must step by
    // +1 so the backed-off needle still lands the run start.
    {
        const std::wstring hay = L"filler line before\nnow aaaaaaaaaaaa tail\nfiller after\n";
        const std::wstring msg = L"aaaaaaaaaazz"; // 10 a's + absent tail -> len12 misses, len8 resolves
        CHECK(batchEqualsSingle(hay, msg, {}), "floor-index: overlapping-run via index == legacy");
        const auto r = ResolvePromptAnchors(hay, { msg });
        CHECK(r[0].found && r[0].needleLen == 8 && r[0].offset == hay.find(L'a'),
              "floor-index: backed-off overlapping-run lands the FIRST candidate");
    }

    // The MISS-CASCADE shape: floor-8 ("cascade ") present as unmarked echoes, every longer prefix
    // absent, markers enforced -> the enforced in-order + global probes reject every echo and the
    // soft fallback lands the first echo as a partial. The full 4-family cascade on both paths.
    {
        std::wstring hay = caret + L" some other real prompt\nassistant filler output line\n";
        for (int i = 0; i < 30; ++i)
        {
            hay += L"cascade " + std::to_wstring(i) + L" echo text that diverges from the real prompt\n";
        }
        const std::wstring msg = L"cascade prompt full text that is nowhere rendered in this scrollback";
        CHECK(batchEqualsSingle(hay, msg, mk), "floor-index: miss-cascade == legacy");
        const auto r = ResolvePromptAnchors(hay, { msg }, mk);
        CHECK(r[0].found && r[0].partial && r[0].needleLen == 8,
              "floor-index: cascade falls back to the floor needle, partial");
    }

    // Marker echo-storm THROUGH the index: 30 unmarked floor echoes, then ONE MARKED one — the
    // enforced candidate walk must skip every unmarked echo and land the marked occurrence in-order.
    {
        std::wstring hay = L"assistant preamble line\n";
        for (int i = 0; i < 30; ++i)
        {
            hay += L"cascadence unmarked echo " + std::to_wstring(i) + L" diverges here\n";
        }
        const size_t markedLine = hay.size();
        hay += caret + L" cascadence marked echo diverges here too\n";
        const std::wstring msg = L"cascadence prompt full text absent from this scrollback entirely";
        CHECK(batchEqualsSingle(hay, msg, mk), "floor-index: echo-storm == legacy");
        const auto r = ResolvePromptAnchors(hay, { msg }, mk);
        CHECK(r[0].found && r[0].offset > markedLine && !r[0].outOfOrder,
              "floor-index: enforced walk skips 30 unmarked echoes, lands the marked one");
    }

    // DENSE fallback: > kFloorIndexMaxHits floor occurrences -> the index aborts to the legacy
    // vectorized scans (which handle dense-hit inputs well) with an identical result — no cliff.
    {
        std::wstring hay;
        for (int i = 0; i < 600; ++i)
        {
            // Contains the floor-8 ("repeated") but diverges immediately after it, so every longer
            // backoff length (13/26/52 here) misses and only the floor needle can land.
            hay += L"repeated echo " + std::to_wstring(i) + L" diverges\n";
        }
        const std::wstring msg = L"repeated floor prompt whose full text never rendered";
        CHECK(batchEqualsSingle(hay, msg, {}), "floor-index: dense (600 candidates) == legacy");
        const auto r = ResolvePromptAnchors(hay, { msg });
        CHECK(r[0].found && r[0].partial && r[0].needleLen == 8,
              "floor-index: dense case still resolves via the floor needle");
    }
}

// Agentmaster (SUMMARY_JUMP.md §5): edge-case + crash-safety fuzz for the prompt resolver with marker
// validation ON. The resolver feeds alt-nav + jump on the UI thread, so a pathological haystack/needle must
// never AV / read OOB / infinite-loop / throw -- a crash here takes the whole app. Each case asserts it
// RETURNS, the result size matches the prompt count, and every found span is in-bounds.
void TestPromptAnchorEdgeCases()
{
    using namespace Agentmaster;
    std::wprintf(L"Prompt resolver edge cases + crash-safety (marker validation on):\n");
    const std::wstring caret(1, static_cast<wchar_t>(0x276F)); // the U+276F prompt ornament
    AnchorOptions mk;
    mk.promptMarkers = std::wstring{ kClaudePromptMarkers };

    // Empty haystack / empty prompt list / empty + whitespace-only prompts (slots preserved, not found).
    CHECK(ResolvePromptAnchors(L"", { L"hello world here" }, mk).size() == 1, "edge: empty haystack -> one not-found slot");
    CHECK(ResolvePromptAnchors(caret + L" hello world here\n", {}, mk).empty(), "edge: empty prompt list -> empty result");
    {
        const std::wstring hay = caret + L" hello world here\n";
        auto r = ResolvePromptAnchors(hay, { L"", L"   \t  ", L"hello world here" }, mk);
        CHECK(r.size() == 3 && !r[0].found && !r[1].found && r[2].found, "edge: empty/ws prompts keep their slot, not found");
        CHECK(AnchorSpansValid(hay, r), "edge: spans valid with empty/ws prompts");
    }

    // A marker-only haystack; a prompt that IS a marker glyph; a needle longer than the whole haystack.
    {
        const std::wstring hay = caret + caret + caret;
        CHECK(AnchorSpansValid(hay, ResolvePromptAnchors(hay, { caret }, mk)), "edge: all-marker haystack + marker-glyph prompt");
    }
    {
        const std::wstring hay = caret + L" ab\n";
        CHECK(AnchorSpansValid(hay, ResolvePromptAnchors(hay, { L"this needle is far longer than the whole tiny haystack xyz" }, mk)), "edge: needle longer than haystack");
    }

    // markerLookback extremes: 0 (no occurrence can ever be "marked" -> everything falls back to legacy) and
    // huge (the lookback window is clamped to the string start, never reads before index 0).
    {
        const std::wstring hay = caret + L" do the thing now\n";
        AnchorOptions z = mk;
        z.markerLookback = 0;
        auto r0 = ResolvePromptAnchors(hay, { L"do the thing now" }, z);
        CHECK(r0[0].found && AnchorSpansValid(hay, r0), "edge: lookback=0 -> soft fallback still resolves");
        AnchorOptions big = mk;
        big.markerLookback = 100000;
        auto rb = ResolvePromptAnchors(hay, { L"do the thing now" }, big);
        CHECK(rb[0].found && AnchorSpansValid(hay, rb), "edge: huge lookback does not read before the buffer start");
    }

    // Embedded NUL + a lone (unpaired) UTF-16 surrogate, in BOTH haystack and needle (wstring holds them) —
    // normalization + the marker scan must treat them as ordinary code units, never crash.
    {
        std::wstring hay = caret + L" abc";
        hay.push_back(L'\0');
        hay += L"def ghi jkl\n";
        std::wstring needle = L"abc";
        needle.push_back(L'\0');
        needle += L"def ghi jkl";
        CHECK(AnchorSpansValid(hay, ResolvePromptAnchors(hay, { needle }, mk)), "edge: embedded NUL + control chars no crash");
    }
    {
        std::wstring hay = caret + L" pre ";
        hay.push_back(static_cast<wchar_t>(0xD800)); // lone high surrogate
        hay += L" post text here\n";
        std::wstring needle = L"pre ";
        needle.push_back(static_cast<wchar_t>(0xD800));
        needle += L" post text here";
        CHECK(AnchorSpansValid(hay, ResolvePromptAnchors(hay, { needle }, mk)), "edge: lone UTF-16 surrogate no crash");
    }

    // Huge marker-only haystack + a huge needle (the FindAcceptable inner loop / normalization must stay
    // bounded and RETURN — a regression here would hang the UI thread, not crash, but is just as fatal).
    {
        const std::wstring hay(200000, static_cast<wchar_t>(0x276F)); // 200k markers, no text
        const std::wstring needle(50000, L'z');
        CHECK(AnchorSpansValid(hay, ResolvePromptAnchors(hay, { needle }, mk)), "edge: 200k-marker haystack + 50k needle returns");
    }

    // Thousands of UNMARKED duplicate occurrences before ONE marked render: the marker scan must skip every
    // unmarked hit (the FindAcceptable forward loop) and terminate on the single marked one.
    {
        std::wstring hay;
        for (int i = 0; i < 2000; ++i)
        {
            hay += L"assistant ctx repeat token here\n"; // 2000 UNMARKED occurrences
        }
        hay += caret + L" repeat token here\n"; // exactly one MARKED render (the last occurrence)
        auto r = ResolvePromptAnchors(hay, { L"repeat token here" }, mk);
        CHECK(r[0].found && AnchorSpansValid(hay, r), "edge: 2000 unmarked + 1 marked -> resolves, terminates");
        CHECK(r[0].offset == hay.rfind(L"repeat token here"), "edge: skipped all unmarked, landed on the marked render");
    }

    // ValidatePromptAnchor crash-safety with marker opts passed (it ignores them, but must not choke).
    {
        const std::wstring hay = caret + L" validate me please\n";
        CHECK(!ValidatePromptAnchor(hay, L"validate me please", hay.size() + 999, mk), "edge: validate past end is safe");
        auto m = ResolveOnePromptAnchor(hay, L"validate me please", 0, mk);
        CHECK(m.found && ValidatePromptAnchor(hay, L"validate me please", m.offset, mk), "edge: validate at the resolved offset");
    }
}

// Agentmaster (SUMMARY_JUMP.md §5): exercise the resolver + marker validation against the REAL on-disk
// Claude session corpus. For each session we extract its real sent prompts (the SAME list the panel /
// alt-nav resolve) and synthesize a realistic rendered buffer -- each prompt as a MARKED `<U+276F> <prompt>`
// render preceded by an UNMARKED assistant echo of its first line -- then resolve with markers AND legacy.
// This stresses the marker code with real-world prompt strings (emoji, RTL, code, huge / multi-line prompts)
// and verifies the two invariants that must ALWAYS hold: (1) every found span is in-bounds (no OOB -> the
// adapter's offset->row map would AV otherwise), and (2) markers NEVER make a prompt that legacy resolved
// disappear (the soft-fallback guarantee). Disambiguation (markers steering off the echo onto the real
// render) is reported + asserted non-zero. Skips silently when no corpus is present (CI / other machines).
void TestPromptAnchorRealCorpus()
{
    using namespace Agentmaster;
    std::wprintf(L"Prompt resolver over the REAL session corpus (markers; crash + never-regress + disambiguation):\n");
    const auto sessions = EnumerateTranscripts(NowMsTest() - 92LL * 24 * 3600 * 1000);
    if (sessions.empty())
    {
        std::wprintf(L"  [info] no live corpus -> skipped\n");
        return;
    }
    const std::wstring caret(1, static_cast<wchar_t>(0x276F));
    AnchorOptions mk;
    mk.promptMarkers = std::wstring{ kClaudePromptMarkers };

    size_t sessionsUsed = 0, totalPrompts = 0, foundMk = 0, foundLegacy = 0;
    size_t regressions = 0, oobSpans = 0, markedHit = 0, improvedByMarker = 0;
    const size_t kMaxSessions = 120, kMaxPromptsPerSession = 40, kReadCapBytes = 2u * 1024 * 1024;

    for (const auto& s : sessions)
    {
        if (sessionsUsed >= kMaxSessions)
        {
            break;
        }
        std::vector<std::wstring> prompts;
        try
        {
            prompts = AnalyzeSessionTranscript(s.path, kReadCapBytes).userMsgs;
        }
        catch (...)
        {
            continue; // a parse throw here is itself a finding, but the never-throw wrappers should prevent it
        }
        if (prompts.empty())
        {
            continue;
        }
        if (prompts.size() > kMaxPromptsPerSession)
        {
            prompts.resize(kMaxPromptsPerSession);
        }

        // Build a realistic rendered buffer; track each prompt's MARKED render + UNMARKED echo offsets.
        std::wstring hay;
        std::vector<size_t> markedPos(prompts.size(), std::wstring::npos);
        std::vector<size_t> echoPos(prompts.size(), std::wstring::npos);
        for (size_t i = 0; i < prompts.size(); ++i)
        {
            const auto needle = PickAnchorNeedle(prompts[i], mk.maxNeedle);
            if (needle.empty())
            {
                continue; // unbuildable (no non-trivial line) -> resolves not-found in BOTH (no regression)
            }
            hay += L"assistant ctx: "; // an UNMARKED echo of the first line (where legacy binds)
            echoPos[i] = hay.size();
            hay += needle;
            hay += L"\n";
            hay += caret; // the REAL marked render
            hay += L" ";
            markedPos[i] = hay.size();
            hay += prompts[i];
            hay += L"\n... assistant reply filler ...\n";
        }

        std::vector<AnchorMatch> rMk, rLeg;
        try
        {
            rMk = ResolvePromptAnchors(hay, prompts, mk);
            rLeg = ResolvePromptAnchors(hay, prompts); // legacy (no markers)
        }
        catch (...)
        {
            continue;
        }

        ++sessionsUsed;
        for (size_t i = 0; i < prompts.size(); ++i)
        {
            ++totalPrompts;
            if (rMk[i].found)
            {
                ++foundMk;
            }
            if (rLeg[i].found)
            {
                ++foundLegacy;
            }
            if (rMk[i].found && (rMk[i].offset > hay.size() || rMk[i].offset + rMk[i].length > hay.size()))
            {
                ++oobSpans;
            }
            if (rLeg[i].found && !rMk[i].found)
            {
                ++regressions; // markers made a legacy-found prompt vanish -> the soft fallback failed
            }
            if (markedPos[i] != std::wstring::npos && rMk[i].found && rMk[i].offset == markedPos[i])
            {
                ++markedHit;
                if (rLeg[i].found && echoPos[i] != std::wstring::npos && rLeg[i].offset == echoPos[i])
                {
                    ++improvedByMarker; // legacy bound the echo; markers bound the real render
                }
            }
        }
    }

    CHECK(oobSpans == 0, "real corpus: no out-of-bounds spans (offset+length <= haystack)");
    CHECK(regressions == 0, "real corpus: markers never make a legacy-found prompt vanish (soft fallback holds)");
    CHECK(sessionsUsed > 0, "real corpus: exercised at least one real session");
    CHECK(totalPrompts == 0 || markedHit > 0, "real corpus: at least one real prompt binds its marked render");
    std::wprintf(L"  [info] sessions=%zu prompts=%zu | found markers=%zu legacy=%zu | marked-hit=%zu improved-by-marker=%zu | oob=%zu regress=%zu\n",
                 sessionsUsed, totalPrompts, foundMk, foundLegacy, markedHit, improvedByMarker, oobSpans, regressions);
}

// Build a synthetic Claude scrollback: `rows` lines of filler with `prompts` "> <prompt>" lines
// evenly spaced. `present`==false makes each summary message carry an absent suffix (the pathological
// all-miss case: every needle forces a full backoff + global rfind scan).
static std::wstring BuildBenchHaystack(int rows, int prompts, std::vector<std::wstring>& outMsgs, bool present)
{
    std::wstring hay;
    hay.reserve(static_cast<size_t>(rows) * 64);
    outMsgs.clear();
    uint64_t lcg = 0x9E3779B97F4A7C15ull;
    auto rnd = [&]() { lcg = lcg * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>(lcg >> 33); };
    const int step = (std::max)(1, rows / (std::max)(1, prompts));
    int made = 0;
    for (int i = 0; i < rows; ++i)
    {
        if (i % step == 0 && made < prompts)
        {
            const std::wstring p = L"benchmark prompt number " + std::to_wstring(made) + L" do the thing carefully and well";
            hay += L"> " + p + L"\n";
            // present: the exact text. miss: a prompt whose LEADING chars are absent (e.g. it scrolled
            // off the top) -> the true not-found path the membership pre-check short-circuits.
            outMsgs.push_back(present ? p : (L"zzqx-absent-" + std::to_wstring(rnd()) + L" " + p));
            ++made;
        }
        else
        {
            hay += L"assistant output line filler tokens ";
            hay += std::to_wstring(rnd());
            hay += L" lorem ipsum dolor sit amet consectetur adipiscing\n";
        }
    }
    while (made < prompts)
    {
        const std::wstring p = L"benchmark prompt number " + std::to_wstring(made) + L" do the thing carefully and well";
        hay += L"> " + p + L"\n";
        outMsgs.push_back(present ? p : (L"zzqx-absent " + p));
        ++made;
    }
    return hay;
}

// The release-0.6.8 UI-freeze's UNIT-COST shape (SUMMARY_JUMP.md §4a): every prompt's FLOOR prefix
// occurs in the haystack (as unmarked echoes) but no longer prefix does, and marker validation is
// enforced — so each prompt pays the full in-order + global-rfind backoff cascade PLUS the marker
// soft-fallback repeat. This is what the floor-hit index caps; the plain "all-miss" case above is
// short-circuited by the membership pre-check and never reaches the cascade.
static std::wstring BuildBenchCascade(int rows, int prompts, std::vector<std::wstring>& outMsgs)
{
    std::wstring hay;
    hay.reserve(static_cast<size_t>(rows) * 64);
    outMsgs.clear();
    hay += std::wstring(1, static_cast<wchar_t>(0x276F)) + L" warmup real prompt line\n"; // enables marker enforcement
    const auto tag = [](int p) {
        std::wstring t = std::to_wstring(p);
        while (t.size() < 4)
        {
            t.insert(t.begin(), L'0');
        }
        return L"p" + t + L" ";
    };
    const int echoes = prompts * 3;
    const int step = (std::max)(1, rows / (std::max)(1, echoes));
    int made = 0;
    for (int i = 0; i < rows; ++i)
    {
        if (i % step == 0 && made < echoes)
        {
            // Contains the prompt's floor-8 ("pNNNN th") but diverges before any longer prefix.
            hay += tag(made % prompts) + L"the-echo" + std::to_wstring(made) + L" diverges from anything real\n";
            ++made;
        }
        else
        {
            hay += L"assistant output filler lorem ipsum dolor sit amet consectetur adipiscing elit\n";
        }
    }
    for (int p = 0; p < prompts; ++p)
    {
        outMsgs.push_back(tag(p) + L"the real prompt text that never got rendered in this scrollback at all");
    }
    return hay;
}

static volatile size_t g_benchSink = 0;

template<class F>
static double TimeMsAvg(int iters, F&& fn)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
    {
        fn();
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / (std::max)(1, iters);
}

void BenchPromptAnchor()
{
    using namespace Agentmaster;
    std::wprintf(L"\n--- PromptAnchor benchmark (resolve = O(haystack); gated + off-thread in prod) ---\n");
    struct Case { const wchar_t* name; int rows; int prompts; int iters; };
    const Case cases[] = {
        { L"small  ~1k rows ", 1000, 20, 200 },
        { L"medium ~10k rows", 10000, 50, 40 },
        { L"large  ~50k rows", 50000, 200, 6 },
    };
    for (const auto& c : cases)
    {
        std::vector<std::wstring> msgsPresent, msgsMiss;
        const auto hay = BuildBenchHaystack(c.rows, c.prompts, msgsPresent, true);
        BuildBenchHaystack(c.rows, c.prompts, msgsMiss, false);
        const double mb = static_cast<double>(hay.size()) * sizeof(wchar_t) / (1024.0 * 1024.0);

        // Correctness sanity inside the bench: every present prompt resolves cleanly.
        const auto rr = ResolvePromptAnchors(hay, msgsPresent);
        int hit = 0;
        for (const auto& m : rr)
        {
            if (m.found && !m.partial && !m.outOfOrder)
            {
                ++hit;
            }
        }
        CHECK(hit == c.prompts, "bench: all present prompts resolve cleanly");

        const double tPresent = TimeMsAvg(c.iters, [&] { const auto r = ResolvePromptAnchors(hay, msgsPresent); g_benchSink += r.size() + (r.empty() ? 0 : r[0].offset); });
        const double tMiss = TimeMsAvg(c.iters, [&] { const auto r = ResolvePromptAnchors(hay, msgsMiss); g_benchSink += r.size(); });
        const double tNorm = TimeMsAvg(c.iters, [&] { const auto n = NormalizeForMatch(hay); g_benchSink += n.size(); });

        // The marker-enforced miss-cascade (the 0.6.8 freeze's unit cost; capped by the floor-hit index).
        std::vector<std::wstring> msgsCascade;
        const auto hayCascade = BuildBenchCascade(c.rows, c.prompts, msgsCascade);
        Agentmaster::AnchorOptions mkBench;
        mkBench.promptMarkers = std::wstring{ kClaudePromptMarkers };
        const double tCascade = TimeMsAvg((std::max)(1, c.iters / 4), [&] { const auto r = ResolvePromptAnchors(hayCascade, msgsCascade, mkBench); g_benchSink += r.size(); });
        const int vIters = 200000;
        const double tValTotal = TimeMsAvg(1, [&] { for (int i = 0; i < vIters; ++i) { g_benchSink += ValidatePromptAnchor(hay, msgsPresent[0], rr[0].offset) ? 1u : 0u; } });

        std::wprintf(L"  %s : haystack=%6.2f MB, prompts=%3d\n", c.name, mb, c.prompts);
        std::wprintf(L"      resolve(present)=%8.3f ms   resolve(all-miss)=%8.3f ms   normalize-only=%8.3f ms\n", tPresent, tMiss, tNorm);
        std::wprintf(L"      resolve(cascade)=%8.3f ms   (marker-enforced floor-present misses; the 0.6.8 freeze shape)\n", tCascade);
        std::wprintf(L"      validate-one    =%8.4f us   (epoch-unchanged fast path = 0)\n", (tValTotal / vIters) * 1000.0);
    }
    std::wprintf(L"  [sink %zu]\n", static_cast<size_t>(g_benchSink));
}

// Agentmaster (SUMMARY_JUMP.md §4 / point 2 "guarantee full batch resolve only"): measure the FULL batch
// resolve on a REAL, heavy on-disk session. Production resolves the ENTIRE prompt list on EVERY scan (no
// cache, by design), so a heavy session is the cost ceiling we commit to. Gated on AM_BENCH_SESSION (full
// path to a .jsonl): it SKIPS (does not fail) when unset, so CI / other machines never depend on a local
// file. "mixed" = the real, noise-filtered prompt list resolved against the trailing kAnchorRecentWindowChars
// of the transcript (recent prompts present, older ones a fast absence) — exactly what ControlCore caps +
// resolves; "all-miss" = the same N with guaranteed-absent needles (each forces a full-haystack scan).
void BenchPromptAnchorRealSession()
{
    using namespace Agentmaster;
    wchar_t envbuf[1024]{};
    const DWORD got = GetEnvironmentVariableW(L"AM_BENCH_SESSION", envbuf, 1024);
    if (got == 0 || got >= 1024)
    {
        std::wprintf(L"\n--- PromptAnchor heavy-session bench: SKIPPED (set AM_BENCH_SESSION=<path-to-.jsonl>) ---\n");
        return;
    }
    const std::wstring path(envbuf, got);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        std::wprintf(L"\n--- PromptAnchor heavy-session bench: file not found (%ls) ---\n", path.c_str());
        return;
    }

    // The real, noise-filtered prompt list the panel / alt-nav resolve (production parity).
    const auto info = AnalyzeSessionTranscript(path, 0);
    const std::vector<std::wstring>& prompts = info.userMsgs;

    // Raw transcript -> wide, used as a haystack stand-in; cap to the production recent window so recent
    // prompts are present and older ones have "scrolled off" (the realistic mix ControlCore resolves).
    std::wstring raw;
    {
        std::FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") == 0 && fp)
        {
            std::fseek(fp, 0, SEEK_END);
            const long sz = std::ftell(fp);
            std::fseek(fp, 0, SEEK_SET);
            std::string bytes(sz > 0 ? static_cast<size_t>(sz) : 0u, '\0');
            if (!bytes.empty())
            {
                const size_t rd = std::fread(bytes.data(), 1, bytes.size(), fp);
                bytes.resize(rd);
            }
            std::fclose(fp);
            if (!bytes.empty())
            {
                const int wlen = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
                if (wlen > 0)
                {
                    raw.resize(static_cast<size_t>(wlen));
                    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), raw.data(), wlen);
                }
            }
        }
    }
    std::wstring hay = raw.size() > kAnchorRecentWindowChars ? raw.substr(raw.size() - kAnchorRecentWindowChars) : raw;
    const double mb = static_cast<double>(hay.size()) * sizeof(wchar_t) / (1024.0 * 1024.0);

    const auto rr = ResolvePromptAnchors(hay, prompts);
    int found = 0;
    for (const auto& m : rr)
    {
        if (m.found)
        {
            ++found;
        }
    }

    std::vector<std::wstring> miss;
    miss.reserve(prompts.size());
    for (size_t i = 0; i < prompts.size(); ++i)
    {
        miss.push_back(L"zqxjw9-absent-prompt-" + std::to_wstring(i)); // 8+ chars, nowhere in the haystack
    }

    const int iters = 10;
    const double tMixed = TimeMsAvg(iters, [&] { const auto r = ResolvePromptAnchors(hay, prompts); g_benchSink += r.size(); });
    const double tMiss = TimeMsAvg(iters, [&] { const auto r = ResolvePromptAnchors(hay, miss); g_benchSink += r.size(); });

    std::wprintf(L"\n--- PromptAnchor heavy-session bench (REAL transcript; full batch resolve, no cache) ---\n");
    std::wprintf(L"  session : %ls\n", path.c_str());
    std::wprintf(L"  prompts=%zu  resolved-in-tail=%d  haystack=%.2f MB (capped at kAnchorRecentWindowChars)\n",
                 prompts.size(), found, mb);
    std::wprintf(L"  resolve(mixed/realistic)=%8.3f ms   resolve(all-miss/worst)=%8.3f ms   [one scan, UI thread, x%d]\n",
                 tMixed, tMiss, iters);
    std::wprintf(L"  [sink %zu]\n", static_cast<size_t>(g_benchSink));
}

void TestSummaryUserMsgNoise()
{
    using namespace Agentmaster;
    std::wprintf(L"Summary user-message noise filter (only human prompts numbered):\n");
    const std::wstring bullet(1, static_cast<wchar_t>(0x25CF)); // ● assistant/tool bullet
    const std::wstring rec(1, static_cast<wchar_t>(0x23FA)); // ⏺
    const std::wstring branch(1, static_cast<wchar_t>(0x23BF)); // ⎿ tool-result branch

    // Genuine human prompts: NOT noise (stay in the numbered list).
    CHECK(!SeIsCommandNoise(L"fix the build please"), "real prompt is not noise");
    CHECK(!SeIsCommandNoise(L"[Image #1] make this the README header"), "image paste is a real user msg");
    CHECK(!SeIsCommandNoise(L"### Results Output - are they correct?"), "markdown-header prompt kept");
    CHECK(!SeIsCommandNoise(L"use a " + bullet + L" bullet in the list"), "marker MID-text is not noise (only leading)");

    // Verbatim-pasted Claude-Code TUI output: noise (the reported "assistant message in my summary").
    CHECK(SeIsCommandNoise(bullet + L" Bug Summary - Launch crash-loop fixed"), "pasted assistant bullet (U+25CF) is noise");
    CHECK(SeIsCommandNoise(L"  " + bullet + L" Plan: six functions"), "leading whitespace + bullet still noise");
    CHECK(SeIsCommandNoise(rec + L" recording-marker paste"), "U+23FA marker is noise");
    CHECK(SeIsCommandNoise(branch + L" Wrote 13 lines to file"), "U+23BF tool-result branch is noise");

    // Pre-existing system-injected noise still caught (regression guard).
    CHECK(SeIsCommandNoise(L"<system-reminder>do x</system-reminder>"), "system-reminder still noise");
    CHECK(SeIsCommandNoise(L"<command-name>/clear</command-name>"), "command echo still noise");
}

// Agentmaster (PENDING_INPUT.md): the pure unsent-draft detector. Marker/rule glyphs are built from
// code points (this TU compiles without /utf-8, so the source stays pure-ASCII -- no raw glyph, no \u
// in a literal). Covers box identification, single/multi-line extraction, the empty box, a sent
// prompt vs the live box, menu-selection rejection, and the rule classifier.
void TestPendingInput()
{
    std::wprintf(L"-- PendingInput (draft detection) --\n");
    const wchar_t MARK = static_cast<wchar_t>(0x276F); // the heavy right-angle prompt ornament
    const wchar_t MARK2 = static_cast<wchar_t>(0x203A); // the secondary single right-angle quote
    const wchar_t DASH = static_cast<wchar_t>(0x2500); // box-drawing light horizontal (the rule char)
    const std::wstring NL(1, static_cast<wchar_t>(10)); // a literal newline, sans a \n escape in source
    const std::wstring rule(60, DASH);
    const std::wstring marker = std::wstring(1, MARK) + L" "; // "> "
    auto V = [](std::initializer_list<std::wstring> r) { return std::vector<std::wstring>(r); };

    // 1. single-line draft
    {
        const auto d = DetectPendingInput(V({ rule, marker + L"hello world", rule }));
        CHECK(d.boxFound, "pending single: box found");
        CHECK(d.text == L"hello world", "pending single: text extracted");
    }
    // 2. multi-line draft -- continuation indent stripped, an internal blank line preserved
    {
        const auto d = DetectPendingInput(V({ rule, marker + L"line one", L"  line two", L"", L"  123", rule }));
        CHECK(d.boxFound, "pending multi: box found");
        CHECK(d.text == (L"line one" + NL + L"line two" + NL + NL + L"123"), "pending multi: lines joined + indent stripped + blank kept");
    }
    // 3. empty box -> box found, no draft
    {
        const auto d = DetectPendingInput(V({ rule, marker, rule }));
        CHECK(d.boxFound, "pending empty: box found");
        CHECK(d.text.empty(), "pending empty: no draft text");
    }
    // 4. no box at all
    {
        const auto d = DetectPendingInput(V({ L"assistant text", L"more output" }));
        CHECK(!d.boxFound, "pending none: no box");
    }
    // 5. a SENT prompt in scrollback + an empty input box below -> only the bottom box (empty)
    {
        const auto d = DetectPendingInput(V({ std::wstring(1, MARK) + L" previously sent", L"assistant replied", rule, marker, rule }));
        CHECK(d.boxFound, "pending sent+empty: box found");
        CHECK(d.text.empty(), "pending sent+empty: scrollback prompt ignored, box empty");
        CHECK(d.caretRow == 3, "pending sent+empty: caret is the bottom box, not the scrollback prompt");
    }
    // 6. menu selection (question directly above the marker) -> NOT the input box
    {
        const auto d = DetectPendingInput(V({ L"Do you want to proceed?", marker + L"1. Yes", L"  2. No" }));
        CHECK(!d.boxFound, "pending menu: not detected as input box");
    }
    // 7. menu wrapped in rules but with a question line above the marker -> still NOT detected
    {
        const auto d = DetectPendingInput(V({ rule, L"Select an option:", marker + L"1. Yes", L"  2. No", rule }));
        CHECK(!d.boxFound, "pending menu-in-rules: question above marker rejects false box");
    }
    // 8. rule-row classification
    {
        CHECK(IsPendingRuleRow(rule), "pending rule: pure rule is a rule");
        CHECK(!IsPendingRuleRow(std::wstring(3, DASH) + L" 3 files " + std::wstring(3, DASH)), "pending rule: labeled divider is NOT a rule");
        CHECK(!IsPendingRuleRow(L"just some text here"), "pending rule: text is not a rule");
        CHECK(!IsPendingRuleRow(std::wstring(3, DASH)), "pending rule: <6 box chars is not a rule");
    }
    // 9. marker with no following space
    {
        const auto d = DetectPendingInput(V({ rule, std::wstring(1, MARK) + L"text", rule }));
        CHECK(d.boxFound, "pending no-space: box found");
        CHECK(d.text == L"text", "pending no-space: marker stripped without a trailing space");
    }
    // 10. secondary marker U+203A
    {
        const auto d = DetectPendingInput(V({ rule, std::wstring(1, MARK2) + L" hi there", rule }));
        CHECK(d.boxFound, "pending marker2: U+203A recognized");
        CHECK(d.text == L"hi there", "pending marker2: text extracted");
    }
    // 11. trailing blank lines inside the box are trimmed
    {
        const auto d = DetectPendingInput(V({ rule, marker + L"only line", L"", L"", rule }));
        CHECK(d.text == L"only line", "pending trailing-blank: trimmed");
    }
    // 12. one blank row between the top rule and the marker -> still detected
    {
        const auto d = DetectPendingInput(V({ rule, L"", marker + L"padded", rule }));
        CHECK(d.boxFound, "pending blank-after-top-rule: detected");
        CHECK(d.text == L"padded", "pending blank-after-top-rule: text extracted");
    }
    // 13. empty rows
    {
        const auto d = DetectPendingInput(std::vector<std::wstring>{});
        CHECK(!d.boxFound, "pending empty-rows: nothing");
    }
    // 14. CURSOR ARTIFACTS — a focused EMPTY box renders the block/space cursor right after "> "; it must
    // NOT read as a draft (the live false-positive: "... appeared" on an empty box). A non-empty draft
    // carries the cursor at its tail too, which must be stripped without eating the real text.
    {
        const wchar_t BLOCK = static_cast<wchar_t>(0x2588); // full block █ (a common cursor glyph)
        const wchar_t NBSP = static_cast<wchar_t>(0x00A0); // no-break space (a non-ASCII space cursor/pad)
        // empty box, block cursor
        auto d1 = DetectPendingInput(V({ rule, std::wstring(1, MARK) + L" " + std::wstring(1, BLOCK), rule }));
        CHECK(d1.boxFound && d1.text.empty(), "pending cursor: empty box + block cursor -> no draft");
        // empty box, NBSP cursor/pad
        auto d2 = DetectPendingInput(V({ rule, std::wstring(1, MARK) + L" " + std::wstring(1, NBSP), rule }));
        CHECK(d2.boxFound && d2.text.empty(), "pending cursor: empty box + NBSP -> no draft");
        // real draft with a trailing block cursor -> keep the text, drop the cursor
        auto d3 = DetectPendingInput(V({ rule, marker + L"hello" + std::wstring(1, BLOCK), rule }));
        CHECK(d3.text == L"hello", "pending cursor: draft + trailing block cursor stripped");
        // a lone block on the line (just the cursor, no space) -> empty
        auto d4 = DetectPendingInput(V({ rule, std::wstring(1, MARK) + L" " + std::wstring(2, BLOCK), rule }));
        CHECK(d4.boxFound && d4.text.empty(), "pending cursor: a run of block glyphs is the cursor -> empty");
        // the rule classifier must NOT treat a block cursor as a rule (it is not a U+2500-range char... it
        // IS in 2580-259F which IS box-drawing) — but a single block on the caret line isn't a rule row
        // (it has < kMinRuleRun box chars), so the box detection above still holds.
    }

    // ================= CROSS-VERSION HARDENING (measured live 2026-07-23) =================
    // Fixture rows below reproduce REAL Claude Code ConPTY buffers (AttachConsole +
    // ReadConsoleOutputCharacterW, the out-of-band probe) — versions 2.1.217 + 2.1.218. Homogeneous
    // rule rows are runs (the captures show pure U+2500 runs); VISIBLE glyphs ride as raw UTF-8
    // literals (this TU compiles under the bat's /utf-8), while every INVISIBLE byte — above all the
    // load-bearing U+00A0 NBSP separator — is built from its code point so a reviewer can see it.
    const std::wstring NBSPs(1, static_cast<wchar_t>(0x00A0));

    // 15. THE NBSP SEPARATOR (Defect #1, fixed): Claude 2.1.x renders "❯" + U+00A0 + text — no plain
    // space. 1,088 historical [pending] lines + every live capture agree (100% NBSP). The old strip
    // accepted only L' ', so every draft leaked a leading NBSP (chars off by one, display polluted).
    {
        const auto d = DetectPendingInput(V({ rule, std::wstring(1, MARK) + NBSPs + L"yooo", rule }));
        CHECK(d.boxFound, "pending nbsp: box found");
        CHECK(d.text == L"yooo", "pending nbsp: the NBSP separator is stripped (no leading U+00A0)");
        // the empty box on 2.1.217/218 is exactly "❯" + NBSP (no block glyph in the out-of-band read)
        const auto e = DetectPendingInput(V({ rule, std::wstring(1, MARK) + NBSPs, rule }));
        CHECK(e.boxFound && e.text.empty(), "pending nbsp: the bare marker+NBSP empty box stays empty");
    }
    // 16. REAL CAPTURE — 2.1.217 empty box (pid 100072; caret row + status line verbatim).
    {
        const std::wstring rule209(209, DASH);
        const auto d = DetectPendingInput(V({ L"", rule209, L"❯ ", rule209,
                                              L"  ⏵⏵ bypass permissions on (shift+tab to cycle)                                    175289 tokens" }));
        CHECK(d.boxFound && d.text.empty(), "pending capture 2.1.217: empty box detected, no draft");
    }
    // 17. REAL CAPTURE — 2.1.218 collapsed-paste-only draft (pid 104924): the ENTIRE draft is the
    // placeholder. Exact extraction matters — the paste resolver anchors on these bytes.
    {
        const std::wstring rule200(200, DASH);
        const auto d = DetectPendingInput(V({ rule200, L"❯ [Pasted text #1 +341 lines]", rule200 }));
        CHECK(d.boxFound, "pending capture collapsed: box found");
        CHECK(d.text == L"[Pasted text #1 +341 lines]", "pending capture collapsed: placeholder extracted EXACTLY");
    }
    // 18. REAL CAPTURE — 2.1.218 multi-line draft (pid 97776; caret + continuation rows verbatim —
    // the user's own live note about this very feature).
    {
        const std::wstring rule200(200, DASH);
        const auto d = DetectPendingInput(V({ rule200,
                                              L"❯ I want you to learn all about grist,",
                                              L"  TODO: can we know the prompt written here and not just know that there is a prompt? in memory? in disk?",
                                              rule200,
                                              L"  ⏵⏵ bypass permissions on (shift+tab to cycle)                                                  0 tokens" }));
        CHECK(d.boxFound, "pending capture multiline: box found");
        CHECK(d.text == L"I want you to learn all about grist,\nTODO: can we know the prompt written here and not just know that there is a prompt? in memory? in disk?",
              "pending capture multiline: NBSP stripped + continuation indent stripped");
    }
    // 19. REAL CAPTURE — the truncated-paste marker line (wow-sess R38, verbatim) survives extraction
    // inside a box body (the resolver parses it downstream).
    {
        const std::wstring rule200(200, DASH);
        const auto d = DetectPendingInput(V({ rule200,
                                              std::wstring(1, MARK) + NBSPs + L"yooo",
                                              L"  ## The user[...Truncated text #2 +258 lines...]es).",
                                              rule200 }));
        CHECK(d.boxFound, "pending capture truncated: box found");
        CHECK(d.text == L"yooo\n## The user[...Truncated text #2 +258 lines...]es).",
              "pending capture truncated: marker line extracted verbatim");
    }
    // 20. THE MENU FALSE POSITIVE (Defect #2, fixed): the AskUserQuestion side-by-side PREVIEW menu.
    // Reconstructed from the six live false positives ([pending] "2. 2 · Three Floors │ Any Python
    // library, zero-copy — via np.frombuf...", chars=3474) + the real transcript payload: the selected
    // option row reads "❯ N. label │ preview", and the preview pane's FLOATING border row above it is
    // box-drawing-dominated, so the old detector took it for the box's top rule and extracted the
    // whole menu+preview as a "draft". Killed twice over: the floating border fails the COLUMN ANCHOR,
    // and the option row itself trips the menu-shape rejector.
    {
        const std::wstring pad(28, L' ');
        const std::wstring border = pad + L"╭" + std::wstring(60, DASH) + L"╮";
        const std::wstring opt2 = L"❯ 2. 2 · Three Floors             │ Any Python library, zero-copy — via np.frombuf";
        const std::wstring opt3 = L"  3. 3 · Question-Headed             │ ---";
        const std::wstring borderBot = pad + L"╰" + std::wstring(60, DASH) + L"╯";
        const auto d = DetectPendingInput(V({ L"Which template shapes all four interop pages?", border, opt2, opt3, borderBot }));
        CHECK(!d.boxFound, "pending preview-menu: floating pane border is NOT a box rule (anchor)");
        // Even under a hypothetical layout whose rule IS full-width + anchored, the option row's shape
        // ("N. " + a │ column separator with content after it) rejects the candidate.
        const auto d2 = DetectPendingInput(V({ rule, opt2, opt3, rule }));
        CHECK(!d2.boxFound, "pending preview-menu: anchored-rule variant still rejected (menu shape)");
    }
    // 21. ...but a REAL draft that merely starts with "N. " (no column separator) is NOT a menu.
    {
        const auto d = DetectPendingInput(V({ rule, std::wstring(1, MARK) + NBSPs + L"2. fix the tests first", rule }));
        CHECK(d.boxFound && d.text == L"2. fix the tests first", "pending numbered draft: kept (no separator, not a menu)");
    }
    // 22. UPWARD-CONTINUE: a "❯"-leading line PASTED INSIDE the draft body used to abort detection
    // outright (the bottom-most marker failed the rule checks and the scan returned). The candidate
    // scan now continues upward to the true caret, and the pasted line rides the body.
    {
        const auto d = DetectPendingInput(V({ rule,
                                              std::wstring(1, MARK) + NBSPs + L"look at this transcript snippet:",
                                              L"  " + std::wstring(1, MARK) + L" the old prompt I pasted",
                                              rule }));
        CHECK(d.boxFound, "pending pasted-marker: box still found (candidate scan continues upward)");
        CHECK(d.caretRow == 1, "pending pasted-marker: the TRUE caret wins, not the pasted line");
        CHECK(d.text == (L"look at this transcript snippet:" + NL + std::wstring(1, MARK) + L" the old prompt I pasted"),
              "pending pasted-marker: the pasted line stays in the body");
    }
    // 23. THE COLUMN ANCHOR predicate itself.
    {
        CHECK(IsAnchoredPendingRuleRow(rule), "pending anchor: a flush-left rule is anchored");
        CHECK(IsAnchoredPendingRuleRow(L"  " + rule), "pending anchor: a 2-space indent is within tolerance");
        const std::wstring floating = std::wstring(28, L' ') + L"╭" + std::wstring(40, DASH) + L"╮";
        CHECK(IsPendingRuleRow(floating), "pending anchor: the floating border IS rule-like by density");
        CHECK(!IsAnchoredPendingRuleRow(floating), "pending anchor: ...but NOT anchored (the discriminator)");
    }
    // 24. FRAMED-FUTURE tolerance: a caret row behind ONE leading vertical border still detects (the
    // side borders' extraction stays best-effort — PENDING_INPUT.md §4).
    {
        const auto d = DetectPendingInput(V({ rule, L"│ " + std::wstring(1, MARK) + NBSPs + L"framed draft", rule }));
        CHECK(d.boxFound && d.text == L"framed draft", "pending framed: leading vertical border skipped");
    }
    // 25. The "Copy Current Prompt" SOURCE PICK (PENDING_INPUT.md §8) — the one rule behind all three
    // copy menus: a non-empty LIVE buffer read wins; anything else falls back to the observer's
    // remembered (possibly persisted) draft; an empty live read must NOT erase that fallback, since
    // the "3 dots" are still showing it through the clear debounce.
    {
        const auto live = PickCurrentPromptText(L"typed just now", L"one tick old");
        CHECK(live.text == L"typed just now" && live.fromLive, "current-prompt pick: a live read wins over the remembered draft");

        const auto fell = PickCurrentPromptText(L"", L"remembered draft");
        CHECK(fell.text == L"remembered draft" && !fell.fromLive, "current-prompt pick: no live read -> the observer's value");

        // A control that couldn't be read (other window / dormant tab / threw) hands us "" — same path.
        const auto blank = PickCurrentPromptText(L"   " + NL + L"\t", L"remembered draft");
        CHECK(blank.text == L"remembered draft" && !blank.fromLive, "current-prompt pick: a whitespace-only live read counts as empty");

        const auto wsMem = PickCurrentPromptText(L"", L"  ");
        CHECK(wsMem.text.empty() && !wsMem.fromLive, "current-prompt pick: a whitespace-only memory yields nothing (copy no-ops)");

        const auto none = PickCurrentPromptText(L"", L"");
        CHECK(none.text.empty() && !none.fromLive, "current-prompt pick: neither source -> empty, flagged not-live");

        // The live text is handed back VERBATIM — multi-line drafts are copied whole, never first-line.
        const std::wstring multi = L"deploy dev please," + NL + L"then run the harness";
        CHECK(PickCurrentPromptText(multi, L"x").text == multi, "current-prompt pick: multi-line draft copied verbatim");
    }
    // 25a. The DRAFT vs COMPOSE BOX relation + the pull-offer policy (PENDING_INPUT.md §8b) — the rule
    // behind "edit the prompt in the tab, refocus the compose box": which of the two texts is THE
    // prompt, and may we take the draft without destroying what the user composed here.
    {
        using DV = DraftVsBox;
        // ---- the 6-way relation table ----
        CHECK(ClassifyDraftAgainstBox(L"", L"") == DV::NoDraft, "draft-vs-box: no draft at all");
        CHECK(ClassifyDraftAgainstBox(L"composed here", L"   \t ") == DV::NoDraft, "draft-vs-box: a whitespace-only draft is no draft (outranks a full box)");
        CHECK(ClassifyDraftAgainstBox(L"  ", L"the unsent draft") == DV::BoxEmpty, "draft-vs-box: a whitespace-only box is EMPTY (the §8a pull)");
        CHECK(ClassifyDraftAgainstBox(L"same text", L"same text") == DV::Same, "draft-vs-box: identical");
        CHECK(ClassifyDraftAgainstBox(L"same text  ", L"same text") == DV::Same, "draft-vs-box: trailing whitespace is render slack, not intent");
        CHECK(ClassifyDraftAgainstBox(L"fix the tests", L"fix the tests and the docs") == DV::Continuation, "draft-vs-box: the draft EXTENDS the box (a strict prefix)");
        CHECK(ClassifyDraftAgainstBox(L"fix the tests and the docs", L"fix the tests") == DV::BoxAhead, "draft-vs-box: the BOX is ahead of the draft");
        CHECK(ClassifyDraftAgainstBox(L"fix the tests", L"please fix the tests") == DV::Divergent, "draft-vs-box: contained but NOT a prefix is Divergent");
        CHECK(ClassifyDraftAgainstBox(L"fix the tests", L"write release notes") == DV::Divergent, "draft-vs-box: unrelated");
        // The NEWLINE FOLD is load-bearing: a UWP TextBox reports a typed newline as '\r' while the
        // detector emits '\n' — without folding, EVERY multi-line draft would read as Divergent.
        CHECK(ClassifyDraftAgainstBox(L"line one\rline two", L"line one\nline two") == DV::Same, "draft-vs-box: CR box == LF draft (the newline fold)");
        CHECK(ClassifyDraftAgainstBox(L"line one\r\nline two", L"line one\nline two") == DV::Same, "draft-vs-box: CRLF box == LF draft");
        CHECK(ClassifyDraftAgainstBox(L"line one\r", L"line one\nline two") == DV::Continuation, "draft-vs-box: multi-line continuation across newline flavors");
        // Interior whitespace is CONTENT (only trailing slack is dropped) — leading indent included.
        CHECK(ClassifyDraftAgainstBox(L"  indented", L"  indented") == DV::Same, "draft-vs-box: leading indent preserved on both sides");
        CHECK(ClassifyDraftAgainstBox(L"a b", L"a  b") == DV::Divergent, "draft-vs-box: an interior space change is a real difference");

        // ---- the offer policy ----
        CHECK(!EvaluateDraftPull(L"anything", L"").offer, "pull-offer: no draft -> no button");
        CHECK(!EvaluateDraftPull(L"same", L"same").offer, "pull-offer: in sync -> no button");
        CHECK(EvaluateDraftPull(L"", L"the unsent draft").offer, "pull-offer: empty box -> offered");
        CHECK(!EvaluateDraftPull(L"", L"the unsent draft").autoExtend, "pull-offer: an empty box is not an 'extend' (the §8a pull writes it)");
        {
            const auto v = EvaluateDraftPull(L"fix the tests", L"fix the tests and the docs");
            CHECK(v.offer && v.autoExtend, "pull-offer: a continuation may be taken AUTOMATICALLY (nothing is lost)");
        }
        {
            // The one that must NEVER be offered: taking the shorter draft would delete the user's addition.
            const auto v = EvaluateDraftPull(L"fix the tests and the docs", L"fix the tests");
            CHECK(!v.offer && !v.autoExtend, "pull-offer: the box is ahead -> NEVER offered (it would delete text)");
        }
        {
            // Divergent + CONTAINMENT: the box appears verbatim inside the draft (a word prepended in
            // the terminal), so taking it loses nothing — offered at any length, but never automatically.
            const auto v = EvaluateDraftPull(L"fix the tests", L"please fix the tests now");
            CHECK(v.offer && !v.autoExtend, "pull-offer: box contained INSIDE the draft -> offered, not auto");
        }
        {
            // Divergent + UNRELATED: replacing the box would destroy composed text for nothing.
            const auto v = EvaluateDraftPull(L"please review the login flow for a race condition here",
                                             L"write the release notes for version five of the tool");
            CHECK(!v.offer, "pull-offer: an unrelated draft is never offered (it would clobber the box)");
        }
        {
            // Divergent + SIMILAR (a word changed in the middle) over the >50-char floor -> offered.
            const std::wstring boxT = L"please fix the failing tests and then update the documentation";
            const std::wstring drfT = L"please fix the failing tests and later update the documentation";
            const auto v = EvaluateDraftPull(boxT, drfT);
            CHECK(v.similarityPercent >= kDraftSimilarityPercent, "pull-offer: a mid-text edit scores >= the 80% floor");
            CHECK(v.offer && !v.autoExtend, "pull-offer: a similar (same-prompt-evolved) draft is offered, not auto");
        }
        {
            // The LENGTH FLOOR: the same shape below 50 chars must NOT be offered (a short string's
            // ratio is noise) — the user's "(and >50 chars)".
            const auto v = EvaluateDraftPull(L"fix the tests now", L"fix the tests soon");
            CHECK(!v.offer && v.similarityPercent == 0, "pull-offer: under the 50-char floor no ratio is consulted");
        }
        {
            // The CONTAINMENT probe's COST CAP: find() is O(needle * haystack) and this runs per
            // keystroke, so an oversized pair skips the probe and lets the (linear) similarity gate
            // decide alone — the conservative direction (no offer), never a stall.
            const std::wstring bigBox(2000, L'x');
            const std::wstring bigDraft = std::wstring(2000, L'y') + bigBox; // contained, product 8e6 > cap
            const auto over = EvaluateDraftPull(bigBox, bigDraft);
            CHECK(over.relation == DV::Divergent && !over.offer, "pull-offer: an oversized containment probe is skipped (cost cap), so no offer");
            // ...while the shape containment actually exists for — a SHORT box inside a long draft —
            // stays well under the cap and is offered.
            const std::wstring smallBox(100, L'x');
            const std::wstring longDraft = std::wstring(2000, L'y') + smallBox;
            const auto under = EvaluateDraftPull(smallBox, longDraft);
            CHECK(under.relation == DV::Divergent && under.offer, "pull-offer: a short box inside a long draft is under the cap and IS offered");
        }
        // ---- the PROVENANCE guard on the automatic extend (never rewrite text the user typed) ----
        CHECK(TextContinuesSeed(L"pulled draft", L"pulled draft"), "seed: an unchanged box still continues the seed");
        CHECK(TextContinuesSeed(L"pulled draft and more", L"pulled draft"), "seed: the box extends the seed");
        CHECK(!TextContinuesSeed(L"typed by hand", L"pulled draft"), "seed: the user's own text does not continue the seed");
        CHECK(!TextContinuesSeed(L"anything at all", L""), "seed: NOTHING seeded is never 'ours' (the whole point of the guard)");
        CHECK(!TextContinuesSeed(L"pulled", L"pulled draft"), "seed: a box shortened BELOW the seed no longer continues it");
        CHECK(TextContinuesSeed(L"line one\rline two", L"line one\nline two"), "seed: the newline fold applies here too");
        // ---- the similarity metric itself ----
        CHECK(DraftSimilarityPercent(L"", L"") == 100, "similarity: two empty texts are identical");
        CHECK(DraftSimilarityPercent(L"", L"x") == 0, "similarity: one empty text shares nothing");
        CHECK(DraftSimilarityPercent(L"abcdefghij", L"abcdefghij") == 100, "similarity: identical texts score 100");
        CHECK(DraftSimilarityPercent(L"abcdefghij", L"abcXefghij") == 90, "similarity: one changed char of ten -> 90 (prefix 3 + suffix 6)");
        CHECK(DraftSimilarityPercent(L"abcdefghij", L"klmnopqrst") == 0, "similarity: nothing shared at either edge -> 0");
        CHECK(DraftSimilarityPercent(L"abcde", L"abcdefghij") == 50, "similarity: a prefix scores its share of the LONGER text");
        // It can never over-report (prefix + suffix can never exceed the shorter text): the scan for the
        // suffix stops where the prefix ended, so an overlapping run is counted once.
        CHECK(DraftSimilarityPercent(L"aaaa", L"aaaaaaaa") == 50, "similarity: prefix/suffix runs never double-count");
    }
    // 26. The DRAFT SWAP control codes + the clear ladder (PENDING_INPUT.md §9). The swap empties the
    // input box before a queued prompt is submitted into it, then puts the draft back — so the ladder
    // must terminate on every path, and must NEVER report "cleared" for a box that still has text
    // (the whole abort contract hangs on that: no confirmed-empty box => nothing is sent).
    {
        CHECK(BuildInputStash() == std::wstring(1, static_cast<wchar_t>(0x13)), "draft swap: stash is a lone Ctrl+S (0x13)");
        CHECK(BuildInputKill() == std::wstring(1, static_cast<wchar_t>(0x15)), "draft swap: kill is a lone Ctrl+U (0x15)");
        CHECK(BuildInputYank() == std::wstring(1, static_cast<wchar_t>(0x19)), "draft swap: yank is a lone Ctrl+Y (0x19)");
        CHECK(BuildBackspaces(3) == std::wstring(3, static_cast<wchar_t>(0x7f)), "draft swap: backspaces are DEL (0x7f), one per char");
        CHECK(BuildBackspaces(0).empty(), "draft swap: zero backspaces is empty");
        CHECK(BuildBackspaces(kMaxBackspacesPerRound + 500).size() == kMaxBackspacesPerRound, "draft swap: a backspace round is capped");

        const auto spent = [](uint32_t stashes, uint32_t kills, uint32_t rounds, bool shrank, bool useStash = true) {
            DraftClearProgress p;
            p.stashPresses = stashes;
            p.killPresses = kills;
            p.backspaceRounds = rounds;
            p.shrank = shrank;
            p.useStash = useStash;
            return p;
        };

        // An empty (or whitespace-only) box is Done — the same emptiness rule the pick above uses.
        CHECK(DecideDraftClear(L"", spent(0, 0, 0, false)).action == DraftClearAction::Done, "draft clear: empty box -> Done");
        CHECK(DecideDraftClear(L"   " + NL, spent(1, 0, 0, true)).action == DraftClearAction::Done, "draft clear: whitespace-only box -> Done");

        // The happy path: ONE Ctrl+S stashes the whole box aside, and the next read is empty.
        CHECK(DecideDraftClear(L"my draft", spent(0, 0, 0, false)).action == DraftClearAction::Stash, "draft clear: first look at a filled box -> Stash (Ctrl+S)");

        // THE CRITICAL CAP: Ctrl+S is a TOGGLE, so a second press would un-stash the draft straight
        // back into the box. The ladder must never press it twice — it hands over to Ctrl+U instead.
        CHECK(kMaxDraftStashPresses == 1u, "draft clear: the stash rung is exactly ONE press (it is a toggle)");
        CHECK(DecideDraftClear(L"still there", spent(1, 0, 0, false)).action == DraftClearAction::Kill, "draft clear: a stash that did nothing -> Kill, never a second Ctrl+S");
        CHECK(DecideDraftClear(L"still there", spent(1, 0, 0, true)).action == DraftClearAction::Kill, "draft clear: even a stash that SHRANK the box never presses Ctrl+S twice");

        // With the setting off the stash rung is skipped entirely and the ladder starts at Ctrl+U.
        CHECK(DecideDraftClear(L"my draft", spent(0, 0, 0, false, false)).action == DraftClearAction::Kill, "draft clear: Ctrl+S disabled -> start at the kill rung");

        // A multi-line draft can take a kill press per line: keep pressing WHILE it is shrinking.
        CHECK(DecideDraftClear(L"line 2", spent(1, 1, 0, true)).action == DraftClearAction::Kill, "draft clear: still shrinking -> Kill again");

        // A press that changed NOTHING means the binding is not the one we assumed: stop pressing it
        // and fall through to the erase rung rather than burning the remaining presses.
        {
            const auto p = DecideDraftClear(L"unchanged", spent(1, 1, 0, false));
            CHECK(p.action == DraftClearAction::Backspace, "draft clear: a kill that did not shrink -> Backspace");
            CHECK(p.backspaces > 9, "draft clear: backspaces cover the box plus a margin");
        }

        // The kill rung is bounded even while it keeps shrinking...
        CHECK(DecideDraftClear(L"still here", spent(1, kMaxDraftKillPresses, 0, true)).action == DraftClearAction::Backspace, "draft clear: kill presses are capped -> Backspace");
        // ...and once the ladder has moved on it never goes BACK to Stash or Kill (no alternating).
        CHECK(DecideDraftClear(L"still here", spent(0, 0, 1, true)).action == DraftClearAction::Backspace, "draft clear: past the earlier rungs, a shrinking box stays on Backspace");
        // The erase rung is bounded too, and then the swap gives up -> ABORT (nothing is sent).
        CHECK(DecideDraftClear(L"stubborn", spent(1, kMaxDraftKillPresses, kMaxDraftBackspaceRounds, false)).action == DraftClearAction::GiveUp, "draft clear: exhausted ladder -> GiveUp");
        CHECK(DecideDraftClear(L"stubborn", spent(0, 0, kMaxDraftBackspaceRounds, true)).action == DraftClearAction::GiveUp, "draft clear: exhausted backspaces -> GiveUp even while shrinking");

        // TERMINATION: driving the ladder with a worst-case TUI that ignores every keystroke must
        // reach GiveUp in a bounded number of steps — never loop forever — with or without the stash
        // rung, and pressing Ctrl+S AT MOST ONCE along the way.
        for (const bool useStash : { true, false })
        {
            DraftClearProgress p;
            p.useStash = useStash;
            int steps = 0;
            auto last = DraftClearAction::Kill;
            while (steps++ < 64)
            {
                const auto plan = DecideDraftClear(L"never changes", p);
                last = plan.action;
                if (plan.action == DraftClearAction::GiveUp || plan.action == DraftClearAction::Done)
                {
                    break;
                }
                if (plan.action == DraftClearAction::Stash)
                {
                    ++p.stashPresses;
                }
                else if (plan.action == DraftClearAction::Kill)
                {
                    ++p.killPresses;
                }
                else
                {
                    ++p.backspaceRounds;
                }
            }
            CHECK(last == DraftClearAction::GiveUp, "draft clear: an unresponsive TUI terminates at GiveUp");
            CHECK(p.stashPresses <= kMaxDraftStashPresses, "draft clear: Ctrl+S is pressed at most once across a whole run");
            CHECK(steps <= static_cast<int>(kMaxDraftStashPresses + kMaxDraftKillPresses + kMaxDraftBackspaceRounds) + 2, "draft clear: termination is bounded by the rung caps");
        }

        // §9 LEFTOVER BLANK ROWS ("clean up empty lines ... until no newlines appear"). The box TEXT
        // read drops trailing blank rows, so a box that reads empty can still SPAN several visual rows;
        // boxMultiRow (ReadInputBoxBodyRows > 1) is the separate fact the ladder acts on.
        //   * kill-ring mode: an empty box that still spans rows is NOT Done — keep pressing Ctrl+U to
        //     collapse the "" rows one per press; a single-row empty box IS Done.
        CHECK(DecideDraftClear(L"", spent(0, 0, 0, false, false), /*boxMultiRow*/ true).action == DraftClearAction::Kill, "draft clear: empty box that still spans blank rows -> Kill (collapse the newlines)");
        CHECK(DecideDraftClear(L"", spent(0, 1, 0, true, false), /*boxMultiRow*/ true).action == DraftClearAction::Kill, "draft clear: a still-shrinking multi-row empty box keeps Killing the blank rows");
        CHECK(DecideDraftClear(L"", spent(0, 0, 0, false, false), /*boxMultiRow*/ false).action == DraftClearAction::Done, "draft clear: an empty SINGLE-row box is Done");
        //   * stash mode is exempt — Ctrl+S already took the WHOLE box (newlines included) in one press,
        //     so an empty box there is finished regardless of the span (a stray Ctrl+U afterward would
        //     only muddy which rung to restore through).
        CHECK(DecideDraftClear(L"", spent(1, 0, 0, false, true), /*boxMultiRow*/ true).action == DraftClearAction::Done, "draft clear: stash mode ignores the row span (Ctrl+S already took the whole box)");
    }
    // 27. The MOVE-clear DISCARD ladder (PENDING_INPUT.md §8d). The mail button's plain click MOVES
    // the draft into the queue, so its clear must DESTROY the box copy — never Ctrl+S: a stashed
    // draft is AUTO-RESTORED by claude ~0.4s after the NEXT submit (measured live), which re-planted
    // a moved draft right after its own queued copy was delivered. The discard rungs (End+Ctrl+U,
    // then End+Backspaces) are measured true discards; the ladder must tolerate the measured no-op
    // kill round between lines, stay forward-only, and terminate bounded like the swap's.
    {
        CHECK(BuildInputEnd() == L"\x1b[F", "discard: End is CSI F (ESC [ F)");

        const auto dspent = [](uint32_t killRounds, uint32_t killStalls, uint32_t bsRounds, uint32_t bsStalls) {
            DraftDiscardProgress p;
            p.killRounds = killRounds;
            p.killStalls = killStalls;
            p.backspaceRounds = bsRounds;
            p.backspaceStalls = bsStalls;
            return p;
        };

        // Emptiness — the same whitespace rule as everywhere else.
        CHECK(DecideDraftDiscard(L"", dspent(0, 0, 0, 0)).action == DraftDiscardAction::Done, "discard: empty box -> Done");
        CHECK(DecideDraftDiscard(L"   " + NL, dspent(3, 1, 1, 0)).action == DraftDiscardAction::Done, "discard: whitespace-only box -> Done whatever was spent");

        // The happy path starts (and stays) on End+Ctrl+U.
        CHECK(DecideDraftDiscard(L"my draft", dspent(0, 0, 0, 0)).action == DraftDiscardAction::EndKill, "discard: first look at a filled box -> End+Ctrl+U");
        // The measured 3-line alternation: a kill round BETWEEN lines can change nothing once —
        // one stalled round keeps the rung, two consecutive dead rounds judge it dead.
        CHECK(DecideDraftDiscard(L"two lines left", dspent(2, 1, 0, 0)).action == DraftDiscardAction::EndKill, "discard: ONE stalled kill round is tolerated (the measured no-op between lines)");
        CHECK(DecideDraftDiscard(L"unmoved", dspent(2, kDiscardStallLimit, 0, 0)).action == DraftDiscardAction::EndBackspace, "discard: two consecutive dead kill rounds -> End+Backspaces");
        CHECK(DecideDraftDiscard(L"still here", dspent(kMaxDiscardKillRounds, 0, 0, 0)).action == DraftDiscardAction::EndBackspace, "discard: kill rounds are capped -> End+Backspaces");
        {
            const auto p = DecideDraftDiscard(L"0123456789", dspent(0, kDiscardStallLimit, 0, 0));
            CHECK(p.action == DraftDiscardAction::EndBackspace, "discard: a dead kill rung hands over to the erase rung");
            CHECK(p.backspaces > 10, "discard: backspaces cover the box plus a margin");
        }
        // Forward-only: once a backspace round has run, the kill rung is never revisited.
        CHECK(DecideDraftDiscard(L"tail", dspent(1, 0, 1, 0)).action == DraftDiscardAction::EndBackspace, "discard: past the kill rung the ladder never alternates back");
        // The erase rung is bounded by its cap AND its own stall counter, then GiveUp (draft left —
        // it is already queued, a Shift+Click-equivalent end state).
        CHECK(DecideDraftDiscard(L"stubborn", dspent(1, kDiscardStallLimit, kMaxDiscardBackspaceRounds, 0)).action == DraftDiscardAction::GiveUp, "discard: exhausted backspace rounds -> GiveUp");
        CHECK(DecideDraftDiscard(L"stubborn", dspent(0, kDiscardStallLimit, 2, kDiscardStallLimit)).action == DraftDiscardAction::GiveUp, "discard: two dead backspace rounds -> GiveUp");

        // TERMINATION against a worst-case TUI that ignores every keystroke: the stall limits bound
        // the whole run at 2 dead rounds per rung — never the (much larger) runaway caps.
        {
            DraftDiscardProgress p;
            int steps = 0;
            auto last = DraftDiscardAction::EndKill;
            while (steps++ < 64)
            {
                const auto plan = DecideDraftDiscard(L"never changes", p);
                last = plan.action;
                if (plan.action == DraftDiscardAction::GiveUp || plan.action == DraftDiscardAction::Done)
                {
                    break;
                }
                if (plan.action == DraftDiscardAction::EndKill)
                {
                    ++p.killRounds;
                    ++p.killStalls; // nothing changed — the unresponsive TUI
                }
                else
                {
                    ++p.backspaceRounds;
                    ++p.backspaceStalls;
                }
            }
            CHECK(last == DraftDiscardAction::GiveUp, "discard: an unresponsive TUI terminates at GiveUp");
            CHECK(steps <= static_cast<int>(2 * kDiscardStallLimit) + 2, "discard: termination is bounded by the stall limits (2 dead rounds per rung)");
        }
        // The measured 3-line kill sequence (change / no-op / change / no-op / change) never leaves
        // the kill rung and ends Done — the caller-side stall accounting the ladder is designed for.
        {
            DraftDiscardProgress p;
            const wchar_t* boxes[] = { L"one alpha\ntwo beta\nthree gamma", L"one alpha\ntwo beta", L"one alpha\ntwo beta", L"one alpha", L"one alpha", L"" };
            bool stayedOnKillRung = true;
            for (int i = 0; i + 1 < 6; ++i)
            {
                const auto plan = DecideDraftDiscard(boxes[i], p);
                if (plan.action != DraftDiscardAction::EndKill)
                {
                    stayedOnKillRung = false;
                    break;
                }
                const bool changed = std::wstring_view{ boxes[i] } != std::wstring_view{ boxes[i + 1] };
                ++p.killRounds;
                p.killStalls = changed ? 0 : p.killStalls + 1;
            }
            CHECK(stayedOnKillRung, "discard: the measured 3-line alternation stays on the kill rung throughout");
            CHECK(DecideDraftDiscard(L"", p).action == DraftDiscardAction::Done, "discard: ...and the emptied box ends Done");
        }

        // §9 LEFTOVER BLANK ROWS ("clean up empty lines ... until no newlines appear"). The box TEXT
        // read drops trailing blank rows, so an empty-reading box can still SPAN rows; boxMultiRow
        // (ReadInputBoxBodyRows > 1) keeps End+Ctrl+U collapsing the "" rows until the box is one row.
        CHECK(DecideDraftDiscard(L"", dspent(0, 0, 0, 0), /*boxMultiRow*/ true).action == DraftDiscardAction::EndKill, "discard: empty box that still spans blank rows -> End+Ctrl+U (collapse the newlines)");
        CHECK(DecideDraftDiscard(L"", dspent(5, 1, 0, 0), /*boxMultiRow*/ true).action == DraftDiscardAction::EndKill, "discard: keep collapsing blank rows while the kill rung is alive");
        CHECK(DecideDraftDiscard(L"", dspent(0, 0, 0, 0), /*boxMultiRow*/ false).action == DraftDiscardAction::Done, "discard: an empty SINGLE-row box is Done");
        // The all-blank-box collapse (span 4 -> 3 -> 2 -> 1): the text reads "" throughout, but each
        // End+Ctrl+U drops one row, which the caller records as a change (killStalls stays 0) — so the
        // rung never falsely stalls to backspaces before the newlines are gone. Ends Done at one row.
        {
            DraftDiscardProgress p;
            bool stayedOnKillRung = true;
            for (int span = 4; span > 1; --span)
            {
                const auto plan = DecideDraftDiscard(L"", p, /*boxMultiRow*/ true);
                if (plan.action != DraftDiscardAction::EndKill)
                {
                    stayedOnKillRung = false;
                    break;
                }
                ++p.killRounds;
                p.killStalls = 0; // the row span dropped => progress, never a stall
            }
            CHECK(stayedOnKillRung, "discard: collapsing consecutive blank rows stays on the kill rung (a row drop is progress, not a stall)");
            CHECK(DecideDraftDiscard(L"", p, /*boxMultiRow*/ false).action == DraftDiscardAction::Done, "discard: ...and the single-row empty box ends Done");
        }
    }
}

// Agentmaster (PENDING_INPUT.md §2b): the paste-cache resolver — marker grammar, the two counting
// conventions, content-anchored validation arithmetic (the REAL wow-sess numbers), ambiguity refusal,
// expansion, and the impure ClaudeSpawn adapter against a temp dir.
void TestPendingPaste()
{
    std::wprintf(L"-- PendingPaste (paste-cache resolver) --\n");
    const std::wstring NL(1, static_cast<wchar_t>(10));

    // Build a synthetic 274-SEGMENT file shaped like the live fixture (bf8eefafa3e80676.txt):
    // segment 8 = the head-cut line, segment 266 = the tail-resume line, last segment UNTERMINATED
    // (273 newlines / 274 segments — the exact shape whose collapsed marker read "+273 lines").
    std::wstring paste;
    for (int i = 1; i <= 274; ++i)
    {
        if (i == 8)
        {
            paste += L"## The user's report (verbatim)";
        }
        else if (i == 266)
        {
            paste += L"  shells, or if the About-tab toggle's \"applies at next start\" story changes).";
        }
        else
        {
            paste += L"line " + std::to_wstring(i);
        }
        if (i != 274)
        {
            paste += NL; // no trailing newline — the live file ends mid-segment
        }
    }

    // 1. Marker grammar.
    {
        const auto ms = FindPasteMarkers(L"yooo" + NL + L"[Pasted text #1 +273 lines]" + NL + L"tail");
        CHECK(ms.size() == 1, "paste grammar: one collapsed marker");
        CHECK(!ms[0].truncated && ms[0].index == 1 && ms[0].lines == 273, "paste grammar: collapsed N/M parsed");
        CHECK(ms[0].draftLine == 1 && ms[0].headFragment.empty() && ms[0].tailFragment.empty(), "paste grammar: whole-line collapsed has no fragments");

        const auto mt = FindPasteMarkers(L"## The user[...Truncated text #2 +258 lines...]es).");
        CHECK(mt.size() == 1, "paste grammar: one truncated marker");
        CHECK(mt[0].truncated && mt[0].index == 2 && mt[0].lines == 258, "paste grammar: truncated N/M parsed");
        CHECK(mt[0].headFragment == L"## The user" && mt[0].tailFragment == L"es).", "paste grammar: fragments captured");

        CHECK(FindPasteMarkers(L"[Pasted text #1 +2 line]").size() == 1, "paste grammar: singular 'line' accepted");
        CHECK(FindPasteMarkers(L"[Pasted text]").empty(), "paste grammar: countless bracket ignored");
        CHECK(FindPasteMarkers(L"[Image #3]").empty(), "paste grammar: image marker ignored");
        CHECK(FindPasteMarkers(L"[Pasted text #1 +5 linesX]").empty(), "paste grammar: unterminated marker ignored");
        CHECK(FindPasteMarkers(L"a [Pasted text #1 +5 lines] b [Pasted text #2 +9 lines]").size() == 2, "paste grammar: two markers on one line");
    }
    // 2. Counting conventions + collapsed validation. The live file proved M == the NEWLINE count of
    // an unterminated 274-segment file; a terminated file's M == its segment count. Both accepted.
    {
        PasteMarker m;
        m.truncated = false;
        m.lines = 273;
        CHECK(ValidatePasteFile(m, paste), "paste collapsed: M == newline count of the unterminated live shape");
        m.lines = 274;
        CHECK(ValidatePasteFile(m, paste), "paste collapsed: M == segment count also accepted");
        m.lines = 272;
        CHECK(!ValidatePasteFile(m, paste), "paste collapsed: off-by-two refused");
        m.lines = 3;
        CHECK(ValidatePasteFile(m, L"a" + NL + L"b" + NL + L"c" + NL), "paste collapsed: terminated 3-liner matches +3");
    }
    // 3. Truncated validation — the REAL arithmetic: head "## The user" prefixes segment 8, tail
    // "es)." suffixes segment 266, and 266 - 8 == 258 == M. Refusals: wrong M, thin anchors.
    {
        PasteMarker m;
        m.truncated = true;
        m.lines = 258;
        m.headFragment = L"## The user";
        m.tailFragment = L"es).";
        size_t headSeg = 0;
        CHECK(ValidatePasteFile(m, paste, &headSeg), "paste truncated: the live arithmetic validates");
        CHECK(headSeg == 8, "paste truncated: head-cut segment located");
        m.lines = 257;
        CHECK(!ValidatePasteFile(m, paste), "paste truncated: off-by-one M refused");
        m.lines = 258;
        m.headFragment = L"##";
        m.tailFragment = L"x";
        CHECK(!ValidatePasteFile(m, paste), "paste truncated: thin anchors refused (never guess)");
    }
    // 4. Resolution: unique match wins; two count-identical files are AMBIGUOUS -> refused.
    {
        PasteMarker m;
        m.truncated = false;
        m.lines = 273;
        std::vector<PasteFileText> files{ { L"real.txt", paste }, { L"other.txt", L"just one line" } };
        auto res = ResolvePasteMarkers({ m }, files);
        CHECK(res.size() == 1 && res[0].resolved && res[0].fileName == L"real.txt", "paste resolve: unique match resolves");
        files.push_back({ L"twin.txt", paste });
        res = ResolvePasteMarkers({ m }, files);
        CHECK(res.size() == 1 && !res[0].resolved, "paste resolve: two matching files -> refused (ambiguous)");
    }
    // 5. Expansion: a whole-line collapsed marker expands to the file; typed text around a collapsed
    // marker refuses; the truncated form substitutes segments i..i+M (subsuming the fragments).
    {
        PasteMarker m = FindPasteMarkers(L"before" + NL + L"[Pasted text #1 +273 lines]" + NL + L"after")[0];
        const auto expanded = ExpandPasteMarker(L"before" + NL + L"[Pasted text #1 +273 lines]" + NL + L"after", m, paste);
        CHECK(!expanded.empty(), "paste expand: collapsed whole-line expands");
        CHECK(expanded.find(L"before" + NL + L"line 1" + NL) == 0, "paste expand: head stitched");
        CHECK(expanded.find(NL + L"after") == expanded.size() - 6, "paste expand: tail stitched");
        CHECK(expanded.find(L"## The user's report (verbatim)") != std::wstring::npos, "paste expand: real content present");

        const std::wstring inlineDraft = L"typed [Pasted text #1 +273 lines] more";
        const auto im = FindPasteMarkers(inlineDraft);
        CHECK(im.size() == 1 && ExpandPasteMarker(inlineDraft, im[0], paste).empty(), "paste expand: typed text around a collapsed marker refuses (never eat the user's words)");

        const std::wstring truncDraft = L"yooo" + NL + L"## The user[...Truncated text #2 +258 lines...]es).";
        const auto tm = FindPasteMarkers(truncDraft);
        const auto texp = ExpandPasteMarker(truncDraft, tm[0], paste);
        CHECK(!texp.empty(), "paste expand: truncated expands");
        CHECK(texp.find(L"yooo" + NL + L"## The user's report (verbatim)" + NL) == 0, "paste expand: head-cut segment restored in full");
        CHECK(texp.find(L"story changes).") != std::wstring::npos, "paste expand: tail-resume segment restored in full");
        CHECK(ExpandPasteMarker(truncDraft, tm[0], L"wrong file").empty(), "paste expand: failed validation refuses");
    }
    // 6. The impure adapter (ClaudeSpawn): a temp cache dir round-trip + the annotation format.
    {
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / L"am-paste-test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        {
            std::ofstream f(dir / L"aabbccdd00112233.txt", std::ios::binary);
            const std::string utf8 = "alpha\nbeta\ngamma"; // 2 newlines / 3 segments
            f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        }
        const auto refs = ResolvePendingPasteRefsIn(L"x" + NL + L"[Pasted text #1 +3 lines]" + NL + L"[Pasted text #2 +99 lines]", dir.wstring());
        CHECK(refs == L"paste #1 (+3 lines) -> aabbccdd00112233.txt (by line count)" + NL + L"paste #2 (+99 lines) -> unresolved",
              "paste adapter: annotation lines (resolved [count tier labeled] + unresolved)");
        CHECK(ResolvePendingPasteRefsIn(L"no markers here", dir.wstring()).empty(), "paste adapter: no markers -> empty");
        CHECK(ResolvePendingPasteRefsIn(L"[Pasted text #1 +3 lines]", (dir / L"missing").wstring()) == L"paste #1 (+3 lines) -> unresolved",
              "paste adapter: missing cache dir -> unresolved, never a throw");
        // The §10 whole-draft expansion adapter against the same temp cache (the restore re-fill's
        // fill-text source): all-resolvable expands, any unresolvable refuses the WHOLE draft, a
        // marker-free draft passes through verbatim, a missing dir refuses rather than throws.
        {
            const auto ex = ExpandPendingDraftPastesIn(L"x" + NL + L"[Pasted text #1 +3 lines]", dir.wstring());
            CHECK(ex.complete && ex.expandedCount == 1 && ex.text == L"x" + NL + L"alpha" + NL + L"beta" + NL + L"gamma",
                  "expand adapter: a resolvable draft expands whole (fill text carries the real content)");
        }
        {
            const auto ex = ExpandPendingDraftPastesIn(L"x" + NL + L"[Pasted text #1 +3 lines]" + NL + L"[Pasted text #2 +99 lines]", dir.wstring());
            CHECK(!ex.complete && ex.text.empty(), "expand adapter: one unresolvable marker refuses the WHOLE draft (never a lossy literal re-type)");
        }
        {
            const auto ex = ExpandPendingDraftPastesIn(L"plain draft, no markers", dir.wstring());
            CHECK(ex.complete && ex.expandedCount == 0 && ex.text == L"plain draft, no markers", "expand adapter: marker-free draft passes through verbatim");
        }
        {
            const auto ex = ExpandPendingDraftPastesIn(L"[Pasted text #1 +3 lines]", (dir / L"missing").wstring());
            CHECK(!ex.complete && ex.text.empty(), "expand adapter: missing cache dir -> refused, never a throw");
        }
        fs::remove_all(dir, ec);
    }
    // 7. The PURE whole-draft expansion (PENDING_INPUT.md §10 — ExpandDraftPasteMarkers): the
    // all-or-refuse brain the restore re-fill's fill text comes from. Refusal cases mirror the §9
    // draft-swap rule: a placeholder that cannot be verified is never re-typed as a literal label.
    {
        // Two distinct cache files whose counts can't cross-match (file A: 2 newlines / 3 segments;
        // file B: 4 newlines / 5 segments — "+3" hits only A, "+5" hits only B).
        const std::vector<PasteFileText> files{
            { L"a.txt", L"alpha" + NL + L"beta" + NL + L"gamma" },
            { L"b.txt", L"uno" + NL + L"dos" + NL + L"tres" + NL + L"quatro" + NL + L"cinco" },
        };
        {
            const auto ex = ExpandDraftPasteMarkers(L"no markers at all", files);
            CHECK(ex.complete && ex.expandedCount == 0 && ex.text == L"no markers at all", "expand pure: marker-free draft is trivially complete");
        }
        {
            const auto ex = ExpandDraftPasteMarkers(L"head" + NL + L"[Pasted text #1 +3 lines]" + NL + L"mid" + NL + L"[Pasted text #2 +5 lines]" + NL + L"tail", files);
            CHECK(ex.complete && ex.expandedCount == 2, "expand pure: every marker on its own line expands");
            CHECK(ex.text == L"head" + NL + L"alpha" + NL + L"beta" + NL + L"gamma" + NL + L"mid" + NL + L"uno" + NL + L"dos" + NL + L"tres" + NL + L"quatro" + NL + L"cinco" + NL + L"tail",
                  "expand pure: LAST-first substitution keeps earlier markers' recorded lines valid (order + stitching exact)");
        }
        {
            const auto ex = ExpandDraftPasteMarkers(L"[Pasted text #1 +3 lines]" + NL + L"[Pasted text #2 +99 lines]", files);
            CHECK(!ex.complete && ex.text.empty(), "expand pure: any unresolved marker refuses the whole draft");
        }
        {
            const auto ex = ExpandDraftPasteMarkers(L"[Pasted text #1 +3 lines]", { files[0], { L"twin.txt", files[0].text } });
            CHECK(!ex.complete, "expand pure: an ambiguous marker (two count-identical files) refuses");
        }
        {
            const auto ex = ExpandDraftPasteMarkers(L"a [Pasted text #1 +3 lines] b [Pasted text #2 +5 lines]", files);
            CHECK(!ex.complete, "expand pure: two markers sharing one draft line refuse (expansion is line-replacing — either would eat the other)");
        }
        {
            const auto ex = ExpandDraftPasteMarkers(L"typed [Pasted text #1 +3 lines] more", files);
            CHECK(!ex.complete, "expand pure: typed text around a collapsed marker refuses the whole (ExpandPasteMarker's never-eat-the-user's-words rule)");
        }
        {
            // The REAL truncated fixture (the 274-segment live shape from the top of this test):
            // the restore re-fill's flagship case — the wow-sess draft expanding to the briefing.
            const std::wstring truncDraft = L"yooo" + NL + L"## The user[...Truncated text #2 +258 lines...]es).";
            const auto ex = ExpandDraftPasteMarkers(truncDraft, { { L"real.txt", paste } });
            CHECK(ex.complete && ex.expandedCount == 1, "expand pure: the live truncated arithmetic expands whole-draft");
            CHECK(ex.text.find(L"yooo" + NL + L"## The user's report (verbatim)" + NL) == 0 && ex.text.find(L"story changes).") != std::wstring::npos,
                  "expand pure: truncated head-cut + tail-resume segments restored in full");
        }
    }
}
