// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster - ConversationImage: a Claude Code session's WHOLE conversation drawn as tall pictures that look like its
// terminal (MESSAGING.md section 21.39 - the Telegram card's "All Screen" and /allscreen).
//
// Why the transcript and not the terminal: a "screenshot of everything" cannot come from the terminal. Claude Code's
// fullscreen renderer (CLAUDE_CODE_NO_FLICKER=1, `/tui fullscreen` - on for every session of this machine) lives in the
// ALTERNATE screen buffer, which has no scrollback at all, so the terminal only ever holds the rows on screen. The classic
// renderer's scrollback is capped too (historySize), is cleared by /compact and is redrawn on every resize. The transcript is
// the only complete record, and every session has one, also a dormant tab or one in a window that is not up.
//
// The pipeline (RenderConversationImages, off the UI thread):
//   1. TranscriptTurns renders the conversation turn by turn as Markdown at a detail level (the Copy > Transcript levels;
//      Default = every tool call on one line), following the fork / plan-restart chain, so "the whole conversation" is the
//      one Copy > Transcript and the summary panel show.
//   2. MarkdownBlocks parses it (cmark) into a flat list of blocks.
//   3. BuildConversationSheet lays the blocks out on a terminal grid (a ScreenSnapshot) the way Claude Code draws a
//      conversation: the prompt on a gray band after "❯", "●" before Claude's text and each tool call (green when it went
//      well, red when it failed), "⎿" before what came back, the turn as a rule. Markdown inside the messages keeps its
//      bold, italics, code and lists.
//   4. ScreenImage's tall renderer draws the sheet with the screen renderer's look into PNG parts of a bounded height
//      (RenderTallScreenSnapshotPngs), starting a part at a turn where it can.
//
// Pure and plain C++ + Win32 (no WinRT, no PCH) like its parts. The layout (BuildConversationSheet) does no I/O; the m5 suite
// checks it on hand-written Markdown. MarkdownBlocks needs cmark, so the standalone harness compiles this unit only when the
// vcpkg cmark of a full build is present (run-m5-tests.bat; a stub stands in otherwise).

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "MarkdownBlocks.h"
#include "ScreenImage.h"
#include "ScreenSnapshot.h"
#include "TranscriptTurns.h"

namespace Agentmaster
{
    // ----- the palette: Claude Code's dark look on Windows Terminal's default scheme (Campbell) -----
    // Fixed, not the session's own terminal colors: the picture is drawn from the transcript, so there may be no terminal to
    // ask (a dormant tab, a closed window), and one fixed look makes every conversation picture read the same.

    /// <summary>The sheet's background (Campbell's black).</summary>
    inline constexpr uint32_t kConvBg = 0x0C0C0C;
    /// <summary>Body text (Campbell's foreground).</summary>
    inline constexpr uint32_t kConvText = 0xCCCCCC;
    /// <summary>Bold text, Claude's own bullet, the prompt's text.</summary>
    inline constexpr uint32_t kConvBright = 0xF2F2F2;
    /// <summary>Secondary text: notes, what a tool returned, a turn's times.</summary>
    inline constexpr uint32_t kConvDim = 0x8E8E8E;
    /// <summary>The turn rules and the quote bars.</summary>
    inline constexpr uint32_t kConvRule = 0x4A4A4A;
    /// <summary>Claude's orange: a turn's number, a session rule, a subagent's bullet, the compaction mark.</summary>
    inline constexpr uint32_t kConvClaude = 0xD77757;
    /// <summary>A tool call that ended well (Claude Code's green bullet).</summary>
    inline constexpr uint32_t kConvOk = 0x4EBA65;
    /// <summary>A tool call that failed (Claude Code's red bullet).</summary>
    inline constexpr uint32_t kConvFail = 0xFF6B80;
    /// <summary>A tool call that got no result (interrupted, still running when the transcript was read).</summary>
    inline constexpr uint32_t kConvPending = 0x8E8E8E;
    /// <summary>The band behind a user's message (Claude Code draws a typed prompt on a gray band).</summary>
    inline constexpr uint32_t kConvPromptBand = 0x2B2B2B;
    /// <summary>The "❯" before a user's message.</summary>
    inline constexpr uint32_t kConvPromptMark = 0xA0A0A0;
    /// <summary>A message from a teammate or another session (✉).</summary>
    inline constexpr uint32_t kConvPeer = 0x5FC4D0;
    /// <summary>Inline `code` (Claude Code's lavender).</summary>
    inline constexpr uint32_t kConvCode = 0xB1B9F9;
    /// <summary>The band behind a code block.</summary>
    inline constexpr uint32_t kConvCodeBand = 0x181818;
    /// <summary>A code block's text.</summary>
    inline constexpr uint32_t kConvCodeText = 0xD4D4D4;
    /// <summary>A link's text (underlined).</summary>
    inline constexpr uint32_t kConvLink = 0x6CB6FF;
    /// <summary>The marker of a list item inside a message.</summary>
    inline constexpr uint32_t kConvMarker = 0xA0A0A0;
    /// <summary>The band behind an ADDED line of a diff: Claude Code's own dark-theme color (rgb 34, 92, 43).</summary>
    inline constexpr uint32_t kConvDiffAddedBg = 0x225C2B;
    /// <summary>The band behind a REMOVED line of a diff: Claude Code's own dark-theme color (rgb 122, 41, 54).</summary>
    inline constexpr uint32_t kConvDiffRemovedBg = 0x7A2936;

