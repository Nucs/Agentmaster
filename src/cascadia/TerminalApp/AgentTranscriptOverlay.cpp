// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the transcript overlay (AgentTranscriptOverlay.h has the design: what it shows, how it
// renders, what closes it). This file is the whole implementation:
//   * _BuildUi            — the host layer (backdrop + card), header (with the ↵ line-wrap toggle), level
//                           tabs, the separator and the three box tabs + "Expand all", the box row, body
//                           (with the ↑ / ↓ jump buttons over its bottom-right corner);
//   * Show / Close        — open / retarget / close, the focus hand-offs; the remembered tab
//                           (_RememberedLevel on open, _RememberLevel on every tab pick — settings.json);
//   * _ToggleBox          — a box tab's box click: its ticks (_RememberBox — settings.json, per tab), the
//                           dependent boxes greyed (_SyncBoxesRow), the tab re-rendered (_ResetView) with
//                           the reader kept on the same turn (_TopTurnNumber → the pending turn jump);
//   * _JumpToEdge         — the ↑ / ↓ buttons (shown by _UpdateJumpButtons), the ↓ end-follow while a view
//                           is still building (_Pump), stopped by the reader's own scroll (_CancelFollowEnd);
//   * _ToggleWrapLines    — the line-wrap toggle: ApplyWrapMode per level view, the reader's place kept by
//                           FindScrollAnchor / RestoreScrollAnchor (_SyncWrapMode), the state remembered
//                           for every window (_RememberedWrapLines on open, _RememberWrapLines per click);
//   * _LoadLevelAsync     — the pool-thread render (TranscriptTurns / the Codex readers + MarkdownBlocks,
//                           and the tool areas to fold, MdFindToolAreas);
//   * _Pump / _BuildNext  — the UI-thread, time-sliced block -> XAML build, which folds each tool area
//                           into a summary row + a hidden panel (_BuildFold) or a one-line note;
//   * _BuildBlockElement  — one MdBlock -> one element: text / code / rule, wrapped in its quote bars
//                           and list indents; a Session chain row's "session N" become hyperlinks
//                           (_AppendSessionLinks), each scrolling session N's heading to the top
//                           (_JumpToSession / _ScrollToSessionHeading, or a pending jump done by _Pump);
//   * _BuildDiffCard      — the Diff tab (TRANSCRIPT_OVERLAY.md §3l): a ```diff block as a GitHub-style card
//                           (_BuildDiffCardBody, _MakeDiffLines: a grid row per line - a numbers cell and a
//                           WRAPPING code cell - over childless bands spanning rows; no horizontal scroll), the
//                           Bash rows as chips (_BuildDiffBashRow), the turn headings'
//                           counts (_BuildDiffHeading), the outline beside the page (_ScanDiffOutline on the
//                           pool thread, _BuildDiffOutline, _UpdateDiffOutlineHighlight, the jumps) and Expand
//                           from the file as it is now (_ExpandDiffCard -> _LoadDiffFileAsync ->
//                           _OnDiffFileLoaded -> _ApplyDiffExpansion / _SyncDiffExpandRows).
#include "pch.h"
#include "AgentTranscriptOverlay.h"

#include "AgentCatchLog.h" // AgentLogCaughtException — Rule #18 forensics for every contained failure
#include "AgentClipboard.h" // RobustCopyTextToClipboard — the retry-looped Win32 clipboard writer every Agentmaster copy uses
#include "AgentCopyActions.h" // kTranscriptCopyMenu — the levels (order, labels, descriptions) the tabs mirror
#include "AgentTipHelpers.h" // AgentSetTip / AgentCloseTipsIn — the islands-safe tooltip host
#include "AgentMaster/ClaudeSpawn.h" // ResolveClaudeDisplayTranscript / ForkSourceTranscriptNote / LogNav / ShortId / AppendStateLog
#include "AgentMaster/Persistence.h" // Load/SaveAppSettings — the remembered level (AppSettings::transcriptOverlayLevel)
#include "AgentMaster/ProcessInspect.h" // ReadConversationText / ReadConversationFollowupText / the Codex rollout resolve
#include "AgentMaster/TranscriptLevelGuide.h" // kTranscriptLevelGuide / TranscriptLevelTip — the (i) panel and the tabs' cheat-sheet tooltips
#include "AgentMaster/TranscriptTurns.h" // RenderTranscriptTurnsFile / TurnsOptionsForLevel / TurnsLevelName
#include "AgentMaster/TranscriptViews.h" // the box tabs: kTranscriptViewBoxes, TranscriptViewOptions / Name / Tip, the box rules

#include <winrt/Windows.UI.Xaml.Automation.h> // AutomationProperties (button names for screen readers)

#include <mmsystem.h> // PlaySoundW — the copy chime every Agentmaster copy plays
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <chrono>
#include <cmath> // std::lround - the Diff cards' five diffstat squares (DiffBarSquares)
#include <cwctype> // iswspace / towlower
#include <limits> // std::numeric_limits - a table cell measured at an unbounded size (_BuildTable)

// Every XAML event handler below (clicks, key presses, pointer presses, focus changes) and every
// dispatcher item contains its own exceptions: one escaping a routed-event handler comes back as a
// stowed-exception fail-fast (0xC000027B) — the whole process, every window — for what is at worst
// a reading view that failed to update (the CLAUDE.md tag-popup gotcha).

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Documents;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
// Narrow using-DECLARATIONS for the color helpers: `using namespace winrt::Windows::UI;` would also pull
// the nested Text namespace into scope (the CLAUDE.md XAML name-clash gotcha).
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::System::DispatcherQueue;
using winrt::Windows::System::DispatcherQueuePriority;
using winrt::Windows::System::VirtualKey;

namespace
{
    // Above every other child of the page's Root (the Sessions page host, the command palette, the
    // dialogs' row-2 placeholders): the overlay must cover whatever is on screen.
    constexpr int32_t kHostZIndex = 900;

    // The input caps (see MdParseOptions). A Max-level render of the largest session on this machine
    // (a 29.5 MB transcript) was 1.5 M characters / ~10 k blocks; the caps sit well above real sessions
    // and exist only so a pathological one degrades into "the view stops here" instead of a stall.
    constexpr size_t kMaxMarkdownChars = 8'000'000; // characters parsed (the rest is still copyable)
    constexpr size_t kMaxBlocks = 25'000; // blocks built into XAML
    constexpr size_t kMaxBlockChars = 150'000; // characters in one block (a huge written file)

    // The build's time slicing: each low-priority dispatcher item builds blocks until this budget is
    // spent (but at least kMinBlocksPerSlice, so a slow machine still makes progress), then yields —
    // input and rendering run between slices, so the window never feels frozen while a long transcript
    // lays out.
    constexpr auto kSliceBudget = std::chrono::milliseconds(12);
    constexpr size_t kMinBlocksPerSlice = 16;

    // Elements per chunk panel. Appending to a panel re-measures it; a two-level stack (chunks of 200)
    // keeps each append from walking ten thousand siblings. Each chunk is also one UI Automation node
    // (AgentTranscriptChunkPanel), which bounds every UIA child list to one chunk's elements.
    constexpr size_t kChunkSize = 200;

    // A chunk panel (of a level, or of an open fold) that UI Automation sees as ONE node of its own: its peer is
    // a plain FrameworkElementAutomationPeer, kept out of the Control and Content views (AccessibilityView Raw,
    // set by MakeChunkPanel), so a screen reader reads exactly what it read before - the chunk's elements, in
    // order - while the Raw tree gains a level of hierarchy.
    //
    // Why it must exist: XAML builds an element's UIA children by walking its visual subtree down to the first
    // descendants that HAVE a peer (FrameworkElementAutomationPeer::GetAutomationPeerChildren), and a plain panel
    // has none - so every block of a level was ONE flat child list of the scroll view (22,231 blocks at Compact
    // for session 489217be). UIA moves to a next sibling by asking the parent for that whole list again
    // (AutomationPeer::NavigateCoreImpl: GetParent, GetChildren, IndexOf), so another process's UIA Find over the
    // window rebuilt it once per block: quadratic, on the UI thread, inside an incoming COM call that nothing can
    // cancel. Production 0.8.0.10 sat there 10+ minutes on 2026-11-07, every window Not Responding, after a
    // 1.27 M-character Compact level opened (stack: RemoteUiaNodeStub::Incoming_Find -> UiaNodeTraverser::
    // GetNextSibling -> CUIAWrapper::Navigate -> FrameworkElementAutomationPeer::GetChildrenCore). With a peer per
    // chunk, a child list holds at most one chunk's elements (kChunkSize) or the level's chunks
    // (kMaxBlocks / kChunkSize = 125), so the same Find is linear in the blocks.
    //
    // Code-only (no IDL, never named in markup): winrt::make returns it as a StackPanel, so the overlay's
    // fields, the reading-position anchor walk and the fold code keep treating chunks as panels.
    struct AgentTranscriptChunkPanel : StackPanelT<AgentTranscriptChunkPanel>
    {
        // XAML asks for the element's peer the first time UIA (or anything else) needs it, then caches it on the
        // element; the peer's default children are the elements of this chunk that have peers.
        // Returns: the peer, or null when it cannot be made - the chunk is then a plain panel again (its
        // elements fall back into the parent's list, the pre-fix tree), which is slow for UIA but correct.
        // Throws nothing: a failure is logged ([exc], throttled) and answered with null, because an exception
        // here would come back to XAML as a failed HRESULT inside a UIA call and fail that call for the client.
        winrt::Windows::UI::Xaml::Automation::Peers::AutomationPeer OnCreateAutomationPeer()
        {
            try
            {
                return winrt::Windows::UI::Xaml::Automation::Peers::FrameworkElementAutomationPeer{ *this };
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay chunk automation peer");
                return nullptr;
            }
        }
    };

    // One chunk panel for _AppendElement / _AppendToFold: an AgentTranscriptChunkPanel, Raw in the accessibility
    // views (see the struct for why both matter).
    // Returns: the new, empty chunk, as the StackPanel the level views store.
    // Throws: winrt::hresult_error when XAML cannot create the panel or set its property (out of memory, a XAML
    // core shutting down) - the same failures a plain StackPanel{} would raise there; the build's dispatcher
    // item contains it like any other build failure.
    StackPanel MakeChunkPanel()
    {
        StackPanel chunk = winrt::make<AgentTranscriptChunkPanel>();
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetAccessibilityView(
            chunk, winrt::Windows::UI::Xaml::Automation::Peers::AccessibilityView::Raw);
        return chunk;
    }

    // Typography (DIPs).
    constexpr double kBodyFontSize = 14.0;
    constexpr double kCodeFontSize = 12.5;
    constexpr double kStatusFontSize = 12.0;
    constexpr double kQuoteBarWidth = 3.0;
    constexpr double kQuotePadding = 10.0;
    constexpr double kBulletMarkerWidth = 16.0;

    // The text appended where a block was cut at kMaxBlockChars.
    constexpr const wchar_t* kCutNote = L"\n\x2026 (cut here - the header's Copy button copies the whole text)";

    // A Markdown TABLE (MdBlockKind::Table, _BuildTable): each cell's padding, the narrowest a column shrinks to when
    // the view is narrower than the table (a column whose own content is narrower keeps its natural width), and how
    // many rows are measured to size the columns - a table longer than that is sized by its first rows (a measure is
    // a text layout; a pathological table must not cost one per cell).
    constexpr double kTableCellPadX = 10.0;
    constexpr double kTableCellPadY = 4.0;
    constexpr double kTableMinColumnWidth = 96.0;
    constexpr size_t kTableMeasuredRows = 300;

    // A FOLD's summary row (a folded tool area): the chevron glyphs (Segoe Fluent Icons ChevronRight /
    // ChevronDown), the row text size, and where the row's text starts — the button's left padding (2)
    // + the chevron's width (12) + the gap after it (6). The "(no tool calls)" note starts at the same
    // x, so every turn's line under "● Claude" lines up whether it folds or not.
    constexpr const wchar_t* kFoldClosedGlyph = L"\xE76C";
    constexpr const wchar_t* kFoldOpenGlyph = L"\xE70D";
    constexpr double kFoldFontSize = 13.0;
    constexpr double kFoldChevronWidth = 12.0;
    constexpr double kFoldChevronGap = 6.0;
    constexpr double kFoldRowPaddingLeft = 2.0;
    constexpr double kFoldRowPaddingTop = 2.0;
    constexpr double kFoldTextIndent = kFoldRowPaddingLeft + kFoldChevronWidth + kFoldChevronGap;

    // The header's LINE-WRAP toggle. The glyph is Segoe Fluent Icons' ReturnKey (↵) — the same symbol the
    // summary panel's wrap toggle shows, but from the header's own icon font, whose 1-em line box keeps the
    // toggle exactly as tall as Copy / Refresh / Close (Segoe UI Symbol's ↵ would be ~4.6 px taller at
    // 14 px and grow the header row). The tooltips state the mode, what a click does, and that the choice
    // follows the user into every window.
    constexpr const wchar_t* kWrapGlyph = L"\xE751";
    constexpr const wchar_t* kWrapOnTip = L"Wrap long lines: ON. A line wider than the view continues on the next line.\n"
                                          L"Click to turn it off: long lines then stop at the right edge, and the rest of each line is hidden.\n"
                                          L"Your choice applies to the transcript in every window.";
    constexpr const wchar_t* kWrapOffTip = L"Wrap long lines: OFF. A line wider than the view stops at the right edge, and the rest of it is hidden "
                                           L"(the Copy button still copies every character).\n"
                                           L"Click to turn it on: long lines then continue on the next line.\n"
                                           L"Your choice applies to the transcript in every window.";
    // An ON toggle's face: the accent as a translucent tint (~28%) with a stronger hairline (~70%) — on the
    // dark card that reads as "pressed / on" without a solid accent block next to the plain icon buttons.
    constexpr uint8_t kToggleOnFillAlpha = 0x47;
    constexpr uint8_t kToggleOnBorderAlpha = 0xB3;

    // The LEVEL GUIDE (the (i) right after the level tabs; TRANSCRIPT_OVERLAY.md 3f). The glyph is Segoe
    // Fluent Icons' Info, a circled i. The panel is wider than a Flyout's default 456 DIPs (its table holds a
    // text column and five level columns) and as tall as the guide needs before it scrolls; the Flyout's own
    // placement keeps it inside the window either way. Each level column fits its head ("Compact") and the
    // "key 4" line under it.
    constexpr const wchar_t* kGuideGlyph = L"\xE946";
    constexpr const wchar_t* kGuideTip = L"What each level shows - click to compare every level, and to see what no level shows";
    constexpr double kGuideMaxWidth = 780.0;
    constexpr double kGuideMaxHeight = 640.0;
    constexpr double kGuideLevelColumnWidth = 68.0;
    constexpr double kGuideTextFontSize = 13.0;
    constexpr double kGuideSmallFontSize = 11.5;
    // The current level's column is a light accent tint behind its cells (~15%), and every other row a faint
    // white wash (~5%) - both translucent, so where they cross both still show.
    constexpr uint8_t kGuideCurrentColumnAlpha = 0x26;
    constexpr uint8_t kGuideStripeAlpha = 0x0D;

    // How deep the reading-position anchor descends: the level's content stack -> a chunk -> an open fold
    // panel -> that fold's chunk -> the element (4). A guard, never reached by the overlay's own tree.
    constexpr int kMaxAnchorDepth = 8;

    // A Session chain link scrolls its session's heading to the top of the view, this many DIPs below the
    // edge — a hair of air, so the heading never reads as cut off. A box click's keep-the-turn jump uses it too.
    constexpr double kSessionJumpGap = 8.0;

    // The SEPARATOR between the five level tabs and the three box tabs (TRANSCRIPT_OVERLAY.md §3j): a hairline as tall as a
    // tab's label, level with it (a tab is 5 DIPs of padding over its 13 DIP label), with air on both sides.
    constexpr double kTabSeparatorHeight = 16.0;
    constexpr double kTabSeparatorTop = 6.0;
    constexpr double kTabSeparatorGap = 6.0;

    // The BOX ROW: its text sizes (a box's words, a level group's caption) and the gaps of its wrapping panel - between two
    // items of a line, and between two lines.
    constexpr double kBoxFontSize = 12.5;
    constexpr double kBoxGroupFontSize = 11.0;
    constexpr float kBoxGapX = 12.0f;
    constexpr float kBoxGapY = 2.0f;
    // A group caption's extra air on its left, so "Default", "Full" and "Max" read as the start of a new group.
    constexpr double kBoxGroupGapLeft = 10.0;
    // What the row says before its boxes.
    constexpr const wchar_t* kBoxRowLead = L"Also show:";

    // The JUMP buttons (↑ to the top, ↓ to the bottom), stacked at the body's bottom-right corner: small round faces (the
    // user's "40% smaller" than the first 34 DIP design), a gap between them, and the corner margins - right of the body's
    // text column but left of the vertical scroll bar's widest (hovered) state, so neither covers the other. The glyphs are
    // Segoe Fluent Icons' Up and Down arrows.
    constexpr double kJumpButtonSize = 20.0;
    constexpr double kJumpGlyphSize = 10.0;
    constexpr double kJumpButtonGap = 4.0;
    constexpr double kJumpMarginRight = 16.0;
    constexpr double kJumpMarginBottom = 10.0;
    constexpr const wchar_t* kJumpTopGlyph = L"\xE74A";
    constexpr const wchar_t* kJumpEndGlyph = L"\xE74B";
    constexpr const wchar_t* kJumpTopTip = L"Jump to the top (Home)";
    constexpr const wchar_t* kJumpEndTip = L"Jump to the bottom (End) - while the view is still being laid out, it follows the end until the last line is there";
    // How far from an edge still counts as AT that edge (DIPs): a fractional offset left by a layout rounding never shows a
    // button that would scroll a pixel.
    constexpr double kJumpEdgeSlack = 1.0;

    // The DIFF tab (TRANSCRIPT_OVERLAY.md 3l): the outline column's width and the most entries it lists (a pathological
    // session must not cost thousands of buttons at once); a card's line limit before it starts closed (its body is then
    // built on its first open - GitHub's "large diffs are not rendered by default"; a line costs two text blocks and a
    // grid row, so the limit keeps one card's build near a build slice); the biggest file Expand reads; the advance of one
    // character of the 12 DIP monospace font (Cascadia Mono's 0.586 em is 7.03 DIPs, Consolas' 6.6: rounded up, so the
    // numbers cell never cuts its mark) and the numbers cell's padding; the pitch of every visual line (wrapped code
    // included); the code's text size. (The Expand step, the gap opened whole and the longest line drawn are
    // TranscriptDiff.h's: kDiffExpandStep, ....)
    constexpr double kDiffOutlineWidth = 240.0;
    constexpr size_t kDiffOutlineMaxEntries = 3000;
    constexpr size_t kDiffCardOpenRows = 300;
    constexpr uint64_t kDiffFileMaxBytes = 32ull * 1024 * 1024;
    constexpr double kDiffCharWidth = 7.3;
    constexpr double kDiffNumbersPadLeft = 6.0;
    constexpr double kDiffNumbersPadRight = 6.0;
    constexpr double kDiffLineHeight = 19.0;
    constexpr double kDiffFontSize = 12.0;
    // Segoe Fluent Icons for the expand rows: Up, Down, and the double arrow between two hunks.
    constexpr const wchar_t* kDiffExpandUpGlyph = L"\xE74A";
    constexpr const wchar_t* kDiffExpandDownGlyph = L"\xE74B";
    constexpr const wchar_t* kDiffExpandBothGlyph = L"\xE8CB";
    // What an expand row says before a click (its tooltip) and the card's legend once it showed lines from the file.
    constexpr const wchar_t* kDiffExpandTip = L"Show more of the file around this change. The lines are read from the file as it is NOW - a later turn, "
                                              L"or you, may have changed them since this turn. They carry a yellow edge.";
    constexpr const wchar_t* kDiffNowNote = L"Lines with a yellow edge are read from the file as it is now - a later turn may have changed them since this turn.";

    // The Diff tab's card arithmetic lives in AgentMaster/TranscriptDiff.h (pure, so the m5 harness tests it); this unit
    // calls it unqualified like its own helpers.
    using ::Agentmaster::DiffBarSquares;
    using ::Agentmaster::DiffDisplayLine;
    using ::Agentmaster::DiffExpandLabel;
    using ::Agentmaster::DiffExpandTake;
    using ::Agentmaster::DiffLinesAboveHunk;
    using ::Agentmaster::DiffNewFirst;
    using ::Agentmaster::DiffNumberDigits;
    using ::Agentmaster::DiffNumbersText;
    using ::Agentmaster::DiffOldFirst;
    using ::Agentmaster::DiffOutlineTitle;
    using ::Agentmaster::DiffRemainingAbove;
    using ::Agentmaster::DiffRemainingBelow;
    using ::Agentmaster::DiffSideCount;
    using ::Agentmaster::kDiffExpandStep;
    using ::Agentmaster::SplitDiffInfo;
    using ::Agentmaster::TakeDiffRepeatCount;

    // The text of a block's runs, joined (a heading's or a paragraph's words). Throws std::bad_alloc only.
    std::wstring BlockText(const ::Agentmaster::MdBlock& block)
    {
        std::wstring text;
        for (const auto& run : block.runs)
        {
            text += run.text;
        }
        return text;
    }

    // Whether a block is one of the Diff tab's Bash rows: a top-level list item ("- `$` <reason> ×N") whose first words are
    // the code span "$". Never throws.
    bool IsDiffBashRow(const ::Agentmaster::MdBlock& block) noexcept
    {
        if (block.kind != ::Agentmaster::MdBlockKind::Paragraph || block.containers.size() != 1 || block.containers[0].kind != ::Agentmaster::MdContainerKind::Item)
        {
            return false;
        }
        for (const auto& run : block.runs)
        {
            if (run.text.find_first_not_of(L" \t") == std::wstring::npos)
            {
                continue;
            }
            return (run.style & ::Agentmaster::kMdCode) != 0 && run.text == L"$";
        }
        return false;
    }

