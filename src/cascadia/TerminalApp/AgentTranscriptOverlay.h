// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the TRANSCRIPT OVERLAY: a session's conversation, rendered as Markdown, in a reading
// view laid over the whole window. Opened by "Show Transcript" (directly below "Copy ▸" in the WT tab
// menu and the Agent Manager's board / tree / side-panel session menu, below "Copy Session Id" in the
// EXTERNAL tree menu, in the summary panel's right-click menu) and by "Copy ▸ Transcript ▸ Show" (the
// first item of every Transcript submenu, the per-tab badge's included). doc/agentmaster/
// TRANSCRIPT_OVERLAY.md is the full description (entries, rendering rules, closing, logs, limits).
//
// Shape: a card 90% of the window wide and 80% tall, centered over a dimmed backdrop that covers the
// page (the terminal, the Agent Manager page, or the Sessions page — "wherever you are"). The card has a
// header (title · Wrap · Copy · Refresh · Close), one TAB per transcript detail level in the Copy menu's
// order (Default · Max · Full · Compact · Minimal), a separator, the three BOX TABS (Custom · User Only ·
// Assistant Only — below), a second separator, the DIFF tab (below), the (i) level guide, an "Expand all" switch for
// the level's folded tool calls,
// the box tabs' row of check boxes (shown only on those tabs), and a scrolling body with the two JUMP
// buttons in its bottom-right corner (below). It opens at the tab the user LAST PICKED, in any window: every tab pick
// is remembered in settings.json (AppSettings::transcriptOverlayLevel, a freshest-disk read-modify-write
// done here) and every open reads it back fresh — Default until a first pick.
//
// THE BOX TABS (TRANSCRIPT_OVERLAY.md §3j; the rules in AgentMaster/TranscriptViews.h): Custom shows both sides of
// each turn, User Only the user's own messages (no final reply, no teammate's message; a turn with nothing of
// theirs is skipped), Assistant Only Claude's side (no message that reached a turn) — each starting from Minimal.
// Above the body a row of TWELVE BOXES, one per feature the higher levels add, grouped by the level they come from
// (Compact · Default · Full · Max), adds any of them to the tab on screen; every box ticked on Custom is exactly Max.
// A box that would change nothing is greyed with its tick kept (the Full / Max call details while the full call
// lines are off; the one-line call reasons while they are on). Each tab keeps its OWN boxes, remembered in
// settings.json for every window (AppSettings::transcriptOverlay<Tab>Boxes, the same freshest-disk RMW per click).
// A click re-renders that tab off the UI thread and keeps the reader on the same turn: the turn whose heading was at
// the top of the view is scrolled back to the top the moment the new render builds it (_ToggleBox → _Pump).
//
// THE DIFF TAB (TRANSCRIPT_OVERLAY.md §3l; after a second separator, key 9): the conversation with the code it changed.
// Its text is TranscriptTurns.h RenderTranscriptDiffFile's Markdown - every turn as Minimal draws it, one "`$` reason ×N"
// row per different Bash reason, one ```diff block per file the turn changed (its NET change, TranscriptDiff.h) - and the
// build draws that text differently: the Bash rows as chips with a count pill, each diff block as a GitHub-style card
// (_BuildDiffCard: a header bar with the file, +a -d, a Viewed box; numbered rows on red / green bands; an expand row
// above each hunk and below the last that reveals more lines read from the file AS IT IS NOW, marked with a yellow edge).
// Diff code WRAPS like every other line (the user's "word wrap without horizontal scrollbar"): each line is a grid row, its
// numbers cell beside a wrapping code cell, so a long line continues under its own code; no card scrolls sideways (with
// the ↵ toggle off, long lines are cut at the right edge like every other line). Left of the page an OUTLINE lists the
// turns that changed files with their files; a click jumps there, and the entry of the turn on top of the page is lit as
// it scrolls.
//
// THE JUMP BUTTONS: two small round buttons stacked in the body's bottom-right corner — ↑ (to the top) shows while
// the view is scrolled down from the top, ↓ (to the bottom) while there is more below; neither shows on a view too
// short to scroll. They show and hide through Opacity + IsHitTestVisible + IsTabStop, never Visibility, so the body
// never re-lays out for them. A ↓ while the level is still being laid out FOLLOWS the end until the build is done
// (a wheel, a press or an upward key in the view cancels that), so it lands on the real last line.
//
// LINE WRAP: the header's ↵ toggle (the first of its buttons, ON by default) decides what a line wider
// than the body does. ON, it wraps at the body's right edge. OFF, it keeps its length and the part past
// the right edge is HIDDEN — not a sideways scroll away; real line breaks still break, and Copy still
// copies every character. The mechanism is one property per level view (ApplyWrapMode in the .cpp): the
// scroll view either hands its content the viewport's width or measures it at infinite width and clips
// it. Like the level it is GLOBAL: remembered in settings.json (AppSettings::transcriptOverlayWrapLines,
// the same freshest-disk read-modify-write) and read back on every open, in any window. A switch keeps the
// reader on the same passage (_SyncWrapMode), and a level that was hidden at the switch catches up — with
// its own passage kept — when it is shown again.
//
// FOLDED TOOL CALLS: at Default, Full and Max, in every turn the list of tool calls and remarks under
// "**● Claude**" (and under an agent entry's header) is folded into ONE summary row ("▸ 4 tool calls:
// Bash ×3, Write · 2 remarks", AgentMaster/MarkdownBlocks.h MdFindToolAreas); the reply under it stays
// open, so those views read like Compact with every call one click away. A click on the row opens /
// closes that list; "Expand all" opens every list of the level (and the lists built after it). A list
// with no call or remark (answers, compactions, an agent's messages) stays open, and a "(no tool calls)"
// list becomes a one-line note. Compact and Minimal never fold: Compact's lists ARE its point - each
// call's one-line reason (transcript_turns v10) - so the fold areas are found only from Default up.
//
// SESSION CHAIN LINKS: a transcript that continues earlier sessions (a fork, a plan restart) opens with
// the "Session chain" list — "session 1 · <id> — started …", "session 2 · <id> — forked from session 1 …"
// — and each session's part starts with a heading "Session N of M · …". Every "session N" in that list
// is a hyperlink: a click scrolls session N's heading to the top of the view (_JumpToSession). The build
// is time-sliced, so a click on a session whose heading is not built yet is remembered and done the
// moment the heading exists. Only a session with a heading gets a link (a one-session chain has none).
// The list, the headings and the mentions come from AgentMaster/MarkdownBlocks.h (MdFindSessionChain /
// MdFindSessionRefs / MdSessionHeadingNumber), found off the UI thread with the parse.
//
// THE LEVEL GUIDE: an (i) button right after the level tabs opens a panel (a Flyout) comparing every level
// side by side - a row per thing a transcript can show, a check mark under each level that shows it, the
// current level's column highlighted - plus what every level shows and what no level shows. Each level
// tab's tooltip is that level's cheat sheet: what it adds over the level below and what it still leaves
// out. Both are drawn from ONE table, AgentMaster/TranscriptLevelGuide.h, whose rows the m5 harness checks
// against TurnsOptionsForLevel - the guide can never disagree with what a level renders. Esc closes an
// open guide first (DismissLevelGuide), the overlay on the next Esc.
//
// A FORK WITH NO MESSAGES YET: a fork has no transcript of its own until its first message, and until
// then its conversation is the one it was forked from. The overlay shows that source's transcript, under
// a dim one-line note naming the source (ClaudeSpawn.h ResolveClaudeDisplayTranscript /
// ForkSourceTranscriptNote — the SAME text Copy ▸ Transcript copies for it), instead of "nothing to show".
//
// How it renders (the expensive part, all of it off the UI thread except the XAML itself):
//   1. a pool thread renders the level — TranscriptTurns for a Claude transcript (the very text
//      "Copy ▸ Transcript ▸ <level>" copies), the plain conversation readers for a Codex rollout —
//      and parses it into flat render blocks (AgentMaster/MarkdownBlocks.h, cmark);
//   2. the UI thread turns the blocks into XAML a TIME SLICE at a time (low-priority dispatcher items
//      of ~12 ms each), so the top of a long transcript shows at once and the window keeps answering
//      input while the rest lays out. Each level keeps its own scroll view, so switching tabs back is
//      instant and keeps that level's scroll position.
// Every text block is selectable (right-click → Copy / Select All, the WT TextMenuFlyout — never the
// per-thread default text flyout, which GH#18599 makes unsafe with several windows on one thread).
// Selection does not span blocks; the header's Copy button copies the whole level's Markdown.
//
// It closes on: a click on the backdrop (outside the card), Esc, the ✕ button, and — driven by the
// page — any tab switch, tab move or tab drag (TerminalPage::_CloseTranscriptOverlay). While open it
// keeps keyboard focus inside the card (a programmatic focus grab by the page behind it — the Manager's
// refresh re-focusing a card, a terminal — is refused), so nothing typed reaches a session hidden
// under it; Esc is also caught page-wide (TerminalPage::_ManagerPaneNavPreviewKeyDown) for the same
// reason — an Esc that reached claude would interrupt its turn.
//
// Ownership: one instance per window, created lazily by the page (TerminalPage::_EnsureTranscriptOverlay)
// and parented into the page's Root grid; ref-counted (winrt::implements) so the background render can
// hold a weak reference and drop a result that lands after a retarget, a close or the window's death.
// UI thread only, except _LoadLevelAsync's background half (which touches no member).

#pragma once

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.Terminal.UI.h> // TextMenuFlyout — the multi-window-safe text context menu

