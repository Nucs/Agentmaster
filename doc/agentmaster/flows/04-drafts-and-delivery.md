# 04 · Unsent drafts and safe delivery (DRF)

> Conventions and fixtures: [README.md](README.md). Spec: PENDING_INPUT.md §2–§12, DELIVERY.md §9, §15;
> CLAUDE.md *Pending-input monitor*.

**What shipped.** 0.7.7: the mail button MOVES the draft into the queue (a true discard, never Claude's
stash), the compose box reconciles with a tab's draft (pull / extend / replace button), the restore
re-fill, Ctrl+Shift+Up, the read-only "unavailable" cursor during a swap. 0.7.9: unsent prompts travel with
a fork (the paste journal), Copy Current Prompt hands over pasted text, drafts read under Claude's
fullscreen renderer, the invisible-character rule and garbled-Hebrew fix.

**The model.** A draft is a FACT, never a state: the text in Claude's input box that was not sent. It is
read off the terminal screen (the bottom `❯` line between flush-left rules), shown as three pulsing dots,
remembered per session (persisted), and protected: anything Agentmaster types into a box holds the box
(the delivery gate) and never merges with, eats or resubmits the user's text.

## Lifecycle model

```text
 user types in the box ──► observed (dots on: tab, board card, side-panel card) ──► remembered (sessions.json + session-store)
        │                                   │
        │   sent / cleared ── 2 empty reads ──► dots off, memory cleared
        │
        ├── a queued prompt goes out ──► SWAP: lock · stash/kill · send · await · restore · unlock ──► draft back in the box
        ├── mail button ──► queued + box cleared (discard ladder)  ·  Shift+click ──► queued, box kept
        ├── fork ──► the fork's box gets the same text (piecewise; pasted blocks re-pasted), never sent
        └── app restart / Deactivate / Resume ──► memory shown as still dots ──► claude starts ──► RE-FILL typed back (no Enter)
```

---

### DRF-01 · The unsent-draft dots appear, follow and clear

> **Since** 0.6.8 (built), fullscreen fix 0.7.9 · **P0** · **Needs** keys into the terminal · **Spec** PENDING_INPUT.md §2, §3, §7, §12

**Promise.** A draft typed and not sent shows three pulsing dots on its tab, its board card and its
side-panel card — in the background too — and they clear when the box empties.

**Phases**
1. **Type a draft.** *Do:* in F-S's terminal type `half a thought` (no Enter). *Expect:* within one scan
   tick (~2.5 s) three dots pulse under the tab's status dot, on the board card and the side-panel card.
   *Check:* [UI] the tab's accessible state (see E2E_TESTING.md §6.3); [ENG] `pendingInput` = the text;
   [LOG] `[pending] <sid8> draft (chars=14 …): half a thought`.
2. **Switch away.** *Do:* select another tab. *Expect:* the dots stay on the background tab (it is still
   read); their color flips light/dark with the tab's rendered background (selected vs not).
3. **Clear.** *Do:* return, press Ctrl+U (or backspace it all). *Expect:* the dots go after two empty reads
   (no flicker on a single repaint). *Check:* [LOG] `[pending] <sid8> cleared`.
4. **Send.** *Do:* type `x [[mock reply "y"]]` + Enter. *Expect:* no dots after the send.

**Edge cases**
- **E1 Fullscreen renderer** (`CLAUDE_CODE_NO_FLICKER=1` in the lane): the same flow — the box lives in the
  ALT screen.
- **E2 Ctrl+G external editor open** holding a pasted Claude box in its text → NOT a draft (the editor's
  `N │` gutter).
- **E3 AskUserQuestion with previews** on screen → the dialog is never read as a draft (menu-shaped lines
  rejected).
- **E4 A draft whose first line is `1. step`** (no `│` after it) → still a draft.
- **E5 A dormant (parked / restored) tab with a remembered draft** → still dots (no pulse while not
  started), the board tooltip says "last seen <ago> — remembered from before…".
- **E6 Dot colors** (cog → Tabs: Pending dots on dark / light) → applied live; dots never invisible on an
  amber tab.
- **E7 Codex tab** → no draft detection (by design).

---

### DRF-02 · A queued prompt is sent around the user's draft (the draft swap)

