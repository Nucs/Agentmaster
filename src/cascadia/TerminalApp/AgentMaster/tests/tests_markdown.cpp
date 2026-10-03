// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - M5 standalone test harness: MARKDOWN BLOCKS — the transcript overlay's Markdown flattener
// (AgentMaster/MarkdownBlocks.{h,cpp}: cmark's tree walked once into flat render blocks, each carrying the
// quotes / list items around it). Pins what the overlay's XAML builder relies on: block kinds and order,
// the container chain + ids (a quote bar runs through the gap between two paragraphs of ONE quote, never
// across two quotes), list markers (only on an item's first block; numbered lists keep their numbers and
// share one marker width), the gaps MdGapAbove picks, raw HTML kept as text, soft breaks kept as line
// breaks, the caps, and a real transcript_turns golden parsed end to end.
//
// cmark is the one dependency the rest of the harness doesn't have: run-m5-tests.bat compiles this test
// for real only when it finds cmark in the vcpkg tree a full msbuild installs (obj\x64\vcpkg — it defines
// AM_HAVE_CMARK and links cmark.lib); otherwise the entry point reports a skip, so the suite never fails
// to build for want of a build tree. Shared CHECK/fixtures/decls live in m5_tests.h.
#include "m5_tests.h"

#ifdef AM_HAVE_CMARK

#include "../MarkdownBlocks.h"

#include <fstream>
#include <iterator> // std::size / istreambuf_iterator

namespace
{
    namespace fs = std::filesystem;

    // The concatenated text of a block's runs (what the overlay's TextBlock shows, styles aside).
    std::wstring BlockText(const MdBlock& block)
    {
        std::wstring text;
        for (const auto& run : block.runs)
        {
            text += run.text;
        }
        return text;
    }

    // The folder m5_tests.exe lives in (the turns\expected goldens sit beside it); "." when unreadable.
    fs::path MarkdownHarnessDir()
    {
        wchar_t buffer[MAX_PATH * 4]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        if (n == 0 || n >= std::size(buffer))
        {
            return fs::path{ L"." };
        }
        return fs::path{ buffer }.parent_path();
    }

