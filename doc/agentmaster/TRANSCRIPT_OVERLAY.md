# Agentmaster — the Transcript Overlay ("Show Transcript")

A session's conversation, rendered as Markdown, in a reading view laid over the window. Status:
**written 2026-10-03, NOT BUILT** (no compile, no deploy — see §9 for the verification checklist). Same day,
second pass: 85% wide, folded tool calls (§3b), the remembered level (§3a) — also not built; then 90% wide
(third ask, same day, not built). 2026-10-05: the ↵ line-wrap toggle (§3c, fourth ask) — not built; then
the Session chain links (§3d, fifth ask, same day) — not built; then a fork with no messages yet shows the
transcript it was forked from (§3e, a sixth report, same day) — not built. 2026-10-31: the (i) level guide and
the tabs' cheat-sheet tooltips (§3f, the seventh ask) — not built.

## 1. The ask (verbatim)

> I would like you to introduce an overlay ontop of a terminal pane, that is going to show the default
> transcript (with tabs to switch to other levels).
> The overlay will occupy 80% height and 60% width of the window.
> In there rendered markdown of the selected transcript mode in a readable way with scrolling.
> The overlay is Transcript->Show which is the top option, then seperator then the rest.
> In menu items, add below Copy, "Show Transcript" which opens the overlay where ever you are.
> Moving a tab closes the overlay, clicking outside the overlay closes the overloay.

The follow-up, same day:

> 1. Make wider to 85%
> 2. The area below "Claude · * Bash — Read memory notes on past release CPU and GPU issues: cat "…"; echo =… · ok"
>    all bash area (not last turn text content) needs to be collapsible so its closer looking to Compact version.
> 3. If user picks Compact, persist the Compact globally as the picked and next transcript opened is going
>    to be opened at Compact tab in any window.

And the third:

> The card is now 85% of the window wide. do 90%.

And the fourth (2026-10-05):

> On top-right add a button for breakline, by default true, but if disabled then text overflowing right
> border is invisible/hidden
> Have it's state persist across all windows

And the fifth (2026-10-05), with a screenshot of a five-session chain — the "Session chain" list, then the
heading "Session 1 of 5 · 26c59294 · started Oct 01 21:25":