> **Since** 0.7.7 · **P0** · **Needs** mock, keys · **Spec** PENDING_INPUT.md §9; DELIVERY.md §3–§6

**Promise.** When the runner (or Send now, or a /handover paste) sends while the user has an unsent draft,
the model receives the queued prompt ALONE and the draft is back in the box afterwards; the user cannot
type into the middle of it.

**Preconditions.** F-S Auto; draft swap on (default), Ctrl+S rung on (default).

**Phases**
1. **Draft + queue.** *Do:* type the draft `my words` in the box (no Enter); queue `q [[mock delay 2s;
   reply "done"]]`.
2. **Lock and clear.** *Expect:* when the send starts the pane turns read-only (the mouse pointer over it
   shows the Windows "unavailable" cursor); Agentmaster stashes the draft (Ctrl+S) and verifies an empty
   box. *Check:* [LOG] `[draft-swap] <sid8> hold + lock …`.
3. **Send.** *Expect:* the prompt is pasted, verified, submitted. *Check:* [MOCK] the newest user message
   is exactly `q [[mock …]]` (no `my words`); [TR] the same.
4. **Restore.** *Expect:* after the turn started and the box no longer shows the prompt, the draft comes
   back (Ctrl+S again, compared to the read text); the lock is released; the dots show the draft again.
   *Check:* [LOG] the outcome line (`restored …`); [UI] the box text (UI Automation TextPattern of the
   terminal, read independently of `pendingInput`).

**Edge cases**
- **E1 Typing during the swap** → keystrokes are ignored (silently) while read-only; nothing interleaves.
- **E2 Ctrl+S rung off** (cog) → the kill-ring path (Ctrl+U / Ctrl+Y); multi-line draft with the cursor
  mid-text restores correctly or falls back to a verbatim re-paste.
- **E3 Draft swap off** (cog → "Preserve my unsent draft…") → the historical merge: draft + prompt submitted
  as one message (documented behavior; the test asserts the merge happens, so the switch is known to work).
- **E4 A draft holding `[Pasted text #1 +40 lines]`** → restored through Claude's own stash (the paste
  binding kept); the verbatim re-paste fallback is refused for it (logged), never typed as a literal label.
- **E5 The box never confirms empty** (an unresponsive TUI) → ABORT: nothing sent, the prompt back to
  Pending, the box restored, unlocked.
- **E6 The user's own earlier stash** (Ctrl+S before) → discarded by ours (documented, one slot).
- **E7 Draft text == prompt text** (Shift-kept mail) → the end state is correct: the prompt sent once, the
  identical draft stays.
- **E8 A user's own read-only** (pane already read-only) → not released by the swap.

---

### DRF-03 · Copy Current Prompt

> **Since** 0.6.8, pasted-text expansion 0.7.9 · **P1** · **Needs** clipboard oracle · **Spec** PENDING_INPUT.md §8, §8e

**Promise.** Every session copy menu's **Copy Current Prompt** puts the unsent draft on the clipboard —
the live box when this window hosts it, the remembered draft otherwise — with pasted blocks as their text.

**Phases**
1. **Live.** *Do:* a draft in the box; tab menu → Copy ▸ Current Prompt. *Expect:* the clipboard holds the
   draft verbatim; a short chime. *Check:* [LOG] `[pending] <sid8> copy current prompt: live chars=N`.
2. **From another window** (F-W2: board card menu in window A for a session in window B). *Expect:* the
   remembered draft. *Check:* [LOG] `… remembered …`.
3. **Pasted block.** *Do:* paste 40 lines (Ctrl+V in the terminal) then type ` tail`; copy. *Expect:* the
   clipboard has the 40 lines + ` tail`, not `[Pasted text #1 +40 lines] tail`. *Check:* [LOG]
   `… expanded 1 paste placeholder(s) … chars=A->B`.
4. **Nothing.** *Do:* empty box, no memory; copy. *Expect:* no clipboard write, no chime; [LOG]
   `… copy current prompt: nothing`.

**Edge cases**
- **E1 Dormant tab** → the remembered draft.
- **E2 A placeholder whose content cannot be found** (a pre-journal paste after a restart) → copied as
  rendered (logged NOT expanded), never guessed.
- **E3 Two copies quickly** (a slow expansion) → the newer wins.
- **E4 Codex session** → the item is not offered.
- **E5 Prompt Queue row Copy** of an `auto` row with a placeholder → expanded; a `typed` row verbatim.