    // A UTF-8 file as UTF-16 ("" when it can't be read — the caller's checks then fail visibly).
    std::wstring ReadUtf8File(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (n <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
        return out;
    }
}

#endif // AM_HAVE_CMARK

void TestMarkdownBlocks()
{
    std::wprintf(L"\n[MarkdownBlocks]\n");
#ifndef AM_HAVE_CMARK
    std::wprintf(L"  [info] skipped: cmark not found under obj\\x64\\vcpkg (run one full msbuild of the solution, then re-run)\n");
#else
    // ---- empty input ----
    {
        CHECK(ParseMarkdownBlocks(L"").blocks.empty(), "md: an empty document has no blocks");
        CHECK(ParseMarkdownBlocks(L"   \n\n  \n").blocks.empty(), "md: a whitespace-only document has no blocks");
    }

    // ---- a transcript_turns turn: heading, speaker label, quoted message (soft break kept) ----
    {
        const auto r = ParseMarkdownBlocks(L"### Turn 1 \x00B7 Jan 02 10:00\n\n**\x276F User**\n> list the files\n> and run the scan\n");
        CHECK(!r.failed && r.blocks.size() == 3, "md: heading + label + quote -> 3 blocks");
        if (r.blocks.size() == 3)
        {
            CHECK(r.blocks[0].kind == MdBlockKind::Heading && r.blocks[0].headingLevel == 3, "md: ### is a level-3 heading");
            CHECK(BlockText(r.blocks[0]) == L"Turn 1 \x00B7 Jan 02 10:00", "md: heading text");
            CHECK(MdIsLabelParagraph(r.blocks[1]), "md: **\x276F User** is a label line");
            CHECK(r.blocks[1].runs.size() == 1 && (r.blocks[1].runs[0].style & kMdBold) != 0, "md: the label is one bold run");
            CHECK(r.blocks[2].containers.size() == 1 && r.blocks[2].containers[0].kind == MdContainerKind::Quote, "md: the message sits in one quote");
            CHECK(BlockText(r.blocks[2]) == L"list the files\nand run the scan", "md: a soft break stays a line break");
            CHECK(MdGapAbove(nullptr, r.blocks[0]) == 0.0, "md: no gap above the first block");
            CHECK(MdGapAbove(&r.blocks[0], r.blocks[1]) == 6.0, "md: the block under a heading sits close");
            CHECK(MdGapAbove(&r.blocks[1], r.blocks[2]) == 4.0, "md: the block under a label sits close");
        }
        // Full / Max: the stats line follows the label with ONE newline, so CommonMark folds both into one
        // paragraph — it must still count as a label (first line bold), and the list under it sit close.
        const auto stats = ParseMarkdownBlocks(L"**\x25CF Claude**\n_3 tool calls \x00B7 1m 30s_\n- \xD83D\xDCAC looking\n"); // the speech-balloon narration glyph as its UTF-16 surrogate pair (one \x escape holds 16 bits)
        CHECK(stats.blocks.size() == 2 && MdIsLabelParagraph(stats.blocks[0]), "md: a label with a folded stats line is still a label");
        CHECK(stats.blocks.size() == 2 && MdGapAbove(&stats.blocks[0], stats.blocks[1]) == 4.0, "md: the list under a label + stats sits close");
        CHECK(!MdIsLabelParagraph(ParseMarkdownBlocks(L"**\x25CF finder-a** \x00B7 teammate \x00B7 scan").blocks.front()), "md: bold + plain text on the first line is not a label");
        MdParseOptions joined;
        joined.softBreaksAsLineBreaks = false;
        const auto j = ParseMarkdownBlocks(L"a\nb", joined);
        CHECK(j.blocks.size() == 1 && BlockText(j.blocks[0]) == L"a b", "md: soft breaks as spaces when asked");
    }

    // ---- a tight list with a nested list: markers, continuation, gaps ----
    {
        const auto r = ParseMarkdownBlocks(L"- one\n- two\n  - nested\n- three\n");
        CHECK(r.blocks.size() == 4, "md: tight list -> 4 paragraphs");
        if (r.blocks.size() == 4)
        {
            const auto& b = r.blocks;
            CHECK(b[0].containers.size() == 1 && b[0].containers[0].kind == MdContainerKind::Item && b[0].containers[0].marker == L"\x2022", "md: first item shows a bullet");
            CHECK(b[0].containers[0].tightList, "md: the list is tight");
            CHECK(b[1].containers.size() == 1 && b[1].containers[0].marker == L"\x2022", "md: second item shows a bullet");
            CHECK(b[2].containers.size() == 2, "md: the nested item sits inside item two");
            CHECK(b[2].containers.size() == 2 && b[2].containers[0].marker.empty() && b[2].containers[0].id == b[1].containers[0].id,
                  "md: item two's later block carries no marker (same container id)");
            CHECK(b[2].containers.size() == 2 && b[2].containers[1].marker == L"\x25E6", "md: a nested bullet steps to the hollow glyph");
            CHECK(b[0].containers[0].listId == b[3].containers[0].listId && b[0].containers[0].id != b[3].containers[0].id,
                  "md: sibling items share the list id, not the item id");
            CHECK(MdGapAbove(&b[0], b[1]) == 3.0, "md: tight sibling items are packed");
            CHECK(MdGapAbove(&b[1], b[2]) == 3.0, "md: a nested tight list starts packed");
            CHECK(MdGapAbove(&b[2], b[3]) == 3.0, "md: back to the outer list, still packed");
            CHECK(MdSharedContainerDepth(b[1], b[2]) == 1, "md: item two is shared with its nested list");
        }
        const auto loose = ParseMarkdownBlocks(L"- one\n\n- two\n");
        CHECK(loose.blocks.size() == 2 && !loose.blocks[0].containers.empty() && !loose.blocks[0].containers[0].tightList, "md: a blank line makes the list loose");
        CHECK(loose.blocks.size() == 2 && MdGapAbove(&loose.blocks[0], loose.blocks[1]) == 8.0, "md: loose items get paragraph spacing");
    }

    // ---- ordered lists keep their numbers and one marker width ----
    {
        const auto r = ParseMarkdownBlocks(L"9. nine\n10. ten\n");
        CHECK(r.blocks.size() == 2, "md: ordered list -> 2 items");
        if (r.blocks.size() == 2)
        {
            CHECK(r.blocks[0].containers[0].ordered && r.blocks[0].containers[0].marker == L"9.", "md: the first number is kept");
            CHECK(r.blocks[1].containers[0].marker == L"10.", "md: numbering continues");
            CHECK(r.blocks[0].containers[0].markerChars == 3 && r.blocks[1].containers[0].markerChars == 3, "md: both items size for the widest marker (10.)");
        }
        const auto paren = ParseMarkdownBlocks(L"1) a\n2) b\n");
        CHECK(paren.blocks.size() == 2 && paren.blocks[1].containers[0].marker == L"2)", "md: a paren delimiter is kept");
    }

    // ---- quotes: one quote's paragraphs share it; two quotes don't ----
    {
        const auto one = ParseMarkdownBlocks(L"> a\n>\n> b\n");
        CHECK(one.blocks.size() == 2 && MdSharedContainerDepth(one.blocks[0], one.blocks[1]) == 1, "md: two paragraphs of one quote share it");
        CHECK(one.blocks.size() == 2 && MdGapAbove(&one.blocks[0], one.blocks[1]) == 8.0, "md: a quote's paragraphs keep paragraph spacing");
        const auto two = ParseMarkdownBlocks(L"> a\n\n> b\n");
        CHECK(two.blocks.size() == 2 && MdSharedContainerDepth(two.blocks[0], two.blocks[1]) == 0, "md: two separate quotes share nothing");
        const auto nested = ParseMarkdownBlocks(L"> outer\n>\n> > inner\n");
        CHECK(nested.blocks.size() == 2 && nested.blocks[1].containers.size() == 2, "md: a nested quote has two containers");
        const auto listInQuote = ParseMarkdownBlocks(L"> - x\n> - y\n");
        CHECK(listInQuote.blocks.size() == 2 && listInQuote.blocks[0].containers.size() == 2 &&
                  listInQuote.blocks[0].containers[0].kind == MdContainerKind::Quote && listInQuote.blocks[0].containers[1].kind == MdContainerKind::Item,
              "md: a list inside a quote nests outermost-first");
    }

    // ---- raw HTML is kept as text ----
    {
        const auto inlineHtml = ParseMarkdownBlocks(L"use List<int> here");
        CHECK(inlineHtml.blocks.size() == 1 && BlockText(inlineHtml.blocks[0]) == L"use List<int> here", "md: inline HTML is kept, plain");
        const auto blockHtml = ParseMarkdownBlocks(L"<details>\nsome text\n</details>\n");
        CHECK(blockHtml.blocks.size() == 1 && blockHtml.blocks[0].rawHtml, "md: an HTML block becomes a raw-HTML paragraph");
        CHECK(blockHtml.blocks.size() == 1 && !blockHtml.blocks[0].runs.empty() && (blockHtml.blocks[0].runs[0].style & kMdCode) != 0 &&
                  BlockText(blockHtml.blocks[0]).find(L"<details>") != std::wstring::npos,
              "md: the HTML block keeps its literal text, monospace");
    }

    // ---- code, rule, inline styles, links, images ----
    {
        const auto code = ParseMarkdownBlocks(L"```bash title=x\nls -la\npwd\n```\n");
        CHECK(code.blocks.size() == 1 && code.blocks[0].kind == MdBlockKind::Code, "md: a fenced block is Code");
        CHECK(code.blocks.size() == 1 && code.blocks[0].code == L"ls -la\npwd", "md: code without the trailing newline");
        CHECK(code.blocks.size() == 1 && code.blocks[0].info == L"bash", "md: the info string's first word is the language");
        const auto rule = ParseMarkdownBlocks(L"a\n\n---\n\nb\n");
        CHECK(rule.blocks.size() == 3 && rule.blocks[1].kind == MdBlockKind::Rule, "md: a thematic break is a Rule");
        CHECK(rule.blocks.size() == 3 && MdGapAbove(&rule.blocks[0], rule.blocks[1]) == 6.0, "md: a rule gets a little more air");

        const auto styled = ParseMarkdownBlocks(L"**bold** and *it* and `code` and [link](https://example.com/x)");
        CHECK(styled.blocks.size() == 1, "md: one styled paragraph");
        if (styled.blocks.size() == 1)
        {
            const auto& runs = styled.blocks[0].runs;
            CHECK(runs.size() == 7, "md: seven runs (styled pieces + plain joins)");
            if (runs.size() == 7)
            {
                CHECK(runs[0].text == L"bold" && runs[0].style == kMdBold, "md: bold run");
                CHECK(runs[1].text == L" and " && runs[1].style == 0, "md: plain join");
                CHECK(runs[2].text == L"it" && runs[2].style == kMdItalic, "md: italic run");
                CHECK(runs[4].text == L"code" && runs[4].style == kMdCode, "md: code span");
                CHECK(runs[6].text == L"link" && runs[6].style == kMdLink && runs[6].url == L"https://example.com/x", "md: link run + url");
            }
        }
        const auto image = ParseMarkdownBlocks(L"![a chart](https://example.com/c.png)");
        CHECK(image.blocks.size() == 1 && BlockText(image.blocks[0]) == L"[image: a chart]", "md: an image is never fetched - it reads [image: alt]");
        CHECK(image.blocks.size() == 1 && !image.blocks[0].runs.empty() && image.blocks[0].runs[0].url == L"https://example.com/c.png", "md: ...linked to its URL");
    }

    // ---- an empty list item still shows its marker ----
    {
        const auto r = ParseMarkdownBlocks(L"-\n- x\n");
        CHECK(r.blocks.size() == 2 && r.blocks[0].runs.empty() && !r.blocks[0].containers.empty() && r.blocks[0].containers[0].marker == L"\x2022",
              "md: an empty item emits an empty paragraph carrying its bullet");
    }

    // ---- the caps ----
    {
        MdParseOptions capped;
        capped.maxBlocks = 2;
        const auto r = ParseMarkdownBlocks(L"a\n\nb\n\nc\n", capped);
        CHECK(r.blocks.size() == 2 && r.truncatedBlocks, "md: maxBlocks stops the walk and flags it");
        MdParseOptions narrow;
        narrow.maxBlockChars = 5;
        const auto t = ParseMarkdownBlocks(L"abcdefghij", narrow);
        CHECK(t.blocks.size() == 1 && BlockText(t.blocks[0]) == L"abcde" && t.blocks[0].truncated, "md: maxBlockChars cuts the block and flags it");
        const auto c = ParseMarkdownBlocks(L"```\n0123456789\n```\n", narrow);
        CHECK(c.blocks.size() == 1 && c.blocks[0].code == L"01234" && c.blocks[0].truncated, "md: maxBlockChars cuts a code block too");
    }

    // ---- a real transcript_turns golden, end to end ----
    {
        const auto path = MarkdownHarnessDir() / L"turns" / L"expected" / L"agents-c1c1c1c1-max.md";
        const auto text = ReadUtf8File(path);
        CHECK(!text.empty(), "md: the agents max golden is readable (turns\\expected beside m5_tests.exe)");
        const auto r = ParseMarkdownBlocks(text);
        CHECK(!r.failed && r.blocks.size() > 30, "md: the golden parses into many blocks");
        size_t headings = 0;
        size_t labels = 0;
        bool bashCode = false;
        for (const auto& b : r.blocks)
        {
            headings += b.kind == MdBlockKind::Heading ? 1 : 0;
            labels += MdIsLabelParagraph(b) ? 1 : 0;
            bashCode = bashCode || (b.kind == MdBlockKind::Code && b.info == L"bash");
        }
        CHECK(headings == 5, "md: the golden's five ### Turn headings");
        CHECK(labels >= 10, "md: the golden's speaker labels are recognized");
        CHECK(bashCode, "md: the golden's bash block (inside a list item) is a Code block");
    }
#endif
}