> the sessions N in this list should be hyperlinks to viewing the title (session row as in the image appears
> at the top when session 1 is clicked.

And a sixth, a report (2026-10-05), about the release tab "sidemenu tooltip (fork) this one!" (session
`1b114791`, a fork of `e897bda5` that had no message yet):

> If I fork a session and don't send a prompt and try to see a summary (requires session id) or open Show
> Transcript then nothing shows: "this one!" in the title so you can find the tab

And the seventh (2026-10-31), after a session compared the five levels in a table (what each shows, what every
level shows, what no level shows):

> add a (i) after the levels tabs so user can see what each tab and the comparison you did
> and tooltips showing info of each level tab that will show like a cheatsheet.

## 2. Where it opens

Every entry opens the overlay over **whatever is on screen** (a terminal tab, the Agent Manager page, the
Sessions page), at the level the user **last picked**, in any window (§3a — Default until a first pick):

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

- A card **90% of the window wide and 80% tall**, centered, over a 40%-black backdrop that covers the whole
  page (all of Root's rows). The size comes from a 3×3 star grid (1:8:1 rows, 5:90:5 columns) — no size
  code, it follows every resize through layout. (60% wide at first, then 85%, then 90% — the follow-ups.)
- **Header**: reading-mode icon · "Transcript" · the session line (title · short id · "Codex") · then, at
  the top-right, **↵ Wrap** (§3c) · Copy · Refresh · Close (✕).
- **Tabs**, one per level in the Copy menu's order — **Default · Max · Full · Compact · Minimal** — the
  current one bright with an accent underline, then the **(i) level guide** (§3f). Each tab's tooltip is that
  level's cheat sheet (§3f), with its digit key. A status line to the right shows progress, then
  "N lines · rendered in X". At the far right, **Expand all / Collapse all** (link-colored) — shown only
  while the current level has folded tool calls (§3b).
- **Body**: one scroll view per level (each keeps its own scroll position; switching back is instant).
- **Keyboard**: Esc closes; 1–5 pick a tab by position; PageUp / PageDown / Home / End / arrows scroll;
  Tab cycles inside the card (through the fold rows too — Enter / Space opens one — and the Session chain
  links, §3d); the app's
  tab-switching chords (Ctrl+Tab, …) still work — and close it.
- Always dark (the Agentmaster surface convention), explicit brushes.

### 3a. The remembered level

- Every tab pick — a click or a digit key, **the tab already showing included** — is written to
  settings.json as `AppSettings::transcriptOverlayLevel`: the level's NAME (`minimal` / `compact` /
  `default` / `full` / `max`, `TurnsLevelName`), so a future reorder of the levels can't remap a saved
  choice. The write is a **freshest-disk read-modify-write of that one field**
  (`AgentTranscriptOverlay::_RememberLevel`, the `ColorPickupFlyout` idiom), skipped when the file already
  holds it — never a stale in-memory copy over a cog Save or another window's change.
- Every open (`Show(source, kRememberedLevel)` — what every menu passes) **reads it fresh from disk**
  (`_RememberedLevel`), so a pick in one window decides the next open in every window, and survives a
  restart. An empty / unknown name (absent key, hand edit) or a failed read opens at **Default**.
- Opening never remembers anything; only a pick does. The cog form never edits the field, so **both cog-Save
  preserve blocks restore it from disk** (`AgentManagerContent.Settings.cpp`, the page's settings handler
  in `TerminalPage.AgentEngine.cpp`) — a Save made with the modal open can't reset a pick.

### 3b. Folded tool calls

In the turns view every speaker part is a header (`**● Claude**`, or an agent entry's
`**● finder-a** · teammate · …`), then ONE bullet list — the tool calls, the remarks between them, the
messages that reached an agent, its brief, compaction markers, with each call's output / background /
answer lines nested under it — then the reply quote. At Default, Full and Max the overlay **folds each such
list into one summary row** and keeps the reply open (Compact and Minimal never fold — below):

    ● Claude
      ▸ 4 tool calls: Bash ×3, Write · 2 remarks
    │ Done: 2 files; the scan failed …

- The areas are found off the UI thread with the parse (`MdFindToolAreas`, `MarkdownBlocks.h`): a
  top-level list whose first item starts right after a **speaker header** — a top-level paragraph whose
  first visible character is `●` in a bold run. The "Session chain" list (under a plain paragraph) and any
  list inside a quoted message are never areas.
- Items are counted by their leading mark (`TranscriptTurns.cpp` `EventLines`): a bold tool name = a call
  (the name is the bold run), `💬` a remark, `✉` a message, a top-level `❯` a prompt / brief, `🗜` a
  compaction, a `❯` nested under a call an answer. `MdToolAreaSummary` names every non-zero count —
  "N tool calls: Bash ×3, Write, … +N more · 2 remarks · 1 answer · 1 compaction" (5 tool names at most,
  most-called first, a name over 40 characters cut with an ellipsis).
- **Folded** (`MdToolAreaFoldable`): a list holding at least one call or remark. A list of only messages /
  prompts / compactions / answers stays open.
- **Only from Default up** (`_RenderLevel` looks for areas at Default, Full and Max only, 2026-10-31): since
  `transcript_turns` v10 Compact lists every call as a one-line REASON (`**Bash**: List the files`) — calls by
  the rule above, but the very thing that level exists to show at a glance — so Compact never folds, and
  Minimal has no lists. (Before v10 Compact's lists held no call, so it never folded by content.)
- A list that is only "(no tool calls)" becomes a one-line dim note ("no tool calls"), lined up with the
  fold rows' text.
- A click on a row (or Enter / Space) opens / closes that list (chevron ▸ / ▾). **Expand all** opens every
  fold of the current level and makes the folds built after it start open; **Collapse all** undoes it. Each
  level keeps its own Expand-all state (kept across ↻ Refresh); every open starts folded.
- Build mechanics (`_BuildNext` / `_BuildFold`): the summary row and a Collapsed panel go into the level's
  chunks; the area's blocks go into the panel, itself chunked by 200 (one real turn held 243 calls — over
  1,000 elements at Max), so an append to an open fold never re-measures a thousand siblings. A collapsed
  panel is never measured, which makes a folded Default / Full / Max view cheaper to lay out than before.
- Checked without a compile (2026-10-03): a Python port of the flattener's shape (markdown-it, CommonMark)
  plus a line-by-line port of the three area functions agreed with every expectation in
  `tests_markdown.cpp`, classified every item of all 55 goldens and of the 12 most recent real sessions at
  Default and Max (zero unknown items), and folded nothing at Compact / Minimal.

### 3c. Line wrap (the ↵ toggle)

- The first of the header's buttons, at the top-right, is **↵** — Segoe Fluent Icons `\xE751` (ReturnKey),
  the summary panel's wrap symbol drawn from the header's own icon font. That font's 1-em line box keeps the
  toggle exactly as tall as Copy / Refresh / Close; Segoe UI Symbol's ↵ (the summary panel's) would be
  ~4.6 px taller at 14 px and grow the header row.
- **ON (the default)**: a tinted face with an accent hairline and a bright glyph. A line wider than the body
  continues on the next line — the view as it was before the toggle existed.
- **OFF**: no face, a dim glyph. Every line keeps its length, and the part past the body's right edge is
  **hidden** — not a sideways scroll away. Real line breaks still break, and the header's Copy still copies
  every character. Code panels, quote bars and rules stretch to the content's width, so their right edges
  are cut off with the text.
- The tooltip and the screen-reader name (a Button has no toggle state) say which mode is on and what a
  click does.
