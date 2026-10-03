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
// Shape: a card 60% of the window wide and 80% tall, centered over a dimmed backdrop that covers the
// page (the terminal, the Agent Manager page, or the Sessions page — "wherever you are"). The card has a
// header (title · Copy · Refresh · Close), one TAB per transcript detail level in the Copy menu's order
// (Default · Max · Full · Compact · Minimal — Default selected on open), and a scrolling body.
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
    };

    class AgentTranscriptOverlay : public winrt::implements<AgentTranscriptOverlay, winrt::Windows::Foundation::IInspectable>
    {
    public:
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
        //   level  — a TurnsLevel value 0..4 (Minimal..Max); out of range is clamped. The menus open
        //            with Default.
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
        };

        // Build every piece of the overlay's XAML (Attach). Throws winrt::hresult_error on a XAML failure.
        void _BuildUi();
        // Make `level` current: tab visuals, view visibility, start its render or resume its build, and
        // (focusBody) move keyboard focus into its scroll view on the next tick.
        void _SelectLevel(int level, bool focusBody);
        // A tab button / digit key: select the level at TAB POSITION `position` (0 == the first tab,
        // Default) and log the switch. Out-of-range positions are ignored.
        void _SelectTabPosition(size_t position);
        // Create `level`'s scroll view + content stack (once) and put it in the body.
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
        // One build slice: turn blocks of the CURRENT level into XAML for ~kSliceBudget, then queue the
        // next slice if blocks remain. Stale (`generation` mismatch) or closed: does nothing.
        void _Pump(uint64_t generation);
        // The level ran out of blocks: add the closing notes (cut / failed), free the blocks, mark Done.
        void _FinishLevel(LevelView& view);
        // Append one built element to the level's current chunk (opening a new chunk when full).
        void _AppendElement(LevelView& view, const winrt::Windows::UI::Xaml::UIElement& element);
        // Build the XAML for one block, wrapped in its containers (quote bars, list indents + markers),
        // with the gap above it from MdGapAbove placed so shared quote bars run through it.
        winrt::Windows::UI::Xaml::FrameworkElement _BuildBlockElement(LevelView& view, const ::Agentmaster::MdBlock& block, const ::Agentmaster::MdBlock* prev);
        // Build the bare content of a block (TextBlock / code panel / rule), without containers.
        winrt::Windows::UI::Xaml::FrameworkElement _BuildBlockContent(LevelView& view, const ::Agentmaster::MdBlock& block);
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
        winrt::Windows::UI::Xaml::Controls::Border _card{ nullptr }; // the reading card (60% x 80% via the host's star grid)
        winrt::Windows::UI::Xaml::Controls::Grid _headerRow{ nullptr }; // icon · title · session line · Copy / Refresh / Close
        winrt::Windows::UI::Xaml::Controls::TextBlock _subtitleText{ nullptr }; // the session line
        winrt::Windows::UI::Xaml::Controls::Grid _tabsRow{ nullptr }; // the level tabs + the status line
        std::vector<winrt::Windows::UI::Xaml::Controls::Button> _tabButtons; // one per kTranscriptCopyMenu entry, in its order
        std::vector<winrt::Windows::UI::Xaml::Controls::TextBlock> _tabLabels; // each tab's label (dim / bright)
        std::vector<winrt::Windows::UI::Xaml::Controls::Border> _tabUnderlines; // each tab's underline (accent when current)
        std::vector<int> _tabLevels; // the TurnsLevel value behind each tab position
        winrt::Windows::UI::Xaml::Controls::TextBlock _statusText{ nullptr }; // right of the tabs: progress / size / timing
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
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _transparentBrush{ nullptr };
        winrt::Windows::UI::Xaml::Media::FontFamily _monoFont{ nullptr };

        std::array<LevelView, 5> _levels{}; // by TurnsLevel value (kTurnsLevelCount)
        TranscriptOverlaySource _source; // what is shown
        int _current = 2; // the current level (TurnsLevel::Default)
        bool _open = false; // shown
        bool _pumpQueued = false; // a build slice is already queued (one at a time)
        // Bumped by every Show-for-a-new-conversation, Refresh and Close: a background render or a
        // queued build slice carrying an older value belongs to a view that no longer exists.
        uint64_t _generation = 0;
        std::function<void()> _onClosed; // the page's focus-restore (SetClosedHandler)
    };
}
