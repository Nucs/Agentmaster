# 16 · Show Transcript, Copy ▸ Transcript and the summary (TRN)

> Conventions and fixtures: [README.md](README.md). Spec: TRANSCRIPT_OVERLAY.md (with its own §9 checklist);
> TAB_OVERLAY.md §13e (the summary panel, the whole conversation); SUMMARY_JUMP.md (the ▸ jump); CLAUDE.md *Per-tab
> link badge*, *TranscriptTurns*. Needs **F-CHAIN**: a seeded chain of sessions — a session forked twice, a plan-mode
> restart, a `/clear` successor and a `/handover` successor (the recorded links written by real commands in a setup
> step, or seeded `sessions.json` records), plus the reference goldens of `tests\turns\expected\` for byte comparison.

**What shipped.** 0.7.7: Transcript Followup (later folded into the levels), the summary's Skills Loaded section and
`scratchpad/<name>` labels, Copy Prompt on a numbered prompt. 0.7.9: **Show Transcript** (a reading view over the
window, one tab per level, the remembered level, folded tool calls, session chain links, ↵ wrap); **Copy ▸ Transcript**
at five levels; the summary lists the WHOLE conversation (a fork's parent, a plan restart, a `/clear` predecessor, a
`/handover` origin); a message typed while Claude works is listed; a fork with no messages yet shows its source
everywhere. 0.7.10: a fork's separator reads `fork <id8>`.

## Lifecycle model

```text
 Show Transcript (any session menu, the badge, the summary panel)
     ─► the card (90% × 80%) at the level LAST PICKED in any window ─► tabs Default · Max · Full · Compact · Minimal (1–5)
     ─► rendered off-thread, built in time slices (the top first) ─► folds · Expand all · ↵ wrap · chain links · Copy · ↻
     ─► closed by Esc · a click outside · ✕ · any tab switch / move / drag / the Manager page ─► focus back to the terminal
 the summary (per-tab panel, side panel, the Manager's Summary pane, the hover card, Copy ▸ Summary)
     ─► AnalyzeSessionTranscript of the session's transcript (or its fork source's, until the fork's first message)
     ─► the conversation upstream: fork parent · plan restart · /clear predecessor · /handover origin, numbered 1..N as one
```

---

### TRN-01 · Show Transcript opens, reads and closes cleanly

> **Since** 0.7.9 · **P0** · **Needs** mock, F-S · **Spec** TRANSCRIPT_OVERLAY.md §2–§5

**Preconditions.** F-S with three turns that called tools (`[[mock bash "echo a"; write C:\e2e\w\t1\f.txt "x"; reply
"done"]]`).

**Phases**
1. **Open.** *Do:* the tab's right-click menu → **Show Transcript** (directly below Copy ▸). *Expect:* a card 90% of the
   window wide and 80% tall over a dimmed page; the header names the session (title · short id); tabs Default · Max ·
   Full · Compact · Minimal with **Default** selected on a first run; the status line ends "N lines · rendered in X".
   *Check:* [LOG] `[nav] transcript-show <sid8> (from tab menu)`, `[transcript] <sid8> show Default: chars=N blocks=M
   rendered in Xms`.
2. **Read.** *Do:* PageDown, End, Home; press `4`. *Expect:* the view scrolls; `4` selects Compact (each level keeps its
   own scroll position). *Check:* [LOG] `[nav] transcript-level <sid8> Compact`.
3. **Remembered.** *Do:* close; reopen from a board card's menu in ANOTHER window. *Expect:* opens at Compact; after a
   restart still Compact; a cog Save meanwhile does not reset it. *Check:* [OS] settings.json `transcriptOverlayLevel:
   "compact"`.
4. **Close and focus.** *Do:* Esc. *Expect:* the card closes; the next key typed reaches the terminal (focus returned).
   Repeat with a click outside, ✕, Ctrl+Tab, a click on another tab, a tab drag, Move to start. *Check:* [LOG]
   `[nav] transcript-close <sid8> (esc|click outside|close button|tab switch|tab drag|tab move)`.

**Edge cases**
- **E1 Esc never reaches claude** → with the card open over a RUNNING session, Esc closes the card and the turn goes on
  ([TR] no interrupt marker).
- **E2 A Max level of a big session** (F-CORP's largest) → the top appears at once, the window stays responsive while it
  fills (MGR-06's probe: the UI thread answers within 250 ms).
- **E3 A Codex session** → the plain-text readers at each level; the header says "Codex".
- **E4 An external row without an id yet** → its Show Transcript item disabled with a reason.
- **E5 Copy in the header** → the current level's whole text on the clipboard, [LOG] `[nav] transcript-copy …`.
- **E6 ↻ Refresh after a new turn** → the new turn appears; folds start closed again; Expand all state kept.
- **E7 The card cannot leak focus**: a Manager refresh or a terminal grabbing focus while it is open → keys still go to
  the card.

---

### TRN-02 · Tool calls fold; Expand all; wrap; chain links

> **Since** 0.7.9 · **P1** · **Needs** mock, F-CHAIN · **Spec** TRANSCRIPT_OVERLAY.md §3b–§3d

**Phases**
1. **Folds.** *Expect at Default:* each turn reads `● Claude`, one row `▸ 3 tool calls: Bash ×2, Write · 1 remark`, then the
   reply; a turn with no tool calls shows the dim note "no tool calls"; the Compact and Minimal tabs fold nothing.
   *Do:* click a row / Enter on it. *Expect:* that list opens (▾); a second click folds it.
2. **Expand all.** *Do:* click Expand all. *Expect:* every list of the level open; Collapse all undoes it; other levels
   unaffected.
3. **Wrap.** *Do:* scroll halfway down a Max level with long lines; click ↵. *Expect:* lines stop wrapping, the part past
   the right edge is hidden (no horizontal bar, no sideways scroll by wheel, touch or arrows), the same passage still at
   the top; ↵ again re-wraps. The setting is global (`transcriptOverlayWrapLines`, default ON) and survives a restart.
4. **Chain links.** *Do:* open F-CHAIN's twice-forked session. *Expect:* the "Session chain" list at the top, every "session
   N" and "forked from session N" link-colored, "this session" and quoted titles plain; a click on "session 1" puts the
   "Session 1 of 3" heading at the top of the view.

**Edge cases**
- **E1 Click a chain link while a big level is still building** → the jump happens when that heading is built; switching
  levels first drops it.
- **E2 A session with no turns of its own** (a fork forked again at once) → its label is plain text.
- **E3 A turn with 243 tool calls** → one fold row; opening it stays responsive.

---

### TRN-03 · Copy ▸ Transcript at five levels

> **Since** 0.7.9 · **P0** · **Needs** F-CHAIN, the goldens · **Spec** CLAUDE.md *TranscriptTurns*

**Promise.** Each level copies exactly what the am-agentmaster-cli skill's `transcript_turns.py` (v9) prints for that
transcript, including the sessions it continues and the subagents it spawned.

**Phases**
1. **Each level.** *Do:* for each of Minimal, Compact, Default, Full, Max: tab menu → Copy ▸ Transcript ▸ <level>.
   *Expect:* the chime; the clipboard holds Markdown. *Check:* [OS] clipboard == the golden for the seeded fixture
   (byte for byte); [LOG] `[nav] copy transcript-<level>`, `[transcript] <sid8> copy <level>: chars=N rendered in Xms`.
2. **What each holds.** Minimal = your messages + each final reply; Compact = + compactions, your answers, slash commands,
   each subagent's brief + report; Default = + every tool call on one line (how it ended) and the remarks between calls;
   Full = + each Bash command, its first output line, per-call durations; Max = + every Write's content and a stats line per
   turn.
3. **Show = Copy.** *Do:* Show Transcript at the same level; its header Copy. *Expect:* the same text.

**Edge cases**
- **E1 A copy, then a second copy before the first finished rendering** → the clipboard holds the second (the generation
  check); [LOG] `superseded by a newer copy`.
- **E2 A session with no transcript yet** → nothing copied; [LOG] `no transcript yet`.
- **E3 A Codex rollout** → the plain-text readers (Minimal / Compact = the per-turn brief; Default / Full / Max = the full
  conversation).
- **E4 A team lead with teammates** → each subagent's entry appears with its brief and report.

---

### TRN-04 · The summary lists the whole conversation

> **Since** 0.7.9 (the `fork` word 0.7.10) · **P0** · **Needs** F-CHAIN · **Spec** TAB_OVERLAY.md §13e

**Phases**
1. **A fork.** *Do:* open the summary panel (the badge's pencil) of F-CHAIN's fork. *Expect:* Messages numbered 1..N straight
   through: the parent's prompts first, then a thin rule `─ fork <parent id8> ─────`, then the fork's own prompts; each
   prompt listed once (the fork's copies of the parent's prompts appear under the parent, keeping their ▸ when this
   terminal shows them).
2. **The separator.** *Do:* hover it; click it. *Expect:* the tooltip names the session, its title and "a fork"; the click
   copies the parent's FULL id (never the word `fork`) with the chime and the tip "Copied session id of up to here".
3. **Other links.** *Expect:* a plan-mode restart, a `/clear` successor and a `/handover` successor each show their
   predecessor's part above a bare-id separator (no `fork` word).
4. **The ⏮ toggle.** *Do:* click ⏮ (leftmost in the times bar). *Expect:* only the part after the last compaction is listed;
   the setting is global (`summaryPanelShowHistory`, default ON) and survives a restart.
5. **Copy ▸ Summary.** *Expect:* the whole conversation, each separator a `──── <full id> ────` rule (`──── fork <full id>
   ────` for a fork).

**Edge cases**
- **E1 Upstream only** → the PARENT's summary never lists its fork's prompts.
- **E2 A rewound prompt** (double-Esc rewind in an earlier session) → not listed (live branch only).
- **E3 A `/compact` inside an earlier session** → its parts before and after both listed, under that session.
- **E4 A chain of 30 sessions** → the panel stays responsive; an unchanged reload does not rebuild (scroll kept).

---

### TRN-05 · What a summary holds

> **Since** 0.7.7 / 0.7.9 · **P1** · **Needs** mock, F-S · **Spec** TAB_OVERLAY.md §13e; SESSIONS.md §3

**Phases**
1. **A message typed while Claude works.** *Do:* send `[[mock hold g1; bash "echo 1"; reply "a"]]`; while it holds, type
   `also check B` + Enter in the terminal; release g1. *Expect:* the summary lists `also check B` at the point the turn took
   it. *Check:* [TR] a `queued_command` attachment (`origin.kind: human`).
2. **Files and skills.** *Do:* a turn that reads, edits and creates files, one inside the session scratchpad, and loads a
   skill. *Expect:* sections Skills Loaded → Files Created → Edited → Read; the scratchpad file reads `scratchpad/<name>`
   and sorts last; each item one clipped line (no wrap).
3. **Copy Prompt.** *Do:* right-click a numbered prompt → Copy Prompt. *Expect:* that prompt verbatim on the clipboard.
4. **The ▸ jump.** *Do:* click a prompt's ▸. *Expect:* the terminal view centers on that prompt's render; a prompt no longer
   on screen (scrolled off the buffer) shows a dim ▸.
5. **Wrap and truncate.** *Do:* ↵ and … in the times bar. *Expect:* real newlines vs literal `\n`; long messages capped vs
   whole; both global and remembered.

**Edge cases**
- **E1 Machine traffic** (a teammate message, a task notification) → never listed as the user's message.
- **E2 Hebrew prompts** → listed intact.
- **E3 A pasted `● ` assistant output** → filtered as noise.

---

### TRN-06 · A fork with no messages yet shows its source everywhere

> **Since** 0.7.9 · **P0** · **Needs** mock, F-S · **Spec** TRANSCRIPT_OVERLAY.md §3e

**Phases**
1. **Fork, send nothing.** *Do:* tab menu → Fork session ▸ Default. *Expect:* [OS] no `<fork id>.jsonl` exists yet.
2. **Every surface.** *Expect:* Show Transcript → the dim italic note "No messages in this fork yet. Below is the transcript of
   session <src8>, the session it was forked from." then the source's turns, at every level; Copy ▸ Transcript ▸ Default →
   the same text with the note on top; Copy ▸ Summary, the Manager's Summary pane, the tab hover card and the summary panel
   → the source's messages (no note). *Check:* [LOG] `[transcript] <fork8> show Default: … from the fork source <src8>'s
   transcript (this fork has no messages yet)`.
3. **The first message.** *Do:* send the fork `[[mock reply "mine"]]`; ↻ in the overlay; hover the tab again. *Expect:* the
   fork's own transcript (with its Session chain), no note; the hover card switches too (never a cached fallback).

---

### TRN-07 · Summaries span compactions

> **Since** 0.7.9 · **P1** · **Needs** mock (a `/compact` turn: the compaction side request answered by the mock) · **Spec** HOOKS.md *Compaction*

**Phases**
1. **Before.** Three prompts, then `/compact` (the mock answers the compaction request).
2. **After.** *Expect:* the session's context size (`Ctx`, wherever it is shown: the hover card, the Telegram card's place
   line) drops to the compaction's `postTokens` the moment it ends, not at the next assistant line;
   the summary, Copy ▸ Transcript, the title and the prompt lists still hold the three earlier prompts; the ▸ jump list
   restarts after the compaction (earlier parts listed without ▸).

---

## Known defects (expected failures until fixed)

### TRN-K1 · A board or tree sort click reverts the summary panel's dragged size
> **P2** · Found by reading on 2026-10-05, still in the code at 57a40dd30 · memory `summary-panel-review-2026-10-05`

The page's settings sink restores the summary toggles from disk but not `summaryPanelWidthFraction` /
`summaryPanelHeightFraction` (`TerminalPage.AgentEngine.cpp`, the preserve block near `summaryPanelShowHistory`), and the
content's sort cycle hands the sink its OWN, older copy. **Repro:** drag the panel's grip to a new size; click the board's
sort button; reopen a tab → the old size. **Expected:** the dragged size stays.

### TRN-K2 · A ▸ jump in the side panel's PEEK mode scrolls a hidden tab
> **P2** · Found by reading on 2026-10-05

`_JumpToPromptInSession` never selects the tab, so a ▸ on a peeked session's summary scrolls a terminal you cannot see
(with the chime); a session in another window gets nothing. **Expected:** the tab selected (or the jump refused).