- **Mechanism — one property per level view** (`ApplyWrapMode`): ON sets the scroll view's horizontal
  scroll bar to `Disabled`, which hands the content the viewport's width, so every TextBlock wraps to it.
  OFF sets it to `Hidden`, which measures the content at INFINITE width and shows no bar; the scroll
  presenter clips at its edge. Read off the WinUI source (`ScrollViewer::UpdateCanScroll` sets
  `CanHorizontallyScroll` from the bar visibility alone; `ScrollContentPresenter::MeasureOverride` then
  gives the child an infinite width; `UpdateClip` clips it). The horizontal scroll MODE stays `Disabled`
  (set once in `_EnsureLevelView`): touch, the wheel and the keyboard all reach the view through
  DirectManipulation, which that mode keeps from panning sideways.
- **Global**: `AppSettings::transcriptOverlayWrapLines` (default true). Every click writes it with the same
  freshest-disk read-modify-write as the level (`_RememberWrapLines`); every open reads it back
  (`_RememberedWrapLines`); both cog-Save preserve blocks restore it. So the choice made in one window is
  the one every window opens with, and it survives a restart. An overlay that is already open in another
  window keeps its own state until it is opened again (there is no live broadcast).
- **The reader stays on the same passage.** `FindScrollAnchor` finds the deepest element crossing the top
  edge of the view — it sums the earlier siblings' `DesiredSize` down through the chunk and fold stacks; a
  folded list's `DesiredSize` is 0 — and how far into that element the edge sits. After the switch,
  `UpdateLayout` lays the view out at once, and `RestoreScrollAnchor` scrolls the same element back to the
  top at the same fraction (`ChangeView` clamps to the extent, so the layout must come first — the board's
  recipe).
- Only the level on screen re-lays out at a click. A hidden level catches up, with its own passage kept,
  when it is shown again (`_SelectLevel` → `_SyncWrapMode`): a collapsed view is not laid out, so the new
  place could not be read, but it keeps the layout and offset it had when it was last shown, and those two
  still agree.
- Unlogged, like the folds and Expand all — a pure view toggle (the CLAUDE.md logging convention).

### 3d. Session chain links

A transcript that continues earlier sessions (a fork, a plan restart — `TranscriptTurns`' chain,
`ChainBlock` / `SessionHeader`) opens with the chain list, and each session's part starts with a heading:

    Session chain, oldest first (the last one is this session):
    • session 1 · 26c59294-… — started Oct 01 21:25
    • session 2 · f48cd627-… · “its title” — forked from session 1 on Oct 04 08:09
    …
    Session 1 of 5 · 26c59294 · started Oct 01 21:25

- **Every "session N" in the list is a link** (link-colored, like the transcript's other links). A click
  scrolls the view so session N's heading is at the top, 8 px below the edge (`kSessionJumpGap`), with no
  animation. The links: each row's own label ("session 2"), "forked from session N", "restarted from
  session N", and the gone-parent row's "session 1 was restarted from it".
- **Never a link**: "this session" (no number); anything inside the row's quoted title (“…” — a title may
  say "fix session 9"); a code span (the ids); text outside the list — the headings' own "forked from
  session N" included, since the headings are the targets.
- **Only a session with a heading gets a link.** `TranscriptTurns` heads a session from its first turn, so
  a session with no turns of its own has no heading: a fork that was forked again at once, or "this
  session" before its first message (2 of 25 real chains, 2026-10-05). Its row label stays plain text. A
  one-session chain (a plan restart whose parent transcript is gone) has no headings at all, so nothing in
  it links.
- **A click before the heading is built**: the build is time-sliced (§4), and a session's heading can be
  thousands of blocks down. The click is kept as the level's pending jump and done by the build slice that
  builds the heading (`_Pump`). A newer click replaces it. A heading past the 25,000-block cap never comes,
  so the end of the build drops the pending jump. A switch to another level drops it too (`_SelectLevel`):
  only the level on screen builds, so the jump would otherwise fire when the reader comes back, scrolling
  away from wherever they are reading by then.