---

### DRF-04 · The compose box pulls the tab's draft in

> **Since** 0.7.7 · **P1** · **Needs** keys, mouse · **Spec** PENDING_INPUT.md §8a, §8b

**Promise.** Clicking into the Prompt Queue's empty compose box offers the session's unsent draft ready to
queue — and never overwrites text the user composed.

**Phases**
1. **Fill.** *Do:* a draft `fix the tests` in the terminal; Manager → select the session → click into the
   EMPTY compose box. *Expect:* one tick later the box holds `fix the tests`, caret at the end. *Check:*
   [LOG] `[nav] compose pull-draft <sid8> src=live how=fill chars=13`.
2. **Not again.** *Do:* clear the box, click again. *Expect:* not re-inserted (one-shot per draft).
3. **Extend.** *Do:* with the pulled text in the box, go back to the terminal and continue the draft
   `fix the tests and lint`; return, click the box. *Expect:* the box is extended in place
   (`how=extend`).
4. **Divergent.** *Do:* edit the box to `something else entirely …` (> 50 chars), change the terminal draft
   to a different long text with ≥ 80% shared edges. *Expect:* a **pull button** beside the paper icon
   (tooltip: it REPLACES the box); click → replaced (`how=button`).

**Edge cases**
- **E1 The user's own typing** (`fix the`) while the draft reads `fix the tests` → NOT extended
  automatically (provenance guard); the button offers it.
- **E2 Box AHEAD of the draft** (box `fix the tests now`, draft `fix the tests`) → never offered.
- **E3 Programmatic focus** (after a queue/send) → no pull.
- **E4 CR vs LF** (multi-line draft) → treated as the same text.
- **E5 Non-Claude selection / external session** → no pull.

---

### DRF-05 · The badge's mail button moves the draft into the queue

> **Since** 0.7.7 · **P0** · **Needs** mock, keys · **Spec** PENDING_INPUT.md §8d; TAB_OVERLAY.md §13j

**Promise.** One click queues the tab's unsent draft as the session's next prompt and clears it from the
box — for real (it does not come back after the next send). Shift+click keeps it in the box.

**Phases**
1. **Disabled without a draft.** *Expect:* the mail button (hover the badge) is disabled.
2. **Move.** *Do:* type `queued from the box [[mock reply "m"]]`, click the mail button. *Expect:* a
   Pending row with that text; the box is emptied (End + Ctrl+U, then backspaces if needed — verified
   empty); the dots clear. *Check:* [ENG] the row Pending; [UI] the box empty.
3. **No ghost.** *Expect:* when the moved prompt is later sent and its turn ends, the box stays EMPTY (no
   auto-restored stash).
4. **Keep.** *Do:* type another draft, Shift+click the mail button. *Expect:* queued AND still in the box.

**Edge cases**
- **E1 Mid-turn** (Running) → the clear does not interrupt the turn (no Esc used).
- **E2 Question guard** → the mail-queued prompt passes it (PQR-08 phase 4).
- **E3 A multi-row draft** → cleared down to one empty row (no blank lines left).
- **E4 A draft with a paste placeholder** → queued as rendered today (the open follow-up: expanding at queue
  time); expect Claude to receive the label text — document the result.
- **E5 Codex tab** → no mail button.

---

### DRF-06 · A reopened session gets its draft typed back (restore re-fill)

> **Since** 0.7.7 · **P0** · **Needs** mock, F-RS / Deactivate · **Spec** PENDING_INPUT.md §5, §10

**Promise.** A session that had an unsent draft when it was closed, parked or the app quit comes back with
that draft in its new box — typed, never sent.

**Phases**
1. **Draft, then quit.** *Do:* F-S with a draft `remember me`; quit the app (or Deactivate the tab).
   *Expect:* the memory is persisted ([ENG] `sessions.json` `pendingInput`).
2. **Reopen.** *Do:* relaunch, reopen windows. *Expect:* the tab (dormant) shows still dots; the board
   tooltip says the draft is remembered.
3. **Start.** *Do:* enter the tab. *Expect:* claude starts; ~1.5 s after start the draft is pasted (no
   Enter); the box is read back. *Check:* [LOG] `[draft-restore] <sid8> armed … typed-back attempt 1/2 …
   RESTORED + VERIFIED`; [UI] the box text; [MOCK] no request carrying `remember me`.

