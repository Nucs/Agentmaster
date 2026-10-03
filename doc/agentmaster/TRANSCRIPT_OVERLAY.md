# Agentmaster — the Transcript Overlay ("Show Transcript")

A session's conversation, rendered as Markdown, in a reading view laid over the window. Status:
**written 2026-10-03, NOT BUILT** (no compile, no deploy — see §9 for the verification checklist).

## 1. The ask (verbatim)

> I would like you to introduce an overlay ontop of a terminal pane, that is going to show the default
> transcript (with tabs to switch to other levels).
> The overlay will occupy 80% height and 60% width of the window.
> In there rendered markdown of the selected transcript mode in a readable way with scrolling.
> The overlay is Transcript->Show which is the top option, then seperator then the rest.
> In menu items, add below Copy, "Show Transcript" which opens the overlay where ever you are.
> Moving a tab closes the overlay, clicking outside the overlay closes the overloay.

## 2. Where it opens

Every entry opens the overlay over **whatever is on screen** (a terminal tab, the Agent Manager page, the
Sessions page), at the **Default** level:

| Menu | Entry |
|---|---|
| WT tab right-click menu | **Show Transcript** directly below **Copy ▸**; **Copy ▸ Transcript ▸ Show** (session tabs only — both follow the Copy submenu's visibility, `Tab::SetAgentCopyMenuVisible`) |
| Agent Manager session menu (board card ⋯ / right-click, Explorer-tree row, side-panel card — one `_PopulateSessionMenu`) | **Show Transcript** directly below **Copy ▸**; **Copy ▸ Transcript ▸ Show** |
| Explorer-tree EXTERNAL row menu | **Show Transcript** below **Copy Session Id** (disabled with a reason when the row has no id yet) |
| Per-tab badge copy button | **Transcript ▸ Show** |
| Summary panel right-click (per-tab and side panel) | **Show Transcript** below **Copy Summary** |

In every Transcript submenu, **Show** is the first item, then a separator, then the five copy levels.
Labels, tooltip and glyph (`\xE736` ReadingMode) are shared constants in `AgentCopyActions.h`
(`kTranscriptShowMenuLabel`, `kShowTranscriptMenuLabel`, `kShowTranscriptMenuTip`, `kShowTranscriptMenuGlyph`).

## 3. What it looks like

- A card **60% of the window wide and 80% tall**, centered, over a 40%-black backdrop that covers the whole
  page (all of Root's rows). The size comes from a 3×3 star grid (1:8:1 rows, 2:6:2 columns) — no size
  code, it follows every resize through layout.
- **Header**: reading-mode icon · "Transcript" · the session line (title · short id · "Codex") · Copy ·
  Refresh · Close (✕).
- **Tabs**, one per level in the Copy menu's order — **Default · Max · Full · Compact · Minimal** — the
  current one bright with an accent underline. Tooltips are the levels' descriptions
  (`kTranscriptCopyMenu`) plus the digit shortcut. A status line to the right shows progress, then
  "N lines · rendered in X".
- **Body**: one scroll view per level (each keeps its own scroll position; switching back is instant).
- **Keyboard**: Esc closes; 1–5 pick a tab by position; PageUp / PageDown / Home / End / arrows scroll;
  Tab cycles inside the card; the app's tab-switching chords (Ctrl+Tab, …) still work — and close it.
- Always dark (the Agentmaster surface convention), explicit brushes.

## 4. Rendering

1. **Off the UI thread** (`AgentTranscriptOverlay::_LoadLevelAsync`, holding only a weak reference): the
   level's text is exactly what **Copy ▸ Transcript ▸ <level>** copies — `RenderTranscriptTurnsFile` for a
   Claude transcript (local time), and for a Codex rollout `ReadConversationFollowupText` (Minimal /
   Compact) or `ReadConversationText` (Default / Full / Max). Then `ParseMarkdownBlocks`
   (`AgentMaster/MarkdownBlocks.{h,cpp}`, cmark) flattens it into render blocks.
2. **On the UI thread, time-sliced** (`_Pump`): low-priority dispatcher items build blocks into XAML for
   ~12 ms each (at least 16 blocks), then yield. The top of a long transcript shows at once; the window
   keeps answering while the rest lays out. Only the CURRENT level builds; a level left mid-build resumes
   where it stopped. Elements go into chunk panels of 200 so an append never re-measures 10,000 siblings.
3. **Caps** (a pathological session degrades to "the view stops here", never a stall): 8 M characters
   parsed, 25,000 blocks built, 150,000 characters per block (a huge written file). A closing note says
   when the view was cut; **Copy always copies the whole level**.

Measured inputs (this machine, 2026-10-03, the Python reference): the largest session (29.5 MB transcript)
renders to 345 KB / 3.7 k lines at Default and 1.5 MB / 22.6 k lines / 619 code blocks at Max.

### Markdown → XAML rules (`MarkdownBlocks.h` + `_BuildBlockElement`)

- **Flat blocks, full context**: each block carries its quotes and list items outermost-first, with ids.
  The gap above a block goes INSIDE the containers it shares with the block above, so a quote's bar runs
  unbroken through the gap between two paragraphs of one quote, and a new quote starts below the gap.
- **Quotes** draw a 3 px bar. The outermost bar takes the current **speaker's** color, set by the last
  speaker label: `**❯ User**` blue, `**● Claude**` orange, `**✉ name**` purple; a heading (a new turn)
  resets it. The label line itself is tinted the same way; lines folded under it (the Full / Max stats
  line) are dimmed. A paragraph that is entirely italic (notes, stats) is dimmed.
- **Lists**: a marker column whose width fits the list's widest marker, so wrapped and continuation text
  hangs aligned. Bullets •, ◦, ▪ by depth; **ordered lists keep their numbers**. Tight lists are packed.
- **Code blocks**: a dark rounded panel, monospace, wrapping, selectable.
- **Raw HTML is kept as text** (inline: plain; block: monospace dim) — `List<int>` or a quoted
  `<system-reminder>` would otherwise vanish.
- **Soft line breaks are real line breaks** (a prompt typed on three lines shows as three lines), kept as
  `\n` inside one Run — a 10,000-line pasted log is one Run, not 20,000 inline objects.
- **Links**: only `http(s)://` become clickable Hyperlinks; any other scheme renders link-colored text.
- **Images are never fetched**: `[image: alt]`, linked to the URL.
- Every text block is selectable with the WT `TextMenuFlyout` (Copy / Select All) — never the per-thread
  default text flyout (GH#18599); the touch selection flyout is off. Selection does not span blocks.

## 5. Closing

| Trigger | Where |
|---|---|
| Click on the backdrop (outside the card) | the backdrop's `PointerPressed` (a sibling of the card, so presses inside never reach it); the close is deferred one tick (no tree mutation inside a pointer handler) |
| Esc | the card's `KeyDown`, AND page-wide in `TerminalPage::_ManagerPaneNavPreviewKeyDown` (tunneling, on Root) — an Esc must never fall through to a terminal behind the overlay, where it would interrupt claude's turn. An open command palette / suggestions box keeps its own Esc |
| ✕ | the header button |
| Any tab switch, close successor, Agent Manager page shown / left | `_ApplyAgentTabSelection` → `_CloseTranscriptOverlay` |
| Tab move (moveTab action, Move to start/end, a drag's commit) | `_TryMoveTab` — explicit, because moving the already-selected tab raises no SelectionChanged |
| Tab drag starts | `_TabReorderActivate`; a tab leaving the window: `_sendDraggedTabToWindow` |

**Focus**: while open the card refuses a focus move to anything else under the page's Root
(`LosingFocus` → `TryCancel`) — a Manager refresh re-focusing a card or a terminal grabbing focus would
otherwise let keystrokes reach a session hidden behind the overlay. Popups (the text context menu) and
leaving the window are allowed. Because the guard also refuses the focus move a closing tab switch makes,
every close hands focus back through `_FocusCurrentTab(true)` (the focused tab, or the Manager page).

## 6. Logging

- `[nav] transcript-show <sid8> [codex] (from <menu>)` — `tab menu`, `session menu`, `tab badge`,
  `side panel summary`.
- `[nav] transcript-level <sid8> <Level>`, `[nav] transcript-copy <sid8> <Level> chars=N`,
  `[nav] transcript-refresh <sid8> <Level>`, `[nav] transcript-close <sid8> (<reason>)` — reasons
  `esc`, `click outside`, `close button`, `tab switch`, `agent manager page`, `tab move`, `tab drag`,
  `tab moved to another window`.
- `[transcript] <sid8> show <Level>: chars=N blocks=M rendered in Xms` (+ `(capped)` /
  `(parse failed part-way)`), or `: nothing to show (<why>) path=…`.

## 7. Files

- `src/cascadia/TerminalApp/AgentMaster/MarkdownBlocks.{h,cpp}` — the pure flattener (cmark; plain C++,
  no PCH). cmark comes from the vcpkg manifest's `terminal` feature (UIMarkdown's port) and is auto-linked
  into TerminalApp.dll.
- `src/cascadia/TerminalApp/AgentTranscriptOverlay.{h,cpp}` — the overlay (`winrt::implements`).
- `src/cascadia/TerminalApp/TerminalPage.AgentTranscript.cpp` — the page side (build, open, close).
- Wiring: `Tab.{h,cpp}` (`ShowTranscriptRequested`, the item + the submenu's Show), `TerminalPage.cpp`
  (the tab event, the Esc / chord routing, the tear-out close), `TabManagement.cpp` (`_TryMoveTab`),
  `TerminalPage.AgentEngine.cpp` (the funnel + drag closes, the Manager handler), `AgentManagerContent.*`
  (`SetShowTranscriptHandler`, the session + external menus), `AgentTabOverlay.*`
  (`SetShowTranscriptHandler`, the badge submenu + the summary menu), `TerminalPage.AgentObserver.cpp` /
  `TerminalPage.AgentSidePanel.cpp` (the badge / side-panel wiring), `AgentCopyActions.h` (the labels).
- Harness: `tests/tests_markdown.cpp` (`TestMarkdownBlocks`) — compiled by `run-m5-tests.bat` only when
  `obj\x64\vcpkg\x64-windows-static` holds cmark (defines `AM_HAVE_CMARK`); otherwise it reports a skip.

## 8. Known limits / follow-ups

- Text selection is per block (a paragraph, a code panel); the header Copy is the whole-level copy.
- No live refresh: the view is a snapshot; ↻ re-renders (a working session keeps writing).
- No find-in-transcript yet.
- A Codex rollout is plain conversation text parsed as Markdown (Codex has no turns view).
- Not virtualized: a level is fully built (time-sliced) — fine at the measured sizes, capped beyond.

## 9. Verification checklist (nothing here has been built)

1. `tests\run-m5-tests.bat` → `[MarkdownBlocks]` passes (or `[info] skipped` without the vcpkg tree).
2. Lib compile-check, then a dev deploy (user's permission first — CLAUDE.md *Development Rules*).
3. Open from each entry in §2: a Claude tab, a forked tab, a team lead (agents), a Codex tab, an external
   row. Check: Default selected, the 60% × 80% size, the five tabs, scrolling, the status line.
4. Close with Esc, a click outside, ✕, Ctrl+Tab, a click on another tab, a tab drag, Move to start.
   Each time keyboard focus must land back in the terminal (type a key) — or on the Manager page.
5. A Max level of a big session: the top appears at once and the window stays responsive while it fills.
6. hooks.log: the `[nav] transcript-*` and `[transcript] … show …` lines.