- **Mechanics**:
  - `MdFindSessionChain` (`MarkdownBlocks.h`) runs off the UI thread with the parse, once per level. The
    list is the top-level list right under the top-level paragraph whose first run is the bold "Session
    chain". Each session's target is its FIRST top-level heading that starts "Session N of M"
    (`MdSessionHeadingNumber`), so a heading a user typed into a quoted message never counts. N is
    1..4096 (`kMdMaxSessionNumber`).
  - `_BuildNext` scans only the list's rows (`MdFindSessionRefs`, the plain runs only), keeps the
    mentions whose session has a heading, and records each session heading's element as it builds it
    (`LevelView::sessionHeadings`).
  - `_AppendSessionLinks` splits a run around its mentions into plain Runs and Hyperlinks. A link has no
    NavigateUri; its Click calls `_JumpToSession` through a weak reference, and its screen-reader name is
    "Go to session N". A Hyperlink is a tab stop, so Tab reaches the links.
  - `_ScrollToSessionHeading`: `UpdateLayout` (a heading built in this very slice has no position yet,
    and `ChangeView` clamps to the extent) → the heading's top in the content's coordinates
    (`TransformToVisual`) → `ChangeView`. It then moves keyboard focus to the scroll view: the clicked
    link, left focused and scrolled out of view, would otherwise be brought back into view (the scroll
    view's BringIntoViewOnFocusChange) the next time focus returns to it, e.g. on a window activation. It
    contains its own failures: a pending jump runs inside a build slice, and a failed scroll must never
    stop the build.
- Each level has its own links and targets: each level's blocks are scanned on their own, so a link always
  points into the level it is drawn in. (In the goldens and the real chains above, every level heads the
  same sessions.)
- Unlogged, like the folds and Expand all — pure view navigation (the CLAUDE.md logging convention).
- Checked without a compile (2026-10-05): a Python port (markdown-it) of the flattener's shape, the three
  helpers and the overlay's link rule passed the inline fixture and the edge cases of `tests_markdown.cpp`,
  the 15 chain goldens (a4 and b2: every row label + every "from session N"; b3: nothing) and the 40 other
  goldens (no chain, no link), and 27 renders of 25 real chained transcripts (up to 5 sessions): every row
  whose session has a heading links its label, and nothing inside a title links.

### 3e. A fork with no messages yet

- **What was wrong**: `claude --resume <src> --fork-session --session-id <fork>` writes no `<fork>.jsonl`
  until the fork's first message (CLAUDE.md Gotchas). The overlay resolved only the fork's own id, so it
  said "Nothing to show yet — this session has not been prompted". Read-only check on the reported tab: the
  record has `forkParentId: e897bda5…`, `projects\*\1b114791….jsonl` does not exist, and the source's
  transcript does.
- **Now**: the overlay renders the SOURCE's transcript under one dim italic line: *No messages in this fork
  yet. Below is the transcript of session e897bda5, the session it was forked from.* Until its first
  message, the fork holds that conversation.
- **One resolver for every display surface**: `ResolveClaudeDisplayTranscript(id, forkParentId)`
  (`AgentMaster/ClaudeSpawn.{h,cpp}`) returns the session's own transcript, else its fork source's, flagged
  with the source id. The note is `ForkSourceTranscriptNote`. The same pair serves:
  - **Copy ▸ Transcript ▸ <level>** (`CopyTranscriptTurnsAsync`): the same note in front, so Show and Copy
    stay one text.
  - **Copy ▸ Summary** (`CopySummaryAsync`), the older copy codes 5 / 8 (`CopyConversationAsync`), the
    **Agent Manager's Summary pane** (`_LoadSummaryForSession`) and the **tab hover card**
    (`_EnsureTabTooltipSummary`): no note — they summarize the source silently, as the per-tab / side-panel
    summary panel has done since 88cb5e415.
- **The fork link comes from the registry** (`SessionInfo::forkParentId`, kept until the fork's first
  content-producing hook). The overlay reads it in `_ShowTranscriptOverlay`, the one function every menu
  goes through; the copy menus and the summaries read it from the record they already hold.
- **The fork's own transcript wins.** A ↻ Refresh re-resolves, so after the fork's first message it shows the
  fork's own transcript. The hover card never caches a fallback path (the panel never did), so its next
  hover switches too.
- Claude only: a never-prompted Codex fork has no rollout either, but its source rollout id is not kept.
- Logged on the existing lines: `[transcript] <sid8> show <Level>: … rendered in Xms from the fork source
  <src8>'s transcript (this fork has no messages yet)`, and `… copy <level>: chars=N rendered in Xms from the
  fork source <src8>'s transcript`.

### 3f. The level guide (i) and the tab cheat sheets

- **The (i)** sits right after the last level tab (inside the tabs' own stack, so it always follows them):
  Segoe Fluent Icons' Info glyph (`\xE946`), dim, lined up with the tab labels. Its tooltip says what it
  opens; its screen-reader name is "What each level shows". A click, Enter or Space opens its **Flyout** (the
  button's own `Button::Flyout`, placed `BottomEdgeAlignedLeft`).
- **The panel** (`_BuildLevelGuidePanel`, rebuilt on every `Opening`, so it always follows the tab on screen):
  - "What each level shows" and one line on reading it (each level shows everything to its left, and more).
  - **The comparison table**: a row per thing a transcript can show, a column per level LEAST detailed first
    (Minimal · Compact · Default · Full · Max — the check marks rise as a staircase), each column headed by
    its label and "key N" (the digit that picks its tab: the tabs run in the Copy menu's order, so the key
    is looked up, not derived from the column). ✓ where the level shows the row, a dim · where it does not.
    The current level's column is tinted with the accent (~15%) and its head drawn in the accent; every
    other row carries a faint wash (~5%), so a row is easy to follow across.
  - **Also in every level** (the turn headers, what was typed while Claude worked, teammates' messages, the
    sessions it was forked or restarted from, the history before each compaction) and **In no level**
    (Claude's thinking, tool output past its first line, an Edit's text, images, reminders / skill text /
    hook output / the compaction summary, rewound prompts and filler lines, long arguments of other tools).
  - A note on Codex (no turns view), and a footer: keys 1–5, the remembered level, "hover a tab".
  - Sizes: up to 780 × 640 DIPs (a Flyout's default 456 is too narrow for six columns); it scrolls past that,
    and the Flyout keeps it inside the window. Dark with explicit brushes through `FlyoutPresenterStyle` —
    a Flyout renders in the popup root, where the card's `RequestedTheme` does not reach.
- **Each tab's tooltip** is that level's cheat sheet (`TranscriptLevelTip`), through the shared tip host
  (`AgentSetTip`) like every other tip:

      Compact (key 4)
      Everything in Minimal, plus:
        ✓ slash commands you ran outside a turn
        ✓ compaction markers
        ✓ your answers to Claude's questions
        ✓ each agent's brief and final report
        ✓ every tool call, with the reason Claude gave
      Leaves out:
        · each call's arguments and outcome (from Default)
        · … (each with the level that first shows it)
      The ⓘ after the tabs compares every level.

  Minimal says "Shows:" instead of "Everything in …, plus:"; Max says "Leaves out nothing another level
  shows." The Copy menu keeps its one-line descriptions (`kTranscriptCopyMenu`); the tabs no longer use them.
- **One table** behind both, `AgentMaster/TranscriptLevelGuide.h` (pure, harness-tested):
  `kTranscriptLevelGuide` — each row's panel text, its tooltip brief, its FIRST level (the levels are
  cumulative, so one level places a row in all five columns) and the `TurnsOptions` member that decides it —
  plus `kTranscriptLevelGuideEveryLevel`, `kTranscriptLevelGuideNoLevel` and the Codex note.
  `TestTranscriptLevelGuide` requires `TurnsOptionsForLevel` to turn each row's option on at exactly its first
  level and above, every ladder feature to have a row, and every level to add one — so the guide can never
  promise what a level does not render. ⚠ A level label lives twice (the menu table for the tabs, the guide
  for the tooltips and the column heads): rename both.
- **Esc closes the guide first.** An open guide closes alone (`DismissLevelGuide`), and the next Esc closes
  the overlay — on both Esc paths, the card's `KeyDown` and the page's tunneling handler
  (`TerminalPage::_DismissTranscriptLevelGuide`), since which one sees the key depends on where focus sits.
  A click outside the guide closes only the guide (a light-dismiss flyout eats the press that dismisses it).
  `Close` hides an open guide (a popup outlives its collapsed owner otherwise). Keyboard focus moves into the
  guide (the focus guard lets a move into a popup through) and back to the (i) when it closes.
- Unlogged, like the folds and the wrap toggle — a pure view action; a failed build of the panel or a failed
  hide is an `[exc]` line (`AgentTranscriptOverlay level guide` / `… dismiss level guide`).

### 3g. Compact lists every tool call with its reason (transcript_turns v10)

The eighth ask (2026-10-31):

> I want to improve Compact level to include 'reason' column for tool calls and having "* Bash: <reason>". On
> >Compact level, show as we do now.

- **What Compact shows now**, in every turn and every agent entry, in order with the answers and compactions:

      ● Claude
      - **Bash**: Count lines of transcript sources and grep messaging doc
      - **Read**: `TRANSCRIPT_OVERLAY.md`
      - **Agent**: Finder A: scan the diff · → ● finder-a
      - **Bash**: Run the old script
        - ❯ rejected — the user said: **use new.py instead**
      - ❯ Which island first? → **A**
      > the final reply

  - The reason is the call's own `description` — what Claude wrote to explain it (Bash, Agent / Task, any tool
    that takes one), whitespace-collapsed, at most 200 characters.
  - A call without one shows what it acted on: a file's NAME (Read / Write / Edit / MultiEdit / NotebookEdit;
    the path is in Default), a pattern (Grep, Glob), a query (ToolSearch, WebSearch), a URL, a skill, "N items"
    (TodoWrite), "→ to: what" (SendMessage), the command's first 80 characters (a Bash without a
    description); else the bare tool (`**mcp__catalog__catalog_whereis**`).
  - AskUserQuestion gets no line of its own: its answer line names the question, as before. A reply to any other
    call (a rejection with words) nests under that call's line, as it does under a full call line.
  - Default, Full and Max are unchanged, byte for byte: a full call line already carries the description.