**Edge cases**
- **E1 The user types first** (types in the box before the fill) → the user's text wins; no fill.
- **E2 A queued prompt consumes the reopened session first** → hands off (taken over), no fill on top.
- **E3 The setting off** (cog → Prompt Queue Runner → restore drafts) → dots only, no typing.
- **E4 A draft holding a paste placeholder after an app restart** → expanded from Claude's paste cache when
  it can be proven; refused otherwise (no literal label typed).
- **E5 The TUI eats the first paste** → re-filled (≤ 2 attempts), else given up and logged.
- **E6 Deactivate → enter** (same run) → a fresh paste in the draft comes back whole (the journal).

---

### DRF-07 · A fork carries the source's unsent prompt

> **Since** 0.7.9 · **P0** · **Needs** mock, keys, paste · **Spec** PENDING_INPUT.md §2c, §11

**Promise.** Forking a session types the source's unsent prompt into the fork's box — including every
pasted block, re-pasted so Claude makes its own placeholder — never sent; the source keeps its draft.

**Phases**
1. **Draft with a paste.** *Do:* in F-S, type `Look at ` then paste 30 lines (Ctrl+V), then type ` and fix
   it`. *Expect:* the source shows `Look at [Pasted text #1 +30 lines] and fix it`.
2. **Fork.** *Do:* tab menu → Fork session. *Expect:* a new tab `<title> (fork)` opens beside the source;
   when its claude is ready the box fills piece by piece (typed text, then the block as its own paste,
   then the rest) and reads like the source's. *Check:* [LOG] `[draft-fork] <fork8> …`; [UI] the fork's box
   text; [MOCK] no request with the draft; the source's box unchanged.
3. **Send from the fork.** *Do:* press Enter in the fork. *Expect:* [MOCK] the fork's request holds the 30
   lines in full.

**Edge cases**
- **E1 Source in another window / dormant / closed** → the remembered draft is carried.
- **E2 The user types into the fork first** → nothing typed over it.
- **E3 Setting off** (cog → Sessions → "Copy my unsent prompt into a forked session") → empty fork box.
- **E4 An image placeholder or a duplicate paste id** → refused (nothing carried), logged.
- **E5 A paste made before the journal existed** (after an app restart) → resolved from the paste cache when
  unique, else refused.
- **E6 Interrupted-turn fork question** (NOT BUILT, INT-06) → a "Fork interrupted" stop watch holds the
  fill until it ends, then re-anchors it.

---

### DRF-08 · Text Claude would refuse or garble arrives clean

> **Since** 0.7.9 · **P1** · **Needs** mock · **Spec** DELIVERY.md §15

**Promise.** Everything Agentmaster types is cleaned with Claude Code's own invisible-character rule (so
Claude never cancels the Enter), and non-ASCII text arrives intact and is recorded once.

**Phases** (each queued, then Send now; compare [MOCK] and [TR])
1. **Zero-width space / bidi control / tag character** → removed; delivered; no "Removed N invisible
   characters" screen; [LOG] `sanitized=a->b`.
2. **Emoji with VS16 (⚠️), an emoji ZWJ sequence, Persian ZWNJ** → kept.
3. **A Hebrew line with an RLM** → kept; a Hebrew prompt with niqqud → NFC, matched by the echo (no
   duplicate row, no lost-send).
4. **A non-ASCII Windows user name** (lane: a guest account named in Hebrew) → hooks still delivered.

---

### DRF-09 · The draft is remembered across sessions and restarts

> **Since** 0.6.8–0.7.9 · **P2** · **Spec** PENDING_INPUT.md §5, §8c

**Phases**
1. **Persist.** Draft in F-S → `sessions.json` `pendingInput` + `pendingInputUnixMs`; `session-store/<sid>.json`
   `draft` (paste-expanded) + `draftAtUnixMs` (≤ 10 s throttle; a clear is written at once).
2. **Close keeps it.** Close the tab → the archived record keeps the draft; Resume here → DRF-06.
3. **Restart session clears it** (the tab's Restart swaps the claude) → the memory cleared (the user
   watched that screen die) — current behavior; RESTART.md D3 plans to restore it.