    // A file's current lines, for the Diff tab's Expand: read whole (at most kDiffFileMaxBytes), UTF-8 with a BOM dropped
    // (bytes that are not UTF-8 are read as the ANSI code page), split at "\n" with each line's trailing CR dropped; a final
    // line break makes no empty last line. Shared reads: an editor holding the file open does not block it.
    // Parameters: path - the file (either separator).
    // Returns the lines, or null when the file cannot be opened or read, or is bigger than the cap. Throws std::bad_alloc only.
    std::shared_ptr<const std::vector<std::wstring>> ReadDiffFileLines(const std::wstring& path)
    {
        wil::unique_hfile file{ ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (!file)
        {
            return nullptr;
        }
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > kDiffFileMaxBytes)
        {
            return nullptr;
        }
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        size_t done = 0;
        while (done < bytes.size())
        {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1u << 24));
            if (!::ReadFile(file.get(), bytes.data() + done, want, &got, nullptr) || got == 0)
            {
                break; // a file that shrank while read: what was read is the file
            }
            done += got;
        }
        bytes.resize(done);
        std::string_view view{ bytes };
        if (view.size() >= 3 && static_cast<unsigned char>(view[0]) == 0xEF && static_cast<unsigned char>(view[1]) == 0xBB && static_cast<unsigned char>(view[2]) == 0xBF)
        {
            view.remove_prefix(3);
        }
        std::wstring text;
        if (!view.empty())
        {
            const int length = static_cast<int>(std::min<size_t>(view.size(), INT_MAX));
            UINT codePage = CP_UTF8;
            int wide = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, view.data(), length, nullptr, 0);
            if (wide <= 0)
            {
                codePage = CP_ACP; // not UTF-8: the machine's ANSI code page reads every byte
                wide = ::MultiByteToWideChar(CP_ACP, 0, view.data(), length, nullptr, 0);
            }
            if (wide > 0)
            {
                text.resize(static_cast<size_t>(wide));
                ::MultiByteToWideChar(codePage, 0, view.data(), length, text.data(), wide);
            }
        }
        auto lines = std::make_shared<std::vector<std::wstring>>();
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos)
            {
                end = text.size();
            }
            size_t stop = end;
            if (stop > start && text[stop - 1] == L'\r')
            {
                --stop;
            }
            lines->emplace_back(text, start, stop - start);
            start = end + 1;
        }
        return lines;
    }

    // A diff card's numbers column width: room for its numbers cell (DiffNumbersText - two numbers of `digits` digits, the
    // blanks between them and the mark: 2 * digits + 4 characters) in the 12 DIP monospace font, plus the cell's padding.
    // The expand rows' arrow and the hunk header rows' words line up with it.
    // Parameters: digits - the card's numberDigits (DiffNumberDigits).
    // Returns the width in DIPs. Never throws.
    double DiffNumbersColumnWidth(int digits) noexcept
    {
        return kDiffCharWidth * static_cast<double>(2 * std::max(digits, 1) + 4) + kDiffNumbersPadLeft + kDiffNumbersPadRight;
    }

    // Point a level's scroll view at a line-wrap mode — the whole mechanism of the ↵ toggle, one property:
    //   wrap ON  -> HorizontalScrollBarVisibility DISABLED: the content is measured at the viewport's width,
    //               so every text block (TextWrapping::Wrap) wraps to it — the view as it always was;
    //   wrap OFF -> HIDDEN: the content is measured at INFINITE width, so no text block wraps (only real line
    //               breaks break), no bar shows, and the scroll presenter clips whatever runs past the right
    //               edge.
    // HorizontalScrollMode stays Disabled (set once in _EnsureLevelView), so touch and the mouse wheel can't
    // pan the clipped part into view: OFF means hidden, not a sideways scroll away. Stretched elements (code
    // panels, quote bars, rules) take the content's width, so with wrap OFF their right edges are cut off
    // too, exactly like the text.
    // Parameters: scroll — the level's scroll view; wrap — true for the wrapping view.
    // Throws winrt::hresult_error on a XAML failure (callers contain it).
    void ApplyWrapMode(const ScrollViewer& scroll, bool wrap)
    {
        scroll.HorizontalScrollBarVisibility(wrap ? ScrollBarVisibility::Disabled : ScrollBarVisibility::Hidden);
    }

    // The reader's place in a level's scroll view, held as an ELEMENT rather than a pixel offset: the PATH of
    // child indices from the level's content stack down through its chunk / fold stacks to the deepest
    // element crossing the top edge of the view, plus how far down that element's layout slot the edge sat
    // (0..1). A line-wrap switch re-lays every block out at a new height, so the old pixel offset would land
    // in another passage; the element + fraction lands the reader back on the same text.
    struct ScrollAnchor
    {
        std::vector<uint32_t> path; // empty == no anchor: the view sat at its top, or the walk failed
        double fraction = 0.0; // 0 == the top edge was at the element's top, 1 == at its bottom
    };

    // Where a vertical stack's FIRST child starts below the top of the stack's own layout slot: its top
    // margin + top padding. Each later child then starts where the one before it ends — that child's
    // DesiredSize, margins included — because the overlay's stacks never set Spacing (a future Spacing
    // would only shift a restored position by that many pixels per earlier sibling: harmless, not wrong).
    // Throws winrt::hresult_error on a XAML failure.
    double StackContentTop(const StackPanel& stack)
    {
        return stack.Margin().Top + stack.Padding().Top;
    }

    // Find the reader's place (ScrollAnchor) in `scroll`, whose content is `content`, from the CURRENT
    // layout: the extent coordinates of every element are summed from its earlier siblings' DesiredSize
    // (a vertical stack places each child right below the previous one; a collapsed child's DesiredSize is
    // 0, so a folded tool list costs nothing and needs no special case). No TransformToVisual: a hidden
    // level's view keeps the layout it had when it was last shown, and this reads exactly that — consistent
    // with the VerticalOffset it kept.
    // Parameters: scroll — the level's scroll view; content — its content stack (scroll.Content()).
    // Returns the anchor; an empty path when the view sits at its top (the top stays the top in any
    // layout), when the top edge lies below every element, or when the walk failed.
    // Never throws (a failure is logged — Rule #18 — and yields no anchor, which keeps the raw offset).
    ScrollAnchor FindScrollAnchor(const ScrollViewer& scroll, const StackPanel& content) noexcept
    {
        ScrollAnchor anchor;
        try
        {
            if (!scroll || !content)
            {
                return anchor;
            }
            const double top = scroll.VerticalOffset();
            if (top < 1.0)
            {
                return anchor;
            }
            StackPanel stack = content;
            double start = StackContentTop(stack); // where `stack`'s first child starts, in extent coordinates
            for (int depth = 0; depth < kMaxAnchorDepth; ++depth)
            {
                const auto children = stack.Children();
                const uint32_t count = children.Size();
                double y = start;
                uint32_t index = 0;
                for (; index < count; ++index)
                {
                    const double height = children.GetAt(index).DesiredSize().Height;
                    if (y + height > top)
                    {
                        break; // this child crosses (or starts at) the top edge
                    }
                    y += height;
                }
                if (index >= count)
                {
                    break; // the edge is below every child (the stack's bottom padding): nothing to hold on to
                }
                anchor.path.push_back(index);
                const auto child = children.GetAt(index);
                // A stack crossing the edge (a chunk, an open fold): one of ITS children is a finer anchor.
                // A stack starting exactly at the edge is anchored as a whole (fraction 0) — same place.
                const auto inner = child.try_as<StackPanel>();
                if (inner && inner.Orientation() == Orientation::Vertical && inner.Children().Size() > 0 && y < top)
                {
                    stack = inner;
                    start = y + StackContentTop(inner);
                    continue;
                }
                const double height = child.DesiredSize().Height;
                anchor.fraction = height > 0.0 ? std::clamp((top - y) / height, 0.0, 1.0) : 0.0;
                return anchor;
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: no anchor — the view keeps its raw offset (clamped by the new extent).
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay find scroll anchor");
        }
        anchor.path.clear(); // an incomplete walk is no anchor
        return anchor;
    }

    // Scroll `scroll` so the anchor's element sits at the top edge again, as far into it as before. Call it
    // only after the view was laid out under its new mode (UpdateLayout): it reads the NEW DesiredSizes,
    // and ChangeView clamps to the current extent.
    // Parameters: scroll / content — as for FindScrollAnchor; anchor — what FindScrollAnchor returned.
    // Returns whether a scroll was requested — false for an empty anchor, or when the tree no longer has the
    // anchor's path (the view then keeps its clamped raw offset).
    // Never throws (a failure is logged — Rule #18).
    bool RestoreScrollAnchor(const ScrollViewer& scroll, const StackPanel& content, const ScrollAnchor& anchor) noexcept
    {
        try
        {
            if (!scroll || !content || anchor.path.empty())
            {
                return false;
            }
            StackPanel stack = content;
            double start = StackContentTop(stack);
            for (size_t depth = 0; depth < anchor.path.size(); ++depth)
            {
                const auto children = stack.Children();
                const uint32_t index = anchor.path[depth];
                if (index >= children.Size())
                {
                    return false; // the tree changed under the anchor (never between find and restore today)
                }
                double y = start;
                for (uint32_t i = 0; i < index; ++i)
                {
                    y += children.GetAt(i).DesiredSize().Height;
                }
                const auto child = children.GetAt(index);
                if (depth + 1 < anchor.path.size())
                {
                    const auto inner = child.try_as<StackPanel>();
                    if (!inner)
                    {
                        return false;
                    }
                    stack = inner;
                    start = y + StackContentTop(inner);
                    continue;
                }
                const double offset = y + anchor.fraction * child.DesiredSize().Height;
                return scroll.ChangeView(nullptr, std::max(offset, 0.0), nullptr, true /* disableAnimation: a jump, not a glide */);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: none needed — the view keeps its clamped raw offset.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay restore scroll anchor");
        }
        return false;
    }

    // The view keeps one LevelView per VIEW in a fixed array: the five TranscriptTurns levels by their TurnsLevel value, then
    // the three box tabs, then the Diff tab (TranscriptViews.h); settings.json stores the remembered tab by NAME
    // (TranscriptViewName). Both assume these counts - a new level or tab must update TranscriptViews.h's table and names first.
    static_assert(::Agentmaster::kTurnsLevelCount == 5, "AgentTranscriptOverlay::_levels starts with the 5 levels; update it with the TurnsLevel set");
    static_assert(::Agentmaster::kTranscriptViewCount == 9, "AgentTranscriptOverlay::_levels holds 9 views (5 levels + 3 box tabs + Diff); update it with TranscriptView");

    // The BOX ROW's layout: a code-only Panel that lays its children out left to right and WRAPS onto a new line when the next
    // one does not fit - UWP XAML has no WrapPanel, and a horizontal StackPanel would cut the last boxes off on a narrow
    // window (twelve boxes and four group captions need ~1,300 DIPs on one line). Every child keeps its desired size; a child
    // wider than the whole line gets a line of its own at the line's width. Each line's children are centered vertically,
    // so a caption sits level with the boxes beside it. Its own copy of the side panel's chips-row panel
    // (TerminalPage.AgentSidePanel.cpp AgentSidePanelWrapPanel, local to that unit) with this row's gaps.
    // Code-only (no IDL, never named in markup — the AgentTranscriptChunkPanel idiom): winrt::make returns it as a Panel.
    struct AgentTranscriptBoxWrapPanel : PanelT<AgentTranscriptBoxWrapPanel>
    {
        // Measure every child against the line's width and stack the lines.
        //   available - the room the row gives (its width bounds a line; the height is unbounded in practice).
        //   returns   - the widest line and the lines' total height.
        //   throws    - nothing of its own; a child's Measure that throws fails XAML's layout pass (plain check boxes and
        //               text blocks: never seen).
        winrt::Windows::Foundation::Size MeasureOverride(const winrt::Windows::Foundation::Size& available)
        {
            const float lineWidth = available.Width;
            const winrt::Windows::Foundation::Size childAvailable{ lineWidth, std::numeric_limits<float>::infinity() };
            float x = 0.0f;
            float y = 0.0f;
            float lineHeight = 0.0f;
            float widest = 0.0f;
            for (const auto& child : Children())
            {
                child.Measure(childAvailable);
                const auto desired = child.DesiredSize();
                const float w = std::min(desired.Width, lineWidth);
                if (x > 0.0f && x + w > lineWidth)
                {
                    // The next line. Never before the line's first child: a child wider than the line still gets placed.
                    y += lineHeight + kBoxGapY;
                    x = 0.0f;
                    lineHeight = 0.0f;
                }
                x += w;
                widest = std::max(widest, x);
                x += kBoxGapX;
                lineHeight = std::max(lineHeight, desired.Height);
            }
            return winrt::Windows::Foundation::Size{ widest, y + lineHeight };
        }

        // Arrange every child where MeasureOverride's walk put it, against the final width.
        //   finalSize - the room the row arranged the panel in.
        //   returns   - finalSize (the panel takes what it was given).
        //   throws    - nothing of its own (see MeasureOverride).
        winrt::Windows::Foundation::Size ArrangeOverride(const winrt::Windows::Foundation::Size& finalSize)
        {
            const float lineWidth = finalSize.Width;
            // Two passes over each line: one to learn the line's height (its children are centered in it), one to place.
            // A line is the run of children MeasureOverride's rule puts together.
            std::vector<UIElement> line;
            float x = 0.0f;
            float y = 0.0f;
            float lineHeight = 0.0f;
            const auto place = [&]() {
                float cx = 0.0f;
                for (const auto& child : line)
                {
                    const auto desired = child.DesiredSize();
                    const float w = std::min(desired.Width, lineWidth);
                    const float top = y + (lineHeight - desired.Height) / 2.0f;
                    child.Arrange(winrt::Windows::Foundation::Rect{ cx, top, w, desired.Height });
                    cx += w + kBoxGapX;
                }
            };
            for (const auto& child : Children())
            {
                const auto desired = child.DesiredSize();
                const float w = std::min(desired.Width, lineWidth);
                if (x > 0.0f && x + w > lineWidth)
                {
                    place();
                    y += lineHeight + kBoxGapY;
                    line.clear();
                    x = 0.0f;
                    lineHeight = 0.0f;
                }
                line.push_back(child);
                x += w + kBoxGapX;
                lineHeight = std::max(lineHeight, desired.Height);
            }
            place();
            return finalSize;
        }
    };

    // The number of a top-level "### Turn N · …" heading (TranscriptTurns writes one per turn, numbered across the whole
    // conversation): what a box click's keep-the-turn jump looks for. A heading inside a quote or a list item (a message
    // that QUOTES such a heading) has containers and never counts; nor does any other heading level, nor "### Work before
    // the first prompt" (no number: the jump then simply starts at the top).
    // Parameters: block — a block of the level.
    // Returns N (1..), or 0 when the block is no turn heading. Never throws (it only reads the block).
    int TurnHeadingNumber(const ::Agentmaster::MdBlock& block) noexcept
    {
        if (block.kind != ::Agentmaster::MdBlockKind::Heading || block.headingLevel != 3 || !block.containers.empty())
        {
            return 0;
        }
        // The heading's text starts with "Turn " and its digits; reading at most the first run's head is enough (the number
        // is plain text right at the start - a bold or code run never opens a turn heading).
        if (block.runs.empty())
        {
            return 0;
        }
        const std::wstring_view text{ block.runs.front().text };
        constexpr std::wstring_view kTurn{ L"Turn " };
        if (text.size() <= kTurn.size() || text.substr(0, kTurn.size()) != kTurn)
        {
            return 0;
        }
        int number = 0;
        for (size_t i = kTurn.size(); i < text.size() && text[i] >= L'0' && text[i] <= L'9'; ++i)
        {
            if (number > 100'000'000)
            {
                return 0; // no real conversation has this many turns: a heading that only looks like one
            }
            number = number * 10 + (text[i] - L'0');
        }
        return number;
    }

    // The settings list that remembers box tab `view`'s ticks (TRANSCRIPT_OVERLAY.md §3j): one AppSettings field per tab.
    // Parameters: settings — the settings to read or change; view — a view index.
    // Returns the tab's list inside `settings`, or nullptr for a view that is no box tab. Never throws.
    std::vector<std::wstring>* RememberedBoxesField(::Agentmaster::AppSettings& settings, int view) noexcept
    {
        switch (static_cast<::Agentmaster::TranscriptView>(view))
        {
        case ::Agentmaster::TranscriptView::Custom:
            return &settings.transcriptOverlayCustomBoxes;
        case ::Agentmaster::TranscriptView::UserOnly:
            return &settings.transcriptOverlayUserOnlyBoxes;
        case ::Agentmaster::TranscriptView::AssistantOnly:
            return &settings.transcriptOverlayAssistantOnlyBoxes;
        default:
            return nullptr;
        }
    }

    // The note an EMPTY tool area becomes: its single item's text without the surrounding blanks and
    // parentheses — "(no tool calls)" -> "no tool calls". Falls back to "no tool calls" for an item with
    // no text. Throws std::bad_alloc only.
    std::wstring EmptyAreaNoteText(const ::Agentmaster::MdBlock& block)
    {
        std::wstring text;
        for (const auto& run : block.runs)
        {
            text += run.text;
        }
        const auto first = text.find_first_not_of(L" \t\r\n(");
        const auto last = text.find_last_not_of(L" \t\r\n)");
        if (first == std::wstring::npos || last == std::wstring::npos || last < first)
        {
            return L"no tool calls";
        }
        return text.substr(first, last - first + 1);
    }

    // One solid brush from ARGB bytes. The overlay makes each color ONCE (in _BuildUi) and shares the
    // brush across every element — a brush per element would multiply objects by the block count.
    SolidColorBrush MakeBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ ColorHelper::FromArgb(a, r, g, b) };
    }

    // The heading sizes (h1..h6): a clear hierarchy without letting a transcript's many "### Turn N"
    // headers (h3) shout over the text.
    double HeadingFontSize(uint8_t level) noexcept
    {
        switch (level)
        {
        case 1:
            return 22.0;
        case 2:
            return 19.0;
        case 3:
            return 16.5;
        default:
            return 14.5;
        }
    }

    // The hanging-indent width of a list item's marker column: room for the widest marker its list
    // shows (MdContainer::markerChars) plus a gap before the text.
    double MarkerColumnWidth(const ::Agentmaster::MdContainer& item) noexcept
    {
        if (!item.ordered)
        {
            return kBulletMarkerWidth;
        }
        return 8.0 + 7.5 * static_cast<double>(item.markerChars);
    }

    // Only plain web links become clickable Hyperlinks. A transcript quotes whatever its messages held;
    // a file:, ms-settings: or custom-protocol link launching on one stray click is not a reading
    // view's business, so those render as link-colored text (still visible, still copyable).
    bool IsWebUrl(const std::wstring& url) noexcept
    {
        const auto startsWith = [&url](std::wstring_view prefix) {
            if (url.size() < prefix.size())
            {
                return false;
            }
            for (size_t i = 0; i < prefix.size(); ++i)
            {
                if (towlower(url[i]) != prefix[i])
                {
                    return false;
                }
            }
            return true;
        };
        return startsWith(L"https://") || startsWith(L"http://");
    }

    // 12345 -> "12,345" for the status line.
    std::wstring WithThousands(size_t value)
    {
        std::wstring digits = std::to_wstring(value);
        for (ptrdiff_t at = static_cast<ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3)
        {
            digits.insert(static_cast<size_t>(at), 1, L',');
        }
        return digits;
    }

    // A duration for the status line: "640 ms" under a second, "2.4 s" above.
    std::wstring FormatDurationMs(int64_t ms)
    {
        if (ms < 1000)
        {
            return std::to_wstring(std::max<int64_t>(ms, 0)) + L" ms";
        }
        const int64_t tenths = (ms + 50) / 100;
        return std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" s";
    }

    // Every run of the block is italic (or blank): transcript_turns' notes and stats lines
    // ("_4 tool calls · 1m 30s · …_"). They render dimmed — secondary information, still readable.
    bool AllItalic(const ::Agentmaster::MdBlock& block) noexcept
    {
        bool sawItalic = false;
        for (const auto& run : block.runs)
        {
            bool blank = true;
            for (const wchar_t ch : run.text)
            {
                if (!iswspace(ch))
                {
                    blank = false;
                    break;
                }
            }
            if (blank)
            {
                continue;
            }
            if ((run.style & ::Agentmaster::kMdItalic) == 0)
            {
                return false;
            }
            sawItalic = true;
        }
        return sawItalic;
    }

    // The speaker glyph a label line starts with: '\x276F' (❯ the user), '\x25CF' (● Claude or an
    // agent), '\x2709' (✉ a teammate / another session), or 0 for anything else.
    wchar_t LabelGlyph(const ::Agentmaster::MdBlock& block) noexcept
    {
        for (const auto& run : block.runs)
        {
            for (const wchar_t ch : run.text)
            {
                if (iswspace(ch))
                {
                    continue;
                }
                return (ch == L'\x276F' || ch == L'\x25CF' || ch == L'\x2709') ? ch : L'\0';
            }
        }
        return L'\0';
    }

    // Add to an element's margin (keeping what is already there): the block builder layers a list
    // indent and a top gap onto the same element in separate steps.
    void AddMargin(const FrameworkElement& element, double left, double top)
    {
        auto margin = element.Margin();
        margin.Left += left;
        margin.Top += top;
        element.Margin(margin);
    }
}

namespace winrt::TerminalApp::implementation
{
    AgentTranscriptOverlay::AgentTranscriptOverlay() :
        // The page constructs the overlay on its UI thread, so this is that thread's queue — every
        // build slice and render hand-off is posted to it.
        _dispatcher{ DispatcherQueue::GetForCurrentThread() }
    {
    }

    AgentTranscriptOverlay::~AgentTranscriptOverlay() = default;

    void AgentTranscriptOverlay::SetClosedHandler(std::function<void()> handler)
    {
        _onClosed = std::move(handler);
    }

    void AgentTranscriptOverlay::Attach(const Grid& pageRoot)
    {
        if (_host || !pageRoot)
        {
            return;
        }
        _pageRoot = pageRoot;
        _BuildUi();
        // Span EVERY row of the page (tab row when it isn't in the titlebar, infobars, content) so the
        // card's 90% x 80% is of the window, and the backdrop catches every click outside it.
        const auto rows = pageRoot.RowDefinitions().Size();
        Grid::SetRow(_host, 0);
        Grid::SetRowSpan(_host, static_cast<int32_t>(std::max<uint32_t>(rows, 1)));
        Canvas::SetZIndex(_host, kHostZIndex);
        pageRoot.Children().Append(_host);
    }