- **Where it comes from**: the reference `transcript_turns.py` v10 (feature `reasons`, on the ladder from
  `--compact` up, shown only while the call list is off; a level never turns on a feature newer than its
  `--preset`, so `--preset v9 --compact` is v9's view), ported to `TranscriptTurns` (`TurnsOptions::reasons`,
  `OpReason` / `OpSubject` / `BaseName`); the port's goldens moved to `--preset v10` (only the 9 Compact
  goldens with calls changed).
- **Every surface follows the level**: Copy ▸ Transcript ▸ Compact, Show Transcript's Compact tab (which never
  folds, §3b), Telegram's `/transcript compact` and All Screen's Compact (with File Diffs, each diff nests under
  its reason line; the picker hint reads "+ call reasons & answers"). **Except the AI search's `read`**
  (`SessionSearchTools`, Compact by default): it keeps `reasons` off — its eval-scored prompt describes the v9
  compact view, and its 6,000 characters per session would cover less of each conversation; turning them on is a
  prompt change, which needs a new eval (SESSIONS.md §9.15).
- **Size** (lines · KB, v9 → v10; Default for scale): a 14-turn session 444 · 32 → 1,021 · 62 (1,148 · 136); a
  2-turn one 110 · 12 → 606 · 36 (618 · 84); a 6-turn one 329 · 29 → 1,125 · 67 (1,199 · 137). About as many
  lines as Default (one per call), about half its bytes. In the overlay Compact can be the LONGER view: Default
  folds its calls (§3b), Compact shows every reason line.
