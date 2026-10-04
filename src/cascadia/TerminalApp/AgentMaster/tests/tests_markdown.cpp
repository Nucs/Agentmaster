// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - M5 standalone test harness: MARKDOWN BLOCKS — the transcript overlay's Markdown flattener
// (AgentMaster/MarkdownBlocks.{h,cpp}: cmark's tree walked once into flat render blocks, each carrying the
// quotes / list items around it). Pins what the overlay's XAML builder relies on: block kinds and order,
// the container chain + ids (a quote bar runs through the gap between two paragraphs of ONE quote, never
// across two quotes), list markers (only on an item's first block; numbered lists keep their numbers and
// share one marker width), the gaps MdGapAbove picks, raw HTML kept as text, soft breaks kept as line
// breaks, the caps, a real transcript_turns golden parsed end to end, and the TOOL AREAS the overlay folds
// (MdFindToolAreas / MdToolAreaFoldable / MdToolAreaSummary — which lists are areas, the item counts, the
// "(no tool calls)" note, the summary text) over an inline fixture and five goldens.
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

    // The tool areas of one turns\expected golden (read + parsed + found), for the fold checks. An
    // unreadable golden yields no areas — the caller's size checks then fail visibly.
    // Parameters: leaf — the golden's file name, e.g. L"agents-c1c1c1c1-default.md".
    std::vector<MdToolArea> GoldenToolAreas(const wchar_t* leaf)
    {
        const auto text = ReadUtf8File(MarkdownHarnessDir() / L"turns" / L"expected" / leaf);
        return MdFindToolAreas(ParseMarkdownBlocks(text).blocks);
    }

    // How many of `areas` the overlay folds (MdToolAreaFoldable).
    size_t CountFoldable(const std::vector<MdToolArea>& areas)
    {
        size_t n = 0;
        for (const auto& area : areas)
        {
            n += MdToolAreaFoldable(area) ? 1 : 0;
        }
        return n;
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
            CHECK(MdIsLabelParagraph(r.blocks[1]), "md: **U+276F User** is a label line");
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

    // ---- tool areas: what the overlay folds (every item kind, the empty note, a list that is NOT an area) ----
    {
        // Blocks: 0 heading · 1 user label · 2 its quote · 3 Claude label · 4..10 the first area (4 remark,
        // 5 Bash, 6 nested output, 7 Bash, 8 Write, 9 AskUserQuestion, 10 nested answer) · 11 reply ·
        // 12 label · 13 "(no tool calls)" · 14 reply · 15 "Session chain" paragraph · 16 its list ·
        // 17 label · 18 compaction · 19 Compact-level answer · 20 reply.
        const std::wstring doc =
            L"### Turn 1\n\n"
            L"**\x276F User**\n> do it\n\n"
            L"**\x25CF Claude**\n"
            L"- \xD83D\xDCAC Looking.\n" // the speech-balloon remark glyph as its surrogate pair
            L"- **Bash** \x2014 List: `ls` \x00B7 ok\n"
            L"  - \x21E2 `a.txt`\n"
            L"- **Bash** \x2014 Again \x00B7 ok\n"
            L"- **Write** `x.py` (2 lines) \x00B7 created\n"
            L"- **AskUserQuestion** \x2014 Which? \x00B7 ok\n"
            L"  - \x276F Which? \x2192 **A**\n\n"
            L"> Done.\n\n"
            L"**\x25CF Claude**\n- (no tool calls)\n\n> Nothing.\n\n"
            L"**Session chain**, oldest first:\n- `a` \x2014 gone\n\n"
            L"**\x25CF Claude**\n"
            L"- \xD83D\xDDDC context compacted\n" // the clamp compaction glyph as its surrogate pair
            L"- \x276F Which? \x2192 **B**\n\n"
            L"> Compact-level.\n";
        const auto r = ParseMarkdownBlocks(doc);
        CHECK(!r.failed && r.blocks.size() == 21, "md areas: the fixture parses into 21 blocks");
        const auto areas = MdFindToolAreas(r.blocks);
        CHECK(areas.size() == 3, "md areas: three areas - the Session chain list (under a plain paragraph) is not one");
        if (r.blocks.size() == 21 && areas.size() == 3)
        {
            const auto& a = areas[0];
            CHECK(a.first == 4 && a.end == 11, "md areas: the first area spans the list's blocks only (the reply after it is outside)");
            CHECK(a.toolCalls == 4 && a.remarks == 1 && a.answers == 1 && a.messages == 0 && a.prompts == 0 && a.compactions == 0 && a.others == 0,
                  "md areas: 4 calls, 1 remark, 1 nested answer (the nested output line is not counted)");
            CHECK(a.tools.size() == 3 && a.tools[0].first == L"Bash" && a.tools[0].second == 2 && a.tools[1].first == L"Write" && a.tools[2].first == L"AskUserQuestion",
                  "md areas: tool names from the bold lead, in first-call order, with counts");
            CHECK(MdToolAreaFoldable(a) && !a.empty, "md areas: an area with calls folds");
            CHECK(MdToolAreaSummary(a) == L"4 tool calls: Bash \x00D7" L"2, Write, AskUserQuestion \x00B7 1 remark \x00B7 1 answer",
                  "md areas: the summary names the calls (most-called first) and every other count");
            CHECK(MdToolAreaSummary(a, 1) == L"4 tool calls: Bash \x00D7" L"2 +2 more \x00B7 1 remark \x00B7 1 answer", "md areas: past maxTools the names collapse into +N more");
            CHECK(MdToolAreaSummary(a, 0) == L"4 tool calls \x00B7 1 remark \x00B7 1 answer", "md areas: maxTools 0 keeps only the count");
            CHECK(areas[1].first == 13 && areas[1].end == 14 && areas[1].empty && !MdToolAreaFoldable(areas[1]),
                  "md areas: a lone (no tool calls) item is an EMPTY area - not folded");
            CHECK(areas[2].first == 18 && areas[2].end == 20 && areas[2].compactions == 1 && areas[2].prompts == 1 && areas[2].toolCalls == 0,
                  "md areas: a Compact-shaped list counts its compaction + its top-level prompt");
            CHECK(!MdToolAreaFoldable(areas[2]), "md areas: a list with no call and no remark (Compact's content) stays open");
            CHECK(MdToolAreaSummary(areas[2]) == L"1 prompt \x00B7 1 compaction", "md areas: the summary of a call-less list");
        }
        // An agent entry header (bold name + plain details) is a speaker header too; a ❯ / ✉ label is not.
        const auto agent = MdFindToolAreas(ParseMarkdownBlocks(L"**\x25CF finder-a** \x00B7 teammate\n- \x2709 **lead**: go\n- **Read** `a.cpp`\n").blocks);
        CHECK(agent.size() == 1 && agent[0].messages == 1 && agent[0].toolCalls == 1 && MdToolAreaFoldable(agent[0]), "md areas: an agent entry's list is an area");
        const auto userList = MdFindToolAreas(ParseMarkdownBlocks(L"**\x276F User**\n- **Bash** \x00B7 ok\n").blocks);
        CHECK(userList.empty(), "md areas: a list under a non-speaker label is not an area");
        const auto quoted = MdFindToolAreas(ParseMarkdownBlocks(L"> **\x25CF Claude**\n> - **Bash** \x00B7 ok\n").blocks);
        CHECK(quoted.empty(), "md areas: a header + list inside a quoted message is not an area");
        // A very long tool name (an MCP tool) is cut to 40 characters, the last one an ellipsis. (Hyphens, not
        // the real "__" separators: intraword underscores are emphasis-neutral in CommonMark, but a test
        // input should not lean on that rule.)
        const auto longName = MdFindToolAreas(ParseMarkdownBlocks(L"**\x25CF Claude**\n- **mcp-catalog-server-with-an-extraordinarily-long-tool-name** \x00B7 ok\n").blocks);
        CHECK(longName.size() == 1 && longName[0].tools.size() == 1 && longName[0].tools[0].first.size() == 40 && longName[0].tools[0].first.back() == L'\x2026',
              "md areas: a long tool name is cut with an ellipsis");
        CHECK(MdFindToolAreas({}).empty() && MdToolAreaSummary(MdToolArea{}).empty(), "md areas: nothing in, nothing out");
    }

    // ---- tool areas over real transcript_turns goldens (every level shape the overlay meets) ----
    {
        const auto agentsDefault = GoldenToolAreas(L"agents-c1c1c1c1-default.md");
        CHECK(agentsDefault.size() == 9, "md golden areas: agents default - 5 speaker lists + 1 lead turn + 3 (no tool calls)");
        CHECK(CountFoldable(agentsDefault) == 6, "md golden areas: agents default - the six lists with calls fold");
        if (agentsDefault.size() == 9)
        {
            CHECK(agentsDefault[0].toolCalls == 3 && agentsDefault[0].remarks == 1 && MdToolAreaSummary(agentsDefault[0]) == L"3 tool calls: Agent \x00D7" L"3 \x00B7 1 remark",
                  "md golden areas: the lead's turn 1 - three Agent calls and a remark");
            CHECK(agentsDefault[1].messages == 2 && agentsDefault[1].remarks == 2 && agentsDefault[1].toolCalls == 2, "md golden areas: finder-a - 2 messages, 2 remarks, 2 calls");
            CHECK(agentsDefault[2].prompts == 1 && agentsDefault[2].toolCalls == 2, "md golden areas: Explore - its brief (a top-level prompt) + 2 calls");
            CHECK(agentsDefault[6].empty && agentsDefault[7].empty && agentsDefault[8].empty, "md golden areas: the three (no tool calls) turns are empty areas");
        }
        const auto agentsCompact = GoldenToolAreas(L"agents-c1c1c1c1-compact.md");
        CHECK(agentsCompact.size() == 4 && CountFoldable(agentsCompact) == 0, "md golden areas: agents compact - 4 agent lists (messages / briefs), none folds");
        const auto sessionMax = GoldenToolAreas(L"fixture_session-max.md");
        CHECK(sessionMax.size() == 3 && CountFoldable(sessionMax) == 2, "md golden areas: session max - the stats line inside the header does not hide the list");
        if (sessionMax.size() == 3)
        {
            CHECK(sessionMax[0].toolCalls == 4 && sessionMax[0].remarks == 2 && sessionMax[0].answers == 0 && sessionMax[0].tools.size() == 2 &&
                      sessionMax[0].tools[0].first == L"Bash" && sessionMax[0].tools[0].second == 3,
                  "md golden areas: session max turn 1 - Bash x3 + Write, 2 remarks; nested code / output / background lines are not answers");
            CHECK(sessionMax[1].empty, "md golden areas: session max turn 2 - (no tool calls) under a header with a stats line");
        }
        const auto questions = GoldenToolAreas(L"fixture_questions-default.md");
        CHECK(questions.size() == 2 && questions[1].toolCalls == 3 && questions[1].answers == 4 &&
                  MdToolAreaSummary(questions[1]) == L"3 tool calls: AskUserQuestion \x00D7" L"3 \x00B7 4 answers",
              "md golden areas: questions default turn 2 - 3 questions, 4 nested answers");
        const auto compaction = GoldenToolAreas(L"fixture_compaction-default.md");
        CHECK(compaction.size() == 5 && compaction[0].empty, "md golden areas: compaction default - the work-before-the-first-prompt turn is empty");
        if (compaction.size() == 5)
        {
            CHECK(compaction[1].toolCalls == 2 && compaction[1].remarks == 2 && compaction[1].compactions == 1 && compaction[1].answers == 1,
                  "md golden areas: compaction default turn 1 - calls, remarks, a compaction and an answer");
        }
        const auto chain = GoldenToolAreas(L"chain-b3b3b3b3-default.md");
        CHECK(chain.size() == 1 && chain[0].empty, "md golden areas: chain default - the Session chain list is not an area");
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