    /// <summary>
    /// The width a conversation is laid out at. 100 columns: Claude Code's text wraps near there on a laptop, and 100 cells of
    /// 12 px make a 1,232 px picture, about a phone's own width.
    /// </summary>
    inline constexpr int kConversationImageCols = 100;
    /// <summary>
    /// The most pictures one request sends. Each is a Telegram call on the chat's flood budget (~18 a minute), and ten
    /// 32,000 px parts hold ~13,300 rows (24 px rows of Cascadia Mono) - about twice the Default level of the largest
    /// transcript measured on this machine (95 MB, 6,945 rows in 6 parts on 2026-10-25). A longer conversation keeps its most
    /// recent rows (BuildConversationSheet).
    /// </summary>
    inline constexpr size_t kConversationImageMaxParts = 10;
    /// <summary>
    /// A single Markdown block is cut at this many characters before it is laid out (MdParseOptions::maxBlockChars; the cut is
    /// noted in the picture). At the Max level a Write's whole file is one code block; 20,000 characters are ~200 rows.
    /// </summary>
    inline constexpr size_t kConversationBlockMaxChars = 20000;

    /// <summary>How a conversation is laid out.</summary>
    struct ConversationSheetOptions
    {
        int cols{ kConversationImageCols }; // the sheet's width in cells (20..kScreenSnapshotMaxCols; a narrower value is raised to 20)
        int maxRows{ 0 }; // 0 = keep every row; else the sheet keeps only its LAST rows, at most this many (a note on top says what was left out)
    };

    /// <summary>A conversation laid out on the grid.</summary>
    struct ConversationSheet
    {
        ScreenSnapshot snapshot; // cols x rows; fontFace "" (the renderer's monospace fallbacks), the palette's colors
        std::vector<uint8_t> cuts; // one per row: kTallCut* for a part that would START at that row (PlanTallImageParts)
        std::vector<int> rowTurn; // one per row: the number of the turn the row belongs to (0 = before the first turn)
        int turns{ 0 }; // the highest turn number laid out (0 = none: a transcript without a turn yet)
        int droppedRows{ 0 }; // rows left out at the TOP because of maxRows (0 = the whole conversation is on the sheet)
        int droppedThroughTurn{ 0 }; // the last turn that lost rows to the cut (0 = none, or only rows before the first turn)
    };

    /// <summary>
    /// Lays a turns-view document (TranscriptTurns' Markdown, parsed by ParseMarkdownBlocks with soft breaks as line breaks)
    /// out on a terminal grid the way Claude Code draws a conversation - see the file comment for the look. Words wrap at the
    /// sheet's width (a word longer than a row is split), wide glyphs take two cells, a combining mark rides on the cell before
    /// it, a TAB goes to the next multiple of 4 columns, and a code block keeps its spaces and wraps at the edge.
    /// </summary>
    /// <param name="blocks">The parsed blocks, top to bottom.</param>
    /// <param name="options">The width and the row cap.</param>
    /// <returns>The sheet: at least one row (an empty document gives one blank row), at most options.maxRows when it is set.</returns>
    /// <exception cref="std::bad_alloc">If the sheet cannot be allocated (memory is bounded by maxRows rows, plus one block's rows).</exception>
    /// <remarks>
    /// The shapes it knows are the ones TranscriptTurns writes (TranscriptTurns.cpp RenderTurn / SpeakerBlock / EventLines /
    /// AgentEntry): "### Turn N" headings, "## Session N of M" headings, the "**❯ User**" / "**● Claude**" / "**✉ name**"
    /// labels (drawn as the band or the bullet they introduce, not as a line - except a label that says more, like "typed while
    /// it worked"), the tool-call list under a speaker (MdFindToolAreas; its "(no tool calls)" note is left out, since Claude
    /// Code's screen shows nothing there), an agent's "**● name** · ..." header (its work indented under it), and the reply
    /// quote. Anything else is drawn as plain Markdown, dimmed when it sits outside a
    /// message (the path legend, the session chain, the notes). With maxRows the cut keeps the END of the conversation - the
    /// part a reader catches up on - and the first row then says how many earlier rows, through which turn, are left out.
    /// </remarks>
    ConversationSheet BuildConversationSheet(const std::vector<MdBlock>& blocks, const ConversationSheetOptions& options);