    void AgentTranscriptOverlay::_BuildUi()
    {
        // --- shared brushes + fonts (one instance each, referenced by every element) ---
        _bodyBrush = MakeBrush(0xFF, 0xE6, 0xE6, 0xE6);
        _dimBrush = MakeBrush(0xFF, 0x9E, 0x9E, 0x9E);
        _inlineCodeBrush = MakeBrush(0xFF, 0xE2, 0xC0, 0x8D);
        _linkBrush = MakeBrush(0xFF, 0x6C, 0xB8, 0xFF);
        _markerBrush = MakeBrush(0xFF, 0x9A, 0x9A, 0x9A);
        _codeTextBrush = MakeBrush(0xFF, 0xD4, 0xD4, 0xD4);
        _codeBackBrush = MakeBrush(0xFF, 0x14, 0x14, 0x14);
        _codeBorderBrush = MakeBrush(0xFF, 0x2E, 0x2E, 0x2E);
        _ruleBrush = MakeBrush(0xFF, 0x3A, 0x3A, 0x3A);
        _quoteBarBrush = MakeBrush(0xFF, 0x55, 0x55, 0x55);
        _userBrush = MakeBrush(0xFF, 0x7F, 0xC1, 0xF0);
        _userBarBrush = MakeBrush(0xFF, 0x3E, 0x8E, 0xD0);
        _claudeBrush = MakeBrush(0xFF, 0xF0, 0xA0, 0x60);
        _claudeBarBrush = MakeBrush(0xFF, 0xC8, 0x74, 0x2C);
        _peerBrush = MakeBrush(0xFF, 0xB9, 0xA0, 0xEE);
        _peerBarBrush = MakeBrush(0xFF, 0x8A, 0x6C, 0xD0);
        _transparentBrush = MakeBrush(0x00, 0x00, 0x00, 0x00);
        auto accent = ColorHelper::FromArgb(0xFF, 0x3B, 0x82, 0xF6); // fallback when the theme has no accent
        try
        {
            if (const auto value = Application::Current().Resources().TryLookup(winrt::box_value(L"SystemAccentColor")))
            {
                accent = winrt::unbox_value_or<Color>(value, accent);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay accent lookup"); // the fallback blue stays
        }
        _accentBrush = SolidColorBrush{ accent };
        _toggleOnBrush = SolidColorBrush{ ColorHelper::FromArgb(kToggleOnFillAlpha, accent.R, accent.G, accent.B) };
        _toggleOnBorderBrush = SolidColorBrush{ ColorHelper::FromArgb(kToggleOnBorderAlpha, accent.R, accent.G, accent.B) };
        _monoFont = FontFamily{ L"Cascadia Mono, Cascadia Code, Consolas" };
        _textMenu = winrt::Microsoft::Terminal::UI::TextMenuFlyout{};
        // The Diff tab's palette: GitHub's dark diff colors (the user picked "like GitHub review"); the translucent bands as
        // GitHub's alphas (.15 = 0x26, .3 = 0x4D, .1 = 0x1A, .06 = 0x0F). DiffPalette in the header says what each colors.
        _diff.card = MakeBrush(0xFF, 0x0D, 0x11, 0x17);
        _diff.border = MakeBrush(0xFF, 0x30, 0x36, 0x3D);
        _diff.head = MakeBrush(0xFF, 0x16, 0x1B, 0x22);
        _diff.code = MakeBrush(0xFF, 0xE6, 0xED, 0xF3);
        _diff.plus = MakeBrush(0xFF, 0x3F, 0xB9, 0x50);
        _diff.minus = MakeBrush(0xFF, 0xF8, 0x51, 0x49);
        _diff.meta = MakeBrush(0xFF, 0x8B, 0x94, 0x9E);
        _diff.number = MakeBrush(0xFF, 0x6E, 0x76, 0x81);
        _diff.addBand = MakeBrush(0x26, 0x2E, 0xA0, 0x43);
        _diff.addNumberBand = MakeBrush(0x4D, 0x3F, 0xB9, 0x50);
        _diff.removeBand = MakeBrush(0x1A, 0xF8, 0x51, 0x49);
        _diff.removeNumberBand = MakeBrush(0x4D, 0xF8, 0x51, 0x49);
        _diff.hunkBand = MakeBrush(0x1A, 0x38, 0x8B, 0xFD);
        _diff.expandBand = MakeBrush(0x0F, 0x38, 0x8B, 0xFD);
        _diff.expandText = MakeBrush(0xFF, 0x9E, 0xCB, 0xFF);
        _diff.now = MakeBrush(0xFF, 0xD2, 0x99, 0x22);
        _diff.chipBorder = MakeBrush(0xFF, 0x3A, 0x3A, 0x3A);
        _diff.chipText = MakeBrush(0xFF, 0x8A, 0x8A, 0x8A);
        _diff.reason = MakeBrush(0xFF, 0xC8, 0xC8, 0xC8);
        _diff.count = MakeBrush(0xFF, 0x9E, 0xCB, 0xFF);
        _diff.outlineRule = MakeBrush(0xFF, 0x2C, 0x2C, 0x2C);
        _diff.outlineTurn = MakeBrush(0xFF, 0xCF, 0xCF, 0xCF);
        _diff.outlineFile = MakeBrush(0xFF, 0xA8, 0xA8, 0xA8);
        _diff.outlineLit = SolidColorBrush{ ColorHelper::FromArgb(0x2E, accent.R, accent.G, accent.B) };

        // --- the host: a 3x3 star grid whose middle cell IS the card's 90% x 80% (no size math, no
        // SizeChanged handler — it follows every window resize through layout alone) ---
        _host = Grid{};
        _host.RequestedTheme(ElementTheme::Dark); // Agentmaster surfaces are always dark (CLAUDE.md conventions)
        _host.Visibility(Visibility::Collapsed);
        for (const double weight : { 1.0, 8.0, 1.0 }) // 10% · 80% · 10% of the height
        {
            RowDefinition row;
            row.Height(GridLength{ weight, GridUnitType::Star });
            _host.RowDefinitions().Append(row);
        }
        for (const double weight : { 5.0, 90.0, 5.0 }) // 5% · 90% · 5% of the width
        {
            ColumnDefinition column;
            column.Width(GridLength{ weight, GridUnitType::Star });
            _host.ColumnDefinitions().Append(column);
        }

        // The backdrop: a SIBLING of the card (not its parent), so a press inside the card never
        // bubbles to it — only a press that lands outside the card reaches this handler.
        _backdrop = Border{};
        _backdrop.Background(MakeBrush(0x66, 0x00, 0x00, 0x00));
        Grid::SetRowSpan(_backdrop, 3);
        Grid::SetColumnSpan(_backdrop, 3);
        _backdrop.PointerPressed([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const PointerRoutedEventArgs& e) {
            try
            {
                e.Handled(true); // the page behind must not see a press meant to close us
                const auto self = weak.get();
                if (!self || !self->_open)
                {
                    return;
                }
                // Deferred: collapsing the very layer under the pointer INSIDE its own press handler is the
                // tree mutation the pointer-handler rule forbids (the Archive / Sessions page lessons).
                self->_dispatcher.TryEnqueue([weak]() {
                    if (const auto overlay = weak.get())
                    {
                        overlay->Close(L"click outside", true);
                    }
                });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay backdrop press");
            }
        });
        _host.Children().Append(_backdrop);

        _card = Border{};
        _card.Background(MakeBrush(0xFF, 0x1E, 0x1E, 0x1E));
        _card.BorderBrush(MakeBrush(0xFF, 0x3C, 0x3C, 0x3C));
        _card.BorderThickness(Thickness{ 1, 1, 1, 1 });
        _card.CornerRadius(CornerRadius{ 8, 8, 8, 8 });
        Grid::SetRow(_card, 1);
        Grid::SetColumn(_card, 1);
        // Tab cycles inside the card: the overlay is modal, so keyboard focus must not walk out into
        // the page hidden behind it.
        _card.TabFocusNavigation(KeyboardNavigationMode::Cycle);
        _host.Children().Append(_card);

        // The card's rows: 0 the header, 1 the tabs, 2 the box row (box tabs only), 3 the divider, 4 the body.
        Grid layout;
        for (const auto height : { GridLength{ 0, GridUnitType::Auto }, GridLength{ 0, GridUnitType::Auto }, GridLength{ 0, GridUnitType::Auto }, GridLength{ 0, GridUnitType::Auto }, GridLength{ 1, GridUnitType::Star } })
        {
            RowDefinition row;
            row.Height(height);
            layout.RowDefinitions().Append(row);
        }
        _card.Child(layout);

        // --- header: [icon] Transcript  <session line ...>  [Wrap] [Copy] [Refresh] [Close] ---
        _headerRow = Grid{};
        for (const auto width : { GridLength{ 0, GridUnitType::Auto }, GridLength{ 0, GridUnitType::Auto }, GridLength{ 1, GridUnitType::Star }, GridLength{ 0, GridUnitType::Auto } })
        {
            ColumnDefinition column;
            column.Width(width);
            _headerRow.ColumnDefinitions().Append(column);
        }
        _headerRow.Margin(Thickness{ 0, 4, 0, 0 });
        {
            FontIcon icon;
            icon.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            icon.Glyph(L"\xE736"); // ReadingMode — the same glyph as the menus' "Show Transcript"
            icon.FontSize(16);
            icon.Foreground(_bodyBrush);
            icon.Margin(Thickness{ 16, 0, 10, 0 });
            icon.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(icon, 0);
            _headerRow.Children().Append(icon);

            TextBlock title;
            title.Text(L"Transcript");
            title.FontSize(15);
            title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            title.Foreground(_bodyBrush);
            title.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(title, 1);
            _headerRow.Children().Append(title);

            _subtitleText = TextBlock{};
            _subtitleText.FontSize(13);
            _subtitleText.Foreground(_dimBrush);
            _subtitleText.TextWrapping(TextWrapping::NoWrap);
            _subtitleText.TextTrimming(TextTrimming::CharacterEllipsis);
            _subtitleText.VerticalAlignment(VerticalAlignment::Center);
            _subtitleText.Margin(Thickness{ 12, 0, 12, 0 });
            Grid::SetColumn(_subtitleText, 2);
            _headerRow.Children().Append(_subtitleText);

            StackPanel buttons;
            buttons.Orientation(Orientation::Horizontal);
            buttons.Spacing(2);
            buttons.Margin(Thickness{ 0, 6, 8, 6 });
            const auto makeIconButton = [this](const wchar_t* glyph, const wchar_t* tip, const wchar_t* name) {
                Button b;
                b.Background(_transparentBrush); // transparent (not null) — still hit-testable, the template still highlights hover
                b.BorderThickness(Thickness{ 0, 0, 0, 0 });
                b.Padding(Thickness{ 9, 6, 9, 6 });
                b.MinWidth(0);
                b.MinHeight(0);
                FontIcon glyphIcon;
                glyphIcon.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
                glyphIcon.Glyph(glyph);
                glyphIcon.FontSize(14);
                // Click-through glyph: a hit-testable FontIcon on a transparent button swallows presses
                // landing on its own pixels (the badge's mkIconBtn lesson) — the button must be the only target.
                glyphIcon.IsHitTestVisible(false);
                b.Content(glyphIcon);
                AgentSetTip(b, winrt::hstring{ tip });
                winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(b, winrt::hstring{ name });
                return b;
            };
            // The ↵ LINE-WRAP toggle — first in the group: a view setting, before the actions. ON (the
            // default): long lines wrap at the body's right edge. OFF: every line keeps its length and the
            // part past the right edge is hidden. GLOBAL — remembered in settings.json and read back on every
            // open, in any window (_ToggleWrapLines / Show). Built by hand, not by makeIconButton: its ON
            // state is a tinted FACE inside the content (see _wrapButtonFace), so the face carries the
            // padding the button carries elsewhere — 1 px hairline + 8 / 5 padding == the icon buttons'
            // 9 / 6, so the toggle is exactly their size.
            _wrapGlyph = FontIcon{};
            _wrapGlyph.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            _wrapGlyph.Glyph(kWrapGlyph);
            _wrapGlyph.FontSize(14);
            _wrapGlyph.IsHitTestVisible(false); // the button must be the only target (the badge's mkIconBtn lesson)
            _wrapButtonFace = Border{};
            _wrapButtonFace.BorderThickness(Thickness{ 1, 1, 1, 1 }); // always 1 px: a flip only recolors it, so nothing moves
            _wrapButtonFace.CornerRadius(CornerRadius{ 4, 4, 4, 4 }); // the button template's corner radius (ControlCornerRadius)
            _wrapButtonFace.Padding(Thickness{ 8, 5, 8, 5 });
            // Click-through too: a TINTED Border is hit-testable and would take the press itself; the
            // button's transparent background already makes its whole area the target.
            _wrapButtonFace.IsHitTestVisible(false);
            _wrapButtonFace.Child(_wrapGlyph);
            _wrapButton = Button{};
            _wrapButton.Background(_transparentBrush); // transparent (not null) — still hit-testable, the template still highlights hover
            _wrapButton.BorderThickness(Thickness{ 0, 0, 0, 0 });
            _wrapButton.Padding(Thickness{ 0, 0, 0, 0 }); // the face carries the padding (see above)
            _wrapButton.MinWidth(0);
            _wrapButton.MinHeight(0);
            _wrapButton.Content(_wrapButtonFace);
            _wrapButton.Click([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_ToggleWrapLines();
                    }
                }
                catch (...)
                {
                    // Rule #18. Recovery: none needed — the mode is applied and remembered before the only
                    // step that can throw here (the toggle's repaint); a stale toggle look fixes itself on
                    // the next click or open.
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay wrap toggle");
                }
            });
            buttons.Children().Append(_wrapButton);
            _UpdateWrapButton(); // the default look (ON) until Show reads the remembered state
            auto copyButton = makeIconButton(L"\xE8C8", L"Copy this level's whole transcript as Markdown - everything, even past where a very long view stops", L"Copy transcript");
            copyButton.Click([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_CopyCurrentLevel();
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay copy"); // no copy, no chime
                }
            });
            buttons.Children().Append(copyButton);
            auto refreshButton = makeIconButton(L"\xE72C", L"Render the transcript again - a working session keeps adding to it", L"Refresh transcript");
            refreshButton.Click([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_Refresh();
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay refresh"); // the view stays as it was
                }
            });
            buttons.Children().Append(refreshButton);
            auto closeButton = makeIconButton(L"\xE711", L"Close (Esc, or click outside the transcript)", L"Close transcript");
            closeButton.Click([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                if (const auto self = weak.get())
                {
                    self->Close(L"close button", true);
                }
            });
            buttons.Children().Append(closeButton);
            Grid::SetColumn(buttons, 3);
            _headerRow.Children().Append(buttons);
        }
        Grid::SetRow(_headerRow, 0);
        layout.Children().Append(_headerRow);

        // --- the level tabs (the Copy menu's order — Default first) + the status line + "Expand all" ---
        _tabsRow = Grid{};
        for (const auto width : { GridLength{ 0, GridUnitType::Auto }, GridLength{ 1, GridUnitType::Star }, GridLength{ 0, GridUnitType::Auto } })
        {
            ColumnDefinition column;
            column.Width(width);
            _tabsRow.ColumnDefinitions().Append(column);
        }
        _tabsRow.Margin(Thickness{ 8, 0, 16, 0 });
        StackPanel tabs;
        tabs.Orientation(Orientation::Horizontal);
        tabs.Spacing(2);
        // Every tab, in the row's order: the five levels in the Copy menu's order (Default first, its words from the menu's
        // own table), then — after a separator — the three box tabs (TRANSCRIPT_OVERLAY.md §3j; their words from
        // TranscriptViews.h), then — after a second separator — the Diff tab (§3l). A tab's position is its digit key (1-9).
        struct TabEntry
        {
            const wchar_t* label;
            int view;
        };
        std::vector<TabEntry> entries;
        for (const auto& item : kTranscriptCopyMenu)
        {
            entries.push_back(TabEntry{ item.label, item.which - kCopyFieldTranscriptLevelBase });
        }
        for (int view = ::Agentmaster::kTranscriptFirstBoxView; view < ::Agentmaster::kTranscriptViewCount; ++view)
        {
            entries.push_back(TabEntry{ ::Agentmaster::TranscriptViewLabel(view), view });
        }
        size_t position = 0;
        for (const auto& item : entries)
        {
            const int level = item.view;
            if (level == ::Agentmaster::kTranscriptFirstBoxView || ::Agentmaster::IsTranscriptDiffView(level))
            {
                // The SEPARATOR the user asked for left of the box tabs: the five levels on its left, the tabs whose boxes
                // the user picks on its right; and a second one left of the Diff tab, which is neither (a page of its own
                // over the same conversation). A childless Border (no composition node), out of the tab order and of what
                // a screen reader reads (Raw).
                Border separator;
                separator.Width(1);
                separator.Height(kTabSeparatorHeight);
                separator.Background(_ruleBrush);
                separator.VerticalAlignment(VerticalAlignment::Top);
                separator.Margin(Thickness{ kTabSeparatorGap, kTabSeparatorTop, kTabSeparatorGap, 0 });
                winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetAccessibilityView(separator, winrt::Windows::UI::Xaml::Automation::Peers::AccessibilityView::Raw);
                tabs.Children().Append(separator);
            }
            Button tab;
            tab.Background(_transparentBrush);
            tab.BorderThickness(Thickness{ 0, 0, 0, 0 });
            tab.Padding(Thickness{ 10, 5, 10, 0 });
            tab.MinWidth(0);
            tab.MinHeight(0);
            // The selection underline lives INSIDE the content, not on the button's border: the button
            // template's hover / press states set its BorderBrush, and a visual-state setter outranks a
            // local value — an underline drawn there would vanish under the pointer.
            StackPanel inner;
            TextBlock label;
            label.Text(item.label);
            label.FontSize(13);
            label.Foreground(_bodyBrush);
            Border underline;
            underline.Height(2);
            underline.CornerRadius(CornerRadius{ 1, 1, 1, 1 });
            underline.Margin(Thickness{ 0, 5, 0, 0 });
            underline.Background(_transparentBrush);
            inner.Children().Append(label);
            inner.Children().Append(underline);
            tab.Content(inner);
            // The tab's tooltip is that level's CHEAT SHEET (what it adds over the level below, what it
            // still leaves out and where each of those first shows), built from the same table as the (i)
            // panel - not the Copy menu's one-line description - so the two can never disagree. A box tab's
            // says which side it shows and how its boxes work (TranscriptViewTip hands a level to TranscriptLevelTip).
            AgentSetTip(tab, winrt::hstring{ ::Agentmaster::TranscriptViewTip(level, position + 1) });
            winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(tab, winrt::hstring{ std::wstring{ item.label } + L" transcript" });
            tab.Click([weak = get_weak(), position](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_SelectTabPosition(position);
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay tab click"); // the current level stays
                }
            });
            tabs.Children().Append(tab);
            _tabButtons.push_back(tab);
            _tabLabels.push_back(label);
            _tabUnderlines.push_back(underline);
            _tabViews.push_back(level);
            ++position;
        }
        // The LEVEL GUIDE: an (i) right after the last tab - inside the tabs' own stack, so it always follows
        // them - whose Flyout compares every level side by side (_BuildLevelGuidePanel). It is the button's
        // own Flyout, so a click, Enter or Space opens it with no handler of ours, and keyboard focus moves
        // into it (the focus guard below lets a move into a popup through).
        {
            FontIcon guideGlyph;
            guideGlyph.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
            guideGlyph.Glyph(kGuideGlyph);
            guideGlyph.FontSize(14);
            guideGlyph.Foreground(_dimBrush);
            guideGlyph.IsHitTestVisible(false); // the button must be the only target (the badge's mkIconBtn lesson)
            _guideButton = Button{};
            _guideButton.Background(_transparentBrush); // transparent (not null) — hit-testable, hover still highlights
            _guideButton.BorderThickness(Thickness{ 0, 0, 0, 0 });
            // On the tab labels' line: a tab is 5 DIPs of padding over its 13 DIP label (the underline below
            // it), so the glyph's 14 DIP box starts 2 DIPs lower to sit level with the words.
            _guideButton.Padding(Thickness{ 8, 7, 8, 7 });
            _guideButton.MinWidth(0);
            _guideButton.MinHeight(0);
            _guideButton.VerticalAlignment(VerticalAlignment::Top);
            _guideButton.Content(guideGlyph);
            AgentSetTip(_guideButton, winrt::hstring{ kGuideTip });
            winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(_guideButton, winrt::hstring{ L"What each level shows" });

            _guideFlyout = Flyout{};
            _guideFlyout.Placement(Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
            // The presenter: wider and taller than a Flyout's defaults (the table needs the room), and dark
            // with explicit brushes - a Flyout renders in the popup root, where the overlay's RequestedTheme
            // does not reach, and theme inheritance into popups is unreliable under XAML Islands (the reason
            // AgentSetTip pins its tips dark).
            Style presenter{ winrt::xaml_typename<FlyoutPresenter>() };
            const auto setters = presenter.Setters();
            setters.Append(Setter{ FrameworkElement::MaxWidthProperty(), winrt::box_value(kGuideMaxWidth) });
            setters.Append(Setter{ FrameworkElement::MaxHeightProperty(), winrt::box_value(kGuideMaxHeight) });
            setters.Append(Setter{ FrameworkElement::RequestedThemeProperty(), winrt::box_value(ElementTheme::Dark) });
            setters.Append(Setter{ Control::BackgroundProperty(), MakeBrush(0xFF, 0x25, 0x25, 0x26) });
            setters.Append(Setter{ Control::BorderBrushProperty(), MakeBrush(0xFF, 0x48, 0x48, 0x48) });
            setters.Append(Setter{ Control::BorderThicknessProperty(), winrt::box_value(Thickness{ 1, 1, 1, 1 }) });
            setters.Append(Setter{ Control::PaddingProperty(), winrt::box_value(Thickness{ 18, 14, 18, 16 }) });
            _guideFlyout.FlyoutPresenterStyle(presenter);
            // Refilled on EVERY open, so the highlighted column is always the level on screen (a tab switch
            // between two opens changes it). A failed build keeps whatever the guide showed last.
            _guideFlyout.Opening([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::Foundation::IInspectable&) {
                try
                {
                    if (const auto self = weak.get(); self && self->_guideFlyout)
                    {
                        self->_guideFlyout.Content(self->_BuildLevelGuidePanel());
                    }
                }
                catch (...)
                {
                    // Rule #18. Recovery: the guide opens with its previous content (empty on a first open).
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay level guide");
                }
            });
            // Kept from the events rather than read off the Flyout, so DismissLevelGuide and Close know
            // whether a hide is needed without asking a popup that may be mid-animation.
            _guideFlyout.Opened([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::Foundation::IInspectable&) {
                if (const auto self = weak.get())
                {
                    self->_guideOpen = true;
                }
            });
            _guideFlyout.Closed([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::Foundation::IInspectable&) {
                if (const auto self = weak.get())
                {
                    self->_guideOpen = false;
                }
            });
            _guideButton.Flyout(_guideFlyout);
            tabs.Children().Append(_guideButton);
        }
        Grid::SetColumn(tabs, 0);
        _tabsRow.Children().Append(tabs);
        _statusText = TextBlock{};
        _statusText.FontSize(kStatusFontSize);
        _statusText.Foreground(_dimBrush);
        _statusText.HorizontalAlignment(HorizontalAlignment::Right);
        _statusText.VerticalAlignment(VerticalAlignment::Center);
        _statusText.TextWrapping(TextWrapping::NoWrap);
        _statusText.TextTrimming(TextTrimming::CharacterEllipsis);
        _statusText.Margin(Thickness{ 12, 0, 0, 0 });
        Grid::SetColumn(_statusText, 1);
        _tabsRow.Children().Append(_statusText);
        // "Expand all" / "Collapse all": opens (or folds back) every tool-call list of the CURRENT level.
        // Link-colored text, so it reads as an action next to the dim status; Collapsed until the current
        // level turns out to have folds (_UpdateExpandAllButton).
        _expandAllLabel = TextBlock{};
        _expandAllLabel.FontSize(kStatusFontSize);
        _expandAllLabel.Foreground(_linkBrush);
        _expandAllLabel.Text(L"Expand all");
        _expandAllButton = Button{};
        _expandAllButton.Background(_transparentBrush); // transparent (not null) — hit-testable, hover still highlights
        _expandAllButton.BorderThickness(Thickness{ 0, 0, 0, 0 });
        _expandAllButton.Padding(Thickness{ 10, 4, 0, 4 });
        _expandAllButton.MinWidth(0);
        _expandAllButton.MinHeight(0);
        _expandAllButton.VerticalAlignment(VerticalAlignment::Center);
        _expandAllButton.Visibility(Visibility::Collapsed);
        _expandAllButton.Content(_expandAllLabel);
        AgentSetTip(_expandAllButton, winrt::hstring{ L"Open or fold back every turn's tool calls at this level. A click on one turn's summary line opens just that turn." });
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(_expandAllButton, winrt::hstring{ L"Expand all" });
        _expandAllButton.Click([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            try
            {
                if (const auto self = weak.get())
                {
                    self->_ToggleExpandAll();
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay expand all"); // the folds stay as they were
            }
        });
        Grid::SetColumn(_expandAllButton, 2);
        _tabsRow.Children().Append(_expandAllButton);
        Grid::SetRow(_tabsRow, 1);
        layout.Children().Append(_tabsRow);

        // --- the BOX ROW (TRANSCRIPT_OVERLAY.md §3j): "Also show:", then the twelve boxes, each level's group under its
        // caption (Compact · Default · Full · Max), in a panel that wraps on a narrow window. Built once; shown, filled and
        // greyed for the box tab on screen by _SyncBoxesRow, Collapsed on a level. Real CheckBoxes: a screen reader and the
        // e2e driver get the Toggle pattern and the state for free, and the dark theme above draws them dark. ---
        _boxesRow = Grid{};
        _boxesRow.Visibility(Visibility::Collapsed);
        _boxesRow.Margin(Thickness{ 18, 0, 16, 4 });
        {
            ColumnDefinition leadColumn;
            leadColumn.Width(GridLength{ 0, GridUnitType::Auto });
            ColumnDefinition boxesColumn;
            boxesColumn.Width(GridLength{ 1, GridUnitType::Star });
            _boxesRow.ColumnDefinitions().Append(leadColumn);
            _boxesRow.ColumnDefinitions().Append(boxesColumn);
            TextBlock lead;
            lead.Text(kBoxRowLead);
            lead.FontSize(kBoxFontSize);
            lead.Foreground(_dimBrush);
            // Level with the words of the first line's boxes (a CheckBox's template is 32 DIPs tall, its words near the
            // middle), and the lead stays at the top when the boxes wrap onto a second line.
            lead.VerticalAlignment(VerticalAlignment::Top);
            lead.Margin(Thickness{ 0, 7, 12, 0 });
            Grid::SetColumn(lead, 0);
            _boxesRow.Children().Append(lead);
            const auto boxPanel = winrt::make<AgentTranscriptBoxWrapPanel>();
            Grid::SetColumn(boxPanel, 1);
            int group = -1; // the level whose caption was added last (-1: none yet)
            for (size_t index = 0; index < std::size(::Agentmaster::kTranscriptViewBoxes); ++index)
            {
                const auto& box = ::Agentmaster::kTranscriptViewBoxes[index];
                if (static_cast<int>(box.level) != group)
                {
                    // A level's caption opens its group: the boxes after it are what that level adds.
                    group = static_cast<int>(box.level);
                    TextBlock caption;
                    caption.Text(::Agentmaster::TranscriptLevelLabel(box.level));
                    caption.FontSize(kBoxGroupFontSize);
                    caption.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
                    caption.Foreground(_dimBrush);
                    caption.Margin(Thickness{ index == 0 ? 0.0 : kBoxGroupGapLeft, 0, 0, 0 });
                    boxPanel.Children().Append(caption);
                }
                CheckBox check;
                // A CheckBox's theme minimums (120 x 32) would give twelve boxes a row each on most windows: as wide as its
                // words instead. Its words are its content - and so its name for a screen reader.
                check.MinWidth(0);
                check.MinHeight(0);
                check.FontSize(kBoxFontSize);
                check.Content(winrt::box_value(winrt::hstring{ box.label }));
                // The tooltip says what the box shows (the level guide's own sentence), where it comes from, and when it
                // has no effect.
                AgentSetTip(check, winrt::hstring{ ::Agentmaster::TranscriptViewBoxTip(box) });
                // Click, never Checked / Unchecked: Click comes only from the user (a press, Space), so _SyncBoxesRow can
                // set IsChecked for another tab without re-entering here.
                check.Click([weak = get_weak(), index](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    try
                    {
                        if (const auto self = weak.get())
                        {
                            self->_ToggleBox(index);
                        }
                    }
                    catch (...)
                    {
                        // Rule #18. Recovery: none needed - the view keeps what it showed; the box's own check may now
                        // disagree with it until the next tab switch re-syncs the row.
                        ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay box click");
                    }
                });
                boxPanel.Children().Append(check);
                _boxChecks.push_back(check);
            }
            _boxesRow.Children().Append(boxPanel);
        }
        Grid::SetRow(_boxesRow, 2);
        layout.Children().Append(_boxesRow);

        Border divider;
        divider.Height(1);
        divider.Background(_ruleBrush);
        Grid::SetRow(divider, 3);
        layout.Children().Append(divider);

        // --- the body: the level views' host, and over its bottom-right corner the two JUMP buttons, ↑ stacked on ↓ (the
        // user's picks: stacked, "40% smaller and more to right bottom"). A layer of its own, so _ReleaseLevels' Clear of
        // the views' host never takes the buttons with it; the buttons come last, so they draw over the views. ---
        _bodyLayer = Grid{};
        Grid::SetRow(_bodyLayer, 4);
        _bodyHost = Grid{};
        _bodyLayer.Children().Append(_bodyHost);
        {
            StackPanel jumps;
            jumps.Orientation(Orientation::Vertical);
            jumps.Spacing(kJumpButtonGap);
            // No background: only the buttons take presses; the gap between them and the rest of the corner pass through
            // to the view under them.
            jumps.HorizontalAlignment(HorizontalAlignment::Right);
            jumps.VerticalAlignment(VerticalAlignment::Bottom);
            jumps.Margin(Thickness{ 0, 0, kJumpMarginRight, kJumpMarginBottom });
            const auto faceBrush = MakeBrush(0xF2, 0x2D, 0x2D, 0x2D);
            const auto edgeBrush = MakeBrush(0xFF, 0x4A, 0x4A, 0x4A);
            const auto makeJump = [this, &faceBrush, &edgeBrush](const wchar_t* glyph, const wchar_t* tip, const wchar_t* name, bool toEnd) {
                FontIcon arrow;
                arrow.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
                arrow.Glyph(glyph);
                arrow.FontSize(kJumpGlyphSize);
                arrow.Foreground(_bodyBrush);
                arrow.IsHitTestVisible(false); // the button must be the only target (the badge's mkIconBtn lesson)
                Button jump;
                jump.Width(kJumpButtonSize);
                jump.Height(kJumpButtonSize);
                jump.MinWidth(0);
                jump.MinHeight(0);
                jump.Padding(Thickness{ 0, 0, 0, 0 });
                jump.CornerRadius(CornerRadius{ kJumpButtonSize / 2, kJumpButtonSize / 2, kJumpButtonSize / 2, kJumpButtonSize / 2 });
                jump.Background(faceBrush);
                jump.BorderBrush(edgeBrush);
                jump.BorderThickness(Thickness{ 1, 1, 1, 1 });
                jump.HorizontalContentAlignment(HorizontalAlignment::Center);
                jump.VerticalContentAlignment(VerticalAlignment::Center);
                jump.Content(arrow);
                AgentSetTip(jump, winrt::hstring{ tip });
                winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(jump, winrt::hstring{ name });
                // Hidden until the view can be scrolled that way (_UpdateJumpButtons): Opacity + hit testing + tab stop, never
                // Visibility, so showing one never re-lays the body out.
                jump.Opacity(0.0);
                jump.IsHitTestVisible(false);
                jump.IsTabStop(false);
                jump.Click([weak = get_weak(), toEnd](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    try
                    {
                        if (const auto self = weak.get())
                        {
                            self->_JumpToEdge(toEnd);
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay jump button"); // the view stays where it was
                    }
                });
                return jump;
            };
            _jumpTopButton = makeJump(kJumpTopGlyph, kJumpTopTip, L"Jump to the top", false);
            _jumpEndButton = makeJump(kJumpEndGlyph, kJumpEndTip, L"Jump to the bottom", true);
            jumps.Children().Append(_jumpTopButton);
            jumps.Children().Append(_jumpEndButton);
            _bodyLayer.Children().Append(jumps);
        }
        layout.Children().Append(_bodyLayer);

        // --- keyboard: Esc closes; 1..9 pick a tab by position (bubbling from whatever inside the card
        // has focus — the scroll view, a selected text block, a button, a box) ---
        _card.KeyDown([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const KeyRoutedEventArgs& e) {
            try
            {
                const auto self = weak.get();
                if (!self || !self->_open)
                {
                    return;
                }
                const auto key = e.Key();
                if (key == VirtualKey::Escape)
                {
                    e.Handled(true);
                    // The topmost thing first: an open level guide closes alone, the overlay on the next Esc.
                    if (!self->DismissLevelGuide())
                    {
                        self->Close(L"esc", true);
                    }
                    return;
                }
                // Plain digits only: a Ctrl / Alt chord with a digit belongs to the app (switchToTab).
                if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
                {
                    return;
                }
                int tabPosition = -1; // (not `position`: /W4 + warnings-as-errors flags shadowing the loop counter above)
                if (key >= VirtualKey::Number1 && key <= VirtualKey::Number9)
                {
                    tabPosition = static_cast<int>(key) - static_cast<int>(VirtualKey::Number1);
                }
                else if (key >= VirtualKey::NumberPad1 && key <= VirtualKey::NumberPad9)
                {
                    tabPosition = static_cast<int>(key) - static_cast<int>(VirtualKey::NumberPad1);
                }
                if (tabPosition >= 0 && static_cast<size_t>(tabPosition) < self->_tabViews.size())
                {
                    e.Handled(true);
                    self->_SelectTabPosition(static_cast<size_t>(tabPosition));
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay key");
            }
        });

        // --- the focus guard: while open, refuse a focus move from inside the card to anything else on
        // the PAGE (the terminal behind us, a Manager card its refresh re-focuses). Leaving the window,
        // or moving into a popup (the text context menu renders in the popup root, outside the page),
        // is allowed — and once closed (_open is cleared FIRST in Close) everything is allowed again. ---
        _card.LosingFocus([weak = get_weak()](const UIElement&, const LosingFocusEventArgs& args) {
            const auto self = weak.get();
            if (!self || !self->_open)
            {
                return;
            }
            try
            {
                const auto next = args.NewFocusedElement();
                if (!next || _IsUnder(next, self->_host) || !_IsUnder(next, self->_pageRoot))
                {
                    return;
                }
                args.TryCancel();
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay focus guard"); // the focus move simply proceeds
            }
        });
    }

    void AgentTranscriptOverlay::Show(const TranscriptOverlaySource& source, int level) noexcept
    {
        try
        {
            if (!_host)
            {
                return;
            }
            const bool sameConversation = _open && source.sessionId == _source.sessionId && source.codex == _source.codex &&
                                          source.codexRolloutId == _source.codexRolloutId;
            if (!sameConversation)
            {
                // A different conversation (or a fresh open): nothing rendered so far applies. Bumping the
                // generation first makes any render / build slice still in flight for the old view a no-op.
                ++_generation;
                _ReleaseLevels();
                _source = source;
                // The box tabs' boxes are, like the tab and the wrap state, the ones last ticked in ANY window — read
                // fresh with every new view of a conversation, before _SelectLevel renders one. An overlay already open
                // on this conversation keeps its own (the same-conversation path below): its views were rendered with them.
                _boxes = _RememberedBoxes();
            }
            else if (!source.title.empty())
            {
                _source.title = source.title; // a rename since the last open
            }
            _open = true;
            _UpdateTitle();
            // The line-wrap state is, like the level, the one last chosen in ANY window — read fresh now, so
            // a click on the ↵ toggle in another window since this one last opened counts. Read BEFORE
            // _SelectLevel, which brings an already-built view of this conversation up to it.
            _wrapLines = _RememberedWrapLines();
            _UpdateWrapButton();
            _host.Visibility(Visibility::Visible);
            // kRememberedLevel (any negative value): the tab last picked in ANY window — a level or a box tab — read
            // fresh from settings.json now; a pick made in another window since this one last opened counts.
            const int wanted = level < 0 ? _RememberedLevel() : level;
            _SelectLevel(std::clamp(wanted, 0, ::Agentmaster::kTranscriptViewCount - 1), true);
        }
        catch (...)
        {
            // Rule #18. Recovery: stay closed — a half-built overlay must not sit over the page eating clicks.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::Show");
            _open = false;
            try
            {
                if (_host)
                {
                    _host.Visibility(Visibility::Collapsed);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::Show collapse");
            }
        }
    }

    void AgentTranscriptOverlay::Close(std::wstring_view reason, bool restoreFocus) noexcept
    {
        if (!_open)
        {
            return;
        }
        // Cleared FIRST: the focus guard reads it, and collapsing the host while focus is inside moves
        // focus out — which must be allowed now.
        _open = false;
        ++_generation;
        try
        {
            // Tooltips live in the shared host's popup, which a collapsed parent does not hide.
            if (_headerRow)
            {
                AgentCloseTipsIn(_headerRow);
            }
            if (_tabsRow)
            {
                AgentCloseTipsIn(_tabsRow);
            }
            // The box row's tips and the jump buttons' tips live in that same popup.
            if (_boxesRow)
            {
                AgentCloseTipsIn(_boxesRow);
            }
            if (_bodyLayer)
            {
                AgentCloseTipsIn(_bodyLayer);
            }
            // The level guide is a popup of its own, so collapsing the host would leave it floating over
            // the page with nothing under it: hide it with the overlay.
            if (_guideFlyout && _guideOpen)
            {
                _guideFlyout.Hide();
            }
            if (_host)
            {
                _host.Visibility(Visibility::Collapsed);
            }
            ::Agentmaster::LogNav(L"transcript-close " + ::Agentmaster::ShortId(_source.sessionId) + L" (" + std::wstring{ reason } + L")");
            // Release the rendered XAML LATER, at low priority: tearing down a long transcript's elements
            // here would delay whatever closed us (a key press, a tab switch). A reopen before it runs
            // finds _open set (or a newer generation) and keeps its own views.
            if (_dispatcher)
            {
                _dispatcher.TryEnqueue(DispatcherQueuePriority::Low, [weak = get_weak(), generation = _generation]() {
                    const auto self = weak.get();
                    if (self && !self->_open && self->_generation == generation)
                    {
                        self->_ReleaseLevels();
                    }
                });
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::Close");
        }
        if (restoreFocus && _onClosed)
        {
            try
            {
                _onClosed();
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay closed handler");
            }
        }
    }

    bool AgentTranscriptOverlay::DismissLevelGuide() noexcept
    {
        if (!_open || !_guideFlyout || !_guideOpen)
        {
            return false;
        }
        try
        {
            // Hide starts the close; Closed clears _guideOpen. Keyboard focus goes back to the (i) button,
            // inside the card, so the next Esc reaches the overlay's own handlers and closes it.
            _guideFlyout.Hide();
            return true;
        }
        catch (...)
        {
            // Rule #18. Recovery: report "nothing was open", so this Esc closes the overlay instead - and
            // Close hides the guide (or the guide closes with its popup's owner).
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay dismiss level guide");
            return false;
        }
    }

    void AgentTranscriptOverlay::_SelectTabPosition(size_t position)
    {
        if (position >= _tabViews.size())
        {
            return;
        }
        const int level = _tabViews[position];
        if (level != _current)
        {
            // The view's settings name: a level's ("compact") or a box tab's ("user-only").
            ::Agentmaster::LogNav(L"transcript-level " + ::Agentmaster::ShortId(_source.sessionId) + L" " + ::Agentmaster::TranscriptViewName(level));
        }
        _SelectLevel(level, true);
        // Every pick — the tab already showing included — is the level the next open uses, in any window
        // ("if the user picks Compact, the next transcript opens at Compact"). Another window may have
        // remembered a different level since this overlay opened, so even a re-pick writes.
        _RememberLevel(level);
    }

    int AgentTranscriptOverlay::_RememberedLevel() noexcept
    {
        try
        {
            // A level's name or a box tab's ("custom" / "user-only" / "assistant-only"); -1 for an empty or unknown one.
            const int level = ::Agentmaster::TranscriptViewFromName(::Agentmaster::LoadAppSettings().transcriptOverlayLevel);
            if (level >= 0)
            {
                return level;
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: open at Default — a failed settings read never blocks the overlay.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay remembered level");
        }
        return static_cast<int>(::Agentmaster::TurnsLevel::Default);
    }

    void AgentTranscriptOverlay::_RememberLevel(int level) noexcept
    {
        try
        {
            const std::wstring name{ ::Agentmaster::TranscriptViewName(level) };
            // FRESHEST disk, one field (the ColorPickupFlyout idiom): never write a stale in-memory copy
            // over a cog Save or another window's settings change.
            auto settings = ::Agentmaster::LoadAppSettings();
            if (settings.transcriptOverlayLevel != name)
            {
                settings.transcriptOverlayLevel = name;
                ::Agentmaster::SaveAppSettings(settings);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: nothing — the switch already happened; only the remembered level is lost.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay remember level");
        }
    }

    const std::vector<std::wstring>& AgentTranscriptOverlay::_BoxesOf(int view) const noexcept
    {
        // A level has no boxes: an empty list, the same object for every caller (an empty vector's construction never
        // throws, and a function-local static is built once, thread-safely).
        static const std::vector<std::wstring> kNoBoxes;
        if (!::Agentmaster::IsTranscriptBoxView(view))
        {
            return kNoBoxes;
        }
        return _boxes[static_cast<size_t>(view - ::Agentmaster::kTranscriptFirstBoxView)];
    }

    ::Agentmaster::TurnsOptions AgentTranscriptOverlay::_ViewOptions(int view) const noexcept
    {
        return ::Agentmaster::TranscriptViewOptions(view, _BoxesOf(view));
    }

    void AgentTranscriptOverlay::_SyncBoxesRow()
    {
        if (!_boxesRow)
        {
            return;
        }
        const bool boxTab = ::Agentmaster::IsTranscriptBoxView(_current);
        const auto wanted = boxTab ? Visibility::Visible : Visibility::Collapsed;
        if (_boxesRow.Visibility() != wanted)
        {
            _boxesRow.Visibility(wanted); // a tab switch between two levels never touches the row's layout
        }
        if (!boxTab)
        {
            return;
        }
        const auto& ticked = _BoxesOf(_current);
        for (size_t index = 0; index < _boxChecks.size() && index < std::size(::Agentmaster::kTranscriptViewBoxes); ++index)
        {
            const auto& box = ::Agentmaster::kTranscriptViewBoxes[index];
            const auto& check = _boxChecks[index];
            // The tab's own tick. Set only when it differs (an IsChecked change runs the box's visual-state change); a
            // programmatic change raises Checked / Unchecked, never the Click _ToggleBox listens to.
            const bool on = ::Agentmaster::TranscriptViewTicks(ticked, box.name);
            const auto shown = check.IsChecked();
            if ((shown && shown.Value()) != on)
            {
                check.IsChecked(on);
            }
            // Greyed while it would change nothing - its tick kept, so it comes back as it was the moment it matters again.
            // A Codex session has no turns view (the body says so): every box is greyed there.
            const bool enabled = !_source.codex && ::Agentmaster::TranscriptViewBoxTakesEffect(box, ticked);
            if (check.IsEnabled() != enabled)
            {
                check.IsEnabled(enabled);
            }
        }
    }

    void AgentTranscriptOverlay::_ToggleBox(size_t index)
    {
        const int view = _current;
        if (!_open || !::Agentmaster::IsTranscriptBoxView(view) || index >= _boxChecks.size() || index >= std::size(::Agentmaster::kTranscriptViewBoxes))
        {
            return;
        }
        const auto& box = ::Agentmaster::kTranscriptViewBoxes[index];
        // The CheckBox flipped itself before it raised Click: its state IS the user's choice.
        const auto checked = _boxChecks[index].IsChecked();
        const bool on = checked && checked.Value();
        auto& ticked = _boxes[static_cast<size_t>(view - ::Agentmaster::kTranscriptFirstBoxView)];
        if (::Agentmaster::TranscriptViewTicks(ticked, box.name) == on)
        {
            return; // the tab already says so - nothing to render again
        }
        ticked = ::Agentmaster::TranscriptViewWithBox(std::move(ticked), box.name, on);
        // Every window's next open (settings.json: this tab's list, this one name). Never throws.
        _RememberBox(view, box.name, on);
        // The dependent boxes' grey follows the full call lines' box ("ops").
        _SyncBoxesRow();
        // Render the tab again with its new boxes, the reader kept on the turn at the top of the view. A view that is still
        // loading has no headings yet: a turn jump left pending by the previous click carries over instead.
        auto& shown = _levels[static_cast<size_t>(view)];
        const int turn = shown.pendingTurnJump > 0 ? shown.pendingTurnJump : _TopTurnNumber(shown);
        _ResetView(view);
        _levels[static_cast<size_t>(view)].pendingTurnJump = turn;
        // The same tab again: nothing pending is dropped (the turn jump just set survives), the new view starts its render,
        // and the box keeps keyboard focus (no focusBody) so the next box is one Tab away.
        _SelectLevel(view, false);
    }

    std::array<std::vector<std::wstring>, ::Agentmaster::kTranscriptBoxViewCount> AgentTranscriptOverlay::_RememberedBoxes() noexcept
    {
        std::array<std::vector<std::wstring>, ::Agentmaster::kTranscriptBoxViewCount> boxes{};
        try
        {
            auto settings = ::Agentmaster::LoadAppSettings();
            for (int view = ::Agentmaster::kTranscriptFirstBoxView; view < ::Agentmaster::kTranscriptViewCount; ++view)
            {
                if (auto* list = RememberedBoxesField(settings, view))
                {
                    boxes[static_cast<size_t>(view - ::Agentmaster::kTranscriptFirstBoxView)] = std::move(*list);
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: every box clear (each tab's Minimal side) - a failed settings read never blocks the overlay.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay remembered boxes");
            boxes = {};
        }
        return boxes;
    }

    void AgentTranscriptOverlay::_RememberBox(int view, std::wstring_view name, bool on) noexcept
    {
        try
        {
            // FRESHEST disk, one tab's list, one name (the ColorPickupFlyout idiom, as _RememberLevel): never a stale
            // in-memory copy over a cog Save, another window's ticks or a newer build's names.
            auto settings = ::Agentmaster::LoadAppSettings();
            auto* list = RememberedBoxesField(settings, view);
            if (!list || ::Agentmaster::TranscriptViewTicks(*list, name) == on)
            {
                return; // not a box tab, or the file already says so (another window ticked it too)
            }
            *list = ::Agentmaster::TranscriptViewWithBox(std::move(*list), name, on);
            ::Agentmaster::SaveAppSettings(settings);
        }
        catch (...)
        {
            // Rule #18. Recovery: nothing - the view already changed; only the remembered tick is lost.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay remember box");
        }
    }

    void AgentTranscriptOverlay::_ResetView(int view) noexcept
    {
        if (view < 0 || view >= static_cast<int>(_levels.size()))
        {
            return;
        }
        auto& stale = _levels[static_cast<size_t>(view)];
        const bool expandAll = stale.expandAll; // the reader's choice, not the text's: kept, as a Refresh keeps it
        // What the body holds for the view: its frame (the Diff tab's grid), else its scroll view.
        FrameworkElement oldFrame{ nullptr };
        if (stale.frame)
        {
            oldFrame = stale.frame;
        }
        else if (stale.scroll)
        {
            oldFrame = stale.scroll;
        }
        // The epoch FIRST: a render still running for the old boxes is dropped when it lands (_ApplyLevelRender).
        ++_viewEpochs[static_cast<size_t>(view)];
        stale = LevelView{};
        stale.expandAll = expandAll;
        if (!oldFrame)
        {
            return;
        }
        try
        {
            // Off the screen at once: the new view is made beside it right away (_EnsureLevelView), in the body's one cell.
            oldFrame.Visibility(Visibility::Collapsed);
            if (_dispatcher)
            {
                // Out of the tree LATER, at low priority: a long view's thousands of elements cost their teardown on the UI
                // thread (XAML's event-table walk per element, CLAUDE.md "XAML's event table"), which must not stall the
                // click. Weak both ways: the overlay may close first (its _ReleaseLevels' Clear then took the view out).
                _dispatcher.TryEnqueue(DispatcherQueuePriority::Low, [weak = get_weak(), weakFrame = winrt::make_weak(oldFrame)]() {
                    try
                    {
                        const auto self = weak.get();
                        const auto frame = weakFrame.get();
                        if (!self || !frame || !self->_bodyHost)
                        {
                            return;
                        }
                        uint32_t at = 0;
                        if (self->_bodyHost.Children().IndexOf(frame, at))
                        {
                            self->_bodyHost.Children().RemoveAt(at);
                        }
                    }
                    catch (...)
                    {
                        // Rule #18. Recovery: the collapsed view stays in the tree until the overlay's next release.
                        ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay release reset view");
                    }
                });
            }
            else
            {
                uint32_t at = 0;
                if (_bodyHost && _bodyHost.Children().IndexOf(oldFrame, at))
                {
                    _bodyHost.Children().RemoveAt(at);
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the view is reset either way; the old XAML leaves with the overlay's next release.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay reset view");
        }
    }

    int AgentTranscriptOverlay::_TopTurnNumber(LevelView& view) noexcept
    {
        try
        {
            if (!view.scroll || !view.content || view.turnHeadings.empty())
            {
                return 0;
            }
            const double top = view.scroll.VerticalOffset();
            if (top < kJumpEdgeSlack)
            {
                return 0; // at the top: the new render starts there anyway
            }
            // A heading that a jump put at the top sits kSessionJumpGap below the edge, and must still count as the turn on
            // top - so the edge is moved down by that much.
            const double edge = top + kSessionJumpGap + kJumpEdgeSlack;
            const auto headingTop = [&view](size_t index) {
                const auto& element = view.turnHeadings[index].second;
                return element ? static_cast<double>(element.TransformToVisual(view.content).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f }).Y) :
                                 std::numeric_limits<double>::infinity();
            };
            // The headings were built top to bottom, so their tops rise with their index: binary search for the last one at
            // or above the edge (every heading before `low` is, every heading from `high` on is not).
            size_t low = 0;
            size_t high = view.turnHeadings.size();
            while (low < high)
            {
                const size_t middle = low + (high - low) / 2;
                if (headingTop(middle) <= edge)
                {
                    low = middle + 1;
                }
                else
                {
                    high = middle;
                }
            }
            return low == 0 ? 0 : view.turnHeadings[low - 1].first;
        }
        catch (...)
        {
            // Rule #18. Recovery: no turn - the re-render starts at the top.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay top turn");
        }
        return 0;
    }

    void AgentTranscriptOverlay::_ScrollToTurnHeading(LevelView& view, size_t index) noexcept
    {
        try
        {
            if (!view.scroll || !view.content || index >= view.turnHeadings.size() || !view.turnHeadings[index].second)
            {
                return;
            }
            const auto& heading = view.turnHeadings[index].second;
            // The session jump's recipe: lay out first (a heading built in this very slice has no position yet, and
            // ChangeView clamps to the current extent), then the heading's top in the content's coordinates is the offset.
            view.scroll.UpdateLayout();
            const auto at = heading.TransformToVisual(view.content).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
            view.scroll.ChangeView(nullptr, std::max(0.0, static_cast<double>(at.Y) - kSessionJumpGap), nullptr, true /* disableAnimation: the same place, not a glide */);
        }
        catch (...)
        {
            // Rule #18. Recovery: the view stays where it was (the top). Contained here: it runs inside a build slice.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay turn jump");
        }
    }

    void AgentTranscriptOverlay::_ScrollViewToEnd(LevelView& view, bool settle) noexcept
    {
        try
        {
            if (!view.scroll)
            {
                return;
            }
            if (settle)
            {
                view.scroll.UpdateLayout(); // the real extent: ChangeView clamps to the current one
            }
            view.scroll.ChangeView(nullptr, view.scroll.ScrollableHeight(), nullptr, true /* disableAnimation: a jump, not a glide */);
        }
        catch (...)
        {
            // Rule #18. Recovery: the view stays where it was. Contained here: the end-follow runs inside a build slice.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay scroll to end");
        }
    }

    void AgentTranscriptOverlay::_JumpToEdge(bool toEnd)
    {
        if (!_open)
        {
            return;
        }
        auto& view = _levels[static_cast<size_t>(_current)];
        if (!view.scroll)
        {
            return;
        }
        // An explicit jump outranks whatever was waiting to scroll this view (a chain link clicked early, a box click's
        // keep-the-turn jump, a Diff outline click): the reader asked for an edge.
        view.pendingSessionJump = 0;
        view.pendingTurnJump = 0;
        view.pendingCardJump = 0;
        if (toEnd)
        {
            _ScrollViewToEnd(view, true);
            // Still being built: follow the end until the build is done (_Pump), so the jump lands on the real last line.
            view.followEnd = view.state == LevelState::Building;
        }
        else
        {
            view.followEnd = false;
            view.scroll.ChangeView(nullptr, 0.0, nullptr, true /* disableAnimation: a jump, not a glide */);
        }
        // The keyboard into the view: Home / End / PageUp go on from here, and the button about to hide keeps no focus.
        view.scroll.Focus(FocusState::Programmatic);
        _UpdateJumpButtons();
    }

    void AgentTranscriptOverlay::_UpdateJumpButtons() noexcept
    {
        try
        {
            bool showTop = false;
            bool showEnd = false;
            if (_open && _current >= 0 && _current < static_cast<int>(_levels.size()))
            {
                const auto& view = _levels[static_cast<size_t>(_current)];
                if (view.scroll)
                {
                    const double offset = view.scroll.VerticalOffset();
                    const double scrollable = view.scroll.ScrollableHeight();
                    showTop = offset > kJumpEdgeSlack;
                    // Not while the view follows its end: each build slice would show it, and the next scroll hide it again.
                    showEnd = !view.followEnd && scrollable > kJumpEdgeSlack && offset < scrollable - kJumpEdgeSlack;
                }
            }
            // Only what changed: this runs for every ViewChanged / SizeChanged of the view on screen.
            const auto apply = [](const Button& button, bool shown, int8_t& last) {
                const int8_t state = static_cast<int8_t>(shown ? 1 : 0);
                if (!button || last == state)
                {
                    return;
                }
                last = state;
                button.Opacity(shown ? 1.0 : 0.0);
                button.IsHitTestVisible(shown);
                button.IsTabStop(shown);
            };
            apply(_jumpTopButton, showTop, _jumpTopShown);
            apply(_jumpEndButton, showEnd, _jumpEndShown);
        }
        catch (...)
        {
            // Rule #18 (the reporter is throttled). Recovery: the buttons keep their last state until the next update.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay jump buttons");
        }
    }

    void AgentTranscriptOverlay::_CancelFollowEnd(const winrt::Windows::Foundation::IInspectable& scroll) noexcept
    {
        for (auto& view : _levels)
        {
            // Identity: the event's sender is the view's own scroll view (a reset view's old scroll matches no view).
            if (view.scroll && view.scroll == scroll)
            {
                if (view.followEnd)
                {
                    view.followEnd = false;
                    _UpdateJumpButtons(); // the ↓ comes back: the reader is above the end now
                }
                return;
            }
        }
    }

    void AgentTranscriptOverlay::_SelectLevel(int level, bool focusBody)
    {
        // A session jump still pending on the level being LEFT (a chain link clicked before its heading was
        // built) is dropped: only the level on screen builds, so the jump would fire when the reader comes
        // back to it — scrolling away from wherever they are reading by then.
        if (level != _current && _current >= 0 && _current < static_cast<int>(_levels.size()))
        {
            auto& left = _levels[static_cast<size_t>(_current)];
            left.pendingSessionJump = 0;
            // The same for a box click's keep-the-turn jump, a ↓'s end-follow and a Diff outline click's card jump: each
            // belongs to the moment the reader left, and would scroll the view when they come back.
            left.pendingTurnJump = 0;
            left.followEnd = false;
            left.pendingCardJump = 0;
        }
        _current = level;
        _UpdateTabVisuals();
        // The box row follows the tab - shown and filled for a box tab, hidden for a level - before the view is made, so
        // the body is laid out once, under the row's final height.
        _SyncBoxesRow();
        _EnsureLevelView(level);
        for (int i = 0; i < static_cast<int>(_levels.size()); ++i)
        {
            // The view's FRAME is what the body holds: its scroll view, or the Diff tab's grid of the outline and the page.
            const auto& entry = _levels[i];
            FrameworkElement shown{ nullptr };
            if (entry.frame)
            {
                shown = entry.frame;
            }
            else if (entry.scroll)
            {
                shown = entry.scroll;
            }
            if (shown)
            {
                shown.Visibility(i == level ? Visibility::Visible : Visibility::Collapsed);
            }
        }
        auto& view = _levels[level];
        // A view last laid out under the other line-wrap mode — it was hidden when the ↵ toggle was clicked,
        // or another window changed the mode before this open — catches up now that it is visible, keeping
        // its reading position. A new view already has the mode (_EnsureLevelView): a no-op.
        _SyncWrapMode(view);
        if (view.state == LevelState::Idle)
        {
            _StartLoad(level);
        }
        else if (view.state == LevelState::Building)
        {
            _SchedulePump(); // resume a build this level paused when another tab was selected
        }
        _UpdateStatus();
        // The jump buttons follow the view on screen: a view kept from before has its own place, a new one none yet.
        _UpdateJumpButtons();
        if (focusBody)
        {
            _FocusBodyDeferred();
        }
    }

    void AgentTranscriptOverlay::_EnsureLevelView(int level)
    {
        auto& view = _levels[level];
        if (view.scroll)
        {
            return;
        }
        // The Diff tab: its frame holds an outline column beside the page, and its build draws cards and chips (§3l).
        const bool diff = ::Agentmaster::IsTranscriptDiffView(level);
        view.isDiff = diff;
        view.content = StackPanel{};
        view.scroll = ScrollViewer{};
        view.scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        // Never a sideways scroll. With line wrap ON the content takes the viewport's width, so every text
        // block WRAPS to it instead of running off to the right; with it OFF the lines keep their length and
        // the part past the right edge is cut off — hidden, since this mode keeps touch and the mouse wheel
        // from panning it into view (ApplyWrapMode has the details).
        view.scroll.HorizontalScrollMode(ScrollMode::Disabled);
        ApplyWrapMode(view.scroll, _wrapLines);
        view.wrapLines = _wrapLines;
        view.scroll.ZoomMode(ZoomMode::Disabled);
        view.scroll.IsTabStop(true); // focusable, so PageUp / PageDown / Home / End / arrows scroll it
        view.scroll.Padding(Thickness{ 24, 14, 24, 18 });
        view.scroll.Content(view.content);
        // The JUMP buttons follow this view while it is on screen: its offset (ViewChanged - a scroll, a jump, a clamp), its
        // extent growing as the build appends (the content's SizeChanged) and its viewport (its own SizeChanged - a window
        // resize, the box row showing). _UpdateJumpButtons reads the CURRENT view and sets only what changed, so a hidden
        // view's events cost a comparison. Each handler reaches the overlay weakly and none captures its own element (a
        // delegate that holds its element keeps it alive - CLAUDE.md "A handler that captures its own element").
        view.scroll.ViewChanged([weak = get_weak(), diff](const winrt::Windows::Foundation::IInspectable&, const ScrollViewerViewChangedEventArgs&) {
            if (const auto self = weak.get())
            {
                self->_UpdateJumpButtons(); // noexcept: contains its own failures
                if (diff)
                {
                    // The Diff outline lights the turn on top of the page as it scrolls (noexcept too).
                    self->_UpdateDiffOutlineHighlight();
                }
            }
        });
        view.scroll.SizeChanged([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const SizeChangedEventArgs&) {
            if (const auto self = weak.get())
            {
                self->_UpdateJumpButtons();
            }
        });
        view.content.SizeChanged([weak = get_weak()](const winrt::Windows::Foundation::IInspectable&, const SizeChangedEventArgs&) {
            if (const auto self = weak.get())
            {
                self->_UpdateJumpButtons();
            }
        });
        // The reader moving the view THEMSELF stops a ↓'s end-follow (LevelView::followEnd): a wheel turn, a press (the scroll
        // bar, a touch, a link, a text selection) or an upward key. handledEventsToo - the scroll view handles every one of
        // them itself. The end-follow's own ChangeView raises none of these, which is why ViewChanged cannot tell them apart.
        view.scroll.AddHandler(UIElement::PointerWheelChangedEvent(),
                               winrt::box_value(PointerEventHandler{ [weak = get_weak()](const winrt::Windows::Foundation::IInspectable& sender, const PointerRoutedEventArgs&) {
                                   if (const auto self = weak.get())
                                   {
                                       self->_CancelFollowEnd(sender);
                                   }
                               } }),
                               true /* handledEventsToo */);
        view.scroll.AddHandler(UIElement::PointerPressedEvent(),
                               winrt::box_value(PointerEventHandler{ [weak = get_weak()](const winrt::Windows::Foundation::IInspectable& sender, const PointerRoutedEventArgs&) {
                                   if (const auto self = weak.get())
                                   {
                                       self->_CancelFollowEnd(sender);
                                   }
                               } }),
                               true /* handledEventsToo */);
        view.scroll.AddHandler(UIElement::KeyDownEvent(),
                               winrt::box_value(KeyEventHandler{ [weak = get_weak()](const winrt::Windows::Foundation::IInspectable& sender, const KeyRoutedEventArgs& e) {
                                   try
                                   {
                                       // Only a key that moves the view UP leaves the end; End, Down and PageDown keep following it.
                                       const auto key = e.Key();
                                       if (key != VirtualKey::Up && key != VirtualKey::PageUp && key != VirtualKey::Home && key != VirtualKey::GamepadDPadUp)
                                       {
                                           return;
                                       }
                                       if (const auto self = weak.get())
                                       {
                                           self->_CancelFollowEnd(sender);
                                       }
                                   }
                                   catch (...)
                                   {
                                       // Rule #18. Recovery: the end-follow goes on (the build's end stops it anyway).
                                       ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay follow key");
                                   }
                               } }),
                               true /* handledEventsToo */);
        if (!diff)
        {
            view.frame = view.scroll;
            _bodyHost.Children().Append(view.scroll);
            return;
        }
        // The Diff tab's frame (the user's "Outline + long page"): [outline | hairline | the page]. The outline scrolls on
        // its own - it never sideways (its entries trim) - and is filled when the render lands (_BuildDiffOutline).
        Grid frame;
        for (const auto width : { GridLength{ kDiffOutlineWidth, GridUnitType::Pixel }, GridLength{ 1, GridUnitType::Pixel }, GridLength{ 1, GridUnitType::Star } })
        {
            ColumnDefinition column;
            column.Width(width);
            frame.ColumnDefinitions().Append(column);
        }
        view.outline = StackPanel{};
        view.outline.Padding(Thickness{ 8, 10, 8, 14 });
        view.outlineScroll = ScrollViewer{};
        view.outlineScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        view.outlineScroll.HorizontalScrollMode(ScrollMode::Disabled);
        view.outlineScroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        view.outlineScroll.ZoomMode(ZoomMode::Disabled);
        view.outlineScroll.IsTabStop(false); // its entries are the stops; the column itself is no place to rest
        view.outlineScroll.Content(view.outline);
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(view.outlineScroll, winrt::hstring{ L"Turns that changed files" });
        Grid::SetColumn(view.outlineScroll, 0);
        frame.Children().Append(view.outlineScroll);
        Border rule; // childless: no composition node
        rule.Background(_diff.outlineRule);
        Grid::SetColumn(rule, 1);
        frame.Children().Append(rule);
        Grid::SetColumn(view.scroll, 2);
        frame.Children().Append(view.scroll);
        view.frame = frame;
        _bodyHost.Children().Append(frame);
    }

    void AgentTranscriptOverlay::_SetLevelMessage(int level, const std::wstring& text)
    {
        auto& view = _levels[level];
        view.message = text;
        if (!view.content)
        {
            return;
        }
        if (!view.messageText)
        {
            view.messageText = TextBlock{};
            view.messageText.FontSize(13);
            view.messageText.Foreground(_dimBrush);
            view.messageText.TextWrapping(TextWrapping::Wrap);
            view.messageText.Margin(Thickness{ 0, 6, 0, 0 });
            view.content.Children().InsertAt(0, view.messageText);
        }
        view.messageText.Text(winrt::hstring{ text });
    }

    void AgentTranscriptOverlay::_StartLoad(int level)
    {
        auto& view = _levels[level];
        view.state = LevelState::Loading;
        _SetLevelMessage(level, L"Rendering the transcript\x2026");
        // The view's options are taken NOW, on the UI thread (a box tab's ticks live here), and travel with the render, as
        // does the view's epoch: a box click while it runs re-renders the view, and this result must then be dropped.
        _LoadLevelAsync(get_weak(), _dispatcher, _source, level, _generation, _viewEpochs[static_cast<size_t>(level)], _ViewOptions(level));
    }

    winrt::fire_and_forget AgentTranscriptOverlay::_LoadLevelAsync(winrt::weak_ref<AgentTranscriptOverlay> weak,
                                                                   DispatcherQueue dispatcher,
                                                                   TranscriptOverlaySource source,
                                                                   int level,
                                                                   uint64_t generation,
                                                                   uint64_t epoch,
                                                                   ::Agentmaster::TurnsOptions options)
    {
        // Every parameter is BY VALUE: the coroutine outlives the call that started it. It holds NO
        // strong reference to the overlay while on the pool thread — only `weak` — so the overlay (and
        // the page that owns it) can never be destroyed here, off its UI thread.
        try
        {
            co_await winrt::resume_background(); // reads + renders the transcript: never on the UI thread
            auto render = std::make_shared<LevelRender>();
            const auto started = std::chrono::steady_clock::now();
            _RenderLevel(source, level, options, *render);
            render->elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            const bool queued = dispatcher && dispatcher.TryEnqueue([weak, render, level, generation, epoch]() {
                // A throw escaping a DispatcherQueue handler is reported against the UI thread: contain it.
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_ApplyLevelRender(level, generation, epoch, std::move(*render));
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay apply render");
                }
            });
            if (!queued)
            {
                // No dispatcher, or it refused: the window closed while the transcript was rendering.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[transcript] " + ::Agentmaster::ShortId(source.sessionId) +
                                                                L" show: the window closed before the render finished\n");
            }
        }
        catch (...)
        {
            // Rule #18 — and the terminate-net every fire_and_forget needs (an escape IS winrt::terminate()).
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_LoadLevelAsync");
        }
    }

    void AgentTranscriptOverlay::_RenderLevel(const TranscriptOverlaySource& source, int level, const ::Agentmaster::TurnsOptions& options, LevelRender& out) noexcept
    {
        try
        {
            const bool boxTab = ::Agentmaster::IsTranscriptBoxView(level);
            const bool diffTab = ::Agentmaster::IsTranscriptDiffView(level);
            // The level a Codex rollout is read at (a box tab and the Diff tab never read one: below).
            const auto turnsLevel = static_cast<::Agentmaster::TurnsLevel>(std::clamp(level, 0, ::Agentmaster::kTurnsLevelCount - 1));
            if (source.codex && boxTab)
            {
                // A box tab builds on Claude's turns view (sides, boxes), which a Codex rollout does not have - its levels read
                // its plain conversation instead. Say so rather than show a level's text under a tab that promises a side.
                out.message = L"Custom, User Only and Assistant Only need Claude's turns view, which a Codex session does not have - "
                              L"its Minimal and Compact tabs show each of your messages with its final reply, Default, Full and Max the whole conversation.";
                return;
            }
            if (source.codex && diffTab)
            {
                // The Diff tab reads Claude Code's own record of each edit (its hunks), which a Codex rollout does not keep.
                out.message = L"Diff needs Claude Code's record of each edit, which a Codex session does not have - "
                              L"its Default, Full and Max tabs show the whole conversation, its patches included.";
                return;
            }
            if (source.codex)
            {
                if (!source.codexRolloutId.empty())
                {
                    out.path = ::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), source.codexRolloutId);
                }
            }
            else if (!source.sessionId.empty())
            {
                // The session's own transcript, else — for a fork with no messages yet — the one it was
                // forked from (the fork holds that conversation until its first message writes its own).
                auto display = ::Agentmaster::ResolveClaudeDisplayTranscript(source.sessionId, source.forkParentId);
                out.path = std::move(display.path);
                out.forkSourceId = std::move(display.forkSourceId);
            }
            if (out.path.empty())
            {
                out.message = source.codex ?
                                  L"Nothing to show yet - this Codex session has not been prompted, so it has no rollout." :
                                  L"Nothing to show yet - this session has not been prompted, so it has no transcript.";
                return;
            }
            // The SAME text the Copy menu's level copies (CopyTranscriptTurnsAsync): the turns view for
            // Claude; for a Codex rollout the brief (Minimal / Compact) or the whole conversation. A box tab is the
            // turns view under the options its boxes and its sides make (TranscriptViewOptions, taken on the UI thread).
            std::wstring text;
            if (source.codex)
            {
                const bool brief = turnsLevel == ::Agentmaster::TurnsLevel::Minimal || turnsLevel == ::Agentmaster::TurnsLevel::Compact;
                text = brief ? ::Agentmaster::ReadConversationFollowupText(out.path, true, 0 /* whole file */) :
                               ::Agentmaster::ReadConversationText(out.path, true, 0 /* whole file */);
            }
            else if (diffTab)
            {
                // The Diff tab's own document (TranscriptTurns.h): Minimal's turns with each turn's Bash reasons and the net
                // change to every file it touched, as ```diff blocks the build draws as cards. Its options are its own
                // (Minimal + agents), so `options` is not read.
                text = ::Agentmaster::RenderTranscriptDiffFile(out.path, /*utc*/ false).value_or(std::wstring{});
            }
            else
            {
                text = ::Agentmaster::RenderTranscriptTurnsFile(out.path, options, /*utc*/ false).value_or(std::wstring{});
            }
            if (text.empty())
            {
                out.message = boxTab  ? L"Nothing to show - the transcript could not be read, or it holds nothing for this tab." :
                              diffTab ? L"Nothing to show - the transcript could not be read, or it holds no turns yet." :
                                        L"Nothing to show - the transcript could not be read, or it holds no messages at this level.";
                return;
            }
            if (!out.forkSourceId.empty())
            {
                // The fork's turns are its SOURCE's: say so above them. Prepended to the text itself (not
                // drawn beside it) so the header's Copy copies exactly what Copy ▸ Transcript copies for
                // this fork (CopyTranscriptTurnsAsync prepends the same note).
                text.insert(0, ::Agentmaster::ForkSourceTranscriptNote(out.forkSourceId));
            }
            out.lines = 1 + static_cast<size_t>(std::count(text.begin(), text.end(), L'\n'));
            std::wstring_view toParse{ text };
            if (toParse.size() > kMaxMarkdownChars)
            {
                // Cut at a line boundary so the last parsed line isn't half a construct.
                const auto cut = toParse.rfind(L'\n', kMaxMarkdownChars);
                toParse = toParse.substr(0, cut == std::wstring_view::npos ? kMaxMarkdownChars : cut);
                out.truncated = true;
            }
            ::Agentmaster::MdParseOptions parseOptions; // (not `options`: that is the render's, a parameter)
            parseOptions.maxBlocks = kMaxBlocks;
            parseOptions.maxBlockChars = kMaxBlockChars;
            // GitHub-style pipe tables become Table blocks (drawn by _BuildTable) instead of paragraphs of '|' lines -
            // Claude writes its comparisons that way, and cmark alone is CommonMark only.
            parseOptions.tables = true;
            auto parsed = ::Agentmaster::ParseMarkdownBlocks(toParse, parseOptions);
            out.blocks = std::move(parsed.blocks);
            out.truncated = out.truncated || parsed.truncatedBlocks;
            out.failed = parsed.failed;
            out.markdown = std::move(text); // after the parse — `toParse` viewed it
            // The tool-call lists the build folds (here, off the UI thread, with the parse: one linear pass
            // over the blocks, and the UI thread then only follows the indices). Only where the FULL call lines
            // show (options.ops: among the levels exactly Default, Full and Max; a box tab with "Tool calls with
            // outcome" ticked): Compact's lists hold its calls' REASON lines (transcript_turns v10) - the very thing
            // that level exists to show at a glance, so folding them would hide its point - and Minimal has no lists
            // at all. A Codex rollout has no speaker headers, so nothing would fold there anyway.
            if (options.ops)
            {
                out.areas = ::Agentmaster::MdFindToolAreas(out.blocks);
            }
            // The Session chain list and the session headings its links point to (one more linear pass;
            // an ordinary one-session transcript yields an empty chain).
            out.chain = ::Agentmaster::MdFindSessionChain(out.blocks);
            if (diffTab)
            {
                // The outline column and the status words, read here off the UI thread like the areas and the chain.
                _ScanDiffOutline(out.blocks, out);
            }
            if (out.blocks.empty())
            {
                // A failed parse already logged its exception (ParseMarkdownBlocks, Rule #18) — say so
                // instead of claiming the level is empty. A box tab can be empty for real: User Only of a
                // conversation whose every turn a teammate started, with no box adding anything.
                out.message = out.failed ? L"The transcript could not be rendered (the details are in hooks.log)." :
                              boxTab     ? L"Nothing to show on this tab - the boxes above can add more." :
                              diffTab    ? L"Nothing to show on this tab." :
                                           L"Nothing to show at this level.";
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: whatever was produced, plus a message saying the render failed.
            out.failed = true;
            if (out.message.empty() && out.blocks.empty())
            {
                out.message = L"The transcript could not be rendered (the details are in hooks.log).";
            }
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_RenderLevel");
        }
    }

    void AgentTranscriptOverlay::_ApplyLevelRender(int level, uint64_t generation, uint64_t epoch, LevelRender&& render)
    {
        if (generation != _generation || !_open || level < 0 || level >= static_cast<int>(_levels.size()))
        {
            return; // retargeted, refreshed or closed since this render started: it describes nothing on screen
        }
        if (epoch != _viewEpochs[static_cast<size_t>(level)])
        {
            return; // a box click re-rendered this one view since: the result shows the OLD boxes
        }
        auto& view = _levels[level];
        if (view.state != LevelState::Loading)
        {
            return;
        }
        view.markdown = std::move(render.markdown);
        view.renderMs = render.elapsedMs;
        view.lines = render.lines;
        view.truncated = render.truncated;
        view.failed = render.failed;
        view.diffSummary = std::move(render.diffSummary); // the Diff tab's status words ("" for every other view)
        // The view's settings name ("compact", "user-only"); a box tab also names the boxes it was rendered with, so a
        // report about a box tab's text can be read back in hooks.log ("boxes=ops,narration", "boxes=none").
        std::wstring viewName{ ::Agentmaster::TranscriptViewName(level) };
        if (::Agentmaster::IsTranscriptBoxView(level))
        {
            std::wstring names;
            for (const auto& name : _BoxesOf(level))
            {
                names += (names.empty() ? L"" : L",") + name;
            }
            viewName += L" boxes=" + (names.empty() ? std::wstring{ L"none" } : names);
        }
        const std::wstring head = L"[transcript] " + ::Agentmaster::ShortId(_source.sessionId) + L" show " + viewName + L": ";
        if (render.blocks.empty())
        {
            view.state = LevelState::Empty;
            // Nothing will be built: a box click's keep-the-turn jump has no turn to land on.
            view.pendingTurnJump = 0;
            if (view.isDiff)
            {
                // The Diff tab's outline column still says why it lists nothing (never a blank column beside the message).
                try
                {
                    _BuildDiffOutline(view, {});
                }
                catch (...)
                {
                    // Rule #18. Recovery: the column stays blank; the page's message says what happened.
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff outline (empty)");
                }
            }
            _SetLevelMessage(level, render.message.empty() ? std::wstring{ L"Nothing to show at this level." } : render.message);
            ::Agentmaster::AppendStateLog(L"hooks.log", head + L"nothing to show (" + view.message + L")" +
                                                            (render.path.empty() ? std::wstring{} : L" path=" + render.path) + L"\n");
        }
        else
        {
            view.blocks = std::move(render.blocks);
            view.built = 0;
            view.areas = std::move(render.areas);
            view.nextArea = 0;
            view.areaPanel = nullptr;
            view.areaEnd = 0;
            view.areaChunk = nullptr;
            view.areaChunkFill = 0;
            view.folds.clear();
            view.hasFolds = std::any_of(view.areas.begin(), view.areas.end(), [](const ::Agentmaster::MdToolArea& area) {
                return ::Agentmaster::MdToolAreaFoldable(area);
            });
            view.chain = std::move(render.chain);
            view.sessionHeadings.clear();
            view.pendingSessionJump = 0;
            view.diffCards.clear();
            view.pendingCardJump = 0;
            if (view.isDiff)
            {
                // The outline column, all of it now (its entries are few - one per changed file - and every click on one must
                // find its target, built or pending).
                try
                {
                    _BuildDiffOutline(view, render.outline);
                }
                catch (...)
                {
                    // Rule #18. Recovery: the page still builds - with the part of the outline that was made, or none.
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff outline");
                }
            }
            view.state = LevelState::Building;
            // The placeholder gives way to content.
            if (view.messageText && view.content)
            {
                uint32_t index = 0;
                if (view.content.Children().IndexOf(view.messageText, index))
                {
                    view.content.Children().RemoveAt(index);
                }
                view.messageText = nullptr;
            }
            view.message.clear();
            ::Agentmaster::AppendStateLog(L"hooks.log", head + L"chars=" + std::to_wstring(view.markdown.size()) + L" blocks=" +
                                                            std::to_wstring(view.blocks.size()) + (view.truncated ? L" (capped)" : L"") +
                                                            (view.failed ? L" (parse failed part-way)" : L"") +
                                                            (view.diffSummary.empty() ? std::wstring{} : L" (" + view.diffSummary + L")") + L" rendered in " +
                                                            std::to_wstring(view.renderMs) + L"ms" +
                                                            (render.forkSourceId.empty() ? std::wstring{} :
                                                                                           L" from the fork source " + ::Agentmaster::ShortId(render.forkSourceId) +
                                                                                               L"'s transcript (this fork has no messages yet)") +
                                                            L"\n");
            if (level == _current)
            {
                _SchedulePump();
            }
        }
        if (level == _current)
        {
            _UpdateStatus();
        }
    }

    void AgentTranscriptOverlay::_SchedulePump()
    {
        if (_pumpQueued || !_dispatcher)
        {
            return;
        }
        _pumpQueued = true;
        // LOW priority: input, layout and rendering all go first — the build only uses the idle time
        // between them, which is what keeps the window responsive while a long level lays out.
        const bool queued = _dispatcher.TryEnqueue(DispatcherQueuePriority::Low, [weak = get_weak(), generation = _generation]() {
            try
            {
                if (const auto self = weak.get())
                {
                    self->_Pump(generation);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay pump item"); // the build stops; what is built stays
            }
        });
        if (!queued)
        {
            _pumpQueued = false;
        }
    }

    void AgentTranscriptOverlay::_Pump(uint64_t generation)
    {
        _pumpQueued = false;
        if (!_open || generation != _generation)
        {
            return;
        }
        auto& view = _levels[_current];
        if (view.state != LevelState::Building)
        {
            return;
        }
        try
        {
            const auto sliceStart = std::chrono::steady_clock::now();
            size_t madeThisSlice = 0;
            while (view.built < view.blocks.size())
            {
                _BuildNext(view); // one block (plus a fold row before an area's first one), or an empty area's note
                ++madeThisSlice;
                if (madeThisSlice >= kMinBlocksPerSlice && std::chrono::steady_clock::now() - sliceStart >= kSliceBudget)
                {
                    break; // yield: the next slice is queued below
                }
            }
            // A chain link clicked before its heading was built: this slice may have built it — jump now
            // (BEFORE _FinishLevel below, which drops a jump whose heading never came).
            if (const size_t pending = view.pendingSessionJump;
                pending > 0 && pending < view.sessionHeadings.size() && view.sessionHeadings[pending])
            {
                view.pendingSessionJump = 0;
                _ScrollToSessionHeading(view, pending);
            }
            // A box click re-rendered this view while the reader was on turn N: the moment a heading of turn N or a LATER
            // one is built, it goes to the top (the new view may skip turn N itself - User Only skips a turn with nothing
            // of the user's). The headings are recorded in build order, so their numbers rise and the last one says
            // whether such a heading exists yet; the first of them is found by a binary search.
            if (const int pendingTurn = view.pendingTurnJump;
                pendingTurn > 0 && !view.turnHeadings.empty() && view.turnHeadings.back().first >= pendingTurn)
            {
                const auto at = std::lower_bound(view.turnHeadings.begin(), view.turnHeadings.end(), pendingTurn, [](const auto& entry, int number) {
                    return entry.first < number;
                });
                view.pendingTurnJump = 0;
                _ScrollToTurnHeading(view, static_cast<size_t>(at - view.turnHeadings.begin()));
            }
            // A Diff outline file entry clicked before its card was built: this slice may have built it. The cards are recorded
            // in block order, so the last one says whether the build got that far.
            if (const size_t pendingCard = view.pendingCardJump;
                pendingCard > 0 && !view.diffCards.empty() && view.diffCards.back()->block >= pendingCard - 1)
            {
                view.pendingCardJump = 0;
                const auto at = std::lower_bound(view.diffCards.begin(), view.diffCards.end(), pendingCard - 1, [](const std::shared_ptr<DiffCard>& card, size_t block) {
                    return card->block < block;
                });
                if (at != view.diffCards.end() && (*at)->root)
                {
                    _ScrollElementToTop(view, (*at)->root);
                }
            }
            if (view.built >= view.blocks.size())
            {
                // The build ends with the turn jump still waiting: the new view has no heading of turn N or later (it
                // skipped the last turns) - the last turn it does have is the nearest place to the old one.
                if (view.pendingTurnJump > 0)
                {
                    view.pendingTurnJump = 0;
                    if (!view.turnHeadings.empty())
                    {
                        _ScrollToTurnHeading(view, view.turnHeadings.size() - 1);
                    }
                }
                _FinishLevel(view);
                // A ↓ during the build lands on the REAL last line now: laid out (UpdateLayout) after the closing note
                // _FinishLevel may have added.
                if (view.followEnd)
                {
                    view.followEnd = false;
                    _ScrollViewToEnd(view, true);
                }
            }
            else if (view.followEnd)
            {
                // Still building after a ↓: keep on the end as it grows. The extent of the LAST layout (this slice's blocks
                // are laid out next frame, and the next slice follows them) - no forced layout per slice.
                _ScrollViewToEnd(view, false);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: stop building this level (what is built stays) and say so.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_Pump");
            view.failed = true;
            // Nothing more will be built: the waiting jumps have nothing to wait for.
            view.pendingTurnJump = 0;
            view.followEnd = false;
            try
            {
                _FinishLevel(view);
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_Pump finish");
                view.state = LevelState::Done;
            }
        }
        _UpdateStatus();
        // The view grew (and may have jumped): the buttons follow. The content's SizeChanged does too, after the layout; this
        // is the belt for a slice whose growth that layout has not reported yet.
        _UpdateJumpButtons();
        if (view.state == LevelState::Building)
        {
            _SchedulePump();
        }
    }

    void AgentTranscriptOverlay::_FinishLevel(LevelView& view)
    {
        // Done FIRST: if the note below throws, the level must still stop building.
        view.state = LevelState::Done;
        std::vector<::Agentmaster::MdBlock>{}.swap(view.blocks); // the XAML holds the content now
        view.built = 0;
        // The area walk is over too: drop its state (a level that ENDS on a tool list still holds that
        // fold's panel here). The folds themselves stay in view.folds for "Expand all".
        std::vector<::Agentmaster::MdToolArea>{}.swap(view.areas);
        view.nextArea = 0;
        view.areaPanel = nullptr;
        view.areaEnd = 0;
        view.areaChunk = nullptr;
        view.areaChunkFill = 0;
        // The chain was only needed to build the links and to know which heading to record; the links and
        // view.sessionHeadings stay. A jump still pending targets a heading the build never reached (cut by
        // the block cap): drop it rather than keep a target that can never exist.
        view.chain = ::Agentmaster::MdSessionChain{};
        view.pendingSessionJump = 0;
        // A Diff outline click whose card never came (cut by the block cap): nothing left to wait for.
        view.pendingCardJump = 0;
        if (view.truncated || view.failed)
        {
            auto note = _MakeTextBlock(kStatusFontSize);
            note.Foreground(_dimBrush);
            note.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            note.Margin(Thickness{ 0, 16, 0, 0 });
            note.Text(view.failed ? winrt::hstring{ L"The rest of this level could not be rendered (the details are in hooks.log). The header's Copy button still copies the whole text." } :
                                    winrt::hstring{ L"The view stops here - this level is too long to show in full. The header's Copy button copies the whole text." });
            _AppendElement(view, note);
        }
    }

    void AgentTranscriptOverlay::_AppendElement(LevelView& view, const UIElement& element)
    {
        if (!view.chunk || view.chunkFill >= kChunkSize)
        {
            // A chunk with its own automation peer: without it a UIA client's walk is quadratic in the blocks
            // (AgentTranscriptChunkPanel).
            view.chunk = MakeChunkPanel();
            view.content.Children().Append(view.chunk);
            view.chunkFill = 0;
        }
        view.chunk.Children().Append(element);
        ++view.chunkFill;
    }

    void AgentTranscriptOverlay::_AppendToFold(LevelView& view, const UIElement& element)
    {
        if (!view.areaChunk || view.areaChunkFill >= kChunkSize)
        {
            // An open fold's elements are UIA children too: its chunks get their own peers like the level's.
            view.areaChunk = MakeChunkPanel();
            view.areaPanel.Children().Append(view.areaChunk);
            view.areaChunkFill = 0;
        }
        view.areaChunk.Children().Append(element);
        ++view.areaChunkFill;
    }

    void AgentTranscriptOverlay::_BuildNext(LevelView& view)
    {
        const size_t index = view.built;
        // Past the folded area being filled: the blocks after it (the reply quote, the next turn) go back
        // to the level's chunks.
        if (view.areaPanel && index >= view.areaEnd)
        {
            view.areaPanel = nullptr;
            view.areaEnd = 0;
            view.areaChunk = nullptr;
            view.areaChunkFill = 0;
        }
        // A belt: skip any area the walk has passed (areas are found on these very blocks, in order and
        // without overlaps, so this never fires — but a stale index must never stall the build).
        while (view.nextArea < view.areas.size() && view.areas[view.nextArea].first < index)
        {
            ++view.nextArea;
        }
        const ::Agentmaster::MdBlock* above = index > 0 ? &view.blocks[index - 1] : nullptr;
        bool firstInFold = false;
        if (!view.areaPanel && view.nextArea < view.areas.size() && view.areas[view.nextArea].first == index)
        {
            const auto& area = view.areas[view.nextArea];
            ++view.nextArea;
            if (area.empty)
            {
                // "(no tool calls)": the note says the same in one dim line, lined up with the fold rows —
                // the bullet item itself is not built.
                _AppendElement(view, _BuildEmptyAreaNote(view.blocks[index], above));
                view.built = std::max(area.end, index + 1);
                return;
            }
            if (::Agentmaster::MdToolAreaFoldable(area))
            {
                view.areaPanel = _BuildFold(view, area, view.blocks[index], above);
                view.areaEnd = area.end;
                view.areaChunk = nullptr; // the new fold's first append opens its first chunk
                view.areaChunkFill = 0;
                firstInFold = true;
            }
            // Otherwise (Compact's own content: answers, compactions, an agent's messages) the list is
            // built open, exactly as before folding existed.
        }
        // The Diff tab draws three of its kinds of blocks its own way (TRANSCRIPT_OVERLAY.md §3l): a top-level ```diff block
        // is a GitHub-style card, a Bash row ("- `$` reason ×N") a chip row, a turn heading carries its colored counts. Only
        // top-level ones: the same shapes QUOTED in a message (a user pasting a diff) stay plain text and code. Never inside
        // a fold (the Diff tab's Minimal options fold nothing - the belt is the areaPanel test).
        if (view.isDiff && !view.areaPanel)
        {
            const auto& block = view.blocks[index];
            FrameworkElement special{ nullptr };
            if (block.kind == ::Agentmaster::MdBlockKind::Code && block.containers.empty() && block.info == L"diff")
            {
                special = _BuildDiffCard(view, block, index);
            }
            else if (IsDiffBashRow(block))
            {
                special = _BuildDiffBashRow(block);
            }
            else if (TurnHeadingNumber(block) > 0)
            {
                special = _BuildDiffHeading(view, block);
            }
            if (special)
            {
                AddMargin(special, 0, ::Agentmaster::MdGapAbove(above, block));
                _AppendElement(view, special);
                // A turn heading is recorded as every view records it: the outline's jumps and its highlight read them.
                if (const int turn = TurnHeadingNumber(block); turn > 0)
                {
                    view.turnHeadings.emplace_back(turn, special);
                }
                ++view.built;
                return;
            }
        }
        // A row of the Session chain list: its "session N" mentions become links — only those whose session
        // has a heading in this level (a one-session chain has none, so nothing there links). Only the
        // list's few blocks pay for the scan.
        const bool hasChain = view.chain.end > view.chain.first;
        std::vector<::Agentmaster::MdSessionRef> sessionRefs;
        if (hasChain && index >= view.chain.first && index < view.chain.end)
        {
            for (const auto& ref : ::Agentmaster::MdFindSessionRefs(view.blocks[index]))
            {
                if (ref.session < view.chain.headings.size() && view.chain.headings[ref.session] != ::Agentmaster::kMdNoBlock)
                {
                    sessionRefs.push_back(ref);
                }
            }
        }
        // The first block inside a fold starts its panel flush: the summary row above took the gap.
        const auto element = _BuildBlockElement(view, view.blocks[index], firstInFold ? nullptr : above, sessionRefs.empty() ? nullptr : &sessionRefs);
        if (view.areaPanel)
        {
            _AppendToFold(view, element);
        }
        else
        {
            _AppendElement(view, element);
        }
        // A session's heading — the very block the chain says its links point to (a session's FIRST
        // heading): record the element, so a click scrolls to it.
        if (hasChain)
        {
            const size_t session = ::Agentmaster::MdSessionHeadingNumber(view.blocks[index]);
            if (session > 0 && session < view.chain.headings.size() && view.chain.headings[session] == index)
            {
                if (view.sessionHeadings.size() <= session)
                {
                    view.sessionHeadings.resize(session + 1, winrt::Windows::UI::Xaml::FrameworkElement{ nullptr });
                }
                view.sessionHeadings[session] = element;
            }
        }
        // A turn's heading ("### Turn N · …", top level only): recorded with its number, so a box click can tell which turn
        // the reader is on and the re-render can bring that turn back to the top (_TopTurnNumber, the pending turn jump).
        // Headings are never inside a fold (a fold holds one list), so every recorded one has a position once laid out.
        if (const int turn = TurnHeadingNumber(view.blocks[index]); turn > 0)
        {
            view.turnHeadings.emplace_back(turn, element);
        }
        ++view.built;
    }

    StackPanel AgentTranscriptOverlay::_BuildFold(LevelView& view, const ::Agentmaster::MdToolArea& area, const ::Agentmaster::MdBlock& firstBlock, const ::Agentmaster::MdBlock* above)
    {
        const bool open = view.expandAll;
        std::wstring summary = ::Agentmaster::MdToolAreaSummary(area);
        if (summary.empty())
        {
            summary = L"Tool calls"; // the summary failed (already logged): still a row that opens the list
        }

        // The panel the area's blocks are appended to — hidden while folded. A hair of air above its first
        // item so an opened list does not touch the row.
        StackPanel panel;
        panel.Visibility(open ? Visibility::Visible : Visibility::Collapsed);
        panel.Margin(Thickness{ 0, 3, 0, 0 });

        // The row: [chevron | summary], the summary wrapping in its own column on a narrow window. Neither
        // child takes hits — the button must be the only target (the badge's mkIconBtn lesson).
        FontIcon chevron;
        chevron.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        chevron.Glyph(open ? kFoldOpenGlyph : kFoldClosedGlyph);
        chevron.FontSize(9);
        chevron.Width(kFoldChevronWidth);
        chevron.Foreground(_dimBrush);
        chevron.VerticalAlignment(VerticalAlignment::Top);
        chevron.Margin(Thickness{ 0, 4, kFoldChevronGap, 0 }); // centered on the summary's FIRST line
        chevron.IsHitTestVisible(false);
        Grid::SetColumn(chevron, 0);

        TextBlock label;
        label.Text(winrt::hstring{ summary });
        label.FontSize(kFoldFontSize);
        label.Foreground(_dimBrush);
        label.TextWrapping(TextWrapping::Wrap);
        label.IsHitTestVisible(false);
        Grid::SetColumn(label, 1);

        Grid content;
        ColumnDefinition chevronColumn;
        chevronColumn.Width(GridLength{ 0, GridUnitType::Auto });
        ColumnDefinition labelColumn;
        labelColumn.Width(GridLength{ 1, GridUnitType::Star });
        content.ColumnDefinitions().Append(chevronColumn);
        content.ColumnDefinitions().Append(labelColumn);
        content.Children().Append(chevron);
        content.Children().Append(label);

        Button row;
        row.Background(_transparentBrush); // transparent (not null): hit-testable, and the template still shows hover
        row.BorderThickness(Thickness{ 0, 0, 0, 0 });
        row.Padding(Thickness{ kFoldRowPaddingLeft, kFoldRowPaddingTop, 8, kFoldRowPaddingTop });
        row.MinWidth(0);
        row.MinHeight(0);
        row.HorizontalAlignment(HorizontalAlignment::Left);
        row.HorizontalContentAlignment(HorizontalAlignment::Left);
        row.Content(content);
        // The gap the area's first block would have had under its header sits above the row instead.
        row.Margin(Thickness{ 0, ::Agentmaster::MdGapAbove(above, firstBlock), 0, 0 });
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(row, winrt::hstring{ L"Tool calls: " + summary });
        // WEAK references: the handler lives on the row, and the panel / chevron must never be kept alive
        // by a handler past the level's release (_ReleaseLevels drops the whole view).
        row.Click([weakPanel = winrt::make_weak(panel), weakChevron = winrt::make_weak(chevron)](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            try
            {
                const auto foldPanel = weakPanel.get();
                const auto foldChevron = weakChevron.get();
                if (foldPanel && foldChevron)
                {
                    _SetFoldOpen(foldPanel, foldChevron, foldPanel.Visibility() != Visibility::Visible);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay fold toggle"); // the fold stays as it was
            }
        });

        _AppendElement(view, row);
        _AppendElement(view, panel);
        view.folds.push_back(FoldParts{ panel, chevron });
        return panel;
    }

    FrameworkElement AgentTranscriptOverlay::_BuildEmptyAreaNote(const ::Agentmaster::MdBlock& firstBlock, const ::Agentmaster::MdBlock* above)
    {
        TextBlock note;
        note.Text(winrt::hstring{ EmptyAreaNoteText(firstBlock) });
        note.FontSize(kFoldFontSize);
        note.Foreground(_dimBrush);
        note.TextWrapping(TextWrapping::Wrap);
        // Where a fold row's TEXT starts (past its padding + chevron), and as far down as that text sits.
        note.Margin(Thickness{ kFoldTextIndent, ::Agentmaster::MdGapAbove(above, firstBlock) + kFoldRowPaddingTop, 0, kFoldRowPaddingTop });
        return note;
    }

    void AgentTranscriptOverlay::_SetFoldOpen(const StackPanel& panel, const FontIcon& chevron, bool open)
    {
        panel.Visibility(open ? Visibility::Visible : Visibility::Collapsed);
        chevron.Glyph(open ? kFoldOpenGlyph : kFoldClosedGlyph);
    }

    void AgentTranscriptOverlay::_ToggleExpandAll()
    {
        auto& view = _levels[_current];
        view.expandAll = !view.expandAll;
        for (const auto& fold : view.folds)
        {
            _SetFoldOpen(fold.panel, fold.chevron, view.expandAll);
        }
        _UpdateExpandAllButton();
    }

    void AgentTranscriptOverlay::_UpdateExpandAllButton()
    {
        if (!_expandAllButton || !_expandAllLabel)
        {
            return;
        }
        const auto& view = _levels[_current];
        _expandAllButton.Visibility(view.hasFolds ? Visibility::Visible : Visibility::Collapsed);
        const winrt::hstring label{ view.expandAll ? L"Collapse all" : L"Expand all" };
        _expandAllLabel.Text(label);
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(_expandAllButton, label);
    }

    void AgentTranscriptOverlay::_ToggleWrapLines()
    {
        _wrapLines = !_wrapLines;
        // The level on screen re-lays out now, keeping the reader on the same passage; a hidden level
        // catches up when it is next shown (_SelectLevel). Neither step throws.
        _SyncWrapMode(_levels[_current]);
        // Every window's next open uses it (Show reads it back). Also noexcept.
        _RememberWrapLines(_wrapLines);
        // Last: the only step that can throw, so a failed repaint never costs the switch itself.
        _UpdateWrapButton();
    }

    void AgentTranscriptOverlay::_UpdateWrapButton()
    {
        if (!_wrapButton || !_wrapButtonFace || !_wrapGlyph)
        {
            return;
        }
        _wrapButtonFace.Background(_wrapLines ? _toggleOnBrush : _transparentBrush);
        _wrapButtonFace.BorderBrush(_wrapLines ? _toggleOnBorderBrush : _transparentBrush);
        _wrapGlyph.Foreground(_wrapLines ? _bodyBrush : _dimBrush);
        // Re-setting a tip only updates its text (and the open tip's content, when this toggle's tip is the
        // one showing) — AgentSetTip wires its pointer handlers once per element.
        AgentSetTip(_wrapButton, winrt::hstring{ _wrapLines ? kWrapOnTip : kWrapOffTip });
        // A Button has no toggle state for a screen reader, so the name carries it.
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(_wrapButton, winrt::hstring{ _wrapLines ? L"Wrap long lines: on" : L"Wrap long lines: off" });
    }

    void AgentTranscriptOverlay::_SyncWrapMode(LevelView& view) noexcept
    {
        try
        {
            if (!view.scroll || view.wrapLines == _wrapLines)
            {
                return;
            }
            // The anchor FIRST: the scroll offset belongs to the layout the view has NOW — for a level that
            // was hidden, the one it had when it was last shown (a collapsed subtree is not re-measured, so
            // its DesiredSizes and its offset still agree).
            const auto anchor = FindScrollAnchor(view.scroll, view.content);
            ApplyWrapMode(view.scroll, _wrapLines);
            view.wrapLines = _wrapLines; // after the apply: a throw above leaves the old value, so the next select retries
            if (!anchor.path.empty())
            {
                // Lay the view out NOW under the new width rule, so the anchor's new place — and the extent
                // ChangeView clamps to — is real (the board's UpdateLayout-then-ChangeView recipe). Legal
                // here: every caller is an input handler or a menu action, never a layout pass. On a long
                // Max level this re-measures every block at once — the same work the next frame would do.
                view.scroll.UpdateLayout();
                RestoreScrollAnchor(view.scroll, view.content, anchor);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the view keeps the mode it reached (view.wrapLines says which) and its raw,
            // clamped scroll offset.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay sync wrap mode");
        }
    }

    bool AgentTranscriptOverlay::_RememberedWrapLines() noexcept
    {
        try
        {
            return ::Agentmaster::LoadAppSettings().transcriptOverlayWrapLines;
        }
        catch (...)
        {
            // Rule #18. Recovery: wrap (the default) — a failed settings read never blocks the overlay.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay remembered wrap");
        }
        return true;
    }

    void AgentTranscriptOverlay::_RememberWrapLines(bool wrap) noexcept
    {
        try
        {
            // FRESHEST disk, one field (the ColorPickupFlyout idiom, as _RememberLevel): never write a stale
            // in-memory copy over a cog Save or another window's settings change.
            auto settings = ::Agentmaster::LoadAppSettings();
            if (settings.transcriptOverlayWrapLines != wrap)
            {
                settings.transcriptOverlayWrapLines = wrap;
                ::Agentmaster::SaveAppSettings(settings);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: nothing — the view already switched; only the remembered state is lost.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay remember wrap");
        }
    }

    TextBlock AgentTranscriptOverlay::_MakeTextBlock(double fontSize) const
    {
        TextBlock text;
        text.FontSize(fontSize);
        text.Foreground(_bodyBrush);
        text.TextWrapping(TextWrapping::Wrap);
        text.IsTextSelectionEnabled(true);
        // The WT text menu (Copy / Select All), never the default: the default text flyout exists ONCE
        // PER THREAD in WinUI 2, and with several windows on this one thread it misbehaves or crashes
        // once the window that first opened it is gone (GH#18599 — why WT made TextMenuFlyout).
        text.ContextFlyout(_textMenu);
        // The touch/pen selection flyout is that same per-thread default: off.
        text.SelectionFlyout(nullptr);
        return text;
    }

    void AgentTranscriptOverlay::_AppendCellRuns(const TextBlock& text, const std::vector<::Agentmaster::MdRun>& runs) const
    {
        for (const auto& source : runs)
        {
            if (source.text.empty())
            {
                continue;
            }
            Run run;
            run.Text(winrt::hstring{ source.text });
            if (source.style & ::Agentmaster::kMdBold)
            {
                run.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            if (source.style & ::Agentmaster::kMdItalic)
            {
                run.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            }
            if (source.style & ::Agentmaster::kMdCode)
            {
                run.FontFamily(_monoFont);
                run.Foreground(_inlineCodeBrush);
            }
            if (source.style & ::Agentmaster::kMdLink)
            {
                // Only plain web links are clickable, as in a paragraph (IsWebUrl); any other scheme is link-colored text.
                winrt::Windows::Foundation::Uri uri{ nullptr };
                if (IsWebUrl(source.url))
                {
                    try
                    {
                        uri = winrt::Windows::Foundation::Uri{ winrt::hstring{ source.url } };
                    }
                    catch (...)
                    {
                        // A malformed URL in a quoted table: logged (throttled), rendered as plain link text.
                        ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay table link uri");
                    }
                }
                if (uri)
                {
                    Hyperlink link;
                    link.NavigateUri(uri);
                    link.Foreground(_linkBrush);
                    link.Inlines().Append(run);
                    text.Inlines().Append(link);
                    continue;
                }
                if ((source.style & ::Agentmaster::kMdCode) == 0)
                {
                    run.Foreground(_linkBrush);
                }
            }
            text.Inlines().Append(run);
        }
    }

    FrameworkElement AgentTranscriptOverlay::_BuildTable(const ::Agentmaster::MdBlock& block)
    {
        const size_t columns = block.tableAlign.size();
        Grid grid;
        // Left-aligned and capped at its natural width (set below): a short table stays as narrow as its content
        // instead of stretching across the view.
        grid.HorizontalAlignment(HorizontalAlignment::Left);
        for (size_t row = 0; row < block.tableRows.size() + (block.truncated ? 1 : 0); ++row)
        {
            RowDefinition definition;
            definition.Height(GridLength{ 0, GridUnitType::Auto });
            grid.RowDefinitions().Append(definition);
        }
        // Each row first gets one childless background band spanning every column (no composition node, the
        // rounded-element gotcha does not apply to a plain rectangle either way): the header a shade lighter, the
        // body rows alternating with a faint wash, every row with a hairline under it.
        const auto headerBrush = MakeBrush(0xFF, 0x2A, 0x2A, 0x2A);
        const auto stripeBrush = MakeBrush(0x0D, 0xFF, 0xFF, 0xFF);
        for (size_t row = 0; row < block.tableRows.size(); ++row)
        {
            Border band;
            band.Background(row == 0 ? Brush{ headerBrush } : row % 2 == 0 ? Brush{ stripeBrush } : Brush{ _transparentBrush });
            band.BorderBrush(_ruleBrush);
            band.BorderThickness(Thickness{ 0, 0, 0, 1 });
            Grid::SetRow(band, static_cast<int32_t>(row));
            Grid::SetColumnSpan(band, static_cast<int32_t>(std::max<size_t>(columns, 1)));
            grid.Children().Append(band);
        }
        // The cells. Each column's NATURAL width - its widest cell laid out on one line - is measured here, on detached
        // text blocks (a measure needs no tree), from the first kTableMeasuredRows rows.
        std::vector<double> natural(columns, 0.0);
        const winrt::Windows::Foundation::Size unbounded{ std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity() };
        for (size_t row = 0; row < block.tableRows.size(); ++row)
        {
            const auto& cells = block.tableRows[row].cells;
            for (size_t column = 0; column < columns && column < cells.size(); ++column)
            {
                auto text = _MakeTextBlock(kBodyFontSize);
                if (row == 0)
                {
                    text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
                }
                switch (block.tableAlign[column])
                {
                case ::Agentmaster::MdAlign::Center:
                    text.TextAlignment(TextAlignment::Center);
                    break;
                case ::Agentmaster::MdAlign::Right:
                    text.TextAlignment(TextAlignment::Right);
                    break;
                case ::Agentmaster::MdAlign::Left:
                case ::Agentmaster::MdAlign::Default:
                default:
                    break;
                }
                text.Margin(Thickness{ kTableCellPadX, kTableCellPadY, kTableCellPadX, kTableCellPadY });
                _AppendCellRuns(text, cells[column]);
                if (row < kTableMeasuredRows)
                {
                    // Measured on ONE line (NoWrap at an unbounded size: the desired width is the whole line), then
                    // set back to wrapping for the layout, where a narrow view wraps the cell instead of cutting it.
                    text.TextWrapping(TextWrapping::NoWrap);
                    text.Measure(unbounded);
                    natural[column] = std::max(natural[column], static_cast<double>(text.DesiredSize().Width));
                    text.TextWrapping(TextWrapping::Wrap);
                }
                Grid::SetRow(text, static_cast<int32_t>(row));
                Grid::SetColumn(text, static_cast<int32_t>(column));
                grid.Children().Append(text);
            }
        }
        // The columns: STAR widths weighted by each column's natural width, never below the column's own width or
        // kTableMinColumnWidth (whichever is smaller), and the whole grid capped at the sum of the natural widths.
        // So a view wide enough shows every column at its natural width (each star share == its weight), and a
        // narrower one shrinks the columns in proportion and wraps their text - the long text column gives way, the
        // narrow ones (a check mark, a level name) keep their width through MinWidth. With line wrap OFF the view
        // measures at an unbounded width, where star columns size to their content: the table keeps its natural
        // width and the part past the right edge is hidden, like every other block.
        double total = 0.0;
        for (size_t column = 0; column < columns; ++column)
        {
            const double width = std::max(natural[column], 1.0);
            ColumnDefinition definition;
            definition.Width(GridLength{ width, GridUnitType::Star });
            definition.MinWidth(std::min(width, kTableMinColumnWidth));
            grid.ColumnDefinitions().Append(definition);
            total += width;
        }
        if (total > 0.0)
        {
            grid.MaxWidth(total);
        }
        if (block.truncated)
        {
            // Rows past the block's character cap were left out (the Copy button still copies them).
            auto note = _MakeTextBlock(kBodyFontSize);
            note.Text(winrt::hstring{ kCutNote });
            note.Foreground(_dimBrush);
            note.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            note.Margin(Thickness{ kTableCellPadX, kTableCellPadY, kTableCellPadX, kTableCellPadY });
            Grid::SetRow(note, static_cast<int32_t>(block.tableRows.size()));
            Grid::SetColumnSpan(note, static_cast<int32_t>(std::max<size_t>(columns, 1)));
            grid.Children().Append(note);
        }
        // The frame: a hairline around the whole table. Square corners - the frame holds children, and a rounded
        // element with children is a composition node every frame walks.
        Border frame;
        frame.BorderBrush(_ruleBrush);
        frame.BorderThickness(Thickness{ 1, 1, 1, 1 });
        frame.HorizontalAlignment(HorizontalAlignment::Left);
        frame.Child(grid);
        return frame;
    }

    FrameworkElement AgentTranscriptOverlay::_BuildBlockContent(LevelView& view, const ::Agentmaster::MdBlock& block, const std::vector<::Agentmaster::MdSessionRef>* sessionRefs)
    {
        switch (block.kind)
        {
        case ::Agentmaster::MdBlockKind::Rule:
        {
            Border rule;
            rule.Height(1);
            rule.Background(_ruleBrush);
            rule.Margin(Thickness{ 0, 4, 0, 4 });
            return rule;
        }
        case ::Agentmaster::MdBlockKind::Code:
        {
            Border box;
            box.Background(_codeBackBrush);
            box.BorderBrush(_codeBorderBrush);
            box.BorderThickness(Thickness{ 1, 1, 1, 1 });
            box.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            box.Padding(Thickness{ 10, 7, 10, 7 });
            auto text = _MakeTextBlock(kCodeFontSize);
            text.FontFamily(_monoFont);
            text.Foreground(_codeTextBrush);
            text.Text(block.truncated ? winrt::hstring{ block.code + kCutNote } : winrt::hstring{ block.code });
            box.Child(text);
            return box;
        }
        case ::Agentmaster::MdBlockKind::Table:
            return _BuildTable(block);
        case ::Agentmaster::MdBlockKind::Heading:
        case ::Agentmaster::MdBlockKind::Paragraph:
        default:
            break;
        }

        const bool heading = block.kind == ::Agentmaster::MdBlockKind::Heading;
        auto text = _MakeTextBlock(heading ? HeadingFontSize(block.headingLevel) : kBodyFontSize);
        // A LABEL paragraph's first line is tinted with its speaker's color, and the lines under it (the
        // Full / Max stats line folded into the same paragraph) are dimmed — set per run below.
        Brush labelBrush{ nullptr };
        if (heading)
        {
            text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            view.speakerBar = nullptr; // a heading starts a new turn / section: no speaker yet
        }
        else if (block.rawHtml)
        {
            // Raw HTML kept as text (MarkdownBlocks.h): monospace + dim, so markup reads as markup.
            text.FontFamily(_monoFont);
            text.FontSize(kCodeFontSize);
            text.Foreground(_dimBrush);
        }
        else if (::Agentmaster::MdIsLabelParagraph(block))
        {
            // A speaker label ("**❯ User**", "**● Claude**", "**✉ finder-a**"): tint it, and remember the
            // speaker so the quote that follows (their message / reply) wears a matching bar.
            switch (LabelGlyph(block))
            {
            case L'\x276F':
                labelBrush = _userBrush;
                view.speakerBar = _userBarBrush;
                break;
            case L'\x25CF':
                labelBrush = _claudeBrush;
                view.speakerBar = _claudeBarBrush;
                break;
            case L'\x2709':
                labelBrush = _peerBrush;
                view.speakerBar = _peerBarBrush;
                break;
            default:
                break;
            }
        }
        else if (AllItalic(block))
        {
            text.Foreground(_dimBrush); // notes / stats lines: secondary
        }

        bool inFirstLine = true; // for a label paragraph: still on the label line
        for (size_t runIndex = 0; runIndex < block.runs.size(); ++runIndex) // indexed: a session link names its run by index
        {
            const auto& source = block.runs[runIndex];
            if (source.text.empty())
            {
                continue;
            }
            // A Session chain row: a run holding "session N" goes in as plain pieces around hyperlinks
            // (_AppendSessionLinks). Such rows are never label paragraphs, so the label tint is not involved;
            // the first-line flag is kept honest anyway.
            if (sessionRefs && _AppendSessionLinks(text, source, runIndex, *sessionRefs))
            {
                if (source.text.find_first_of(L"\r\n") != std::wstring::npos)
                {
                    inFirstLine = false;
                }
                continue;
            }
            Run run;
            run.Text(winrt::hstring{ source.text });
            if (labelBrush && (source.style & (::Agentmaster::kMdCode | ::Agentmaster::kMdLink)) == 0)
            {
                run.Foreground(inFirstLine ? labelBrush : Brush{ _dimBrush });
            }
            if (source.text.find_first_of(L"\r\n") != std::wstring::npos)
            {
                inFirstLine = false; // the label line ended inside this run; everything after is "the rest"
            }
            if (source.style & ::Agentmaster::kMdBold)
            {
                run.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            if (source.style & ::Agentmaster::kMdItalic)
            {
                run.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            }
            if (source.style & ::Agentmaster::kMdCode)
            {
                run.FontFamily(_monoFont);
                if (!block.rawHtml)
                {
                    run.Foreground(_inlineCodeBrush);
                }
            }
            if (source.style & ::Agentmaster::kMdLink)
            {
                winrt::Windows::Foundation::Uri uri{ nullptr };
                if (IsWebUrl(source.url))
                {
                    try
                    {
                        uri = winrt::Windows::Foundation::Uri{ winrt::hstring{ source.url } };
                    }
                    catch (...)
                    {
                        // A malformed URL in a quoted message: logged (throttled), rendered as plain link text.
                        ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay link uri");
                    }
                }
                if (uri)
                {
                    Hyperlink link;
                    link.NavigateUri(uri);
                    link.Foreground(_linkBrush);
                    link.Inlines().Append(run);
                    text.Inlines().Append(link);
                    continue;
                }
                if ((source.style & ::Agentmaster::kMdCode) == 0)
                {
                    run.Foreground(_linkBrush);
                }
            }
            text.Inlines().Append(run);
        }
        if (block.truncated)
        {
            Run note;
            note.Text(kCutNote);
            note.Foreground(_dimBrush);
            note.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            text.Inlines().Append(note);
        }
        return text;
    }

    FrameworkElement AgentTranscriptOverlay::_BuildBlockElement(LevelView& view, const ::Agentmaster::MdBlock& block, const ::Agentmaster::MdBlock* prev, const std::vector<::Agentmaster::MdSessionRef>* sessionRefs)
    {
        FrameworkElement element = _BuildBlockContent(view, block, sessionRefs);
        const double gap = ::Agentmaster::MdGapAbove(prev, block);
        // The gap goes INSIDE the containers this block shares with the one above it: a quote bar they
        // share then runs unbroken through the gap, while a quote that STARTS here begins below it.
        const size_t shared = prev ? ::Agentmaster::MdSharedContainerDepth(*prev, block) : 0;
        const size_t depth = block.containers.size();
        if (shared >= depth)
        {
            AddMargin(element, 0, gap);
        }
        // Wrap from the innermost container out. Item indents with no marker on this block are not
        // wrappers — they fold into a left margin (`pendingIndent`) until the next real wrapper (a marker
        // column or a quote bar) or the outermost level takes them: a continuation line costs no Grid.
        double pendingIndent = 0.0;
        size_t quoteOrdinal = 0; // quotes outside position i, counted below (outermost == 0)
        for (size_t i = 0; i < depth; ++i)
        {
            if (block.containers[i].kind == ::Agentmaster::MdContainerKind::Quote)
            {
                ++quoteOrdinal;
            }
        }
        for (size_t i = depth; i-- > 0;)
        {
            const auto& container = block.containers[i];
            if (container.kind == ::Agentmaster::MdContainerKind::Item)
            {
                const double width = MarkerColumnWidth(container);
                if (container.marker.empty())
                {
                    pendingIndent += width;
                }
                else
                {
                    // This block is the item's FIRST: a [marker | content] grid. The marker can't sit
                    // in a margin — it needs its own column so wrapped lines hang under the text, not it.
                    if (pendingIndent > 0)
                    {
                        AddMargin(element, pendingIndent, 0);
                        pendingIndent = 0;
                    }
                    Grid row;
                    ColumnDefinition markerColumn;
                    markerColumn.Width(GridLength{ width, GridUnitType::Pixel });
                    ColumnDefinition contentColumn;
                    contentColumn.Width(GridLength{ 1, GridUnitType::Star });
                    row.ColumnDefinitions().Append(markerColumn);
                    row.ColumnDefinitions().Append(contentColumn);
                    TextBlock marker;
                    marker.Text(winrt::hstring{ container.marker });
                    marker.FontSize(kBodyFontSize);
                    marker.Foreground(_markerBrush);
                    marker.VerticalAlignment(VerticalAlignment::Top);
                    // A code panel's text starts below its padding, a table's first row below its cell padding: line the
                    // marker up with that text.
                    marker.Margin(Thickness{ 0, block.kind == ::Agentmaster::MdBlockKind::Code ? 7.0 : block.kind == ::Agentmaster::MdBlockKind::Table ? kTableCellPadY + 1.0 : 0.0, 0, 0 });
                    Grid::SetColumn(marker, 0);
                    row.Children().Append(marker);
                    Grid::SetColumn(element, 1);
                    row.Children().Append(element);
                    element = row;
                }
            }
            else
            {
                --quoteOrdinal; // now the ordinal of THIS quote
                if (pendingIndent > 0)
                {
                    AddMargin(element, pendingIndent, 0);
                    pendingIndent = 0;
                }
                Border bar;
                bar.BorderThickness(Thickness{ kQuoteBarWidth, 0, 0, 0 });
                // The outermost quote wears the current speaker's color (their message / reply); quotes
                // nested inside a message stay neutral.
                bar.BorderBrush(quoteOrdinal == 0 && view.speakerBar ? view.speakerBar : Brush{ _quoteBarBrush });
                bar.Padding(Thickness{ kQuotePadding, 0, 0, 0 });
                bar.Child(element);
                element = bar;
            }
            if (i == shared)
            {
                AddMargin(element, 0, gap);
            }
        }
        if (pendingIndent > 0)
        {
            AddMargin(element, pendingIndent, 0);
        }
        return element;
    }

    bool AgentTranscriptOverlay::_AppendSessionLinks(const TextBlock& text, const ::Agentmaster::MdRun& source, size_t runIndex, const std::vector<::Agentmaster::MdSessionRef>& refs)
    {
        // One plain piece of the run, in the run's own bold / italic style (a mention never sits in a code or
        // link run — MdFindSessionRefs skips those).
        const auto makeRun = [&source](std::wstring_view piece) {
            Run run;
            run.Text(winrt::hstring{ piece });
            if (source.style & ::Agentmaster::kMdBold)
            {
                run.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            if (source.style & ::Agentmaster::kMdItalic)
            {
                run.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            }
            return run;
        };
        const std::wstring_view whole{ source.text };
        bool any = false;
        size_t at = 0; // the first unit of the run not appended yet
        for (const auto& ref : refs)
        {
            // Another run's mention — or a range that would overlap / overrun this one (refs come in text
            // order from this very block, so never; the check keeps a bad range from slicing past the end).
            if (ref.run != runIndex || ref.offset < at || ref.length > whole.size() || ref.offset > whole.size() - ref.length)
            {
                continue;
            }
            any = true;
            if (ref.offset > at)
            {
                text.Inlines().Append(makeRun(whole.substr(at, ref.offset - at)));
            }
            Hyperlink link;
            link.Foreground(_linkBrush);
            link.Inlines().Append(makeRun(whole.substr(ref.offset, ref.length)));
            winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(link, winrt::hstring{ L"Go to session " + std::to_wstring(ref.session) });
            // No NavigateUri: Click is the whole behavior. A WEAK reference — the handler lives on the link,
            // and the overlay must never be kept alive by its own XAML.
            link.Click([weak = get_weak(), session = ref.session](const Hyperlink&, const HyperlinkClickEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_JumpToSession(session);
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay session link"); // the view stays where it was
                }
            });
            text.Inlines().Append(link);
            at = ref.offset + ref.length;
        }
        if (!any)
        {
            return false;
        }
        if (at < whole.size())
        {
            text.Inlines().Append(makeRun(whole.substr(at)));
        }
        return true;
    }

    void AgentTranscriptOverlay::_JumpToSession(size_t session)
    {
        auto& view = _levels[_current];
        if (session < view.sessionHeadings.size() && view.sessionHeadings[session])
        {
            view.pendingSessionJump = 0; // a direct jump supersedes any earlier click still waiting
            _ScrollToSessionHeading(view, session);
            return;
        }
        // The heading is further down than the time-sliced build has reached: jump the moment it is built
        // (_Pump). A level that is not building can't build it any more (the block cap cut it): nothing to do.
        if (view.state == LevelState::Building)
        {
            view.pendingSessionJump = session;
        }
    }

    void AgentTranscriptOverlay::_ScrollToSessionHeading(LevelView& view, size_t session) noexcept
    {
        try
        {
            if (!view.scroll || !view.content || session >= view.sessionHeadings.size() || !view.sessionHeadings[session])
            {
                return;
            }
            const auto& heading = view.sessionHeadings[session];
            // Lay the view out first: a heading built in this very slice has no position yet, and ChangeView
            // clamps to the current extent (the board's UpdateLayout-then-ChangeView recipe). Legal here — the
            // callers are a link's Click and a build slice, never a layout pass.
            view.scroll.UpdateLayout();
            // The heading's top in the content's coordinates == the scroll offset that puts it at the top edge
            // (a session heading is a top-level block, never inside a collapsed fold, so it always has a
            // position). TransformToVisual is right HERE, unlike in FindScrollAnchor: this is the level on
            // screen, laid out the line above.
            const auto top = heading.TransformToVisual(view.content).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
            view.scroll.ChangeView(nullptr, std::max(0.0, static_cast<double>(top.Y) - kSessionJumpGap), nullptr, true /* disableAnimation: an anchor jump, not a glide */);
            // Focus to the scroll view: the clicked link, left focused but scrolled away, would be brought back
            // into view the next time focus returns to it (BringIntoViewOnFocusChange — e.g. on a window
            // activation).
            view.scroll.Focus(FocusState::Programmatic);
        }
        catch (...)
        {
            // Rule #18. Recovery: the view stays where it was. Contained HERE, not left to the callers: a
            // pending jump runs inside a build slice (_Pump), whose catch would stop building the level — a
            // failed scroll must never cost the rest of the transcript.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay session jump");
        }
    }

    FrameworkElement AgentTranscriptOverlay::_BuildLevelGuidePanel() const
    {
        using ::Agentmaster::TurnsLevel;
        StackPanel panel;
        panel.RequestedTheme(ElementTheme::Dark);

        // A plain text line of the panel: explicit brush (the popup root is not themed by the card) and
        // wrapping, so a narrow window folds the guide's sentences instead of cutting them off.
        const auto addText = [&panel](std::wstring_view text, double size, const Brush& brush, const Thickness& margin) {
            TextBlock line;
            line.Text(winrt::hstring{ text });
            line.FontSize(size);
            line.Foreground(brush);
            line.TextWrapping(TextWrapping::Wrap);
            line.Margin(margin);
            panel.Children().Append(line);
            return line;
        };

        const auto title = addText(L"What each level shows", 15.0, _bodyBrush, Thickness{ 0, 0, 0, 0 });
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        addText(L"Each level shows everything the level to its left shows, and more. \x2713 marks what a level shows.",
                12.5,
                _dimBrush,
                Thickness{ 0, 4, 0, 10 });

        // --- the comparison table: a text column, then one column per level, least detailed first ---
        constexpr int levelCount = ::Agentmaster::kTurnsLevelCount;
        // The column of the level on screen; none (-1) while a box tab is on screen - those tabs are no column of the table
        // (the paragraph under it describes them).
        const int current = (_current >= 0 && _current < levelCount) ? _current : -1;
        const auto rowCount = static_cast<int32_t>(std::size(::Agentmaster::kTranscriptLevelGuide));
        Grid table;
        {
            ColumnDefinition textColumn;
            textColumn.Width(GridLength{ 1, GridUnitType::Star });
            table.ColumnDefinitions().Append(textColumn);
            for (int level = 0; level < levelCount; ++level)
            {
                ColumnDefinition column;
                column.Width(GridLength{ kGuideLevelColumnWidth, GridUnitType::Pixel });
                table.ColumnDefinitions().Append(column);
            }
            for (int32_t row = 0; row <= rowCount; ++row) // the head row + one per guide row
            {
                RowDefinition definition;
                definition.Height(GridLength{ 0, GridUnitType::Auto });
                table.RowDefinitions().Append(definition);
            }
        }
        // The current level's column: one tinted band behind its head and every cell (added first, so the
        // text draws over it). Childless, so it costs no composition node (the rounded-element gotcha). No band
        // while a box tab is on screen.
        if (current >= 0)
        {
            const auto accent = _accentBrush.Color();
            Border band;
            band.Background(SolidColorBrush{ ColorHelper::FromArgb(kGuideCurrentColumnAlpha, accent.R, accent.G, accent.B) });
            Grid::SetColumn(band, 1 + current);
            Grid::SetRowSpan(band, rowCount + 1);
            table.Children().Append(band);
        }
        // The heads: each level's label, and under it the digit that picks its tab. The tabs run in the Copy
        // menu's order (Default first), so the key is looked up by level rather than derived from the column.
        for (int level = 0; level < levelCount; ++level)
        {
            const bool isCurrent = level == current;
            StackPanel head;
            head.Margin(Thickness{ 0, 4, 0, 6 });
            TextBlock label;
            label.Text(::Agentmaster::TranscriptLevelLabel(static_cast<TurnsLevel>(level)));
            label.FontSize(12.5);
            label.Foreground(isCurrent ? static_cast<Brush>(_accentBrush) : static_cast<Brush>(_bodyBrush));
            label.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            label.HorizontalAlignment(HorizontalAlignment::Center);
            head.Children().Append(label);
            const auto tab = std::find(_tabViews.begin(), _tabViews.end(), level);
            if (tab != _tabViews.end())
            {
                TextBlock key;
                key.Text(L"key " + winrt::to_hstring(static_cast<int64_t>(tab - _tabViews.begin()) + 1));
                key.FontSize(kGuideSmallFontSize);
                key.Foreground(_dimBrush);
                key.HorizontalAlignment(HorizontalAlignment::Center);
                head.Children().Append(key);
            }
            Grid::SetColumn(head, 1 + level);
            table.Children().Append(head);
        }
        // The rows: the text, a faint wash on every other row (easier to follow a row across), and a check
        // mark or a dim dot under each level.
        const auto stripeBrush = MakeBrush(kGuideStripeAlpha, 0xFF, 0xFF, 0xFF);
        int32_t gridRow = 1;
        for (const auto& row : ::Agentmaster::kTranscriptLevelGuide)
        {
            if (gridRow % 2 == 0)
            {
                Border stripe;
                stripe.Background(stripeBrush);
                Grid::SetRow(stripe, gridRow);
                Grid::SetColumnSpan(stripe, 1 + levelCount);
                table.Children().Append(stripe);
            }
            TextBlock text;
            text.Text(row.text);
            text.FontSize(kGuideTextFontSize);
            text.Foreground(_bodyBrush);
            text.TextWrapping(TextWrapping::Wrap);
            text.Margin(Thickness{ 8, 5, 12, 5 });
            text.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetRow(text, gridRow);
            table.Children().Append(text);
            for (int level = 0; level < levelCount; ++level)
            {
                const bool shows = ::Agentmaster::TranscriptLevelShows(row, static_cast<TurnsLevel>(level));
                TextBlock mark;
                mark.Text(shows ? L"\x2713" : L"\x00B7");
                mark.FontSize(kGuideTextFontSize);
                mark.Foreground(shows ? _bodyBrush : _dimBrush);
                mark.HorizontalAlignment(HorizontalAlignment::Center);
                mark.VerticalAlignment(VerticalAlignment::Center);
                Grid::SetRow(mark, gridRow);
                Grid::SetColumn(mark, 1 + level);
                table.Children().Append(mark);
            }
            ++gridRow;
        }
        panel.Children().Append(table);

        // --- what every level shows beyond the first row, and what no level shows ---
        const auto addList = [&](const wchar_t* heading, const auto& items) {
            const auto head = addText(heading, kGuideTextFontSize, _bodyBrush, Thickness{ 0, 14, 0, 2 });
            head.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            for (const wchar_t* item : items)
            {
                addText(std::wstring{ L"\x2022  " } + item, 12.5, _bodyBrush, Thickness{ 8, 2, 0, 0 });
            }
        };
        addList(L"Also in every level", ::Agentmaster::kTranscriptLevelGuideEveryLevel);
        addList(L"In no level", ::Agentmaster::kTranscriptLevelGuideNoLevel);
        // The box tabs after the separator (TRANSCRIPT_OVERLAY.md §3j): no column of the table, one paragraph of their own.
        {
            const auto head = addText(L"Custom, User Only and Assistant Only", kGuideTextFontSize, _bodyBrush, Thickness{ 0, 14, 0, 2 });
            head.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            addText(::Agentmaster::kTranscriptViewsGuideNote, 12.5, _bodyBrush, Thickness{ 8, 2, 0, 0 });
        }
        // The Diff tab after the second separator (TRANSCRIPT_OVERLAY.md §3l): no column of the table either.
        {
            const auto head = addText(L"Diff", kGuideTextFontSize, _bodyBrush, Thickness{ 0, 14, 0, 2 });
            head.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            addText(::Agentmaster::kTranscriptDiffGuideNote, 12.5, _bodyBrush, Thickness{ 8, 2, 0, 0 });
        }
        addText(::Agentmaster::kTranscriptLevelGuideCodexNote, 12.0, _dimBrush, Thickness{ 0, 14, 0, 0 });
        addText(L"Keys 1-" + std::to_wstring(_tabViews.size()) +
                    L" pick a tab. The tab you pick is the one the transcript opens at next time, in every window. Hover a tab for its own list.",
                12.0,
                _dimBrush,
                Thickness{ 0, 6, 0, 0 });
        return panel;
    }

    void AgentTranscriptOverlay::_UpdateStatus()
    {
        // The "Expand all" switch follows the same moments the status does: a level switch, a render
        // landing, a build slice.
        _UpdateExpandAllButton();
        if (!_statusText)
        {
            return;
        }
        const auto& view = _levels[_current];
        std::wstring status;
        switch (view.state)
        {
        case LevelState::Loading:
            status = L"Rendering\x2026";
            break;
        case LevelState::Building:
            status = L"Laying out " + WithThousands(view.built) + L" of " + WithThousands(view.blocks.size()) + L" blocks\x2026";
            break;
        case LevelState::Done:
            // The Diff tab counts what it is about - turns, the ones that changed files, the files, the lines - where the
            // other views count the text's lines.
            status = (view.diffSummary.empty() ? WithThousands(view.lines) + L" lines" : view.diffSummary) + L" \x00B7 rendered in " +
                     FormatDurationMs(view.renderMs);
            if (view.truncated)
            {
                status += L" \x00B7 the view is cut short";
            }
            if (view.failed)
            {
                status += L" \x00B7 incomplete";
            }
            break;
        case LevelState::Idle:
        case LevelState::Empty:
        default:
            break;
        }
        _statusText.Text(winrt::hstring{ status });
    }

    void AgentTranscriptOverlay::_UpdateTitle()
    {
        if (!_subtitleText)
        {
            return;
        }
        std::wstring line;
        if (!_source.title.empty())
        {
            line = _source.title + L"  \x00B7  ";
        }
        line += ::Agentmaster::ShortId(_source.sessionId);
        if (_source.codex)
        {
            line += L"  \x00B7  Codex";
        }
        _subtitleText.Text(winrt::hstring{ line });
    }

    void AgentTranscriptOverlay::_UpdateTabVisuals()
    {
        for (size_t i = 0; i < _tabViews.size(); ++i)
        {
            const bool current = _tabViews[i] == _current;
            // Opacity + underline only — no weight change, which would shift every tab to its right.
            _tabLabels[i].Opacity(current ? 1.0 : 0.62);
            _tabUnderlines[i].Background(current ? Brush{ _accentBrush } : Brush{ _transparentBrush });
        }
    }

    void AgentTranscriptOverlay::_ReleaseLevels() noexcept
    {
        try
        {
            if (_bodyHost)
            {
                _bodyHost.Children().Clear();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay release views");
        }
        for (auto& view : _levels)
        {
            view = LevelView{};
        }
        // The Diff tab's file reads go with the views: the next render may be minutes later and the files changed since (a
        // Refresh exists for exactly that). A read still in flight lands under an older generation and is dropped.
        _diffFiles.clear();
        _diffExpandWaiting.clear();
    }

    void AgentTranscriptOverlay::_FocusBodyDeferred()
    {
        if (!_dispatcher)
        {
            return;
        }
        // Next tick: a scroll view created this tick has not had its first layout, and Focus on it
        // would silently do nothing.
        _dispatcher.TryEnqueue([weak = get_weak(), generation = _generation]() {
            const auto self = weak.get();
            if (!self || !self->_open || self->_generation != generation)
            {
                return;
            }
            try
            {
                if (const auto& scroll = self->_levels[self->_current].scroll)
                {
                    scroll.Focus(FocusState::Programmatic);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay focus body");
            }
        });
    }

    void AgentTranscriptOverlay::_CopyCurrentLevel()
    {
        const auto& view = _levels[_current];
        // The view's settings name: a level's ("compact") or a box tab's ("user-only") - its text is what the boxes made it.
        const auto levelName = ::Agentmaster::TranscriptViewName(_current);
        if (view.markdown.empty())
        {
            ::Agentmaster::LogNav(L"transcript-copy " + ::Agentmaster::ShortId(_source.sessionId) + L" " + levelName + L" (nothing rendered yet)");
            return;
        }
        if (RobustCopyTextToClipboard(view.markdown))
        {
            ::PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC); // the chime every Agentmaster copy plays
            ::Agentmaster::LogNav(L"transcript-copy " + ::Agentmaster::ShortId(_source.sessionId) + L" " + levelName + L" chars=" + std::to_wstring(view.markdown.size()));
        }
        else
        {
            ::Agentmaster::LogNav(L"transcript-copy " + ::Agentmaster::ShortId(_source.sessionId) + L" " + levelName + L" (the clipboard could not be opened)");
        }
    }

    void AgentTranscriptOverlay::_Refresh()
    {
        ::Agentmaster::LogNav(L"transcript-refresh " + ::Agentmaster::ShortId(_source.sessionId) + L" " + ::Agentmaster::TranscriptViewName(_current));
        ++_generation; // a render still running for the old views is dropped when it lands
        // A refresh re-renders the text, not the reader's choice: each view keeps its "Expand all" (and a box tab its
        // boxes, which live outside the views).
        std::array<bool, static_cast<size_t>(::Agentmaster::kTranscriptViewCount)> expandAll{}; // == _levels.size() (static_assert above)
        for (size_t i = 0; i < _levels.size(); ++i)
        {
            expandAll[i] = _levels[i].expandAll;
        }
        _ReleaseLevels();
        for (size_t i = 0; i < _levels.size(); ++i)
        {
            _levels[i].expandAll = expandAll[i];
        }
        _SelectLevel(_current, true);
    }

    // ---- the Diff tab (TRANSCRIPT_OVERLAY.md §3l) ----

    void AgentTranscriptOverlay::_ScanDiffOutline(const std::vector<::Agentmaster::MdBlock>& blocks, LevelRender& out)
    {
        std::vector<DiffOutlineTurn> turns;
        DiffOutlineTurn current;
        bool inTurn = false; // a turn heading was seen: the blocks below belong to it
        bool wantTitle = false; // a "❯" / "✉" label was seen in this turn: the next quoted block is its message
        int turnCount = 0; // numbered turns ("Work before the first prompt" is no turn of the user's)
        int plus = 0;
        int minus = 0;
        std::vector<std::wstring> files; // every changed file once, by its lower-cased shown path (few: a linear find is fine)
        const auto flush = [&]() {
            if (inTurn && !current.files.empty())
            {
                turns.push_back(std::move(current));
            }
            current = DiffOutlineTurn{};
        };
        for (size_t index = 0; index < blocks.size(); ++index)
        {
            const auto& block = blocks[index];
            if (block.kind == ::Agentmaster::MdBlockKind::Heading && block.headingLevel == 3 && block.containers.empty())
            {
                const int number = TurnHeadingNumber(block);
                // DiffTurn's other turn heading: the work a transcript opens with, before any prompt of its own.
                const bool before = number == 0 && BlockText(block).rfind(L"Work before the first prompt", 0) == 0;
                if (number > 0 || before)
                {
                    flush();
                    inTurn = true;
                    wantTitle = false;
                    current.number = number;
                    if (number > 0)
                    {
                        ++turnCount;
                    }
                    else
                    {
                        current.title = L"Work before the first prompt";
                    }
                    continue;
                }
            }
            if (!inTurn)
            {
                continue; // the Session chain list, a fork note: before any turn
            }
            if (block.containers.empty() && ::Agentmaster::MdIsLabelParagraph(block))
            {
                // The user's own message ("❯") or a teammate's ("✉") titles the turn; Claude's reply ("●") never does.
                const wchar_t glyph = LabelGlyph(block);
                wantTitle = current.title.empty() && (glyph == L'\x276F' || glyph == L'\x2709');
                continue;
            }
            if (wantTitle && !block.containers.empty() && block.containers.front().kind == ::Agentmaster::MdContainerKind::Quote)
            {
                wantTitle = false;
                current.title = DiffOutlineTitle(block.kind == ::Agentmaster::MdBlockKind::Code ? block.code : BlockText(block));
                continue;
            }
            if (block.kind == ::Agentmaster::MdBlockKind::Code && block.containers.empty() && block.info == L"diff")
            {
                wantTitle = false;
                const auto parsed = ::Agentmaster::ParseUnifiedDiff(block.code);
                DiffOutlineFile file;
                std::wstring meta;
                SplitDiffInfo(block.infoRest, file.path, meta);
                if (file.path.empty())
                {
                    file.path = parsed.newPath; // a fence without its info: the diff's own "+++" path
                }
                file.added = parsed.added;
                file.removed = parsed.removed;
                file.block = index;
                plus += parsed.added;
                minus += parsed.removed;
                std::wstring key = file.path;
                std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
                if (std::find(files.begin(), files.end(), key) == files.end())
                {
                    files.push_back(std::move(key));
                }
                current.files.push_back(std::move(file));
            }
        }
        flush();
        std::wstring summary = std::to_wstring(turnCount) + (turnCount == 1 ? L" turn" : L" turns");
        if (files.empty())
        {
            summary += L" \x00B7 no file changed";
        }
        else
        {
            summary += L" \x00B7 " + std::to_wstring(turns.size()) + L" with changes \x00B7 " + std::to_wstring(files.size()) +
                       (files.size() == 1 ? L" file" : L" files") + L" \x00B7 +" + std::to_wstring(plus) + L" \x2212" + std::to_wstring(minus);
        }
        out.outline = std::move(turns);
        out.diffSummary = std::move(summary);
    }

    void AgentTranscriptOverlay::_BuildDiffOutline(LevelView& view, const std::vector<DiffOutlineTurn>& turns)
    {
        view.outlineTurns.clear();
        view.outlineFiles.clear();
        view.outlineLit = -1;
        if (!view.outline)
        {
            return;
        }
        view.outline.Children().Clear();
        // An entry: a plain, square button (a stock rounded template is a composition node per instance - CLAUDE.md "A
        // rounded element WITH children"), its words trimmed to the column.
        const auto makeEntry = [this](const UIElement& content, const Thickness& padding) {
            Button entry;
            entry.Background(_transparentBrush); // transparent (not null): hit-testable, and the template still shows hover
            entry.BorderThickness(Thickness{ 0, 0, 0, 0 });
            entry.CornerRadius(CornerRadius{ 0, 0, 0, 0 });
            entry.Padding(padding);
            entry.MinWidth(0);
            entry.MinHeight(0);
            entry.HorizontalAlignment(HorizontalAlignment::Stretch);
            entry.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            entry.Content(content);
            return entry;
        };
        TextBlock head;
        head.Text(turns.empty() ? L"No turn changed a file with Edit, MultiEdit, Write or NotebookEdit." : L"CHANGED FILES BY TURN");
        head.FontSize(11);
        head.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        head.Foreground(_dimBrush);
        head.TextWrapping(TextWrapping::Wrap);
        head.Margin(Thickness{ 6, 0, 0, 6 });
        view.outline.Children().Append(head);
        size_t entries = 0;
        for (size_t t = 0; t < turns.size(); ++t)
        {
            const auto& turn = turns[t];
            if (entries >= kDiffOutlineMaxEntries)
            {
                // A pathological session: the page still holds every card; the outline stops listing them.
                TextBlock more;
                more.Text(L"+ " + std::to_wstring(turns.size() - t) + L" more turns with changes - scroll the page to see them");
                more.FontSize(12);
                more.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
                more.Foreground(_dimBrush);
                more.TextWrapping(TextWrapping::Wrap);
                more.Margin(Thickness{ 6, 8, 0, 0 });
                view.outline.Children().Append(more);
                break;
            }
            // The turn: "Turn 4 · <its message's first words>" (the work before the first prompt has its own words).
            const std::wstring words = turn.number > 0 ? L"Turn " + std::to_wstring(turn.number) + (turn.title.empty() ? std::wstring{} : L" \x00B7 " + turn.title) :
                                                         turn.title;
            TextBlock label;
            label.Text(winrt::hstring{ words });
            label.FontSize(12.5);
            label.Foreground(_diff.outlineTurn);
            label.TextWrapping(TextWrapping::NoWrap);
            label.TextTrimming(TextTrimming::CharacterEllipsis);
            const auto entry = makeEntry(label, Thickness{ 6, 4, 6, 4 });
            entry.Margin(Thickness{ 0, t == 0 ? 0.0 : 6.0, 0, 0 });
            AgentSetTip(entry, winrt::hstring{ words + L"\nClick to scroll the page to this turn." });
            winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(entry, winrt::hstring{ words });
            // By value: the turn's number and its first card (the jump target of the work before the first prompt). The
            // overlay weakly; no element is captured (CLAUDE.md "A handler that captures its own element").
            entry.Click([weak = get_weak(), number = turn.number, firstBlock = turn.files.empty() ? size_t{ 0 } : turn.files.front().block](
                            const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                try
                {
                    if (const auto self = weak.get())
                    {
                        self->_JumpToDiffTurn(number, firstBlock);
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff outline turn"); // the page stays where it was
                }
            });
            view.outline.Children().Append(entry);
            view.outlineTurns.emplace_back(turn.number, entry);
            ++entries;
            for (const auto& file : turn.files)
            {
                // The file: its NAME (the folder is in the tooltip - the column is narrow, and a trimmed path would lose the
                // name) and its +a -d.
                const size_t slash = file.path.find_last_of(L"/\\");
                TextBlock name;
                name.Text(winrt::hstring{ slash == std::wstring::npos ? file.path : file.path.substr(slash + 1) });
                name.FontFamily(_monoFont);
                name.FontSize(11.5);
                name.Foreground(_diff.outlineFile);
                name.TextWrapping(TextWrapping::NoWrap);
                name.TextTrimming(TextTrimming::CharacterEllipsis);
                Grid::SetColumn(name, 0);
                TextBlock counts;
                counts.FontSize(11.5);
                counts.Margin(Thickness{ 8, 0, 0, 0 });
                if (file.added > 0 || file.removed == 0)
                {
                    Run added;
                    added.Text(L"+" + std::to_wstring(file.added));
                    added.Foreground(_diff.plus);
                    counts.Inlines().Append(added);
                }
                if (file.removed > 0)
                {
                    Run removed;
                    removed.Text(std::wstring{ file.added > 0 ? L" " : L"" } + L"\x2212" + std::to_wstring(file.removed));
                    removed.Foreground(_diff.minus);
                    counts.Inlines().Append(removed);
                }
                Grid::SetColumn(counts, 1);
                Grid line;
                ColumnDefinition nameColumn;
                nameColumn.Width(GridLength{ 1, GridUnitType::Star });
                ColumnDefinition countsColumn;
                countsColumn.Width(GridLength{ 0, GridUnitType::Auto });
                line.ColumnDefinitions().Append(nameColumn);
                line.ColumnDefinitions().Append(countsColumn);
                line.Children().Append(name);
                line.Children().Append(counts);
                const auto fileEntry = makeEntry(line, Thickness{ 20, 2, 6, 2 });
                AgentSetTip(fileEntry, winrt::hstring{ file.path + L"\nClick to scroll the page to this file's changes." });
                winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(fileEntry, winrt::hstring{ file.path });
                fileEntry.Click([weak = get_weak(), block = file.block](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
                    try
                    {
                        if (const auto self = weak.get())
                        {
                            self->_JumpToDiffCard(block);
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff outline file"); // the page stays where it was
                    }
                });
                view.outline.Children().Append(fileEntry);
                view.outlineFiles.emplace_back(file.block, fileEntry);
                ++entries;
            }
        }
    }

    void AgentTranscriptOverlay::_UpdateDiffOutlineHighlight() noexcept
    {
        try
        {
            if (!_open || _current < 0 || _current >= static_cast<int>(_levels.size()))
            {
                return;
            }
            auto& view = _levels[static_cast<size_t>(_current)];
            if (!view.isDiff || view.outlineTurns.empty() || !view.scroll)
            {
                return;
            }
            const int top = _TopTurnNumber(view);
            int lit = -1;
            if (top > 0)
            {
                // Linear: the entries are few, and "Work before the first prompt" entries (number 0) can sit anywhere in a
                // chain, so the numbers are not sorted for a binary search.
                for (size_t i = 0; i < view.outlineTurns.size(); ++i)
                {
                    if (view.outlineTurns[i].first == top)
                    {
                        lit = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (lit == view.outlineLit)
            {
                return; // the same entry (or none) - ViewChanged fires every frame of a scroll: set nothing
            }
            const auto paint = [this](const Button& entry, bool on) {
                if (!entry)
                {
                    return;
                }
                entry.Background(on ? Brush{ _diff.outlineLit } : Brush{ _transparentBrush });
                if (const auto label = entry.Content().try_as<TextBlock>())
                {
                    label.Foreground(on ? _bodyBrush : _diff.outlineTurn);
                }
            };
            if (view.outlineLit >= 0 && view.outlineLit < static_cast<int>(view.outlineTurns.size()))
            {
                paint(view.outlineTurns[static_cast<size_t>(view.outlineLit)].second, false);
            }
            view.outlineLit = lit;
            if (lit < 0)
            {
                return;
            }
            const auto& entry = view.outlineTurns[static_cast<size_t>(lit)].second;
            paint(entry, true);
            // Keep the lit entry in sight: scroll the outline only when it lies outside the outline's viewport.
            if (view.outlineScroll && view.outline && entry)
            {
                const double y = entry.TransformToVisual(view.outline).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f }).Y;
                const double height = entry.ActualHeight();
                const double offset = view.outlineScroll.VerticalOffset();
                const double viewport = view.outlineScroll.ViewportHeight();
                if (y < offset)
                {
                    view.outlineScroll.ChangeView(nullptr, std::max(0.0, y - 8.0), nullptr, true);
                }
                else if (y + height > offset + viewport)
                {
                    view.outlineScroll.ChangeView(nullptr, y + height - viewport + 8.0, nullptr, true);
                }
            }
        }
        catch (...)
        {
            // Rule #18 (throttled). Recovery: the highlight stays as it was until the next scroll.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff outline highlight");
        }
    }

    void AgentTranscriptOverlay::_JumpToDiffTurn(int number, size_t firstBlock)
    {
        if (!_open)
        {
            return;
        }
        auto& view = _levels[static_cast<size_t>(_current)];
        if (!view.isDiff)
        {
            return;
        }
        if (number <= 0)
        {
            _JumpToDiffCard(firstBlock); // the work before the first prompt has no numbered heading: its first card
            return;
        }
        // An explicit move: whatever was waiting to scroll this view is dropped.
        view.pendingSessionJump = 0;
        view.pendingTurnJump = 0;
        view.pendingCardJump = 0;
        view.followEnd = false;
        const auto at = std::lower_bound(view.turnHeadings.begin(), view.turnHeadings.end(), number, [](const auto& entry, int wanted) {
            return entry.first < wanted;
        });
        if (at != view.turnHeadings.end() && at->first == number)
        {
            _ScrollToTurnHeading(view, static_cast<size_t>(at - view.turnHeadings.begin()));
            return;
        }
        if (view.state == LevelState::Building)
        {
            view.pendingTurnJump = number; // _Pump scrolls it to the top the moment the build reaches it
        }
    }

    void AgentTranscriptOverlay::_JumpToDiffCard(size_t block)
    {
        if (!_open)
        {
            return;
        }
        auto& view = _levels[static_cast<size_t>(_current)];
        if (!view.isDiff)
        {
            return;
        }
        view.pendingSessionJump = 0;
        view.pendingTurnJump = 0;
        view.pendingCardJump = 0;
        view.followEnd = false;
        // The cards are recorded in block order.
        const auto at = std::lower_bound(view.diffCards.begin(), view.diffCards.end(), block, [](const std::shared_ptr<DiffCard>& card, size_t wanted) {
            return card->block < wanted;
        });
        if (at != view.diffCards.end() && (*at)->block == block && (*at)->root)
        {
            _ScrollElementToTop(view, (*at)->root);
            return;
        }
        if (view.state == LevelState::Building)
        {
            view.pendingCardJump = block + 1; // _Pump scrolls to it the moment the build reaches it
        }
    }

    void AgentTranscriptOverlay::_ScrollElementToTop(LevelView& view, const FrameworkElement& element) noexcept
    {
        try
        {
            if (!view.scroll || !view.content || !element)
            {
                return;
            }
            // The session jump's recipe: lay out first (an element built in this very slice has no position yet, and
            // ChangeView clamps to the current extent), then the element's top in the content's coordinates is the offset.
            view.scroll.UpdateLayout();
            const auto at = element.TransformToVisual(view.content).TransformPoint(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
            view.scroll.ChangeView(nullptr, std::max(0.0, static_cast<double>(at.Y) - kSessionJumpGap), nullptr, true /* disableAnimation: a jump, not a glide */);
        }
        catch (...)
        {
            // Rule #18. Recovery: the view stays where it was. Contained here: a pending jump runs inside a build slice.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff card jump");
        }
    }

    FrameworkElement AgentTranscriptOverlay::_BuildDiffHeading(LevelView& view, const ::Agentmaster::MdBlock& block)
    {
        view.speakerBar = nullptr; // a heading starts a new turn: no speaker yet (as _BuildBlockContent does)
        auto text = _MakeTextBlock(HeadingFontSize(block.headingLevel));
        text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        const std::wstring all = BlockText(block);
        // DiffTurn ends the heading of a turn that changed files with " · N file(s) · +A -D". Read it off the end; anything
        // that does not match exactly is drawn as it is.
        const size_t plusAt = all.rfind(L" \x00B7 +");
        if (plusAt != std::wstring::npos && plusAt > 0)
        {
            const std::wstring tail = all.substr(plusAt + 4); // "A -D"
            const size_t space = tail.find(L" -");
            const auto digits = [](std::wstring_view part) {
                return !part.empty() && std::all_of(part.begin(), part.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
            };
            const size_t filesAt = all.rfind(L" \x00B7 ", plusAt - 1);
            if (space != std::wstring::npos && digits(std::wstring_view{ tail }.substr(0, space)) && digits(std::wstring_view{ tail }.substr(space + 2)) &&
                filesAt != std::wstring::npos)
            {
                const std::wstring files = all.substr(filesAt + 3, plusAt - (filesAt + 3)); // "N files"
                if (files.size() > 5 && (files.ends_with(L" file") || files.ends_with(L" files")))
                {
                    Run headRun;
                    headRun.Text(winrt::hstring{ all.substr(0, filesAt) });
                    text.Inlines().Append(headRun);
                    // The counts the GitHub way: the files dim, "+A" green, "−D" red - a lighter weight than the heading.
                    Run filesRun;
                    filesRun.Text(winrt::hstring{ L" \x00B7 " + files + L" \x00B7 " });
                    filesRun.Foreground(_dimBrush);
                    filesRun.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
                    text.Inlines().Append(filesRun);
                    Run added;
                    added.Text(winrt::hstring{ L"+" + tail.substr(0, space) });
                    added.Foreground(_diff.plus);
                    added.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
                    text.Inlines().Append(added);
                    Run removed;
                    removed.Text(winrt::hstring{ L" \x2212" + tail.substr(space + 2) });
                    removed.Foreground(_diff.minus);
                    removed.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
                    text.Inlines().Append(removed);
                    return text;
                }
            }
        }
        text.Text(winrt::hstring{ all });
        return text;
    }

    FrameworkElement AgentTranscriptOverlay::_BuildDiffBashRow(const ::Agentmaster::MdBlock& block)
    {
        // The reason's runs: everything after the "$" code span (IsDiffBashRow found it first), its leading blank dropped,
        // and the " ×N" DiffTurn appends to a repeated reason taken off the end into the count.
        std::vector<::Agentmaster::MdRun> runs;
        bool sawChip = false;
        for (const auto& run : block.runs)
        {
            if (!sawChip)
            {
                if (run.text.find_first_not_of(L" \t") != std::wstring::npos)
                {
                    sawChip = true; // the "$" itself: drawn as the chip
                }
                continue;
            }
            runs.push_back(run);
        }
        while (!runs.empty())
        {
            auto& first = runs.front();
            const size_t at = first.text.find_first_not_of(L" \t");
            if (at == std::wstring::npos)
            {
                runs.erase(runs.begin());
                continue;
            }
            first.text.erase(0, at);
            break;
        }
        int count = 1;
        if (!runs.empty() && (runs.back().style & ::Agentmaster::kMdCode) == 0)
        {
            count = TakeDiffRepeatCount(runs.back().text);
        }
        Grid row;
        ColumnDefinition chipColumn;
        chipColumn.Width(GridLength{ 0, GridUnitType::Auto });
        ColumnDefinition reasonColumn;
        reasonColumn.Width(GridLength{ 1, GridUnitType::Star });
        row.ColumnDefinitions().Append(chipColumn);
        row.ColumnDefinitions().Append(reasonColumn);
        // The "$" chip: a childless rounded frame BEHIND the text in the same cell (a rounded element WITH children is a
        // composition node every frame walks - CLAUDE.md), level with the reason's first line.
        Grid chip;
        chip.VerticalAlignment(VerticalAlignment::Top);
        chip.Margin(Thickness{ 0, 2, 8, 0 });
        Border frame;
        frame.BorderBrush(_diff.chipBorder);
        frame.BorderThickness(Thickness{ 1, 1, 1, 1 });
        frame.CornerRadius(CornerRadius{ 3, 3, 3, 3 });
        TextBlock dollar;
        dollar.Text(L"$");
        dollar.FontFamily(_monoFont);
        dollar.FontSize(11);
        dollar.Foreground(_diff.chipText);
        dollar.Margin(Thickness{ 5, 0, 5, 1 });
        chip.Children().Append(frame);
        chip.Children().Append(dollar);
        Grid::SetColumn(chip, 0);
        row.Children().Append(chip);
        // The reason: selectable, its own styles (a command shown as code stays code) - a table cell's run rules.
        auto reason = _MakeTextBlock(12.5);
        reason.Foreground(_diff.reason);
        _AppendCellRuns(reason, runs);
        if (count > 1)
        {
            Run times;
            times.Text(winrt::hstring{ L"  \x00D7" + std::to_wstring(count) });
            times.Foreground(_diff.count);
            times.FontSize(11.5);
            times.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            reason.Inlines().Append(times);
        }
        Grid::SetColumn(reason, 1);
        row.Children().Append(reason);
        return row;
    }

    FrameworkElement AgentTranscriptOverlay::_BuildDiffCard(LevelView& view, const ::Agentmaster::MdBlock& block, size_t index)
    {
        auto card = std::make_shared<DiffCard>();
        card->block = index;
        auto parsed = ::Agentmaster::ParseUnifiedDiff(block.code);
        card->hunks = std::move(parsed.hunks);
        card->numbered = !parsed.unnumbered;
        card->truncated = block.truncated;
        // The file Expand reads: the "+++" side - none for "/dev/null" (nothing is left of it).
        if (!parsed.newPath.empty() && parsed.newPath != L"/dev/null")
        {
            card->filePath = parsed.newPath;
        }
        std::wstring shown;
        std::wstring meta;
        SplitDiffInfo(block.infoRest, shown, meta);
        if (shown.empty())
        {
            shown = card->filePath.empty() ? std::wstring{ L"(a file)" } : card->filePath;
        }
        size_t lineCount = 0;
        int largest = 0;
        for (const auto& hunk : card->hunks)
        {
            lineCount += hunk.rows.size();
            largest = std::max({ largest, hunk.oldStart + hunk.oldLines, hunk.newStart + hunk.newLines });
        }
        // Room for the numbers a few expansions below the last hunk reach too, so the numbers column never widens mid-card.
        card->numberDigits = DiffNumberDigits(largest + 4 * kDiffExpandStep);
        // GitHub's "large diffs are not rendered by default": a long card starts closed and builds its body on its first open.
        card->open = lineCount <= kDiffCardOpenRows;

        // --- the frame: square - a rounded element with children is a composition node every frame walks ---
        Border frame;
        frame.Background(_diff.card);
        frame.BorderBrush(_diff.border);
        frame.BorderThickness(Thickness{ 1, 1, 1, 1 });
        StackPanel stack;
        frame.Child(stack);

        // --- the header bar: [the toggle: chevron · +a -d · five squares · path · what it is] [Viewed] ---
        Grid head;
        head.Background(_diff.head);
        {
            ColumnDefinition toggleColumn;
            toggleColumn.Width(GridLength{ 1, GridUnitType::Star });
            ColumnDefinition viewedColumn;
            viewedColumn.Width(GridLength{ 0, GridUnitType::Auto });
            head.ColumnDefinitions().Append(toggleColumn);
            head.ColumnDefinitions().Append(viewedColumn);
        }
        // Nothing inside the toggle takes hits: the button must be the only target (the badge's mkIconBtn lesson).
        FontIcon chevron;
        chevron.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        chevron.Glyph(card->open ? kFoldOpenGlyph : kFoldClosedGlyph);
        chevron.FontSize(9);
        chevron.Width(kFoldChevronWidth);
        chevron.Foreground(_diff.meta);
        chevron.VerticalAlignment(VerticalAlignment::Center);
        chevron.IsHitTestVisible(false);
        Grid::SetColumn(chevron, 0);
        TextBlock counts;
        counts.FontSize(12);
        counts.VerticalAlignment(VerticalAlignment::Center);
        counts.Margin(Thickness{ 8, 0, 0, 0 });
        counts.IsHitTestVisible(false);
        if (parsed.added > 0 || parsed.removed == 0)
        {
            Run added;
            added.Text(L"+" + std::to_wstring(parsed.added));
            added.Foreground(_diff.plus);
            counts.Inlines().Append(added);
        }
        if (parsed.removed > 0)
        {
            Run removed;
            removed.Text(std::wstring{ parsed.added > 0 ? L" " : L"" } + L"\x2212" + std::to_wstring(parsed.removed));
            removed.Foreground(_diff.minus);
            counts.Inlines().Append(removed);
        }
        Grid::SetColumn(counts, 1);
        // GitHub's five diffstat squares: childless (rounded but childless is no composition node of its own).
        StackPanel bars;
        bars.Orientation(Orientation::Horizontal);
        bars.Spacing(1);
        bars.VerticalAlignment(VerticalAlignment::Center);
        bars.Margin(Thickness{ 6, 0, 10, 0 });
        bars.IsHitTestVisible(false);
        {
            const auto [green, red] = DiffBarSquares(parsed.added, parsed.removed);
            for (int square = 0; square < 5; ++square)
            {
                Border cell;
                cell.Width(7);
                cell.Height(7);
                cell.CornerRadius(CornerRadius{ 1, 1, 1, 1 });
                cell.Background(square < green ? _diff.plus : square < green + red ? _diff.minus : _diff.border);
                bars.Children().Append(cell);
            }
        }
        Grid::SetColumn(bars, 2);
        TextBlock path;
        path.Text(winrt::hstring{ shown });
        path.FontFamily(_monoFont);
        path.FontSize(12);
        path.Foreground(_diff.code);
        path.TextWrapping(TextWrapping::NoWrap);
        path.TextTrimming(TextTrimming::CharacterEllipsis);
        path.VerticalAlignment(VerticalAlignment::Center);
        path.IsHitTestVisible(false);
        Grid::SetColumn(path, 3);
        TextBlock what;
        what.Text(winrt::hstring{ meta });
        what.FontSize(11.5);
        what.Foreground(_diff.meta);
        what.TextWrapping(TextWrapping::NoWrap);
        what.VerticalAlignment(VerticalAlignment::Center);
        what.Margin(Thickness{ 10, 0, 0, 0 });
        what.IsHitTestVisible(false);
        Grid::SetColumn(what, 4);
        Grid toggleContent;
        for (const auto width : { GridLength{ 0, GridUnitType::Auto }, GridLength{ 0, GridUnitType::Auto }, GridLength{ 0, GridUnitType::Auto }, GridLength{ 1, GridUnitType::Star }, GridLength{ 0, GridUnitType::Auto } })
        {
            ColumnDefinition column;
            column.Width(width);
            toggleContent.ColumnDefinitions().Append(column);
        }
        toggleContent.Children().Append(chevron);
        toggleContent.Children().Append(counts);
        toggleContent.Children().Append(bars);
        toggleContent.Children().Append(path);
        toggleContent.Children().Append(what);
        Button toggle;
        toggle.Background(_transparentBrush); // transparent (not null): hit-testable, and the template still shows hover
        toggle.BorderThickness(Thickness{ 0, 0, 0, 0 });
        toggle.CornerRadius(CornerRadius{ 0, 0, 0, 0 });
        toggle.Padding(Thickness{ 10, 6, 10, 6 });
        toggle.MinWidth(0);
        toggle.MinHeight(0);
        toggle.HorizontalAlignment(HorizontalAlignment::Stretch);
        toggle.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        toggle.Content(toggleContent);
        AgentSetTip(toggle, winrt::hstring{ (card->filePath.empty() ? shown : card->filePath) + L"\nClick to open or close this file's changes." });
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(toggle, winrt::hstring{ L"Changes to " + shown + (meta.empty() ? std::wstring{} : L", " + meta) });
        Grid::SetColumn(toggle, 0);
        head.Children().Append(toggle);
        CheckBox viewed;
        viewed.Content(winrt::box_value(winrt::hstring{ L"Viewed" }));
        viewed.FontSize(11.5);
        viewed.Foreground(_diff.meta);
        viewed.MinWidth(0);
        viewed.MinHeight(0);
        viewed.VerticalAlignment(VerticalAlignment::Center);
        viewed.Margin(Thickness{ 6, 0, 10, 0 });
        AgentSetTip(viewed, winrt::hstring{ L"Viewed: closes this file's changes and dims it in the outline, while the transcript stays open." });
        Grid::SetColumn(viewed, 1);
        head.Children().Append(viewed);
        stack.Children().Append(head);

        // --- the body: the header's rule, then the hunks (built now, or on the first open of a long card) ---
        StackPanel body;
        body.Visibility(card->open ? Visibility::Visible : Visibility::Collapsed);
        Border headRule; // childless: no composition node
        headRule.Height(1);
        headRule.Background(_diff.border);
        body.Children().Append(headRule);
        stack.Children().Append(body);

        card->root = frame;
        card->body = body;
        card->chevron = chevron;
        card->viewed = viewed;
        view.diffCards.push_back(card);
        if (card->open)
        {
            _BuildDiffCardBody(card);
        }
        // The handlers reach the overlay and the card WEAKLY: the card owns its XAML, the XAML owns the handlers - a strong
        // reference here would be a cycle nothing frees (CLAUDE.md "A handler that captures its own element").
        toggle.Click([weak = get_weak(), weakCard = std::weak_ptr<DiffCard>{ card }](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            try
            {
                const auto self = weak.get();
                const auto target = weakCard.lock();
                if (self && target)
                {
                    self->_SetDiffCardOpen(target, !target->open);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff card toggle"); // the card stays as it was
            }
        });
        // Click, never Checked / Unchecked: only the user's own click (a press, Space) means "viewed".
        viewed.Click([weak = get_weak(), weakCard = std::weak_ptr<DiffCard>{ card }](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            try
            {
                const auto self = weak.get();
                const auto target = weakCard.lock();
                if (self && target && target->viewed)
                {
                    const auto checked = target->viewed.IsChecked();
                    self->_SetDiffCardViewed(target, checked && checked.Value());
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff card viewed"); // the card stays as it was
            }
        });
        return frame;
    }

    void AgentTranscriptOverlay::_BuildDiffCardBody(const std::shared_ptr<DiffCard>& card)
    {
        if (!card || card->bodyBuilt || !card->body)
        {
            return;
        }
        // Marked FIRST: a failure part-way leaves what was built, never a second copy on the next open.
        card->bodyBuilt = true;
        const size_t count = card->hunks.size();
        card->hunkPanels.assign(count, StackPanel{ nullptr });
        card->revealedAbove.assign(count, 0);
        card->revealedBelow.assign(count, 0);
        card->located.assign(count, -2);
        card->expandRows.assign(count + 1, Button{ nullptr });
        card->expandLabels.assign(count + 1, TextBlock{ nullptr });
        // Expand reads the file as it is now (the user's pick), so it needs a file and line numbers to find the hunk by.
        const bool expandable = card->numbered && !card->filePath.empty();
        // The hunks go straight into the body: no horizontal scroll view - the code WRAPS (the user's "word wrap without
        // horizontal scrollbar"), each line in its own grid row (_MakeDiffLines), so the page's width is the card's.
        const double numbersWidth = DiffNumbersColumnWidth(card->numberDigits);
        for (size_t h = 0; h < count; ++h)
        {
            const auto& hunk = card->hunks[h];
            // An expand row only where lines lie above the hunk (one at the file's first line has none).
            if (expandable && DiffLinesAboveHunk(card->hunks, h) > 0)
            {
                card->body.Children().Append(_MakeDiffExpandRow(card, h));
            }
            StackPanel hunkPanel;
            if (card->numbered)
            {
                // GitHub's "@@" row on its blue band, its words where the code starts - wrapping too (a long function name
                // in its section), a line high at least.
                Grid header;
                header.Background(_diff.hunkBand);
                header.MinHeight(kDiffLineHeight);
                TextBlock words;
                words.Text(winrt::hstring{ DiffDisplayLine(::Agentmaster::FormatHunkHeader(hunk)) });
                words.FontFamily(_monoFont);
                words.FontSize(kDiffFontSize);
                words.Foreground(_diff.meta);
                words.TextWrapping(TextWrapping::Wrap);
                words.LineHeight(kDiffLineHeight);
                words.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
                words.VerticalAlignment(VerticalAlignment::Center);
                words.Margin(Thickness{ numbersWidth + 4.0, 0, 12, 0 });
                header.Children().Append(words);
                hunkPanel.Children().Append(header);
            }
            hunkPanel.Children().Append(_MakeDiffLines(::Agentmaster::NumberHunkRows(hunk), card->numberDigits, false));
            card->body.Children().Append(hunkPanel);
            card->hunkPanels[h] = hunkPanel;
        }
        if (expandable && count > 0)
        {
            card->body.Children().Append(_MakeDiffExpandRow(card, count));
        }
        const auto addNote = [this, &card](std::wstring_view words) {
            TextBlock note;
            note.Text(winrt::hstring{ words });
            note.FontSize(11.5);
            note.Foreground(_diff.meta);
            note.TextWrapping(TextWrapping::Wrap);
            note.Margin(Thickness{ 10, 4, 10, 6 });
            card->body.Children().Append(note);
        };
        if (!card->numbered)
        {
            addNote(L"No line numbers: Claude Code recorded only the replaced text of this change, so nothing around it can be shown.");
        }
        if (card->truncated)
        {
            addNote(L"This change is cut here - it is too long to show in full. The header's Copy button copies the whole text.");
        }
        // The file may be read already (another card of it expanded): the rows say what they can show right away.
        if (expandable)
        {
            if (const auto known = _diffFiles.find(card->filePath); known != _diffFiles.end())
            {
                _SyncDiffExpandRows(*card, known->second.get(), known->second == nullptr);
            }
        }
    }

    FrameworkElement AgentTranscriptOverlay::_MakeDiffLines(const std::vector<::Agentmaster::DiffRow>& rows, int digits, bool fromFileNow) const
    {
        // [numbers | code]: the numbers cell a fixed width (its text is monospace and padded, DiffNumbersText), the code a
        // star - so it takes the rest of the card's width, WRAPS inside it, and the bands below reach the card's right edge.
        Grid grid;
        ColumnDefinition numbersColumn;
        numbersColumn.Width(GridLength{ DiffNumbersColumnWidth(digits), GridUnitType::Pixel });
        ColumnDefinition codeColumn;
        codeColumn.Width(GridLength{ 1, GridUnitType::Star });
        grid.ColumnDefinitions().Append(numbersColumn);
        grid.ColumnDefinitions().Append(codeColumn);
        // One Auto row per line: a wrapped line's row grows to its visual lines, and the next line starts below it - the
        // numbers of every line stay level with that line's FIRST visual line, whatever the width.
        const auto count = static_cast<int32_t>(rows.size());
        for (int32_t line = 0; line < count; ++line)
        {
            RowDefinition definition;
            definition.Height(GridLength{ 0, GridUnitType::Auto });
            grid.RowDefinitions().Append(definition);
        }
        // The bands first (drawn under the text): one per run of added / removed lines, spanning the run's rows across both
        // columns, and a stronger one under their numbers. Childless and never hit-tested; a band spans rows (RowSpan), so it
        // follows the lines' wrapped heights by itself.
        for (size_t first = 0; first < rows.size();)
        {
            const wchar_t kind = rows[first].kind;
            size_t end = first + 1;
            while (end < rows.size() && rows[end].kind == kind)
            {
                ++end;
            }
            if (kind == L'+' || kind == L'-')
            {
                const bool added = kind == L'+';
                const auto top = static_cast<int32_t>(first);
                const auto span = static_cast<int32_t>(end - first);
                Border band;
                band.Background(added ? _diff.addBand : _diff.removeBand);
                band.IsHitTestVisible(false);
                Grid::SetRow(band, top);
                Grid::SetRowSpan(band, span);
                Grid::SetColumnSpan(band, 2);
                grid.Children().Append(band);
                Border numbers;
                numbers.Background(added ? _diff.addNumberBand : _diff.removeNumberBand);
                numbers.IsHitTestVisible(false);
                Grid::SetRow(numbers, top);
                Grid::SetRowSpan(numbers, span);
                grid.Children().Append(numbers);
            }
            first = end;
        }
        if (fromFileNow && count > 0)
        {
            // The yellow edge of lines read from the file as it is now (they may be newer than the turn): every row.
            Border edge;
            edge.Width(3);
            edge.HorizontalAlignment(HorizontalAlignment::Left);
            edge.Background(_diff.now);
            edge.IsHitTestVisible(false);
            Grid::SetRowSpan(edge, count);
            grid.Children().Append(edge);
        }
        // Each line: its numbers cell and its code cell, both one pitch per visual line (BlockLineHeight) and top-aligned, so
        // the numbers sit on the code's first line whatever font fallback a character needs.
        for (int32_t line = 0; line < count; ++line)
        {
            const auto& row = rows[static_cast<size_t>(line)];
            TextBlock numbers;
            numbers.Text(winrt::hstring{ DiffNumbersText(row, digits) });
            numbers.FontFamily(_monoFont);
            numbers.FontSize(kDiffFontSize);
            numbers.Foreground(_diff.number);
            numbers.TextWrapping(TextWrapping::NoWrap);
            numbers.LineHeight(kDiffLineHeight);
            numbers.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
            numbers.VerticalAlignment(VerticalAlignment::Top);
            numbers.Padding(Thickness{ kDiffNumbersPadLeft, 0, kDiffNumbersPadRight, 0 });
            Grid::SetRow(numbers, line);
            grid.Children().Append(numbers);
            // The code: selectable with the shared text menu (a line copies as code, without its numbers or mark), WRAPPING
            // at the card's edge (the text block's own Wrap: word boundaries first, a long token broken when it must). At
            // least a line high, so an empty line keeps its row.
            auto code = _MakeTextBlock(kDiffFontSize);
            code.Text(winrt::hstring{ row.kind == L'\\' ? L"\\" + DiffDisplayLine(row.text) : DiffDisplayLine(row.text) });
            code.FontFamily(_monoFont);
            code.Foreground(row.kind == L'\\' ? _diff.meta : _diff.code);
            code.LineHeight(kDiffLineHeight);
            code.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
            code.MinHeight(kDiffLineHeight);
            code.VerticalAlignment(VerticalAlignment::Top);
            code.Padding(Thickness{ 4, 0, 12, 0 });
            Grid::SetRow(code, line);
            Grid::SetColumn(code, 1);
            grid.Children().Append(code);
        }
        return grid;
    }

    Button AgentTranscriptOverlay::_MakeDiffExpandRow(const std::shared_ptr<DiffCard>& card, size_t row)
    {
        const size_t count = card->hunks.size();
        const bool below = row >= count;
        const bool first = row == 0 && !below;
        // Before the file is read, the hunks' numbers say how many lines lie above (a gap or the file's start); below the
        // last hunk nothing is known yet.
        const std::wstring words = below ? DiffExpandLabel(true, false, -1) : DiffExpandLabel(false, first, DiffLinesAboveHunk(card->hunks, row));
        FontIcon arrow;
        arrow.FontFamily(FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        arrow.Glyph(below ? kDiffExpandDownGlyph : first ? kDiffExpandUpGlyph : kDiffExpandBothGlyph);
        arrow.FontSize(10);
        arrow.Foreground(_diff.expandText);
        arrow.HorizontalAlignment(HorizontalAlignment::Center);
        arrow.VerticalAlignment(VerticalAlignment::Center);
        arrow.IsHitTestVisible(false); // the button must be the only target (the badge's mkIconBtn lesson)
        Grid::SetColumn(arrow, 0);
        TextBlock label;
        label.Text(winrt::hstring{ words });
        label.FontSize(12);
        label.Foreground(_diff.expandText);
        label.TextWrapping(TextWrapping::NoWrap);
        label.VerticalAlignment(VerticalAlignment::Center);
        label.IsHitTestVisible(false);
        Grid::SetColumn(label, 1);
        Grid content;
        ColumnDefinition arrowColumn; // the numbers' width: the arrow sits in the numbers column, the words where code starts
        arrowColumn.Width(GridLength{ DiffNumbersColumnWidth(card->numberDigits), GridUnitType::Pixel });
        ColumnDefinition labelColumn;
        labelColumn.Width(GridLength{ 1, GridUnitType::Star });
        content.ColumnDefinitions().Append(arrowColumn);
        content.ColumnDefinitions().Append(labelColumn);
        content.Children().Append(arrow);
        content.Children().Append(label);
        Button button;
        button.Background(_diff.expandBand);
        button.BorderThickness(Thickness{ 0, 0, 0, 0 });
        button.CornerRadius(CornerRadius{ 0, 0, 0, 0 }); // square: a rounded template is a composition node per row
        button.Padding(Thickness{ 0, 3, 0, 3 });
        button.MinWidth(0);
        button.MinHeight(0);
        button.HorizontalAlignment(HorizontalAlignment::Stretch);
        button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        button.Content(content);
        AgentSetTip(button, winrt::hstring{ kDiffExpandTip });
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(button, winrt::hstring{ words });
        // The row by its INDEX and the card weakly (the card owns the button: a strong reference would be a cycle).
        button.Click([weak = get_weak(), weakCard = std::weak_ptr<DiffCard>{ card }, row](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            try
            {
                const auto self = weak.get();
                const auto target = weakCard.lock();
                if (self && target)
                {
                    self->_ExpandDiffCard(target, row);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff expand row"); // the card stays as it was
            }
        });
        if (row < card->expandRows.size())
        {
            card->expandRows[row] = button;
            card->expandLabels[row] = label;
        }
        return button;
    }

    void AgentTranscriptOverlay::_SetDiffCardOpen(const std::shared_ptr<DiffCard>& card, bool open)
    {
        if (!card)
        {
            return;
        }
        if (open && !card->bodyBuilt)
        {
            _BuildDiffCardBody(card); // a long card's first open: its body is made now
        }
        card->open = open;
        if (card->body)
        {
            card->body.Visibility(open ? Visibility::Visible : Visibility::Collapsed);
        }
        if (card->chevron)
        {
            card->chevron.Glyph(open ? kFoldOpenGlyph : kFoldClosedGlyph);
        }
    }

    void AgentTranscriptOverlay::_SetDiffCardViewed(const std::shared_ptr<DiffCard>& card, bool viewed)
    {
        if (!card)
        {
            return;
        }
        // GitHub's rule: a file marked viewed folds away; unmarked, it opens again.
        _SetDiffCardOpen(card, !viewed);
        // Its outline entry dims while it is viewed (only the Diff view has cards).
        const auto& view = _levels[static_cast<size_t>(::Agentmaster::TranscriptView::Diff)];
        for (const auto& entry : view.outlineFiles)
        {
            if (entry.first == card->block && entry.second)
            {
                entry.second.Opacity(viewed ? 0.45 : 1.0);
                break;
            }
        }
    }

    void AgentTranscriptOverlay::_ExpandDiffCard(const std::shared_ptr<DiffCard>& card, size_t row) noexcept
    {
        try
        {
            if (!_open || !card || card->filePath.empty() || !card->numbered || row > card->hunks.size())
            {
                return;
            }
            if (const auto known = _diffFiles.find(card->filePath); known != _diffFiles.end())
            {
                _ApplyDiffExpansion(*card, row, known->second);
                return;
            }
            // Not read yet: this click waits for the read - one read per file, however many clicks queue behind it.
            const bool reading = std::any_of(_diffExpandWaiting.begin(), _diffExpandWaiting.end(), [&card](const auto& waiting) {
                return std::get<0>(waiting) == card->filePath;
            });
            _diffExpandWaiting.emplace_back(card->filePath, std::weak_ptr<DiffCard>{ card }, row);
            if (row < card->expandLabels.size() && card->expandLabels[row])
            {
                card->expandLabels[row].Text(L"Reading the file\x2026");
            }
            if (!reading)
            {
                _LoadDiffFileAsync(get_weak(), _dispatcher, card->filePath, _generation);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the card stays as it was; another click tries again.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff expand");
        }
    }

    void AgentTranscriptOverlay::_ApplyDiffExpansion(DiffCard& card, size_t row, const std::shared_ptr<const std::vector<std::wstring>>& fileLines)
    {
        const size_t count = card.hunks.size();
        if (count == 0 || row > count || !card.bodyBuilt || card.located.size() != count)
        {
            return;
        }
        if (!fileLines)
        {
            _SyncDiffExpandRows(card, nullptr, true); // the file cannot be read: every row says so
            return;
        }
        const auto& lines = *fileLines;
        // Every hunk's place in the file now, once: a gap's count needs the hunk above it too.
        for (size_t h = 0; h < count; ++h)
        {
            if (card.located[h] == -2)
            {
                card.located[h] = ::Agentmaster::LocateHunkNewSide(lines, card.hunks[h]);
            }
        }
        const size_t h = row < count ? row : count - 1;
        const auto& hunk = card.hunks[h];
        const int at = card.located[h];
        if (at >= 0 && card.hunkPanels[h])
        {
            // A file the change CREATED had no old side: its expanded lines get no old numbers.
            const bool oldSide = DiffSideCount(hunk, L'-') > 0;
            std::vector<::Agentmaster::DiffRow> rows;
            if (row < count)
            {
                const int remaining = DiffRemainingAbove(card.hunks, card.located, card.revealedAbove, h);
                if (remaining > 0)
                {
                    // A short gap between two hunks opens whole (GitHub's way); otherwise one step, read upwards.
                    const int take = DiffExpandTake(remaining, h > 0);
                    const int end = at - card.revealedAbove[h]; // one past the last line this step shows
                    for (int j = end - take; j < end; ++j)
                    {
                        ::Agentmaster::DiffRow line;
                        line.kind = L' ';
                        line.text = lines[static_cast<size_t>(j)];
                        line.newNo = DiffNewFirst(hunk) - (at - j);
                        line.oldNo = oldSide ? DiffOldFirst(hunk) - (at - j) : 0;
                        rows.push_back(std::move(line));
                    }
                    // Under the hunk's "@@" row, above the lines shown before: the file is read upwards.
                    card.hunkPanels[h].Children().InsertAt(card.numbered ? 1u : 0u, _MakeDiffLines(rows, card.numberDigits, true));
                    card.revealedAbove[h] += take;
                }
            }
            else
            {
                const int end = at + DiffSideCount(hunk, L'+'); // the file line right after the hunk
                const int remaining = DiffRemainingBelow(lines.size(), at, hunk, card.revealedBelow[h]);
                if (remaining > 0)
                {
                    const int take = DiffExpandTake(remaining, false);
                    const int first = end + card.revealedBelow[h];
                    for (int j = first; j < first + take; ++j)
                    {
                        ::Agentmaster::DiffRow line;
                        line.kind = L' ';
                        line.text = lines[static_cast<size_t>(j)];
                        line.newNo = DiffNewFirst(hunk) + DiffSideCount(hunk, L'+') + (j - end);
                        line.oldNo = oldSide ? DiffOldFirst(hunk) + DiffSideCount(hunk, L'-') + (j - end) : 0;
                        rows.push_back(std::move(line));
                    }
                    card.hunkPanels[h].Children().Append(_MakeDiffLines(rows, card.numberDigits, true));
                    card.revealedBelow[h] += take;
                }
            }
            if (!rows.empty() && !card.note && card.body)
            {
                // The legend, once per card: what the yellow edge means.
                card.note = TextBlock{};
                card.note.Text(kDiffNowNote);
                card.note.FontSize(11.5);
                card.note.Foreground(_diff.meta);
                card.note.TextWrapping(TextWrapping::Wrap);
                card.note.Margin(Thickness{ 10, 4, 10, 6 });
                card.body.Children().Append(card.note);
            }
        }
        _SyncDiffExpandRows(card, &lines, false);
    }

    void AgentTranscriptOverlay::_SyncDiffExpandRows(DiffCard& card, const std::vector<std::wstring>* fileLines, bool readFailed)
    {
        const size_t count = card.hunks.size();
        if (count == 0 || card.located.size() != count)
        {
            return;
        }
        if (fileLines)
        {
            for (size_t h = 0; h < count; ++h)
            {
                if (card.located[h] == -2)
                {
                    card.located[h] = ::Agentmaster::LocateHunkNewSide(*fileLines, card.hunks[h]);
                }
            }
        }
        for (size_t row = 0; row <= count && row < card.expandRows.size() && row < card.expandLabels.size(); ++row)
        {
            const auto& button = card.expandRows[row];
            const auto& label = card.expandLabels[row];
            if (!button || !label)
            {
                continue; // no row there (a hunk at the file's start)
            }
            const bool below = row >= count;
            const size_t h = below ? count - 1 : row;
            int remaining = -1; // -1: not known yet
            std::wstring why; // set: the row can show nothing, and says why
            if (readFailed)
            {
                why = L"The file cannot be read now - it was moved, deleted or is locked";
            }
            else if (fileLines)
            {
                if (card.located[h] < 0)
                {
                    why = L"These lines are no longer in the file - it changed after this turn";
                }
                else if (below)
                {
                    remaining = DiffRemainingBelow(fileLines->size(), card.located[h], card.hunks[h], card.revealedBelow[h]);
                }
                else
                {
                    remaining = DiffRemainingAbove(card.hunks, card.located, card.revealedAbove, h);
                }
            }
            else if (!below)
            {
                remaining = std::max(DiffLinesAboveHunk(card.hunks, h) - card.revealedAbove[h], 0);
            }
            if (why.empty() && remaining == 0)
            {
                // Nothing (more) to show: the row goes, and the lines on both sides of it meet.
                if (button.Visibility() != Visibility::Collapsed)
                {
                    button.Visibility(Visibility::Collapsed);
                }
                continue;
            }
            const std::wstring words = why.empty() ? DiffExpandLabel(below, row == 0, remaining) : why;
            label.Text(winrt::hstring{ words });
            label.Opacity(why.empty() ? 1.0 : 0.6);
            button.IsEnabled(why.empty());
            winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(button, winrt::hstring{ words });
        }
    }

    winrt::fire_and_forget AgentTranscriptOverlay::_LoadDiffFileAsync(winrt::weak_ref<AgentTranscriptOverlay> weak,
                                                                       DispatcherQueue dispatcher,
                                                                       std::wstring path,
                                                                       uint64_t generation)
    {
        // Every parameter BY VALUE (the coroutine outlives its caller) and only a WEAK overlay off the UI thread - the
        // last reference must never drop on a pool thread (CLAUDE.md "A fire_and_forget coroutine can drop the LAST page
        // reference").
        try
        {
            co_await winrt::resume_background(); // a file of up to 32 MB: never on the UI thread
            std::shared_ptr<const std::vector<std::wstring>> lines;
            try
            {
                lines = ReadDiffFileLines(path);
            }
            catch (...)
            {
                // Rule #18. Recovery: null - the card says the file cannot be read.
                ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay read diff file");
            }
            const bool queued = dispatcher && dispatcher.TryEnqueue([weak, path, lines, generation]() {
                if (const auto self = weak.get())
                {
                    self->_OnDiffFileLoaded(path, lines, generation); // noexcept: contains its own failures
                }
            });
            if (!queued)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[transcript] diff expand: the window closed before the file was read path=" + path + L"\n");
            }
        }
        catch (...)
        {
            // Rule #18 - and the terminate-net every fire_and_forget needs (an escape IS winrt::terminate()).
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_LoadDiffFileAsync");
        }
    }

    void AgentTranscriptOverlay::_OnDiffFileLoaded(const std::wstring& path, std::shared_ptr<const std::vector<std::wstring>> lines, uint64_t generation) noexcept
    {
        try
        {
            if (generation != _generation || !_open)
            {
                return; // the overlay was retargeted, refreshed or closed since: the cards it was read for are gone
            }
            _diffFiles[path] = lines;
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[transcript] " + ::Agentmaster::ShortId(_source.sessionId) + L" diff expand read " +
                                                            (lines ? std::to_wstring(lines->size()) + L" lines" : std::wstring{ L"nothing (the file cannot be read)" }) +
                                                            L" path=" + path + L"\n");
            // The expansions that waited for this file, in click order; the others keep waiting for theirs.
            std::vector<std::tuple<std::wstring, std::weak_ptr<DiffCard>, size_t>> ready;
            for (auto it = _diffExpandWaiting.begin(); it != _diffExpandWaiting.end();)
            {
                if (std::get<0>(*it) == path)
                {
                    ready.push_back(std::move(*it));
                    it = _diffExpandWaiting.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            for (const auto& item : ready)
            {
                const auto card = std::get<1>(item).lock();
                if (!card)
                {
                    continue; // its view was released since
                }
                try
                {
                    _ApplyDiffExpansion(*card, std::get<2>(item), lines);
                }
                catch (...)
                {
                    // Rule #18. Recovery: that card stays as it was; the next waiting expansion still runs.
                    ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff expansion");
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the waiting cards keep their rows; another click reads again.
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay diff file loaded");
        }
    }

    bool AgentTranscriptOverlay::_IsUnder(const DependencyObject& element, const DependencyObject& ancestor) noexcept
    {
        if (!element || !ancestor)
        {
            return false;
        }
        try
        {
            DependencyObject current = element;
            // A focused Hyperlink is a TextElement, not a visual: VisualTreeHelper can't walk it, so start
            // from the text control that hosts it.
            if (const auto textElement = current.try_as<TextElement>())
            {
                current = textElement.ElementStart().VisualParent();
            }
            for (int guard = 0; current && guard < 1024; ++guard)
            {
                if (current == ancestor)
                {
                    return true;
                }
                current = VisualTreeHelper::GetParent(current);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentTranscriptOverlay::_IsUnder");
        }
        return false;
    }
}