#include "AgentMaster/MarkdownBlocks.h" // MdBlock — the render blocks a level is built from
#include "AgentMaster/TranscriptDiff.h" // DiffHunk — the Diff tab's cards (TRANSCRIPT_OVERLAY.md §3l)
#include "AgentMaster/TranscriptViews.h" // kTranscriptViewCount / kTranscriptViewBoxes / TurnsOptions — the tabs, the boxes, a view's render options

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace winrt::TerminalApp::implementation
{
    // WHICH conversation the overlay shows and how to find its transcript. Built by the page from the
    // registry (a managed session) or by the Manager from an EXTERNAL row.
    struct TranscriptOverlaySource
    {
        // The session's id: a Claude conversation id (its transcript is <id>.jsonl, found by
        // ResolveClaudeTranscriptPath), or — for Codex — the managed handle / rollout id, used only for
        // the header and the logs. Retargeting compares it, so it must identify the conversation.
        std::wstring sessionId;
        // A Codex rollout, rendered through the plain conversation readers (Codex has no turns view):
        // Minimal / Compact show the per-turn brief, Default / Full / Max the whole conversation —
        // exactly what the Copy menu's levels copy for a Codex session.
        bool codex = false;
        // Codex only: the rollout uuid (SessionInfo::codexSessionId, or an external row's id). Empty
        // for a codex that was never prompted — the overlay then says there is nothing to show yet.
        std::wstring codexRolloutId;
        // The display title for the header (SessionInfo::title or the external row's title). May be
        // empty — the header then shows the short id alone.
        std::wstring title;
        // Claude only: the session's fork source (SessionInfo::forkParentId, read from the registry when
        // the overlay is opened; empty for any other session, an external row and Codex). A fork that
        // has no messages of its own has no transcript yet, so the overlay renders this source's
        // transcript under a note saying so (ResolveClaudeDisplayTranscript / ForkSourceTranscriptNote)
        // instead of "nothing to show". The session's own transcript, once it exists, always wins.
        std::wstring forkParentId;
    };

    class AgentTranscriptOverlay : public winrt::implements<AgentTranscriptOverlay, winrt::Windows::Foundation::IInspectable>
    {
    public:
        // Show's `level` for "open at the tab the user last picked" — a level or a box tab
        // (AppSettings::transcriptOverlayLevel, read fresh from settings.json at that moment) — what every menu passes.
        static constexpr int kRememberedLevel = -1;

        AgentTranscriptOverlay();
        // Drops the XAML references only — never calls a UI-affine method, because a page torn down
        // off the UI thread (the fire_and_forget last-ref gotcha) destroys its members wherever it
        // dies. The host stays in the page's Root until the page itself goes.
        ~AgentTranscriptOverlay();

        // Build the overlay's XAML (collapsed) and parent it into the page's Root grid, spanning every
        // row and drawn above Root's other children (Canvas.ZIndex). Idempotent: a second call is a
        // no-op. UI thread.
        // Parameters:
        //   pageRoot — TerminalPage::Root(). Kept (strong ref) as the boundary of the focus guard: a
        //              focus move to an element under it but outside the overlay is refused while open.
        // Throws winrt::hresult_error when the XAML can't be created or parented (the page's caller
        // contains it — the overlay is then simply unavailable).
        void Attach(const winrt::Windows::UI::Xaml::Controls::Grid& pageRoot);

        // Open the overlay on `source` at `level`, or — when it is already open on the same
        // conversation — just switch to `level` (the levels already rendered stay). A DIFFERENT
        // conversation discards every rendered level and starts over, and reads the box tabs' remembered
        // boxes fresh (an overlay already open keeps its own). Takes keyboard focus (one
        // dispatcher tick later, once the view is laid out). UI thread. No-op before Attach.
        // Parameters:
        //   source — the conversation (see TranscriptOverlaySource).
        //   level  — a VIEW index 0..8 (TranscriptViews.h TranscriptView: a TurnsLevel value 0..4 for
        //            Minimal..Max, then 5 Custom, 6 User Only, 7 Assistant Only, 8 Diff; above 8 is clamped), or
        //            kRememberedLevel (any negative value) for the tab last picked in any window — read from
        //            settings.json right now, Default when nothing was picked yet or the stored name is
        //            unknown. The menus pass kRememberedLevel. Opening does NOT itself remember anything:
        //            only a tab pick does.
        // Never throws: a XAML failure is logged (Rule #18) and the overlay stays closed.
        void Show(const TranscriptOverlaySource& source, int level) noexcept;

        // Close the overlay. No-op when it is closed. Cancels every render / layout still running for
        // it (a result that lands later is dropped) and releases the rendered XAML on a later
        // low-priority tick, so a big transcript's teardown never delays whatever closed it.
        // Parameters:
        //   reason       — why, for the [nav] transcript-close line ("esc", "tab switch", ...).
        //   restoreFocus — hand keyboard focus back to the page (the closed handler) — true when the
        //                  overlay closed ITSELF (Esc, backdrop, ✕); false when the page closed it in
        //                  the middle of its own focus work (a tab switch selects and focuses a tab).
        // Never throws.
        void Close(std::wstring_view reason, bool restoreFocus) noexcept;

        // Whether the overlay is open (shown). The page reads it to route Esc and to skip closes.
        bool IsOpen() const noexcept
        {
            return _open;
        }

        // The page's "give focus back" callback, run after a self-initiated close (see Close). The
        // page wires TerminalPage::_FocusCurrentTab — the active terminal, or the Agent Manager page.
        void SetClosedHandler(std::function<void()> handler);

        // Esc closes the TOPMOST thing first: when the level guide (the (i) panel after the tabs) is open,
        // close it and report that the key was used, so the caller does not also close the overlay. Both Esc
        // paths call it first - the card's own KeyDown and the page's tunneling handler
        // (TerminalPage::_DismissTranscriptLevelGuide), since which of them sees a key pressed while the
        // guide is up depends on where keyboard focus sits (inside the guide's popup, or still on the (i)).
        // Returns true when a guide was open and is now closing; false when none was open (the caller then
        // closes the overlay as before). UI thread. Never throws: a failed hide is logged (Rule #18) and
        // reported as "nothing was open", so the Esc still closes the overlay - which hides the guide too.
        bool DismissLevelGuide() noexcept;

    private:
        // The lifecycle of one level's view. Only the CURRENT level's view builds; a level left
        // mid-build pauses and resumes where it stopped when its tab is selected again.
        enum class LevelState : uint8_t
        {
            Idle, // never requested (no view yet, or released)
            Loading, // the background render is running
            Building, // blocks are being turned into XAML, a time slice per dispatcher tick
            Done, // fully laid out
            Empty, // nothing to show (no transcript yet / unreadable / an empty level) — `message` says which
        };

        // One folded tool area as built: the panel holding the area's blocks (Collapsed while folded) and
        // the summary row's chevron (▸ folded / ▾ open). Kept per level so "Expand all" can flip them all.
        struct FoldParts
        {
            winrt::Windows::UI::Xaml::Controls::StackPanel panel{ nullptr };
            winrt::Windows::UI::Xaml::Controls::FontIcon chevron{ nullptr };
        };

        // The Diff tab's OUTLINE (TRANSCRIPT_OVERLAY.md §3l): every turn that changed files, with its files - computed off the
        // UI thread from the parsed blocks (_RenderLevel), drawn in the column left of the page when the render lands.
        struct DiffOutlineFile
        {
            std::wstring path; // as the card names it
            int added = 0;
            int removed = 0;
            size_t block = 0; // the card's block index (the jump target, LevelView::diffCards)
        };
        struct DiffOutlineTurn
        {
            int number = 0; // the turn's number in the whole conversation (0 = the work before the first prompt)
            std::wstring title; // the first line of the user's message that started it, cut to fit
            std::vector<DiffOutlineFile> files;
        };

        // One GitHub-style DIFF CARD of the Diff tab (_BuildDiffCard): what it shows and the parts its clicks change. Owned by
        // its view (LevelView::diffCards); every handler on the card reaches it through a weak_ptr, so no delegate keeps the
        // card - or its XAML - alive (the CLAUDE.md "handler that captures its own element" gotcha).
        struct DiffCard
        {
            size_t block = 0; // its block index in the view (the outline's jump target)
            std::wstring filePath; // the file on disk ("+++" side) Expand reads; "" when there is none (nothing to expand from)
            std::vector<::Agentmaster::DiffHunk> hunks; // the parsed hunks (numbered; an unnumbered card has no Expand)
            bool numbered = true; // false: the change carried no line numbers (an older Claude Code): no Expand
            bool truncated = false; // the block was cut at the view's block-size cap: its last hunk is partial (a note says so)
            bool bodyBuilt = false; // the body exists (a big card builds it on its first open)
            bool open = true; // the body shows
            int numberDigits = 3; // the digits every line number is padded to (DiffNumberDigits of the card's biggest number, expansions included): the numbers column's width
            winrt::Windows::UI::Xaml::FrameworkElement root{ nullptr }; // the whole card (what the outline scrolls to)
            winrt::Windows::UI::Xaml::Controls::StackPanel body{ nullptr }; // the hunks' panels, expand rows and notes
            winrt::Windows::UI::Xaml::Controls::FontIcon chevron{ nullptr }; // ▾ open / ▸ closed
            winrt::Windows::UI::Xaml::Controls::CheckBox viewed{ nullptr }; // GitHub's "Viewed": ticked closes the card
            // Per hunk: the panel holding its header row and its lines - context revealed from the file goes in at its top
            // (above the header row) or its bottom - and how many lines above / below it were already revealed.
            std::vector<winrt::Windows::UI::Xaml::Controls::StackPanel> hunkPanels;
            std::vector<int> revealedAbove;
            std::vector<int> revealedBelow;
            // The expand rows and their labels: index h is the row ABOVE hunk h (the gap from the previous hunk, or the file's
            // start); index hunks.size() is the row below the last hunk. A row that has nothing (more) to show is collapsed;
            // a null entry was never built (an unnumbered card, no file).
            std::vector<winrt::Windows::UI::Xaml::Controls::Button> expandRows;
            std::vector<winrt::Windows::UI::Xaml::Controls::TextBlock> expandLabels;
            // Where each hunk's new side sits in the file as it is now (LocateHunkNewSide): -2 not looked up yet, -1 not
            // there (the file changed since), else the 0-based line.
            std::vector<int> located;
            // The card's legend under the hunks ("yellow edge: from the file now", "the file cannot be read"), made on the
            // first expansion that needs it.
            winrt::Windows::UI::Xaml::Controls::TextBlock note{ nullptr };
        };

        // One tab's state + view: a detail level's, or a box tab's. Indexed by VIEW (TranscriptViews.h TranscriptView: the
        // TurnsLevel value 0 Minimal .. 4 Max, then 5 Custom, 6 User Only, 7 Assistant Only, then 8 Diff).
        struct LevelView
        {
            LevelState state = LevelState::Idle;
            // The rendered Markdown — what the header's Copy button copies (the WHOLE text, even past
            // the view's caps). Empty until the render lands.
            std::wstring markdown;
            // Blocks not yet turned into XAML; freed once the level is Done.
            std::vector<::Agentmaster::MdBlock> blocks;
            // How many of `blocks` are already built (the resume point of a paused build).
            size_t built = 0;
            // The parse stopped at the block cap / the input was cut at the character cap: a closing
            // note tells the reader the view ends early (Copy still has everything).
            bool truncated = false;
            // The parse failed part-way (already logged): a closing note says the view is incomplete.
            bool failed = false;
            // Lines in `markdown`, for the status line.
            size_t lines = 0;
            // How long the background render + parse took, for the status line and the log.
            int64_t renderMs = 0;
            // The text the body shows instead of content (Loading / Empty), or empty.
            std::wstring message;
            // This level's scroll view (one per level — it keeps its own scroll position).
            winrt::Windows::UI::Xaml::Controls::ScrollViewer scroll{ nullptr };
            // The scroll content: a vertical stack of CHUNK panels (see _AppendElement).
            winrt::Windows::UI::Xaml::Controls::StackPanel content{ nullptr };
            // The chunk currently being filled, and how many elements it holds. Every chunk comes from
            // MakeChunkPanel: a StackPanel that is also one UI Automation node, so no UIA child list ever holds
            // more than one chunk's elements (a flat list made a UIA client's search quadratic - UI_THREAD.md §16).
            winrt::Windows::UI::Xaml::Controls::StackPanel chunk{ nullptr };
            size_t chunkFill = 0;
            // The placeholder TextBlock showing `message` while Loading / Empty (removed when content
            // starts building).
            winrt::Windows::UI::Xaml::Controls::TextBlock messageText{ nullptr };
            // The bar color for top-level quotes, from the last speaker label built ("**❯ User**" →
            // blue, "**● Claude**" → orange, "**✉ name**" → purple); null == the neutral bar. Reset by
            // a heading (a new turn). Carried across time slices because the build is sequential.
            winrt::Windows::UI::Xaml::Media::Brush speakerBar{ nullptr };
            // The level's tool areas (MdFindToolAreas, found with the parse), in block order; freed once
            // the level is Done. The build walks them alongside the blocks:
            std::vector<::Agentmaster::MdToolArea> areas;
            // the next area the build has not reached yet;
            size_t nextArea = 0;
            // the FOLD panel the build is filling — set from an area's first block to its last, and every
            // block in between goes into it instead of the level's chunks (null outside a folded area);
            winrt::Windows::UI::Xaml::Controls::StackPanel areaPanel{ nullptr };
            // one past that area's last block (meaningful while areaPanel is set);
            size_t areaEnd = 0;
            // the chunk of that fold panel being filled, and how many elements it holds — a fold is
            // chunked like the level itself (one real turn held 243 calls, 1,000+ elements at Max), so an
            // append to an OPEN fold never re-measures a thousand siblings (_AppendToFold); made by
            // MakeChunkPanel like the level's chunks, so an open fold is a bounded UIA list too.
            winrt::Windows::UI::Xaml::Controls::StackPanel areaChunk{ nullptr };
            size_t areaChunkFill = 0;
            // "Expand all" is on for this level: every fold flips open, and folds built from now on start
            // open. A single fold's own click never changes it. Kept across a Refresh.
            bool expandAll = false;
            // The level has at least one foldable area — shows the "Expand all" switch while current.
            bool hasFolds = false;
            // Every fold built so far, for "Expand all" / "Collapse all".
            std::vector<FoldParts> folds;
            // The line-wrap mode this level's scroll view is set to. It lags _wrapLines while the level is
            // hidden — a switch re-lays out only the level on screen, because a collapsed view can't be laid
            // out to find the reader's place again — and _SelectLevel catches it up when the level is shown.
            bool wrapLines = true;
            // The level's session chain (MdFindSessionChain, found with the parse): which blocks form the
            // "Session chain" list (their "session N" mentions become links) and which block holds each
            // session's heading. Freed once the level is Done — the built links and sessionHeadings carry
            // everything a click needs after that.
            ::Agentmaster::MdSessionChain chain;
            // The BUILT session headings, by session number (index 0 unused; null until built): what a chain
            // link scrolls to (_JumpToSession). Kept for the view's life, so the links keep working after the
            // build; released with the view (_ReleaseLevels).
            std::vector<winrt::Windows::UI::Xaml::FrameworkElement> sessionHeadings;
            // A chain link clicked before its heading was built (the build is time-sliced, and a session's
            // heading can be thousands of blocks down): the session to scroll to the moment its heading
            // exists (_Pump), 0 == none. A newer click replaces it; the end of the build drops it (the heading
            // lay past the block cap and will never exist), and so does a switch to another level
            // (_SelectLevel — the jump would otherwise fire when the reader comes back).
            size_t pendingSessionJump = 0;
            // The BUILT turn headings ("### Turn N · …", top level only), in build order — so their numbers rise — each
            // with its number in the whole conversation. A box click reads which turn sits at the top of the view
            // (_TopTurnNumber) and the re-render scrolls that turn back to the top (pendingTurnJump). Kept for the view's
            // life; released with it.
            std::vector<std::pair<int, winrt::Windows::UI::Xaml::FrameworkElement>> turnHeadings;
            // A box click re-rendered this view while the reader was on turn N: scroll the first BUILT heading numbered N
            // or later to the top the moment it exists (_Pump) — later, because the new view may skip turn N (User Only
            // skips a turn with nothing of the user's). 0 == none. When the build ends without one, the last heading built
            // is taken. Dropped by a switch to another tab and by a jump button (an explicit move outranks it).
            int pendingTurnJump = 0;
            // The ↓ jump button was clicked while this view was still being built: every build slice scrolls on to the new
            // end, and the end of the build settles on the real last line (_Pump). The reader's own scroll — a wheel turn,
            // a press, an upward key in the view (_CancelFollowEnd) — the ↑ button and a switch to another tab all stop it.
            bool followEnd = false;
            // --- the Diff tab only (TRANSCRIPT_OVERLAY.md §3l) ---
            // This view is the Diff tab: its frame holds the outline beside the page, its code blocks of language "diff"
            // become diff cards, its Bash rows become chips.
            bool isDiff = false;
            // What the body holds for this view: the scroll view itself, or (the Diff tab) a grid of the outline and the
            // scroll view. _SelectLevel shows / hides it; _ResetView takes it out of the tree.
            winrt::Windows::UI::Xaml::FrameworkElement frame{ nullptr };
            // The outline column (the Diff tab): its own scroll view and the stack its entries go in.
            winrt::Windows::UI::Xaml::Controls::ScrollViewer outlineScroll{ nullptr };
            winrt::Windows::UI::Xaml::Controls::StackPanel outline{ nullptr };
            // The outline's turn entries with their turn numbers, in rising order (the highlight follows the turn on top of
            // the page), and the file entries by their card's block index, in rising order (a card ticked "Viewed" dims its
            // entry).
            std::vector<std::pair<int, winrt::Windows::UI::Xaml::Controls::Button>> outlineTurns;
            std::vector<std::pair<size_t, winrt::Windows::UI::Xaml::Controls::Button>> outlineFiles;
            // The outline entry highlighted now (an index into outlineTurns; -1 none).
            int outlineLit = -1;
            // The diff cards built so far, in block order (shared: their handlers hold weak_ptrs).
            std::vector<std::shared_ptr<DiffCard>> diffCards;
            // A file entry clicked before its card was built: the card's block index + 1 to scroll to the moment it exists
            // (_Pump), 0 == none. Dropped like the other pending jumps.
            size_t pendingCardJump = 0;
            // The status line's words for a Diff view that is done ("23 turns · 9 with changes · 14 files · +412 -87").
            std::wstring diffSummary;
        };

        // What the background render hands the UI thread for one level.
        struct LevelRender
        {
            std::wstring markdown; // the full rendered text (Copy)
            std::vector<::Agentmaster::MdBlock> blocks; // the parsed blocks (possibly capped)
            bool truncated = false; // blocks or input were capped
            bool failed = false; // the parse failed part-way
            size_t lines = 0; // lines in `markdown`
            int64_t elapsedMs = 0; // render + parse time
            std::wstring message; // why there is nothing to show (blocks empty), or empty
            std::wstring path; // the transcript / rollout read, for the log ("" when none was found)
            std::wstring forkSourceId; // set when `path` is the FORK SOURCE's transcript (the session has none of its own yet), for the log
            std::vector<::Agentmaster::MdToolArea> areas; // the tool areas among `blocks` (MdFindToolAreas)
            ::Agentmaster::MdSessionChain chain; // the Session chain list + the session headings among `blocks` (MdFindSessionChain)
            std::vector<DiffOutlineTurn> outline; // the Diff tab: its turns that changed files, with their files (BuildDiffOutline)
            std::wstring diffSummary; // the Diff tab: its status line's words
        };

        // Build every piece of the overlay's XAML (Attach). Throws winrt::hresult_error on a XAML failure.
        void _BuildUi();
        // Make `level` (a view index) current: drop the left view's pending session jump, pending turn jump and
        // end-follow (each would fire when the reader comes back), tab visuals, the box row (_SyncBoxesRow — shown and
        // filled for a box tab, hidden for a level), view visibility, bring its view up to the current line-wrap mode
        // (_SyncWrapMode — a view laid out under the other mode re-lays out at once, keeping its passage), start its
        // render or resume its build, the jump buttons (_UpdateJumpButtons), and (focusBody) move keyboard focus into its
        // scroll view on the next tick.
        void _SelectLevel(int level, bool focusBody);
        // A tab button / digit key: select the view at TAB POSITION `position` (0 == the first tab,
        // Default), log the switch, and REMEMBER the view for the next open in any window
        // (_RememberLevel — on every pick, the tab already showing included). Out-of-range positions are
        // ignored.
        void _SelectTabPosition(size_t position);
        // The tab the user last picked (AppSettings::transcriptOverlayLevel, read fresh from settings.json) as a view
        // index (TranscriptViewFromName: a level's or a box tab's name); Default when the stored name is empty / unknown
        // or the read fails (logged). UI thread (a small file read, the ColorPickupFlyout idiom).
        static int _RememberedLevel() noexcept;
        // Store `level` (a view index) as the tab the next open uses, by its name (TranscriptViewName): a FRESHEST-DISK
        // read-modify-write of just that field (never a stale in-memory copy — a cog Save or another window's pick is
        // never clobbered), skipped when the file already holds it. Best-effort: a failed write (logged) costs only the
        // remembered tab, never the switch the user just made.
        // Parameters: level — the view index picked (0..8).
        static void _RememberLevel(int level) noexcept;
        // The ticked boxes of the box tab `view` (the in-memory copy Show read from settings.json and each click updates).
        // Parameters: view — a box tab's view index (5..7); any other view reads as an empty list.
        // Returns the tab's ticked names (or a static empty list). Never throws.
        const std::vector<std::wstring>& _BoxesOf(int view) const noexcept;
        // The render options of `view`: a level's own (TurnsOptionsForLevel), or a box tab's from its ticked boxes and its
        // sides (TranscriptViewOptions). Never throws.
        ::Agentmaster::TurnsOptions _ViewOptions(int view) const noexcept;
        // Show or hide the box row for the CURRENT view: hidden on a level; on a box tab shown, every box's check set to
        // the tab's own ticks, and each box enabled only while it would change the render (TranscriptViewBoxTakesEffect;
        // a Codex session has no turns view, so every box is disabled there). Programmatic IsChecked changes raise no
        // Click, so this never re-enters _ToggleBox. No-op before _BuildUi made the row.
        // Throws winrt::hresult_error on a XAML failure (every caller contains it).
        void _SyncBoxesRow();
        // A click on box `index` (kTranscriptViewBoxes) of the box tab on screen: the CheckBox already flipped itself, so
        // its IsChecked is the new state. Updates the tab's ticks, remembers the click for every window (_RememberBox),
        // re-greys the dependent boxes (_SyncBoxesRow), then re-renders the tab (_ResetView + _SelectLevel) with the
        // turn at the top of the view as its pending turn jump — the reader stays on the same turn. Keyboard focus stays
        // on the box, so several boxes can be clicked or toggled with Space in a row. Deliberately unlogged — a view
        // toggle like ↵ Wrap (the CLAUDE.md logging convention); the re-render's [transcript] line names the boxes.
        // Parameters: index — the box's position in kTranscriptViewBoxes.
        // Throws winrt::hresult_error on a XAML failure (the box's Click handler contains it).
        void _ToggleBox(size_t index);
        // The three box tabs' ticked lists as settings.json holds them now (fresh from disk), in view order (Custom, User
        // Only, Assistant Only). Every list empty when the read fails (logged) — every box clear, the tabs' Minimal sides.
        // UI thread (a small file read, the ColorPickupFlyout idiom).
        static std::array<std::vector<std::wstring>, ::Agentmaster::kTranscriptBoxViewCount> _RememberedBoxes() noexcept;
        // Store one click on a box for every window's next open: a FRESHEST-DISK read-modify-write of just that tab's list
        // — the box's own name added or removed (TranscriptViewWithBox), every other entry kept, so another window's ticks
        // and a newer build's unknown names survive — skipped when the file already says so. Best-effort: a failed write
        // (logged) costs only the remembered tick; the view on screen has already changed.
        // Parameters: view — the box tab (5..7); name — the box's name; on — its new state.
        static void _RememberBox(int view, std::wstring_view name, bool on) noexcept;
        // Discard ONE view's render and XAML so it renders anew — a box click changed what it shows. Its frame (its scroll
        // view; the Diff tab's grid) leaves the screen at once (Collapsed) and the tree on a later LOW-priority tick (tearing a long view's thousands of
        // elements down inside the click would stall it); its "Expand all" is kept; its epoch is bumped, so a render still
        // running for the old boxes is dropped when it lands (_ApplyLevelRender). The other views are untouched.
        // Parameters: view — the view index (0..8).
        // Never throws: a XAML failure is logged (Rule #18); the view is reset either way.
        void _ResetView(int view) noexcept;
        // The number of the turn the reader is on in `view`: the last BUILT turn heading at or above the top edge of the
        // view, found by a binary search over view.turnHeadings (their tops rise with their numbers) — each probe one
        // TransformToVisual of a laid-out element of the view on screen. 0 when the view sits at its top, when the top
        // edge is above the first turn (the chain list, the notes before the first turn), or when nothing is built.
        // Parameters: view — the CURRENT view (laid out; a hidden view's positions are not real).
        // Never throws: a failure is logged (Rule #18) and answers 0 (the re-render then starts at the top).
        int _TopTurnNumber(LevelView& view) noexcept;
        // Scroll `view` so its built turn heading at `index` (view.turnHeadings) sits a few pixels below the top edge —
        // the session jump's recipe (_ScrollToSessionHeading: UpdateLayout first, then ChangeView with no animation),
        // without moving keyboard focus (the box that asked for it keeps it).
        // Parameters: view — the current view; index — a position in view.turnHeadings (out of range: no-op).
        // Never throws: a failure is logged (Rule #18) and the view stays where it was — it runs inside a build slice,
        // whose catch would otherwise stop the build.
        void _ScrollToTurnHeading(LevelView& view, size_t index) noexcept;
        // A jump button's click: scroll the current view to its top (`toEnd` false) or to its last line (`toEnd` true),
        // with no animation, and move keyboard focus to the view (Home / End / PageUp keep working, and the button that
        // is about to hide never keeps focus). An explicit jump drops the view's pending session and turn jumps. ↓ on a
        // view still building also starts its end-follow (LevelView::followEnd); ↑ stops it.
        // Parameters: toEnd — true for ↓ (the bottom), false for ↑ (the top).
        // Throws winrt::hresult_error on a XAML failure (the button's Click handler contains it).
        void _JumpToEdge(bool toEnd);
        // Scroll `view` to its last line, with no animation. `settle` lays the view out first (UpdateLayout), so the extent
        // ChangeView clamps to is the real one — a click and the end of a build settle; a build slice's end-follow does not
        // (it takes the extent of the last layout, one slice behind, and the next slice catches up), so a long build never
        // pays a forced layout per slice.
        // Parameters: view — the current view; settle — force the layout first.
        // Never throws: a failure is logged (Rule #18) and the view stays where it was — it runs inside a build slice too.
        void _ScrollViewToEnd(LevelView& view, bool settle) noexcept;
        // Show or hide the two jump buttons for the CURRENT view: ↑ while it is scrolled down from the top, ↓ while there is
        // more below (not while the view follows its end — the next build slice would only hide it again). Opacity +
        // IsHitTestVisible + IsTabStop only, each set only when it changes, so it is cheap enough for every ViewChanged /
        // SizeChanged and never re-lays anything out. Both hidden while the overlay is closed or the view has no scroll.
        // Never throws: a failure is logged (Rule #18) and the buttons keep their last state.
        void _UpdateJumpButtons() noexcept;
        // The reader moved the view whose scroll view is `scroll` themself (a wheel turn, a press — the scroll bar, a
        // touch, a link — or an upward key): stop that view's end-follow, so the next build slice does not drag them
        // back down. Called from the view's input handlers (handledEventsToo: the scroll view handles all of them).
        // Parameters: scroll — the event's sender.
        // Never throws.
        void _CancelFollowEnd(const winrt::Windows::Foundation::IInspectable& scroll) noexcept;

        // ---- the Diff tab (TRANSCRIPT_OVERLAY.md §3l) ----

        // Fill the Diff view's outline column from its render's turns: per turn a button ("Turn 4 · <its message's first
        // words>", the files' total +/-) and under it a button per file (its path, +a -d). A click scrolls the page to that
        // turn's heading or that file's card - at once when it is built, else the moment the build reaches it (the pending
        // turn / card jumps, _Pump). Built once per render (the outline is short: one entry per changed file); a turn
        // without changes is not listed - it stays in the page as its messages.
        // Parameters: view — the Diff view; turns — its outline (LevelRender::outline).
        // Throws winrt::hresult_error / std::bad_alloc (_ApplyLevelRender's callers contain them).
        void _BuildDiffOutline(LevelView& view, const std::vector<DiffOutlineTurn>& turns);
        // Light the outline entry of the turn at the top of the Diff page (the last turn heading at or above the edge,
        // _TopTurnNumber - only an exact match: a turn without changes has no entry, and lighting a neighbour would say the
        // reader is somewhere they are not), and scroll the outline to keep it in sight. Runs on the page's ViewChanged and
        // after a jump; sets only on a change.
        // Never throws: a failure is logged (Rule #18) and the highlight stays as it was.
        void _UpdateDiffOutlineHighlight() noexcept;
        // An outline turn entry's click: scroll the Diff page to turn `number`'s heading (_ScrollToTurnHeading), or - not
        // built yet - make it the view's pending turn jump, done by _Pump the moment the heading exists. "Work before the
        // first prompt" has no number (0): its first file's card is the target instead (`firstBlock`).
        // Parameters: number — the turn's number (0: the work before the first prompt); firstBlock — that turn's first card's
        //             block index.
        // Throws winrt::hresult_error on a XAML failure (the entry's Click handler contains it).
        void _JumpToDiffTurn(int number, size_t firstBlock);
        // An outline file entry's click: scroll the Diff page to the card of block `block` (_ScrollElementToTop), or - not
        // built yet - make it the view's pending card jump (_Pump). A newer click replaces an older one.
        // Parameters: block — the card's block index.
        // Throws winrt::hresult_error on a XAML failure (the entry's Click handler contains it).
        void _JumpToDiffCard(size_t block);
        // A turn heading of the Diff page ("### Turn 4 · <times> · 2 files · +9 -1"): the heading as every view draws it, with
        // its trailing counts drawn the GitHub way - the "2 files" dim, "+9" green, "-1" red. A heading without counts (a turn
        // that changed nothing, a session heading) is drawn as usual.
        // Parameters: view — the Diff view (a heading resets its speaker bar); block — the heading.
        // Returns the heading's text block. Throws winrt::hresult_error / std::bad_alloc (the build's catch contains them).
        winrt::Windows::UI::Xaml::FrameworkElement _BuildDiffHeading(LevelView& view, const ::Agentmaster::MdBlock& block);
        // A Bash row of the Diff page ("- `$` <reason> ×N", IsDiffBashRow): a "$" chip, the reason (its own styled runs:
        // a command shown as code stays code), and - for a reason repeated in the turn - a "×N" pill. No list marker: the
        // chip is the row's mark.
        // Parameters: block — the row's list-item paragraph.
        // Returns the row. Throws winrt::hresult_error / std::bad_alloc (the build's catch contains them).
        winrt::Windows::UI::Xaml::FrameworkElement _BuildDiffBashRow(const ::Agentmaster::MdBlock& block);
        // Build a GitHub-style DIFF CARD for a ```diff block of the Diff view: a header bar (chevron, the file as shown, its
        // +a -d with five bars, what the block is, a "Viewed" box) over the hunks - each hunk's header row, its lines with the
        // old and new line numbers on red / green bands - and an expand row above every hunk and below the last one. A card
        // of more than kDiffCardOpenRows lines starts closed and builds its body on its first open.
        // Parameters: view — the Diff view (the card is recorded in view.diffCards); block — the code block; index — its
        //             block index (the outline's jump target).
        // Returns the card's root element. Throws winrt::hresult_error / std::bad_alloc (the build's catch contains them).
        winrt::Windows::UI::Xaml::FrameworkElement _BuildDiffCard(LevelView& view, const ::Agentmaster::MdBlock& block, size_t index);
        // Build a card's body: per hunk its expand row (only where lines can lie above it: a hunk at the file's start has
        // none), its header row and its lines; then the expand row below the last hunk, and the note of a card cut at the
        // block cap or without line numbers. No horizontal scroll view: the code WRAPS (the user's ask) - with the ↵ toggle
        // on, a long line continues on the next visual line under its own code; with ↵ off the page measures it at its full
        // width and cuts it at the right edge like every other line. At the card's build, or on a big card's first open.
        // Parameters: card — the card (its hunks parsed). Throws winrt::hresult_error / std::bad_alloc.
        void _BuildDiffCardBody(const std::shared_ptr<DiffCard>& card);
        // A run of diff lines as ONE grid: a row per line, each row a numbers cell (DiffNumbersText: old number, new number,
        // +/- mark, in a monospace font) and a WRAPPING code cell, so a long line's continuation hangs under its own code
        // and its numbers stay level with its first visual line; under them childless bands spanning a run's rows (green
        // under added lines, red under removed ones, a stronger tint under their numbers). Two text blocks per line and a
        // row definition - never a grid per line. The code cells are selectable with the shared text menu (a line copies as
        // code, without its numbers or mark); a selection stays inside one line. Lines read from the file as it is now
        // (`fromFileNow`) carry a yellow left edge.
        // Parameters: rows — the lines (NumberHunkRows' numbering: 0 = no number on that side); digits — the card's
        //             numberDigits; fromFileNow — see above.
        // Returns the grid. Throws winrt::hresult_error / std::bad_alloc.
        winrt::Windows::UI::Xaml::FrameworkElement _MakeDiffLines(const std::vector<::Agentmaster::DiffRow>& rows, int digits, bool fromFileNow) const;
        // Build expand row `row` of `card` (a full-width button: an arrow and its label) and record it in card.expandRows /
        // card.expandLabels; its click runs _ExpandDiffCard. Its label says what a click shows - before the file is read, as
        // much as the hunks' own numbers tell (_SyncDiffExpandRows refines it once the file is known).
        // Parameters: card — the card; row — h for the row above hunk h, hunks.size() for the row below the last.
        // Returns the button. Throws winrt::hresult_error / std::bad_alloc.
        winrt::Windows::UI::Xaml::Controls::Button _MakeDiffExpandRow(const std::shared_ptr<DiffCard>& card, size_t row);
        // Open or close a card: its body's visibility and its chevron, building the body first when it was never built.
        // Parameters: card — the card; open — the new state. Throws winrt::hresult_error / std::bad_alloc.
        void _SetDiffCardOpen(const std::shared_ptr<DiffCard>& card, bool open);
        // The "Viewed" box's click (GitHub's: a file the reader is done with): ticked closes the card and dims its outline
        // entry; unticked opens it again. Lives as long as the overlay's view (never remembered across opens).
        // Parameters: card — the card; viewed — the box's new state. Throws winrt::hresult_error / std::bad_alloc.
        void _SetDiffCardViewed(const std::shared_ptr<DiffCard>& card, bool viewed);
        // An expand row's click: reveal more lines around a hunk from the file AS IT IS NOW (the user's pick): the file's
        // lines when already read, else a read off the UI thread first (_LoadDiffFileAsync), the expansion queued for it
        // (one read per file however many clicks wait for it).
        // Parameters: card — the card; row — the expand row's index (h: above hunk h; hunks.size(): below the last).
        // Never throws: a failure is logged (Rule #18) and the card stays as it was.
        void _ExpandDiffCard(const std::shared_ptr<DiffCard>& card, size_t row) noexcept;
        // Apply one expansion with the file's lines known: find each hunk in the file (LocateHunkNewSide, once per hunk),
        // then insert up to kDiffExpandStep lines - going up from the hunk (a gap between two hunks of at most
        // kDiffGapExpandAll lines opens whole), or down from the last one - numbered from the hunk's own numbers and marked
        // "from the file now". A hunk the file no longer holds, or a file that cannot be read, turns the rows into a dim
        // note saying so (_SyncDiffExpandRows).
        // Parameters: card — the card; row — as for _ExpandDiffCard; fileLines — the current file (null: it cannot be read).
        // Throws winrt::hresult_error / std::bad_alloc (_ExpandDiffCard and _OnDiffFileLoaded contain them).
        void _ApplyDiffExpansion(DiffCard& card, size_t row, const std::shared_ptr<const std::vector<std::wstring>>& fileLines);
        // Bring every expand row of `card` in line with what is known: with the file read (`fileLines`), each row's label
        // counts the lines it can still show and a row with none left is collapsed; a row whose hunk is no longer in the file
        // (or a file that cannot be read) is disabled with a dim label saying why. Without the file, the hunks' numbers
        // decide (a gap of 0 lines has no row).
        // Parameters: card — the card; fileLines — the current file, or null when it is not read yet / cannot be read;
        //             readFailed — true when it was read and could not be.
        // Throws winrt::hresult_error / std::bad_alloc.
        void _SyncDiffExpandRows(DiffCard& card, const std::vector<std::wstring>* fileLines, bool readFailed);
        // Read a file's current lines on a pool thread (UTF-8, a BOM dropped, at most kDiffFileMaxBytes), then hand them to the
        // UI thread (_OnDiffFileLoaded). Static, holding only `weak` (the last-ref-on-a-pool-thread gotcha); never lets an
        // exception escape (a fire_and_forget escape is a process kill).
        // Parameters: weak / dispatcher — the overlay and its UI thread; path — the file; generation — the overlay's stamp.
        static winrt::fire_and_forget _LoadDiffFileAsync(winrt::weak_ref<AgentTranscriptOverlay> weak,
                                                         winrt::Windows::System::DispatcherQueue dispatcher,
                                                         std::wstring path,
                                                         uint64_t generation);
        // A file read landed: cache its lines (null for a file that could not be read) and run every expansion that waited
        // for it, in click order. A result for an older generation (the overlay was retargeted, refreshed or closed) is dropped.
        // Parameters: path — the file; lines — its lines, or null; generation — the stamp the read started under.
        // Never throws: a failure is logged (Rule #18).
        void _OnDiffFileLoaded(const std::wstring& path, std::shared_ptr<const std::vector<std::wstring>> lines, uint64_t generation) noexcept;
        // Scroll `view` so `element` sits a hair below the top edge, with no animation (an outline file entry's jump) - the
        // session jump's recipe (UpdateLayout first: ChangeView clamps to the current extent). Focus is not moved.
        // Parameters: view — the current view; element — a built element of its content.
        // Never throws: a failure is logged (Rule #18) and the view stays where it was (it runs inside a build slice too).
        void _ScrollElementToTop(LevelView& view, const winrt::Windows::UI::Xaml::FrameworkElement& element) noexcept;
        // Create `level`'s scroll view + content stack (once), set to the current line-wrap mode
        // (_wrapLines), and put it in the body.
        void _EnsureLevelView(int level);
        // Show `text` in place of `level`'s content (Loading / Empty), creating the placeholder once.
        void _SetLevelMessage(int level, const std::wstring& text);
        // Start `level`'s (a view's) background render (state Loading), with the view's render options as they stand now
        // (_ViewOptions — a box tab's ticks are read HERE, on the UI thread, never by the pool thread) and its epoch.
        void _StartLoad(int level);
        // The background half: render + parse on a pool thread, then hand the result to
        // _ApplyLevelRender on the UI thread. Static so it holds NO reference to the overlay while off
        // the UI thread — only `weak`, resolved back on the UI thread (the last-ref-on-a-pool-thread
        // teardown gotcha). Never lets an exception escape (a fire_and_forget escape is a process kill).
        // Parameters: weak / dispatcher — the overlay and its UI thread; source — the conversation; level — the view
        //   index; generation / epoch — the overlay's and the view's stamps at the start (_ApplyLevelRender drops a
        //   result whose stamps are stale); options — the view's render options (a copy: the pool thread owns it).
        static winrt::fire_and_forget _LoadLevelAsync(winrt::weak_ref<AgentTranscriptOverlay> weak,
                                                      winrt::Windows::System::DispatcherQueue dispatcher,
                                                      TranscriptOverlaySource source,
                                                      int level,
                                                      uint64_t generation,
                                                      uint64_t epoch,
                                                      ::Agentmaster::TurnsOptions options);
        // Render view `level` of `source` into `out` (path resolve, the view's text under `options`, the block parse; the
        // tool areas are folded wherever the full call lines show — options.ops, which is exactly Default, Full and Max
        // among the levels). A box tab of a Codex session has nothing to render (Codex has no turns view): `out` gets a
        // message saying so. Runs on the pool thread; touches no member. Never throws (contained + logged).
        static void _RenderLevel(const TranscriptOverlaySource& source, int level, const ::Agentmaster::TurnsOptions& options, LevelRender& out) noexcept;
        // The Diff tab's OUTLINE and status words, read off its parsed blocks on the pool thread (_RenderLevel): every top-level
        // turn heading starts a turn (TurnHeadingNumber; "Work before the first prompt" is turn 0); the first line of the first
        // message quoted under a "❯" or "✉" label is its title; every top-level ```diff block is one of its files - its shown
        // path from the fence's info, its +/- from its rows (ParseUnifiedDiff). A turn with no diff block is counted but left
        // out of the outline. The words: "23 turns · 9 with changes · 14 files · +412 −87" (files counted once each, by path).
        // Parameters: blocks — the parsed Diff document; out — receives `outline` and `diffSummary`.
        // Throws std::bad_alloc only (_RenderLevel's net contains it).
        static void _ScanDiffOutline(const std::vector<::Agentmaster::MdBlock>& blocks, LevelRender& out);
        // UI thread: adopt a finished render — unless the overlay was retargeted, refreshed or closed
        // since (`generation` mismatch) or this one view was reset by a box click (`epoch` mismatch) — and start
        // building its XAML.
        void _ApplyLevelRender(int level, uint64_t generation, uint64_t epoch, LevelRender&& render);
        // Queue one build slice (Low priority), unless one is already queued.
        void _SchedulePump();
        // One build slice: turn blocks of the CURRENT level into XAML for ~kSliceBudget, do the level's
        // pending session jump once its heading exists (a chain link clicked before the build reached it),
        // its pending turn jump once a heading of that turn or a later one exists (a box click; the last
        // heading built when the build ends without one), keep a ↓-followed view on its end (settled on the
        // real last line when the build ends), refresh the jump buttons, then queue the next slice if blocks
        // remain. Stale (`generation` mismatch) or closed: does nothing.
        void _Pump(uint64_t generation);
        // Build the level's next block (view.built) — or, at the start of a tool area, its fold row first
        // (then the block goes into the fold's panel), or for an empty area its one-line note (skipping
        // the "(no tool calls)" item) — and advance view.built past what was built. A block of the Session
        // chain list gets its "session N" links (only for sessions with a heading in this level), and a
        // session heading is recorded in view.sessionHeadings for those links. A top-level "### Turn N"
        // heading is recorded in view.turnHeadings (a box click's keep-the-turn jump).
        void _BuildNext(LevelView& view);
        // Start a folded tool area: append its summary row (a button: chevron + MdToolAreaSummary) and
        // the panel its blocks go into (Collapsed unless the level's expandAll is on) to the level's
        // chunks, and record both in view.folds.
        // Parameters:
        //   view       — the level being built.
        //   area       — the area (MdToolAreaFoldable).
        //   firstBlock — the area's first block (for the gap above the row).
        //   above      — the block just above the area (its speaker header), or nullptr.
        // Returns the panel the area's blocks are appended to.
        winrt::Windows::UI::Xaml::Controls::StackPanel _BuildFold(LevelView& view, const ::Agentmaster::MdToolArea& area, const ::Agentmaster::MdBlock& firstBlock, const ::Agentmaster::MdBlock* above);
        // The one-line note an EMPTY area becomes ("no tool calls", from its single item, lined up with
        // the fold rows' text).
        // Parameters: firstBlock — the area's only block; above — the block above it (the gap), or nullptr.
        winrt::Windows::UI::Xaml::FrameworkElement _BuildEmptyAreaNote(const ::Agentmaster::MdBlock& firstBlock, const ::Agentmaster::MdBlock* above);
        // Open or close one fold: its panel's visibility and its chevron (▾ open / ▸ folded). Static so the
        // row's click handler needs no reference to the overlay.
        static void _SetFoldOpen(const winrt::Windows::UI::Xaml::Controls::StackPanel& panel, const winrt::Windows::UI::Xaml::Controls::FontIcon& chevron, bool open);
        // The "Expand all" switch: flip the current level's expandAll and open / close every fold of it.
        void _ToggleExpandAll();
        // Show the "Expand all" switch only while the current level has folds, labelled for what a click
        // does next ("Expand all" / "Collapse all").
        void _UpdateExpandAllButton();
        // The header's ↵ LINE-WRAP toggle: flip _wrapLines, re-lay out the level on screen under the new
        // mode while keeping the reader on the same passage (_SyncWrapMode; a hidden level catches up when
        // it is next shown), remember the choice for every window's next open (_RememberWrapLines), then
        // repaint the toggle. Deliberately unlogged — a pure view toggle (the CLAUDE.md logging convention).
        // Throws winrt::hresult_error only from the repaint (the toggle's Click handler contains it); the
        // mode is already applied and remembered by then.
        void _ToggleWrapLines();
        // Paint the toggle for _wrapLines: ON = an accent-tinted face with an accent hairline and a bright
        // glyph; OFF = no face and a dim glyph. The tooltip and the screen-reader name state the mode and
        // what a click does. No-op before _BuildUi made the toggle.
        // Throws winrt::hresult_error on a XAML failure (every caller contains it).
        void _UpdateWrapButton();
        // Bring `view`'s scroll view to _wrapLines, keeping the reader on the same passage: find the
        // element at the top of the view in the CURRENT layout (FindScrollAnchor in the .cpp), switch the
        // mode, lay the view out at once (UpdateLayout — the extent and the anchor's new place must be
        // real before ChangeView, which clamps to the extent), and scroll the anchor back to the top.
        // No-op when the view has no scroll view yet or already has the mode. Call it only for the VISIBLE
        // level: a collapsed view is not laid out, so its anchor's new place can't be read.
        // Parameters: view — the level to bring up to date (the current one).
        // Never throws: a failure is logged (Rule #18); view.wrapLines then says which mode the view
        // really has, so the next select retries a switch that never applied.
        void _SyncWrapMode(LevelView& view) noexcept;
        // The line-wrap state last chosen in ANY window (AppSettings::transcriptOverlayWrapLines, read fresh
        // from settings.json — a small file read on the UI thread, the ColorPickupFlyout idiom).
        // Returns true (wrap, the default) when long lines should wrap — also when the read fails (logged).
        static bool _RememberedWrapLines() noexcept;
        // Store `wrap` as the state every window's next open uses: a FRESHEST-DISK read-modify-write of just
        // that field (never a stale in-memory copy over a cog Save or another window's change), skipped when
        // the file already holds it. Best-effort: a failed write (logged) costs only the remembered state —
        // the view on screen has already switched.
        // Parameters: wrap — the state just chosen (true == long lines wrap).
        static void _RememberWrapLines(bool wrap) noexcept;
        // The level ran out of blocks: add the closing notes (cut / failed), free the blocks, the areas and
        // the chain, drop a pending session jump whose heading never came (cut by the block cap), mark Done.
        // (A pending turn jump and an end-follow are settled by _Pump around this call: both need the last
        // blocks built, and the end-follow the closing note too.)
        void _FinishLevel(LevelView& view);
        // Append one built element to the level's current chunk (opening a new chunk when full).
        void _AppendElement(LevelView& view, const winrt::Windows::UI::Xaml::UIElement& element);
        // Append one built element to the fold being filled (view.areaPanel), in chunks of the same size.
        // Only while view.areaPanel is set.
        void _AppendToFold(LevelView& view, const winrt::Windows::UI::Xaml::UIElement& element);
        // Build the XAML for one block, wrapped in its containers (quote bars, list indents + markers),
        // with the gap above it from MdGapAbove placed so shared quote bars run through it.
        // Parameters: view / block / prev — the level, the block, the block drawn above it (nullptr for none);
        //   sessionRefs — the block's "session N" mentions to draw as links (a Session chain row), or nullptr.
        winrt::Windows::UI::Xaml::FrameworkElement _BuildBlockElement(LevelView& view, const ::Agentmaster::MdBlock& block, const ::Agentmaster::MdBlock* prev, const std::vector<::Agentmaster::MdSessionRef>* sessionRefs = nullptr);
        // Build the bare content of a block (TextBlock / code panel / rule / table), without containers.
        // Parameters: view / block — the level and the block; sessionRefs — as for _BuildBlockElement.
        winrt::Windows::UI::Xaml::FrameworkElement _BuildBlockContent(LevelView& view, const ::Agentmaster::MdBlock& block, const std::vector<::Agentmaster::MdSessionRef>* sessionRefs = nullptr);
        // Build a Markdown TABLE block (MdBlockKind::Table - a GitHub-style pipe table, which MarkdownBlocks finds with
        // MdParseOptions::tables): a Grid in a hairline frame, the header row bold on a lighter band, body rows striped,
        // each cell a selectable text block with its inline styles and its column's alignment. Columns are STAR widths
        // weighted by their natural (one-line) widths and the grid is capped at their sum, so a wide view shows the
        // table at its natural size and a narrow one shrinks it and wraps its cells; a column never shrinks below
        // kTableMinColumnWidth (or its own width, when smaller). The natural widths come from measuring each cell of the
        // first kTableMeasuredRows rows once, detached, on the UI thread - the cost of a table is one text layout per
        // cell, paid only for tables. A table cut at the block cap gets the cut note as a last row.
        // Parameters: block - the Table block (tableAlign gives the column count; every row holds that many cells).
        // Returns the framed table. Throws winrt::hresult_error / std::bad_alloc (the build's catch contains it).
        winrt::Windows::UI::Xaml::FrameworkElement _BuildTable(const ::Agentmaster::MdBlock& block);
        // Append a table cell's runs to its text block: bold, italic, monospace code and links exactly as a paragraph
        // draws them (only http(s) links are clickable), without what only a paragraph has (a speaker label's tint, a
        // Session chain row's links, raw HTML's dimming).
        // Parameters: text - the cell's text block; runs - the cell's styled runs (MdTableCell).
        // Throws winrt::hresult_error / std::bad_alloc (the build's catch contains it); a malformed link URL is logged
        // and drawn as link-colored text.
        void _AppendCellRuns(const winrt::Windows::UI::Xaml::Controls::TextBlock& text, const std::vector<::Agentmaster::MdRun>& runs) const;
        // Append run `source` of a Session chain row to `text` with its "session N" mentions as hyperlinks
        // (each a Hyperlink whose Click calls _JumpToSession) and the text around them as plain runs in the
        // run's own bold / italic style. A row is never a speaker label, so no label tint is involved.
        // Parameters:
        //   text     — the row's TextBlock.
        //   source   — the run to append.
        //   runIndex — its index in the block's runs (MdSessionRef::run).
        //   refs     — the row's mentions (MdFindSessionRefs, filtered to sessions with a heading).
        // Returns false, appending nothing, when no mention lies in this run — the caller then builds the
        // run as usual. Throws winrt::hresult_error / std::bad_alloc (the build's catch contains it).
        bool _AppendSessionLinks(const winrt::Windows::UI::Xaml::Controls::TextBlock& text, const ::Agentmaster::MdRun& source, size_t runIndex, const std::vector<::Agentmaster::MdSessionRef>& refs);
        // A Session chain link was clicked: scroll the CURRENT level's heading of `session` to the top
        // (_ScrollToSessionHeading) — or, when the build hasn't reached that heading yet, remember it as the
        // level's pending jump, done the moment the heading is built (_Pump). Only the level on screen can be
        // clicked, so the current level is always the link's level.
        // Parameters: session — N of the clicked "session N" (1-based).
        // Throws nothing in practice: the scroll contains its own failures (_ScrollToSessionHeading). Not
        // marked noexcept only so the link's Click handler keeps its belt catch.
        void _JumpToSession(size_t session);
        // Scroll `view` so the built heading of `session` sits a few pixels below the top edge, then move
        // keyboard focus to the scroll view: the clicked link, left focused and scrolled out of view, would
        // otherwise be brought back into view — dragging the reader back to the list — the next time focus
        // returns to it (the scroll view's BringIntoViewOnFocusChange; a window activation restores focus).
        // The view is laid out first (UpdateLayout): a heading built in this very slice has no position yet,
        // and ChangeView clamps to the current extent. No-op when the heading is not built.
        // Parameters: view — the level (the current one); session — N.
        // Never throws: a XAML failure is logged (Rule #18) and the view stays where it was. Contained here
        // because a pending jump runs inside a build slice (_Pump), whose own catch would stop the level's
        // build — a failed scroll must never cost the rest of the transcript.
        void _ScrollToSessionHeading(LevelView& view, size_t session) noexcept;
        // A selectable, wrapping TextBlock with the shared text context menu — the base of every text
        // element the overlay draws.
        winrt::Windows::UI::Xaml::Controls::TextBlock _MakeTextBlock(double fontSize) const;
        // Build the level guide's panel (the (i) Flyout's content) for the CURRENT level: a title, the
        // comparison table from AgentMaster/TranscriptLevelGuide.h (a row per kTranscriptLevelGuide entry; a
        // column per level, least detailed first so the check marks rise as a staircase, each headed by its
        // label and the digit key that picks its tab; the current level's column tinted and its head in the
        // accent), then "Also in every level", "In no level", the Codex note and a keyboard footer. Rebuilt
        // on every open (the Flyout's Opening), so the highlighted column always follows the tab on screen;
        // ~100 small elements, built only when the user asks for the guide.
        // Returns the panel's root element (a StackPanel), its brushes explicit (the overlay is always dark).
        // Throws winrt::hresult_error on a XAML failure and std::bad_alloc (the Opening handler contains both:
        // the guide then opens with whatever content it last had).
        winrt::Windows::UI::Xaml::FrameworkElement _BuildLevelGuidePanel() const;
        // The status line under the tabs, from the current level's state.
        void _UpdateStatus();
        // The header's session line (title · short id · Codex).
        void _UpdateTitle();
        // Repaint the tab row: the current level's tab bright + underlined, the others dim.
        void _UpdateTabVisuals();
        // Drop every level's view and render (the XAML leaves the body). UI thread.
        void _ReleaseLevels() noexcept;
        // Move keyboard focus into the current level's scroll view on the next tick (its first layout
        // must happen before Focus can land).
        void _FocusBodyDeferred();
        // The header's Copy: the current level's whole Markdown to the clipboard (+ chime + [nav]).
        void _CopyCurrentLevel();
        // The header's Refresh: discard every rendered level and render the current one again (the
        // transcript keeps growing while a session works).
        void _Refresh();
        // Is `element` `ancestor` or under it in the visual tree? The focus guard's test.
        static bool _IsUnder(const winrt::Windows::UI::Xaml::DependencyObject& element, const winrt::Windows::UI::Xaml::DependencyObject& ancestor) noexcept;

        winrt::Windows::System::DispatcherQueue _dispatcher{ nullptr }; // this window's UI thread (captured at construction — the overlay is built on it)
        winrt::Windows::UI::Xaml::Controls::Grid _pageRoot{ nullptr }; // the page's Root (the focus guard's boundary)
        winrt::Windows::UI::Xaml::Controls::Grid _host{ nullptr }; // the full-page layer (backdrop + card), Collapsed while closed
        winrt::Windows::UI::Xaml::Controls::Border _backdrop{ nullptr }; // the dim surface outside the card — a press here closes
        winrt::Windows::UI::Xaml::Controls::Border _card{ nullptr }; // the reading card (90% x 80% via the host's star grid)
        winrt::Windows::UI::Xaml::Controls::Grid _headerRow{ nullptr }; // icon · title · session line · Wrap / Copy / Refresh / Close
        winrt::Windows::UI::Xaml::Controls::TextBlock _subtitleText{ nullptr }; // the session line
        winrt::Windows::UI::Xaml::Controls::Button _wrapButton{ nullptr }; // the header's ↵ line-wrap toggle, first of its buttons (_ToggleWrapLines)
        // That toggle's FACE, its whole content: tinted with an accent hairline while ON. Inside the content
        // (not the button's own background) because the button template's hover / press states set that
        // background through visual-state setters, which outrank a local value — an ON fill drawn there
        // would vanish under the pointer.
        winrt::Windows::UI::Xaml::Controls::Border _wrapButtonFace{ nullptr };
        winrt::Windows::UI::Xaml::Controls::FontIcon _wrapGlyph{ nullptr }; // that toggle's ↵ glyph: bright while ON, dim while OFF
        winrt::Windows::UI::Xaml::Controls::Grid _tabsRow{ nullptr }; // the level tabs + the status line
        std::vector<winrt::Windows::UI::Xaml::Controls::Button> _tabButtons; // one per tab: the kTranscriptCopyMenu levels in its order, then Custom, User Only, Assistant Only, then Diff
        std::vector<winrt::Windows::UI::Xaml::Controls::TextBlock> _tabLabels; // each tab's label (dim / bright)
        std::vector<winrt::Windows::UI::Xaml::Controls::Border> _tabUnderlines; // each tab's underline (accent when current)
        std::vector<int> _tabViews; // the VIEW index behind each tab position (a TurnsLevel value, a box tab's 5..7, or the Diff tab's 8) — the digit keys and the guide's "key N" read it
        // The BOX ROW under the tabs (TRANSCRIPT_OVERLAY.md §3j): "Also show:" and the twelve boxes grouped by level, in a
        // wrapping panel. Collapsed unless a box tab is current (_SyncBoxesRow); built once with the overlay.
        winrt::Windows::UI::Xaml::Controls::Grid _boxesRow{ nullptr };
        std::vector<winrt::Windows::UI::Xaml::Controls::CheckBox> _boxChecks; // one per kTranscriptViewBoxes entry, in its order
        // Each box tab's ticked box names, in view order (Custom, User Only, Assistant Only): read fresh from settings.json by
        // every Show of a new conversation (_RememberedBoxes), changed by every box click (_ToggleBox) — the in-memory copy the
        // renders use; settings.json gets each click through its own RMW (_RememberBox).
        std::array<std::vector<std::wstring>, ::Agentmaster::kTranscriptBoxViewCount> _boxes{};
        // Each view's render epoch: bumped by _ResetView (a box click re-renders ONE view), carried by its render, and checked
        // when the render lands — the overlay-wide _generation would also throw away the other views' renders. Never reset
        // with the views, so an epoch never repeats while the overlay lives.
        std::array<uint64_t, ::Agentmaster::kTranscriptViewCount> _viewEpochs{};
        // The body's layer: the level views' host (_bodyHost) with the jump buttons drawn over its bottom-right corner. A
        // separate grid, so releasing the views (_bodyHost's Clear) never takes the buttons with them.
        winrt::Windows::UI::Xaml::Controls::Grid _bodyLayer{ nullptr };
        // The JUMP buttons, stacked at the body's bottom-right: ↑ to the top, ↓ to the bottom (_JumpToEdge). Hidden until the
        // current view can be scrolled that way (_UpdateJumpButtons).
        winrt::Windows::UI::Xaml::Controls::Button _jumpTopButton{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _jumpEndButton{ nullptr };
        // What each jump button was last set to (-1 never, 0 hidden, 1 shown), so an update that changes nothing sets nothing.
        int8_t _jumpTopShown = -1;
        int8_t _jumpEndShown = -1;
        // The Diff tab's Expand (TRANSCRIPT_OVERLAY.md §3l): every file read so far, by its path as the card names it - its
        // current lines, or null for a file that could not be read (gone, locked, too big) - kept for the overlay's open (a
        // Refresh, a close or another conversation clears it: the file may have changed); and the expansions waiting for a
        // read still in flight (the path, the card, the expand row).
        std::unordered_map<std::wstring, std::shared_ptr<const std::vector<std::wstring>>> _diffFiles;
        std::vector<std::tuple<std::wstring, std::weak_ptr<DiffCard>, size_t>> _diffExpandWaiting;
        // The level guide: the (i) button right after the last level tab, and the Flyout it opens (its
        // Button::Flyout, so a click or Enter / Space opens it with no handler of ours). The Flyout is built
        // once with the overlay and refills its content on every Opening (_BuildLevelGuidePanel).
        winrt::Windows::UI::Xaml::Controls::Button _guideButton{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Flyout _guideFlyout{ nullptr };
        // Whether that Flyout is open, kept from its Opened / Closed events (DismissLevelGuide reads it to let
        // Esc close the guide before the overlay; Close hides an open guide).
        bool _guideOpen = false;
        winrt::Windows::UI::Xaml::Controls::TextBlock _statusText{ nullptr }; // right of the tabs: progress / size / timing
        winrt::Windows::UI::Xaml::Controls::Button _expandAllButton{ nullptr }; // far right of the tabs row: "Expand all" / "Collapse all" (only while the level has folds)
        winrt::Windows::UI::Xaml::Controls::TextBlock _expandAllLabel{ nullptr }; // that switch's text
        winrt::Windows::UI::Xaml::Controls::Grid _bodyHost{ nullptr }; // holds every level's scroll view (one visible)
        winrt::Microsoft::Terminal::UI::TextMenuFlyout _textMenu{ nullptr }; // ONE shared Copy / Select All menu for every text block

        // Shared brushes / fonts — created once, referenced by thousands of elements.
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _bodyBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _dimBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _inlineCodeBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _linkBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _markerBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _codeTextBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _codeBackBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _codeBorderBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _ruleBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _quoteBarBrush{ nullptr }; // the neutral quote bar
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _userBrush{ nullptr }; // "❯" speaker: label text
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _userBarBrush{ nullptr }; // "❯" speaker: quote bar
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _claudeBrush{ nullptr }; // "●" speaker: label text
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _claudeBarBrush{ nullptr }; // "●" speaker: quote bar
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _peerBrush{ nullptr }; // "✉" speaker: label text
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _peerBarBrush{ nullptr }; // "✉" speaker: quote bar
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _accentBrush{ nullptr }; // the current tab's underline (system accent)
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _toggleOnBrush{ nullptr }; // an ON toggle's face: the accent at ~28% — a tint on the dark card, never a solid accent block
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _toggleOnBorderBrush{ nullptr }; // an ON toggle's hairline: the accent at ~70%, what makes ON read at a glance
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _transparentBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::FontFamily _monoFont{ nullptr };
        // The Diff tab's colors (TRANSCRIPT_OVERLAY.md §3l: GitHub's dark diff palette), made once in _BuildUi and shared by
        // every card, line run, chip and outline entry - a brush per element would multiply objects by the line count.
        struct DiffPalette
        {
            winrt::Windows::UI::Xaml::Media::SolidColorBrush card{ nullptr }; // a card's face (#0d1117)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush border{ nullptr }; // a card's frame and its header's rule (#30363d)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush head{ nullptr }; // a card's header bar (#161b22)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush code{ nullptr }; // the path and the code (#e6edf3)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush plus{ nullptr }; // "+a", an added square (#3fb950)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush minus{ nullptr }; // "-d", a removed square (#f85149)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush meta{ nullptr }; // what a card is, "Viewed", the hunk header (#8b949e)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush number{ nullptr }; // the line numbers (#6e7681)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush addBand{ nullptr }; // under an added line
            winrt::Windows::UI::Xaml::Media::SolidColorBrush addNumberBand{ nullptr }; // under an added line's numbers (stronger)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush removeBand{ nullptr }; // under a removed line
            winrt::Windows::UI::Xaml::Media::SolidColorBrush removeNumberBand{ nullptr }; // under a removed line's numbers (stronger)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush hunkBand{ nullptr }; // under a hunk's "@@" header row
            winrt::Windows::UI::Xaml::Media::SolidColorBrush expandBand{ nullptr }; // an expand row's face
            winrt::Windows::UI::Xaml::Media::SolidColorBrush expandText{ nullptr }; // an expand row's words and arrow (#9ecbff)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush now{ nullptr }; // the yellow edge of lines read from the file now (#d29922)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush chipBorder{ nullptr }; // a Bash row's "$" chip frame (#3a3a3a)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush chipText{ nullptr }; // the "$" (#8a8a8a)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush reason{ nullptr }; // a Bash row's reason (#c8c8c8)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush count{ nullptr }; // a Bash row's "×N" (#9ecbff)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush outlineRule{ nullptr }; // the line between the outline and the page (#2c2c2c)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush outlineTurn{ nullptr }; // an outline turn entry's words (#cfcfcf)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush outlineFile{ nullptr }; // an outline file entry's name (#a8a8a8)
            winrt::Windows::UI::Xaml::Media::SolidColorBrush outlineLit{ nullptr }; // the lit turn entry's face (the accent at ~18%)
        };
        DiffPalette _diff;

        std::array<LevelView, ::Agentmaster::kTranscriptViewCount> _levels{}; // by view index (TranscriptViews.h: the five levels, then the three box tabs, then the Diff tab)
        TranscriptOverlaySource _source; // what is shown
        int _current = 2; // the current view (TurnsLevel::Default)
        // Whether long lines WRAP (true, the default) or keep their length and are cut off at the body's
        // right edge. GLOBAL: Show reads it from settings.json on every open
        // (AppSettings::transcriptOverlayWrapLines) and the ↵ toggle writes it, so every window opens with
        // the last choice; an overlay already open in another window keeps its own state until it is opened
        // again. Each level view follows it through LevelView::wrapLines (_SyncWrapMode).
        bool _wrapLines = true;
        bool _open = false; // shown
        bool _pumpQueued = false; // a build slice is already queued (one at a time)
        // Bumped by every Show-for-a-new-conversation, Refresh and Close: a background render or a
        // queued build slice carrying an older value belongs to a view that no longer exists.
        uint64_t _generation = 0;
        std::function<void()> _onClosed; // the page's focus-restore (SetClosedHandler)
    };
}