    /// <summary>One picture of a conversation, handed over as soon as it is encoded (RenderConversationImages' onPart).</summary>
    struct ConversationImagePart
    {
        TallImagePart picture; // the PNG and the rows it shows
        size_t index{ 0 }; // 0-based, in conversation order
        size_t count{ 0 }; // how many parts the conversation has
        int firstTurn{ 0 }; // the turn of the part's first row (0 = before the first turn)
        int lastTurn{ 0 }; // the turn of its last row
    };

    /// <summary>What RenderConversationImages draws, and how.</summary>
    struct ConversationImageOptions
    {
        TurnsLevel level{ TurnsLevel::Default }; // how much of each turn is drawn (the Copy > Transcript levels)
        // §21.39 "File diffs": true = every file change (Edit, MultiEdit, Write, NotebookEdit) is drawn as a diff under its call,
        // whatever the level (TurnsOptions::editDiffs; at Minimal and Compact the file-changing calls are listed for it); false =
        // the level's own default, which shows no diffs (only Max shows each Write's whole content).
        bool editDiffs{ false };
        int cols{ kConversationImageCols }; // the sheet's width in cells
        size_t maxParts{ kConversationImageMaxParts }; // the most pictures (at least 1)
        TallScreenImageOptions image; // the cell, the padding and the part height
        bool utc{ false }; // the turns' times in UTC instead of this machine's local time
        /// <summary>Called with each part as soon as it is drawn (its PNG moved in), on the calling thread; empty = every part's bytes stay in the result.</summary>
        std::function<void(ConversationImagePart&&)> onPart;
    };

    /// <summary>The pictures of a conversation (or why there are none), with what a log line needs to say how it went.</summary>
    struct ConversationImageResult
    {
        std::vector<ConversationImagePart> parts; // in order; a picture's `png` is empty when onPart took it
        int rows{ 0 }; // rows drawn (after the cap)
        int droppedRows{ 0 }; // rows left out at the top (ConversationSheet::droppedRows)
        int droppedThroughTurn{ 0 }; // the last turn that lost rows to the cap
        int turns{ 0 }; // the highest turn number drawn
        size_t markdownChars{ 0 }; // the size of the turns view the sheet was laid out from
        ScreenImageGrid grid; // the cell (widthPx = every picture's width)
        std::wstring fontFamily; // the family the text was drawn in
        int64_t turnsMs{ 0 }; // reading the transcript and rendering the turns view
        int64_t layoutMs{ 0 }; // parsing the Markdown and laying the sheet out
        int64_t drawMs{ 0 }; // drawing and encoding every part (onPart's own time included)
        std::wstring error; // "" on success; else what failed (parts drawn before a failure stay in `parts`)
    };

    /// <summary>
    /// Draws a Claude Code transcript's whole conversation as tall PNG pictures (the pipeline of the file comment). Reads the
    /// transcript and the chain it continues, so it costs a disk read of each and seconds of drawing for a long one: call it
    /// on a worker thread, never on a UI thread.
    /// </summary>
    /// <param name="transcript">The session's .jsonl (ResolveClaudeTranscriptPath; a never-messaged fork passes its source's).</param>
    /// <param name="options">The level, the width, the part cap, the cell and the part callback.</param>
    /// <returns>The parts in order with the drawing's statistics, or the reason in `error`. Never throws.</returns>
    /// <remarks>
    /// The row cap is computed from the font actually found: (part height - padding) / cell height rows per part, times
    /// maxParts. A conversation longer than that keeps its most recent rows (BuildConversationSheet), and its first picture
    /// starts with the note saying how much was left out. A failure is reported, never thrown (Rule #18: an exception is logged
    /// with its throw site and reported as "an exception while drawing the conversation").
    /// </remarks>
    ConversationImageResult RenderConversationImages(const std::filesystem::path& transcript, const ConversationImageOptions& options) noexcept;
}
