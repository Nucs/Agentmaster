// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MarkdownBlocks: CommonMark parsed by cmark and FLATTENED into a top-to-bottom list
// of render blocks. It feeds the transcript overlay (AgentTranscriptOverlay — "Show Transcript" /
// "Copy ▸ Transcript ▸ Show"), which turns each block into one small XAML element.
//
// Why a flat list and not a document tree: the overlay builds its XAML a few blocks per dispatcher
// tick (a Max-level transcript is ~1.5 MB of Markdown, tens of thousands of lines), and a flat list
// is what a time-sliced builder can resume at any index. Each block therefore carries its whole
// context — the block quotes and list items around it, OUTERMOST FIRST — so it can be built on its
// own, and two neighbors can be compared to decide the gap between them (MdGapAbove).
//
// Deliberate choices, each different from how WT's own Markdown pane renders (UIMarkdown's
// MarkdownToXaml), because a transcript is not a README:
//   - RAW HTML IS KEPT AS TEXT. A prompt that says "use List<int>" or carries a <system-reminder>
//     block parses as inline / block HTML; dropping it (what an HTML sanitizer or MarkdownToXaml
//     does) would silently delete words the user wrote.
//   - SOFT LINE BREAKS ARE REAL LINE BREAKS (MdParseOptions::softBreaksAsLineBreaks, default on).
//     transcript_turns quotes every message line by line, so a prompt typed on three lines is three
//     soft-broken lines of one paragraph — joining them with spaces (CommonMark's HTML default) would
//     not show the prompt as it was typed. Claude Code's own TUI keeps the line breaks too.
//   - Line breaks stay INSIDE the run text as '\n' instead of separate break nodes, so a pasted
//     10,000-line log inside a quote is ONE run, not 20,000 XAML inline objects.
//   - Images are NOT fetched: they render as "[image: <alt>]" linked to their URL. A transcript can
//     quote any URL a user pasted; loading it unasked would be a network request nobody made.
//   - Ordered lists keep their numbers (MarkdownToXaml renders them as bullets).
//
// Plain C++ + Win32 (UTF-8 <-> UTF-16 conversion) + cmark, no WinRT and no PCH. cmark comes from the
// repo's vcpkg manifest (the "terminal" feature UIMarkdown already uses); the msbuild links it into
// TerminalApp.dll through vcpkg's auto-link. The standalone harness links it only when the vcpkg tree
// of a full build is present (run-m5-tests.bat, AM_HAVE_CMARK). Every entry point is NEVER-THROW.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster
{
    // Inline style bits for one MdRun. They combine (bold + italic + link is one run). Kept as plain
    // bits rather than an enum class so the renderer can test them with a mask and the parser can OR
    // the enclosing emphasis / strong / link state into a run in one expression.
    inline constexpr uint8_t kMdBold = 0x01; // inside **strong** — the renderer draws it SemiBold
    inline constexpr uint8_t kMdItalic = 0x02; // inside *emphasis* — italic
    inline constexpr uint8_t kMdCode = 0x04; // an `inline code` span (or raw HTML) — monospace; never combined with a link's own text styling
    inline constexpr uint8_t kMdLink = 0x08; // inside a link or an image — MdRun::url holds the destination

    // One styled stretch of text inside a Paragraph or Heading block. Adjacent runs with the same
    // style AND url are merged while parsing, so a typical paragraph is a handful of runs.
    struct MdRun
    {
        // The visible text. May contain '\n' — a hard break, or a soft break when
        // MdParseOptions::softBreaksAsLineBreaks is on; the renderer passes it to XAML as is, which
        // breaks the line there (one Run, however many lines).
        std::wstring text;
        // kMd* bits OR'ed together; 0 == plain body text.
        uint8_t style = 0;
        // The link destination, exactly as written in the Markdown (not validated — the renderer
        // decides which schemes are clickable). Empty unless style has kMdLink.
        std::wstring url;
    };

    // The two kinds of container a block can sit in. Paragraphs inside a list inside a quote get
    // both, in nesting order.
    enum class MdContainerKind : uint8_t
    {
        Quote, // a "> " block quote — the renderer draws a vertical bar down its left side
        Item, // one list item — the renderer indents by a marker column (and shows the marker on the item's first block)
    };

    // One enclosing container of a block. A block's containers are ordered OUTERMOST FIRST, and two
    // blocks share a container exactly when the entries at the same position have the same `id` —
    // that equality is what lets the renderer run a quote's bar unbroken through the gap between
    // two paragraphs of the same quote, yet start a new bar for the next quote.
    struct MdContainer
    {
        MdContainerKind kind = MdContainerKind::Quote;
        // Unique per container node within ONE parse (1, 2, 3, ... in document order). Never 0 for a
        // real container. Compared, never interpreted.
        uint32_t id = 0;
        // Item only: the id of the list this item belongs to (its siblings carry the same value).
        // Lets MdGapAbove tell "the next item of the same list" from "a nested list started".
        uint32_t listId = 0;
        // Item only: the list is TIGHT (no blank lines between its items) — the renderer packs its
        // items and their paragraphs closer than paragraph spacing, as CommonMark's HTML does.
        bool tightList = false;
        // Item only: an ordered (numbered) list. Selects the marker column's width formula.
        bool ordered = false;
        // Item only: the widest marker this item's list can show, in characters — 1 for a bullet,
        // digits(highest number) + 1 for an ordered list ("10." == 3). Every block of every item of
        // that list indents by the same width, so wrapped and continuation text lines up.
        uint8_t markerChars = 1;
        // Item only: the marker to show ("•", "◦", "▪" by nesting depth, or "3." / "3)"). It is set
        // ONLY on the copy held by the item's FIRST block; the item's later blocks (a second
        // paragraph, a code block, a nested list) carry it empty and get a blank hanging indent.
        std::wstring marker;
    };

    // What a block draws.
    enum class MdBlockKind : uint8_t
    {
        Paragraph, // wrapped text from `runs` (also a raw HTML block — see MdBlock::rawHtml)
        Heading, // wrapped text from `runs`, larger and SemiBold by `headingLevel`
        Code, // a fenced or indented code block: `code`, monospace, on a dark panel
        Rule, // a thematic break ("---"): a thin horizontal line
    };

    // One render block: the unit the overlay turns into one XAML element (plus the wrappers its
    // containers need). Self-contained on purpose — see the file comment.
    struct MdBlock
    {
        MdBlockKind kind = MdBlockKind::Paragraph;
        // Heading only: 1..6 (clamped). 0 for every other kind.
        uint8_t headingLevel = 0;
        // Paragraph only: this block is a raw HTML BLOCK kept as literal text (a <details> or
        // <system-reminder> chunk quoted from a message). Its single run is plain text; the renderer
        // shows it monospace so markup reads as markup.
        bool rawHtml = false;
        // The quotes / list items around the block, OUTERMOST FIRST. Empty at the top level.
        std::vector<MdContainer> containers;
        // Paragraph / Heading: the styled text, in order. May be empty (an empty list item still
        // emits a Paragraph so its marker shows).
        std::vector<MdRun> runs;
        // Code only: the literal text, without the trailing newline cmark keeps.
        std::wstring code;
        // Code only: the first word of the fence's info string (the language, e.g. "bash"), or empty.
        std::wstring info;
        // The block's text was cut at MdParseOptions::maxBlockChars. The renderer appends a note
        // saying so — the full text is still in the Markdown the overlay's Copy button copies.
        bool truncated = false;
    };

    // Knobs for ParseMarkdownBlocks. The defaults are what the transcript overlay wants.
    struct MdParseOptions
    {
        // A soft line break (a newline inside a paragraph) becomes '\n' instead of a space. See the
        // file comment for why the transcript wants this; turn it off for prose written to wrap.
        bool softBreaksAsLineBreaks = true;
        // Stop after this many blocks (0 == no limit). The result's truncatedBlocks says it happened.
        // A guard against a transcript so long its XAML would exhaust memory; the overlay sets it.
        size_t maxBlocks = 0;
        // Cut a single block's text after this many UTF-16 units (0 == no limit) and flag it
        // `truncated`. A 5 MB written file inside a Max-level code block would otherwise be one
        // TextBlock whose layout alone stalls the UI thread.
        size_t maxBlockChars = 0;
    };

    // What ParseMarkdownBlocks produced.
    struct MdParseResult
    {
        // The blocks, top to bottom.
        std::vector<MdBlock> blocks;
        // MdParseOptions::maxBlocks was reached: the document continues past the last block.
        bool truncatedBlocks = false;
        // The parse failed part-way (out of memory inside cmark, or an exception — already logged to
        // hooks.log via LogSwallowedException). `blocks` holds whatever was parsed before the failure.
        bool failed = false;
    };

    // Parse `markdown` (CommonMark, as transcript_turns and Claude write it) into render blocks.
    // Parameters:
    //   markdown — the document. Converted to UTF-8 for cmark (an unpaired surrogate becomes U+FFFD,
    //              a NUL is replaced by cmark itself); any size, but the cost is linear in it.
    //   options  — see MdParseOptions; the defaults suit the transcript overlay.
    // Returns the blocks in document order (empty for an empty or whitespace-only document), with
    // truncatedBlocks / failed set as described on MdParseResult.
    // Never throws: a failure is logged (Rule #18) and reported through MdParseResult::failed.
    // Pure: no I/O besides the failure log; safe on any thread (the overlay calls it on a pool thread).
    MdParseResult ParseMarkdownBlocks(std::wstring_view markdown, const MdParseOptions& options = {}) noexcept;

    // How many leading containers `prev` and `cur` share (same id at the same position). The
    // renderer puts the vertical gap above `cur` INSIDE that many containers: a quote bar shared with
    // the previous block runs unbroken through the gap, a quote that starts at `cur` begins below it.
    // Returns 0 when either has no containers. Never throws.
    size_t MdSharedContainerDepth(const MdBlock& prev, const MdBlock& cur) noexcept;

    // The vertical gap, in DIPs, the renderer leaves above `cur`.
    // Parameters:
    //   prev — the block drawn just above `cur`, or nullptr when `cur` is the first block.
    //   cur  — the block being placed.
    // Returns 0 for the first block; otherwise a small fixed gap chosen by the pair:
    //   * a heading gets a section break above it, and the block after a heading sits close to it;
    //   * the items of a TIGHT list (and the start of one, and an item's own continuation blocks)
    //     are packed, as CommonMark's HTML packs a tight list;
    //   * the block after a LABEL paragraph (one whose first line is bold and nothing else —
    //     transcript_turns' "**❯ User**" / "**● Claude**" speaker lines, MdIsLabelParagraph) sits close
    //     to its label;
    //   * code blocks and rules get a little more air than text; plain paragraphs get paragraph spacing.
    // Never throws.
    double MdGapAbove(const MdBlock* prev, const MdBlock& cur) noexcept;

    // True when `b` STARTS with a LABEL line: a Paragraph whose FIRST line is bold and nothing else (at
    // most 200 characters) — the shape of transcript_turns' speaker lines ("**❯ User**", "**● Claude**",
    // "**✉ finder-a**"). Later lines may hold anything: at the Full / Max levels the turn's italic stats
    // line follows "**● Claude**" with a single newline, and CommonMark folds both into one paragraph.
    // The renderer tints the label line (and dims the rest of that paragraph) and keeps the next block
    // close (MdGapAbove). Never throws.
    bool MdIsLabelParagraph(const MdBlock& b) noexcept;
}
