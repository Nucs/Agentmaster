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
// order (Default · Max · Full · Compact · Minimal), an "Expand all" switch for the level's folded tool
// calls, and a scrolling body. It opens at the level the user LAST PICKED, in any window: every tab pick
// is remembered in settings.json (AppSettings::transcriptOverlayLevel, a freshest-disk read-modify-write
// done here) and every open reads it back fresh — Default until a first pick.
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
// FOLDED TOOL CALLS: in every turn the list of tool calls and remarks under "**● Claude**" (and under an
// agent entry's header) — the part Compact leaves out — is folded into ONE summary row ("▸ 4 tool calls:
// Bash ×3, Write · 2 remarks", AgentMaster/MarkdownBlocks.h MdFindToolAreas); the reply under it stays
// open, so the Default / Full / Max views read like Compact with every call one click away. A click on
// the row opens / closes that list; "Expand all" opens every list of the level (and the lists built after
// it). A list with only Compact's own content (answers, compactions, an agent's messages) stays open, and
// a "(no tool calls)" list becomes a one-line note.
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

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
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
        // Show's `level` for "open at the level the user last picked" (AppSettings::transcriptOverlayLevel,
        // read fresh from settings.json at that moment) — what every menu passes.
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
        // conversation discards every rendered level and starts over. Takes keyboard focus (one
        // dispatcher tick later, once the view is laid out). UI thread. No-op before Attach.
        // Parameters:
        //   source — the conversation (see TranscriptOverlaySource).
        //   level  — a TurnsLevel value 0..4 (Minimal..Max; above 4 is clamped), or kRememberedLevel (any
        //            negative value) for the level last picked in any window — read from settings.json
        //            right now, Default when nothing was picked yet or the stored name is unknown. The
        //            menus pass kRememberedLevel. Opening does NOT itself remember anything: only a tab
        //            pick does.
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

        // One detail level's state + view. Indexed by TurnsLevel value (0 Minimal .. 4 Max).
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
            // The chunk currently being filled, and how many elements it holds.
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
            // append to an OPEN fold never re-measures a thousand siblings (_AppendToFold).
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
        };

        // Build every piece of the overlay's XAML (Attach). Throws winrt::hresult_error on a XAML failure.
        void _BuildUi();
        // Make `level` current: drop the left level's pending session jump (it would fire when the reader
        // comes back), tab visuals, view visibility, bring its view up to the current line-wrap mode
        // (_SyncWrapMode — a view laid out under the other mode re-lays out at once, keeping its passage),
        // start its render or resume its build, and (focusBody) move keyboard focus into its scroll view on
        // the next tick.
        void _SelectLevel(int level, bool focusBody);
        // A tab button / digit key: select the level at TAB POSITION `position` (0 == the first tab,
        // Default), log the switch, and REMEMBER the level for the next open in any window
        // (_RememberLevel — on every pick, the tab already showing included). Out-of-range positions are
        // ignored.
        void _SelectTabPosition(size_t position);
        // The level the user last picked (AppSettings::transcriptOverlayLevel, read fresh from
        // settings.json) as a TurnsLevel value; Default when the stored name is empty / unknown or the
        // read fails (logged). UI thread (a small file read, the ColorPickupFlyout idiom).
        static int _RememberedLevel() noexcept;
        // Store `level` as the level the next open uses: a FRESHEST-DISK read-modify-write of just that
        // field (never a stale in-memory copy — a cog Save or another window's pick is never clobbered),
        // skipped when the file already holds it. Best-effort: a failed write (logged) costs only the
        // remembered level, never the switch the user just made.
        // Parameters: level — the TurnsLevel value picked (0..4).
        static void _RememberLevel(int level) noexcept;
        // Create `level`'s scroll view + content stack (once), set to the current line-wrap mode
        // (_wrapLines), and put it in the body.
        void _EnsureLevelView(int level);
        // Show `text` in place of `level`'s content (Loading / Empty), creating the placeholder once.
        void _SetLevelMessage(int level, const std::wstring& text);
        // Start `level`'s background render (state Loading).
        void _StartLoad(int level);
        // The background half: render + parse on a pool thread, then hand the result to
        // _ApplyLevelRender on the UI thread. Static so it holds NO reference to the overlay while off
        // the UI thread — only `weak`, resolved back on the UI thread (the last-ref-on-a-pool-thread
        // teardown gotcha). Never lets an exception escape (a fire_and_forget escape is a process kill).
        static winrt::fire_and_forget _LoadLevelAsync(winrt::weak_ref<AgentTranscriptOverlay> weak,
                                                      winrt::Windows::System::DispatcherQueue dispatcher,
                                                      TranscriptOverlaySource source,
                                                      int level,
                                                      uint64_t generation);
        // Render `level` of `source` into `out` (path resolve, the level text, the block parse). Runs on
        // the pool thread; touches no member. Never throws (contained + logged).
        static void _RenderLevel(const TranscriptOverlaySource& source, int level, LevelRender& out) noexcept;
        // UI thread: adopt a finished render — unless the overlay was retargeted, refreshed or closed
        // since (`generation` mismatch) — and start building its XAML.
        void _ApplyLevelRender(int level, uint64_t generation, LevelRender&& render);
        // Queue one build slice (Low priority), unless one is already queued.
        void _SchedulePump();
        // One build slice: turn blocks of the CURRENT level into XAML for ~kSliceBudget, do the level's
        // pending session jump once its heading exists (a chain link clicked before the build reached it),
        // then queue the next slice if blocks remain. Stale (`generation` mismatch) or closed: does nothing.
        void _Pump(uint64_t generation);
        // Build the level's next block (view.built) — or, at the start of a tool area, its fold row first
        // (then the block goes into the fold's panel), or for an empty area its one-line note (skipping
        // the "(no tool calls)" item) — and advance view.built past what was built. A block of the Session
        // chain list gets its "session N" links (only for sessions with a heading in this level), and a
        // session heading is recorded in view.sessionHeadings for those links.
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
        // Build the bare content of a block (TextBlock / code panel / rule), without containers.
        // Parameters: view / block — the level and the block; sessionRefs — as for _BuildBlockElement.
        winrt::Windows::UI::Xaml::FrameworkElement _BuildBlockContent(LevelView& view, const ::Agentmaster::MdBlock& block, const std::vector<::Agentmaster::MdSessionRef>* sessionRefs = nullptr);
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
        std::vector<winrt::Windows::UI::Xaml::Controls::Button> _tabButtons; // one per kTranscriptCopyMenu entry, in its order
        std::vector<winrt::Windows::UI::Xaml::Controls::TextBlock> _tabLabels; // each tab's label (dim / bright)
        std::vector<winrt::Windows::UI::Xaml::Controls::Border> _tabUnderlines; // each tab's underline (accent when current)
        std::vector<int> _tabLevels; // the TurnsLevel value behind each tab position
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

        std::array<LevelView, 5> _levels{}; // by TurnsLevel value (kTurnsLevelCount)
        TranscriptOverlaySource _source; // what is shown
        int _current = 2; // the current level (TurnsLevel::Default)
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