- The level guide (§3f) gained the row, and its Default rows say "each call's arguments and outcome" and "when
  background tasks finished" (an agent's calls show at Compact now, as reason lines).

## 4. Rendering

1. **Off the UI thread** (`AgentTranscriptOverlay::_LoadLevelAsync`, holding only a weak reference): the
   level's text is exactly what **Copy ▸ Transcript ▸ <level>** copies — `RenderTranscriptTurnsFile` for a
   Claude transcript (local time; for a fork with no messages yet, its source's transcript under the §3e
   note), and for a Codex rollout `ReadConversationFollowupText` (Minimal /
   Compact) or `ReadConversationText` (Default / Full / Max). Then `ParseMarkdownBlocks`
   (`AgentMaster/MarkdownBlocks.{h,cpp}`, cmark) flattens it into render blocks, `MdFindToolAreas`
   marks the tool-call lists to fold (§3b), and `MdFindSessionChain` finds the Session chain list and the
   session headings its links point to (§3d).
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
- **Code blocks**: a dark rounded panel, monospace, wrapping while line wrap is ON (§3c), selectable.
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
| Esc | the card's `KeyDown`, AND page-wide in `TerminalPage::_ManagerPaneNavPreviewKeyDown` (tunneling, on Root) — an Esc must never fall through to a terminal behind the overlay, where it would interrupt claude's turn. An open command palette / suggestions box keeps its own Esc. An open level guide (§3f) closes first, on either path; the next Esc closes the overlay |
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
  `(parse failed part-way)`, + ` from the fork source <src8>'s transcript (this fork has no messages yet)`
  for §3e), or `: nothing to show (<why>) path=…`. A Copy ▸ Transcript of such a fork ends its
  `[transcript] <sid8> copy <level>: chars=N …` line with ` from the fork source <src8>'s transcript`.
- A level pick is the `transcript-level` line (the remembered-level write adds no line; a failed write is
  an `[exc]` line). Opening / closing a fold, Expand all, the ↵ line-wrap toggle and a Session chain link
  click are pure view actions and are deliberately unlogged (the CLAUDE.md logging convention); a failed
  wrap-state read or write, or a failed re-layout, is an `[exc]` line (`AgentTranscriptOverlay remembered
  wrap` / `remember wrap` / `sync wrap mode` / `find scroll anchor` / `restore scroll anchor`), and so is a
  failed session jump (`AgentTranscriptOverlay session jump`, the view stays where it was; `AgentTranscriptOverlay
  session link`, the click handler's belt) or a failed chain scan (`MdFindSessionChain` / `MdFindSessionRefs`
  — what was found so far still links).

## 7. Files

- `src/cascadia/TerminalApp/AgentMaster/MarkdownBlocks.{h,cpp}` — the pure flattener (cmark; plain C++,
  no PCH). cmark comes from the vcpkg manifest's `terminal` feature (UIMarkdown's port) and is auto-linked
  into TerminalApp.dll.
- `src/cascadia/TerminalApp/AgentTranscriptOverlay.{h,cpp}` — the overlay (`winrt::implements`).
- `src/cascadia/TerminalApp/AgentMaster/TranscriptLevelGuide.h` — the level guide's one table (§3f): the
  (i) panel's rows, every-level and no-level lists, the Codex note, `TranscriptLevelTip` (a tab's cheat sheet).
  Harness: `tests/tests_turns.cpp` (`TestTranscriptLevelGuide`: each row's option against
  `TurnsOptionsForLevel`, the ladder covered, the order, no em dash, the tips' shape). The page's Esc seam:
  `TerminalPage::_DismissTranscriptLevelGuide` (`TerminalPage.AgentTranscript.cpp`, called from
  `_ManagerPaneNavPreviewKeyDown`).
- `src/cascadia/TerminalApp/TerminalPage.AgentTranscript.cpp` — the page side (build, open, close; reads
  the fork link for §3e).
- `src/cascadia/TerminalApp/AgentMaster/ClaudeSpawn.{h,cpp}` — `ResolveClaudeDisplayTranscript` +
  `ForkSourceTranscriptNote` (§3e), shared with the copy actions (`AgentTabOverlay.Actions.cpp`,
  `AgentTabOverlay.Internal.h`), the Manager's Summary pane (`AgentManagerContent.AutoTesting.cpp`) and the
  tab hover card (`TerminalPage.AgentObserver.cpp`). Harness: `tests/tests_spawn_sched.cpp`
  (`TestSpawnBuilders`: the fallback, the own-transcript win, no link / self link / gone source / empty id,
  the note's shape).
- Wiring: `Tab.{h,cpp}` (`ShowTranscriptRequested`, the item + the submenu's Show), `TerminalPage.cpp`
  (the tab event, the Esc / chord routing, the tear-out close), `TabManagement.cpp` (`_TryMoveTab`),
  `TerminalPage.AgentEngine.cpp` (the funnel + drag closes, the Manager handler), `AgentManagerContent.*`
  (`SetShowTranscriptHandler`, the session + external menus), `AgentTabOverlay.*`
  (`SetShowTranscriptHandler`, the badge submenu + the summary menu), `TerminalPage.AgentObserver.cpp` /
  `TerminalPage.AgentSidePanel.cpp` (the badge / side-panel wiring), `AgentCopyActions.h` (the labels).
- The remembered level and the line-wrap state: `AgentMaster/SessionModels.h`
  (`AppSettings::transcriptOverlayLevel`, `AppSettings::transcriptOverlayWrapLines`),
  `AgentMaster/Persistence.cpp` (the `transcriptOverlayLevel` and `transcriptOverlayWrapLines` keys), and
  the two cog-Save preserve blocks (`AgentManagerContent.Settings.cpp`, `TerminalPage.AgentEngine.cpp`).
- Harness: `tests/tests_markdown.cpp` (`TestMarkdownBlocks`, incl. the tool areas over an inline fixture and
  five goldens, and the Session chain links over an inline fixture, the edge cases and four chain goldens —
  its `OverlaySessionLinks` mirrors `_BuildNext`'s link rule, so keep the two in step) — compiled by
  `run-m5-tests.bat` only when `obj\x64\vcpkg\x64-windows-static` holds cmark
  (defines `AM_HAVE_CMARK`); otherwise it reports a skip. `tests/tests_persistence.cpp` pins both settings'
  round trips and their defaults ("default" / true).

## 8. Known limits / follow-ups

- Text selection is per block (a paragraph, a code panel); the header Copy is the whole-level copy.
- No live refresh: the view is a snapshot; ↻ re-renders (a working session keeps writing).
- No find-in-transcript yet.
- A Codex rollout is plain conversation text parsed as Markdown (Codex has no turns view) — it has no
  speaker headers, so nothing folds.
- Not virtualized: a level is fully built (time-sliced) — fine at the measured sizes, capped beyond. A
  folded list's elements are built too (only their layout is skipped); building them lazily on first open
  would make a big Max level faster still, at the cost of an "Expand all" that builds hundreds of blocks
  at once.
- Which folds are open, and Expand all, are not remembered across opens (every open starts folded); only
  the level and the line-wrap state are remembered.
- A line-wrap click updates the overlay it was made in; an overlay already open in another window keeps
  its state until it is opened again (no cross-window broadcast — each window has at most one overlay, and
  the next open there reads the new state).
- With line wrap OFF, the hidden part of a line is still in its text block: the block's right-click Select
  All + Copy take it, and the header's Copy takes the whole level. Turning wrap back on is the way to READ
  it — the view never scrolls sideways.
- Session chain links (§3d) are drawn only in the chain list: a heading's "forked from session N" and any
  "session N" in the conversation stay plain text. A session with no turns of its own has no heading, so its
  row label stays plain.
- A session near the END of a level can't reach the top edge: `ChangeView` clamps to the extent, so a jump
  to a short last part (often "this session") shows its heading lower down, with the end of the level below
  it.
- A pending jump (a click before the heading is built) is dropped when the build ends without the heading
  (cut by the 25,000-block cap) and when the reader switches to another level; the click has to be made
  again.
- There is no "back" after a jump; Home (the scroll view has keyboard focus after a jump) returns to the
  list at the top.
- A fork with no messages yet (§3e) shows its source's WHOLE transcript. A RUNNING fork holds only the
  source's lines written before its claude started, so turns the source added after that show although the
  fork does not have them. A dormant fork (after a restart or a Deactivate) re-forks from the source's current
  transcript when it wakes, so for it the view is exact. A cut at the fork's process start is possible
  (`AnalyzeSessionTranscript` takes a byte limit; the turns view would need one) but is not done.
- The Agent Manager's Summary pane caches the fork's summary by (id, last activity): a source that grows
  while the fork stays unprompted does not refresh it. The fork's first message does.
- Codex forks (§3e) are not covered, and neither is alt+↑ / alt+↓ prompt navigation in a fresh fork's
  terminal (it reads the fork's own transcript, so it finds no prompts until the first message).

## 9. Verification checklist (nothing here has been built)

1. `tests\run-m5-tests.bat` → `[MarkdownBlocks]` passes (or `[info] skipped` without the vcpkg tree).
2. Lib compile-check, then a dev deploy (user's permission first — CLAUDE.md *Development Rules*).
3. Open from each entry in §2: a Claude tab, a forked tab, a team lead (agents), a Codex tab, an external
   row. Check: the remembered level selected (Default on a first run), the 90% × 80% size, the five tabs,
   scrolling, the status line.
4. Close with Esc, a click outside, ✕, Ctrl+Tab, a click on another tab, a tab drag, Move to start.
   Each time keyboard focus must land back in the terminal (type a key) — or on the Manager page.
5. A Max level of a big session: the top appears at once and the window stays responsive while it fills.
6. hooks.log: the `[nav] transcript-*` and `[transcript] … show …` lines.
7. Folds: at Default every turn reads "● Claude · ▸ N tool calls: … · reply"; a click opens one list, a
   second click folds it; Expand all / Collapse all flip every list of the level; a "(no tool calls)" turn
   shows the dim note; the Compact and Minimal tabs fold nothing; an agent entry folds its own list.
8. Remembered level: pick Compact, close, reopen from another window → Compact; restart the app → still
   Compact; open the cog, Save → still Compact (`transcriptOverlayLevel` in settings.json).
9. Line wrap: the ↵ toggle is the first button at the top-right, the same size as Copy / Refresh / Close
   (the header row must not grow), tinted while ON. Scroll halfway down a Max level with long lines, click
   it: lines stop wrapping, the part past the right edge is hidden (no horizontal bar, no sideways scroll by
   wheel, touch or arrow keys), code panels run off the edge, and the view still shows the same passage.
   Switch to another level (it catches up, keeping its own place), click again to wrap. Close, reopen in
   another window → the same state; restart → still; a cog Save → still (`transcriptOverlayWrapLines` in
   settings.json). Hover the toggle: the tooltip names the mode.
10. Session chain links: open a session forked several times (the screenshot's five-session chain). In the
    "Session chain" list every "session N" and every "from session N" is link-colored; "this session" and
    the quoted titles are not. Click "session 1" → the "Session 1 of 5" heading sits at the top of the view.
    Open the Max level of a big chain and at once click the last session's link while the status line still
    says it is building → the view jumps when that heading is built (not before), unless you switch levels
    first. A session with no turns of its own (a fork forked again at once) has a plain label. Tab reaches
    the links; check that Enter follows one. After a jump, switch to another window and back → the view
    stays on the heading (focus moved to the scroll view).
11. A fork with no messages yet (§3e): fork a session (tab menu → Fork session) and send nothing. Show
    Transcript → the dim note "No messages in this fork yet. Below is the transcript of session <src8>…",
    then the source's turns, at every level. Copy ▸ Transcript ▸ Default → the same text with the note on
    top. Copy ▸ Summary, the Agent Manager's Summary pane (select the fork), the tab hover card and the
    summary panel all show the source's messages. Send the fork a message, then ↻ Refresh the overlay → the
    fork's own transcript (with its Session chain), no note. hooks.log: the `[transcript] … from the fork
    source …` lines.
12. The level guide (§3f): an (i) right after "Minimal", level with the tab labels. Hover it → its tooltip.
    Click it → the panel opens below it, dark, wider than a normal flyout: the table's five columns run
    Minimal → Max with "key 5", "key 4", "key 1", "key 3", "key 2" under them, the current level's column
    tinted and its head in the accent, the check marks rising as a staircase, every other row washed; then
    "Also in every level", "In no level", the Codex note and the footer. Switch to another tab, reopen → the
    tint follows. Esc → only the panel closes, focus back on the (i); Esc again → the overlay closes. Click
    outside the panel → only the panel closes. With the panel open press Ctrl+Tab → the overlay and the panel
    close together. Keyboard only: Tab to the (i), Enter → the panel opens. Hover each tab → its cheat sheet
    (Minimal "Shows:", Max "Leaves out nothing another level shows.", the others "Everything in …, plus:"
    and "Leaves out:" with "(from <level>)"), dark, wrapped inside the tip's width.
13. Compact's reason lines (§3g): open a session with Bash, Read and Edit calls at the Compact tab → every turn
    lists "Bash: <its description>", "Read: `<file name>`", … unfolded (no "▸ N tool calls" row), then the
    reply; Default still folds. Copy ▸ Transcript ▸ Compact → the same text. Telegram: `/transcript compact`
    has the lines; 📜 All Screen ▸ Compact draws them (with File Diffs, each diff under its reason line); the
    picker's Compact button reads "+ call reasons & answers". The Sessions page's AI search still reads compact
    sessions without them.
