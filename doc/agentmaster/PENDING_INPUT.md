# Pending-input monitor — detect an UNSENT draft in a Claude tab's input box

> **2026-09-26 — Claude Code's FULLSCREEN renderer (§12).** With `CLAUDE_CODE_NO_FLICKER=1` (or
> `/tui fullscreen`), Claude Code 2.1.282 draws its whole UI, input box included, in the terminal's
> ALTERNATE screen buffer. A 2026-09-18 guard (`132e19b5d`) answered "no box" for any alt-buffer screen
> without reading it. Release 0.6.8.12 was the first release build to carry that guard, and in it the
> monitor went dark for every Claude tab: the mail button (§8d) never enabled, Copy Current Prompt came
> back empty, the §10 re-fill gave up, and the Tests Autorunner held every advance on "input box not
> visible". `ControlCore::ReadPendingInputDraft` now reads the active buffer, main or alt, and tells a
> child full-screen app (the Ctrl+G editor) apart by content. Real-capture fixtures in
> `TestPendingInput` cases 28–29 pin both sides. Rides the next deploy.
>
> **2026-09-25 — the FORK CARRY (§11) + the PASTE JOURNAL (§2c).** Forking a session now copies the
> message sitting unsent in the source's input box into the new fork's box (never submitted), pasted
> PIECEWISE so the fork's box reads like the source's, every `[Pasted text #N +M lines]` backed by its
> real content. That needed a new content source: current Claude Code (measured on 2.1.280) writes its
> `paste-cache/<hash>.txt` only at **SUBMIT** time, so an unsent paste exists only in claude's memory —
> §2b's "at paste time" no longer holds, and every recent `paste-refs` line in both profiles read
> `unresolved`. Agentmaster is the terminal, so it now RECORDS every paste it passes to a Claude session
> (§2c) and resolves placeholders against that journal first. Copy Current Prompt (§8e), the §8c store
> and the §10 re-fill resolve through it too. Pure core `AgentMaster/DraftCarry.h` (unit-tested:
> `TestDraftCarry`); cog toggle **SESSIONS ▸ "Copy my unsent prompt into a forked session"**
> (`AppSettings::carryDraftOnFork`, default ON). Rides the next deploy.
>
> Status: **complete + BULLETPROOFED (2026-07-23) — detection + the "yes pending / no pending" observer
> NOTIFY + the "3 dots" animation on BOTH surfaces, PLUS: the cross-version hardening pass (§2a — the NBSP
> separator fix, the anchored-rule + menu-shape rejectors that killed the six live AskUserQuestion
> false positives, the candidate upward-continue), the PASTE-CACHE resolver (§2b — a draft's
> `[Pasted text #N +M lines]` placeholder resolves to its real on-disk content, content-anchored +
> arithmetic-verified), and PERSISTENCE (§5 — the draft survives an Agentmaster restart/crash as a
> staleness-labeled memory and revalidates against the live box on resume), and the draft as a COPYABLE
> value (§8 — a **`Copy Current Prompt`** item in all three session copy menus: a LIVE buffer read where
> the window hosts the tab, falling back to the observer's remembered draft). **The RE-FILL ON RESUME
> (§10, 2026-07-27) closes the loop: a reopened session's remembered draft is TYPED BACK into its fresh
> input box** — the /handover-standby fill channel (bracketed paste, NO submit CR, read-back verified;
> paste placeholders expanded against the cache or the fill refused), gated by the cog's
> `restoreDraftOnResume` (default ON) — so an unsent message survives a restart as a real box draft, one
> Enter away, instead of a display-only memory. **The CONTINUATION CHECK + the conditional pull button
> (§8b) and the durable per-session draft CACHE (§8c, 2026-07-27) complete the compose-box half:** the
> Manager's Auto-Testing box now compares what you composed here against the session's live draft
> (`EvaluateDraftPull` — fill an empty box, EXTEND a strict prefix, never overwrite a box that is ahead)
> and offers a **pull button** for a genuinely different draft only when taking it wouldn't destroy your
> text (contained in it, or >=80% similar over 50 chars); and every draft change is mirrored into the
> durable per-session store (`session-store/<sid>.json` `draft` + `draftAtUnixMs`, paste-EXPANDED,
> upsert-throttled per session, cleared un-throttled, one off-thread writer with an ordering guard).**
> Pure detector + resolver +
> the registry notify-on-flip/stamping + the persistence round-trip are unit-tested (engine harness
> 2,954/2,954 incl. REAL-capture fixtures from live 2.1.217/2.1.218 buffers + the §10 latch/expansion
> suites + the §8b relation/offer/similarity table + the §8c store round-trip); the full chain lib-compiles
> green (TerminalControlLib + TerminalAppLib); the fixed detector + the resolver are LIVE-VERIFIED
> out-of-band against the running fixture session ('wow sess': 1,036 chars extracted clean, its truncated
> paste resolved to `bf8eefafa3e80676.txt` and expanded to the full 18,647-char briefing). The in-app
> deploy of this pass rides the next deploy cycle (build/deploy needs the user's permission).

## 1. What this is & why it can't be a hook

Claude Code renders an **input box** at the bottom of its TUI where the user types the next prompt.
Until the user presses **Enter** that text is a **draft** — it has not been submitted. This is the **one
session fact the hook/transcript pipeline can never carry**: hooks fire on *submit* (`UserPromptSubmit`),
and the transcript only records *sent* messages, so a draft a user typed-but-didn't-send is invisible to
every push/pull path the rest of the engine uses.

The only way to know a tab is holding an unsent message is to **read it out of the rendered terminal
buffer**. This monitor does exactly that — periodically, read-only — and records the draft as a transient
fact on the session, so a future **tab indicator** can show "this tab has an unsent message" (the user's
"have some indicator (later on) in the tab showing we have a message there").

### State vs FACT (Correctness Rule #7 / #13)

The project **never screen-scrapes for session STATE** (Running / Waiting / NeedsApproval / …) — the Ink
TUI repaints constantly, so state is hook- and transcript-driven. The pending draft is **NOT state**: it
is a transient **draft fact**, the screen-read analog of Claude's presence heartbeat (`sessions/<pid>.json`,
also a read FACT). It is never authoritative for anything but "this tab has unsent text", never feeds
`SessionState`, and the read is **strictly read-only** — the monitor never writes to a shell (the
invisibility invariant). This is the *only* place Agentmaster reads the rendered buffer for a session
fact, and it does so precisely because no other signal exists.

## 2. How the input box is identified (the detection)

The input box renders as a `❯` prompt line **wrapped by `─` horizontal rules** (ASCII stand-ins below:
`>` = `❯` U+276F, `---` = the `─` U+2500 rule):

```
----------------------------------------------------------   (top rule)
> first line of the draft, possibly long and soft-wrapped
  a continuation line (2-space indent aligning under "> ")
----------------------------------------------------------   (bottom rule)
```

**Two facts pin it down** (both required — the user's guidance: *"the bottom-most ❯ … wrapped around
─────"*):

1. it is the **BOTTOM-MOST** line whose first non-whitespace glyph (after at most ONE leading vertical
   border char, for a future framed render) is the prompt marker `❯` (U+276F; `›` U+203A is also
   accepted) **that also passes fact 2** — a candidate failing fact 2 does NOT abort detection; the
   scan **continues upward** to the next marker row (so a `❯`-leading line the user PASTED into the
   draft body can never hide the true caret above it), and
2. it is **WRAPPED by ANCHORED rule rows** — a plain `─` rule **directly above** the `❯` line (within
   2 rows, tolerating one intervening blank), and another rule **below** the body; both **flush-left**
   (≤ 4 columns of indent — `IsAnchoredPendingRuleRow`; the box's rules start at column 0 in every
   observed render, while a menu's floating pane border starts mid-row — see §2a).

Fact 2 is what separates the live input box from the other things that also start with `❯`:

| looks like | example | rejected because |
|---|---|---|
| a **SENT prompt** in scrollback | `❯ a message I sent earlier` | rendered INLINE, no surrounding box (no rule directly above + below) |
| a **menu selection** cursor | `❯ 1. Yes` under "Do you want to proceed?" | the line directly above the `❯` is the question text, not a rule |
| an **AskUserQuestion PREVIEW menu** row | `❯ 2. 2 · Three Floors  │ Any Python library, zero-copy…` | the pane border above it is rule-*like* but **not anchored**, and the row itself trips the **menu-shape rejector** (§2a) |

A **rule row** is a row *dominated by* box-drawing characters (U+2500..U+257F): ≥ 6 of them AND ≥ 80% of
the row's visible (non-space) chars. The 80% floor rejects a *labeled* divider (`── 3 files ──`, used
elsewhere by Claude) while accepting a plain or corner-framed rule. A **box** rule must additionally be
**anchored** (flush-left) — the discriminator that keeps a floating right-column pane border from
counting.

**Extraction**: the body rows `[caret, bottomRule)` — strip the `❯` marker (+ one following space) from the
first line and the 2-space continuation indent from the rest, join with `\n`, drop trailing blank lines,
and **strip the trailing CURSOR**. The last step matters: a focused input box renders its cursor as a
**block glyph** in the cell after `❯ ` (the "white box" you see) — and a non-empty draft carries the cursor
at its tail too — which the buffer holds as a real character, so without stripping it an **empty box reads
as a draft** (a false "pending"). `IsIgnorable` = whitespace (incl. non-ASCII spaces like NBSP, in case the
cursor/padding is one) **+ a block-element glyph** (U+2580–U+259F, the cursor); the trailing ignorable run
is stripped, so an empty box collapses to `""` while a real draft keeps its text (its content sits to the
*left* of its trailing cursor). The block range is **outside** the rule's box-drawing range (U+2500–U+257F),
so a lone block cursor is never mistaken for a rule. An empty box (`❯ ` + cursor) ⇒ empty text → **no pending
draft**. (Reconstruction is best-effort for display; the load-bearing output is "is there any content".)
If a future build's cursor is some other glyph, the `[pending] … cp: <hex> …` log names it (the appear-log
prints the draft's leading code points), so the strip set can be widened without guessing.

The buffer position is independent of the user's **scroll**: Claude renders inline, so the box always sits
at the bottom of the buffer (`GetLastNonSpaceCharacter`), and the adapter reads a bounded window of the
**last rows** regardless of where the WT scrollback is scrolled.

## 2a. Cross-version facts (measured live, 2026-07-23) — the hardening pass

Every renderer assumption below was **measured**, not guessed, against (a) the whole recorded `[pending]`
log history — **1,088 lines across both profiles' hooks.log**, every one printing the draft's leading
code points — and (b) an out-of-band **live-fleet capture sweep**: `AttachConsole` +
`ReadConsoleOutputCharacterW` over every reachable `claude.exe` on the machine (76 live processes
spanning **2.1.211 → 2.1.218**; the 59 that failed to attach are console-less ORPHANS — their terminal
hosts are gone — not render variants; all 17 attachable screens, 2.1.217 + 2.1.218, rendered the box and
the detector found it 17/17).

- **The post-marker separator is U+00A0 NBSP, not a space** — `❯ yooo` is `276F 00A0 79…`. 100% of the
  1,088 historical lines + every live capture agree. The original extractor stripped only `L' '`, so
  **every draft ever recorded leaked a leading NBSP** (`chars=2 cp: 00A0 0079` for a one-char draft —
  the "Defect #1" this pass fixed: the strip now accepts any single whitespace, and the continuation
  indent is NBSP-tolerant too). The trailing-cursor strip already handled the *empty*-box NBSP, so the
  load-bearing boolean was never wrong — the leak was display/precision only.
- **The box rules are full-width and flush-left (column 0)** in every observed render — the basis for
  the **anchor** requirement in §2.
- **The one real false-positive class was the AskUserQuestion side-by-side PREVIEW menu** (options left,
  `│` U+2502, preview right). Six live `[pending]` lines — e.g. `2. 2 · Three Floors │ Any Python
  library, zero-copy — via np.frombuf…`, one a 3,474-char "draft" — were selected OPTION rows extracted
  as drafts: the preview pane's floating border (`╭──…──╮` mid-row) satisfies the box-drawing density
  test, so the option row below it read as a rule-topped caret. Killed twice over: the border fails the
  **column anchor**, and the row itself trips the **menu-shape rejector** (`IsMenuOptionCaret`: post-
  marker text `N. ` + a `│` column separator with content after it). Deliberately narrow: a real draft
  starting `2. fix the tests` (no `│`) is NOT rejected; a real draft whose *first line* both starts
  `N. ` and carries a mid-line `│` with text after it would be — the documented, vanishingly-rare
  trade-off (pinned by tests).
- **The candidate scan continues upward** past a failed candidate instead of aborting — a `❯`-leading
  line pasted *inside* the draft body (a quoted transcript snippet) used to make detection return
  nothing; now the true caret above it wins and the pasted line rides the body.
- **Failure posture**: a future render change degrades to *no box found* (missed dots — loud in the
  fixture suite, silent-but-safe live), never to a false "pending". The `[pending] … cp:` log names any
  new leading glyph without a deploy, and the REAL-capture fixtures in `TestPendingInput` (rows lifted
  byte-for-byte from the 2.1.217/2.1.218 sweeps, NBSP and all) fail the harness the moment extraction
  drifts.

## 2b. The PASTE-CACHE resolver — what a `[Pasted text …]` placeholder really holds

> ⚠ **Superseded as the PRIMARY source (2026-09-25) — see §2c.** On Claude Code **2.1.280** the cache
> file is written at **SUBMIT** time, not paste time: every file's mtime equals its `history.jsonl`
> submit timestamp to the second (checked on the four newest, 141 KB included), each history entry
> references it as `pastedContents[N].contentHash`, and the unsent `+31 lines` drafts logged that day had
> no file at all. So this resolver can only answer for content that was **submitted before** (an identical
> re-paste — the leaf is content-addressed), and line-count ambiguity makes even that weak (on the dev box
> 126 of the line counts 1..399 match more than one of the 415 cache files; `+31` matches **11**). It
> remains the FALLBACK tier behind the journal. The text below describes the observation it was built on.

Claude Code spills a large paste to **`<CLAUDE_CONFIG_DIR | ~/.claude>/paste-cache/<16-hex>.txt`** *at
paste time* (content-addressed leaf — an identical re-paste reuses the same file; observed stable since
2026-01, ~268 files on the fixture machine). The input box then renders a **placeholder**, in one of two
forms backed by the SAME cache file (both observed live on one draft):

```
❯ yooo
  [Pasted text #1 +273 lines]                          <- COLLAPSED (whole paste hidden)

❯ yooo
  #  Fix: "Enable Debug Mode" + restart does NOT …     <- EXPANDED, middle elided:
  ## The user[...Truncated text #2 +258 lines...]es).
```

So the detector extracts a draft whose text *names* content it does not contain. **`PendingPaste.h`**
(pure, header-only — the `PendingInput.h` idiom) resolves the markers back to the real file,
**content-anchored, never by mtime** (the cache is global across sessions — a time window races), and
**refuses rather than guesses**:

- **TRUNCATED** (strong tier): the draft carries real paste content around the marker. The text before
  the marker is a **prefix of file segment `i`** (the head-cut line), the text after is a **suffix of
  segment `j`**, and **`j − i == M` exactly**. Proven on the live fixture: head `## The user` = prefix
  of segment 8, tail `es).` = suffix of segment 266, 266 − 8 = 258 = the marker's `+258`. Two content
  anchors + an exact offset ⇒ practically collision-proof.
- **COLLAPSED** (weaker tier, labeled): nothing of the paste is visible, so the only signal is the
  **line count** — `M` matches the file's newline count **or** segment count (both conventions
  observed: the live 274-segment unterminated file carried `+273`), and the match must be **unique**
  across the cache. The annotation carries **`(by line count)`** so a count-coincident stale file is
  never presented as verified content (live example: a `+341` collapsed marker resolves to a Feb-15
  file with exactly 341 segments — plausibly correct via content-addressed dedup, unprovable from
  outside).
- **Ambiguity refuses** (two candidate files ⇒ unresolved), and `ExpandPasteMarker` additionally
  refuses to expand a collapsed marker that shares its line with typed text (never eat the user's
  words). Expansion of a truncated marker substitutes segments `i..j` verbatim (they subsume the
  visible fragments) — live-verified: the 1,036-char wow-sess draft expanded to the full 18,647-char
  briefing.

The impure half lives in `ClaudeSpawn.cpp`: `ClaudePasteCacheDir()` (the `ClaudeProjectsDir` resolution)
+ `ResolvePendingPasteRefs(draft)` — enumerate + read the cache (2 MiB/file, 64 MiB, 4096-file caps),
run the pure resolver, render one annotation line per marker
(`truncated #2 (+258 lines) -> bf8eefafa3e80676.txt` / `paste #3 (+99 lines) -> unresolved`). The UI
lane resolves **off-thread** on a draft-text change (`_ResolvePendingPasteRefsFor` — detached
fire_and_forget; the registry takes the verdict QUIETLY via `SetPendingPasteRefs`, which drops a late
resolve whose draft meanwhile cleared), logs it (`[pending] <sid8> paste-refs: …`), and the board card's
hover tip appends it. The annotation persists with the draft (§5) — the cache file itself is the durable
content copy, so no blob is ever duplicated into our records.

## 2c. The PASTE JOURNAL — the content of an UNSENT paste (built 2026-09-25)

**Why.** An unsent paste's content lives only in claude's process memory (§2b's banner), so no file
can resolve a fresh `[Pasted text #N +M lines]`. But every paste passes through Agentmaster — it is the
terminal — so the registry now keeps a per-session **journal** of what was pasted:
`SessionRegistry::NotePastedText` / `PasteJournal` / `ClearPasteJournal` / `MovePasteJournal`.

**What claude does with a paste** (read from its 2.1.280 bundle — the prompt input's paste handler), which
the journal mirrors exactly (`DraftCarry.h`):

| step | claude's code | mirrored by |
|---|---|---|
| normalize | `Bun.stripANSI(t).replace(/\r\n\|\r/g,"\n").replaceAll("\t","    ")` (after WT's paste filter dropped C0/C1 and folded line ends) | `NormalizePastedTextLikeClaude` |
| count | `M = (t.match(/\r\n\|\r\|\n/g) \|\| []).length` — newline SEQUENCES | `ClaudePasteNewlineCount` |
| collapse | `t.length > 800 \|\| M > min(rows-10, 2)` | `ClaudeWouldCollapsePaste` (2 on any usable terminal) |
| render | `M == 0 ? "[Pasted text #N]" : "[Pasted text #N +M lines]"` — the bare form §2b's grammar never knew | `FindDraftPlaceholders` |
| id | minted past every id already referenced in the input — ids grow with time | the chronological assignment below |

**Feeds** (everything that puts a paste into a Claude box):
- **the clipboard** — `TerminalPage::_PasteFromClipboardHandler` (Ctrl+V, Ctrl+Shift+V, Shift+Ins, right-click,
  the `paste` action, the text-menu Paste): the session is resolved on the UI thread from the pasting
  control's content id (`_ClaudeSessionForContentId`), and the text is journaled after the multi-line / large
  paste warnings (a rejected paste never reached claude) and right before `HandleClipboardData`. Bracketed
  pastes only (only those can become placeholders).
- **our own no-submit fills** — the §10 re-fill, the /handover-standby fill, the §9 swap's re-paste fallback,
  and the §11 fork carry's blocks (journaled under the FORK at arm time). Submitted sends (the autorunner,
  Send-now) are deliberately NOT journaled: they leave the box, and a journaled submission could be matched
  to a later placeholder whose own paste went unrecorded.

**Rules.** Only collapse-eligible pastes are kept (an inline paste never backs a placeholder); an identical
consecutive re-paste refreshes the newest entry; ≤16 entries / 16 M chars per session (oldest dropped), an
entry over 4 M chars is never kept, and the fleet stays under 64 M chars (globally oldest evicted). A side
map, never inside `SessionInfo` (a paste can be megabytes and snapshots are copied on hot UI paths).
**Transient** — an app restart forgets it (a restored draft then falls back to §2b, i.e. the pre-journal
behavior). **Pruned** when the scan confirms the draft CLEARED (`_ScanPendingInput`'s flip to empty — the
prompt was sent or discarded), moved onto the new id by a restore-fresh (`_LaunchClaudeSession`), dropped by
`Remove`.

**Resolution** (`ResolveDraftCarry`, pure; `ResolvePendingDraftCarry` the adapter — reads the cache only when
the journal can't answer):
1. **journal** — collapsed placeholders grouped by M; within a group, ordered by claude's id, they take the
   most recent same-M entries in paste order (`#1 +31` ← the older 31-line paste, `#2 +31` ← the newer). A
   truncated placeholder takes the newest entry its head/tail anchors validate against (the ELIDED MIDDLE is
   cut out — the visible head/tail stay as typed text). Works MID-LINE: the substitution is by span, never
   by line, so typed words beside a placeholder are never eaten.
2. **paste-cache** (§2b) — whole-line collapsed placeholders with a unique count match, truncated by anchors.
3. **unresolved** — policy per caller: **live** (a fork, a copy, the §8c store) keeps an unbacked
   **mid-line** placeholder as literal text (that is prose QUOTING one — "make sure the [Pasted text #1 +31
   lines] are expanded"), and refuses a whole-line one (a real paste whose content would be lost); **strict**
   (the §10 re-fill across a restart) refuses any.
Always refused: `[Image #N]` / `[Audio #N]` (the bytes can't be re-attached) and a duplicate paste id (one of
the two is a quote and nothing tells which).

**Known gaps.** Drag-and-dropped TEXT (`TermControl::_DragDropHandler` → `PasteText`, never through the
clipboard handler) and pastes made before the journal existed (i.e. before this build) are not journaled —
they fall to the cache tier. A user's own Ctrl+S stash→unstash cycle clears the box long enough for the prune.

## 3. Architecture (one pure brain, a thin adapter, a UI-lane poll)

```
TerminalPage::_ScanPendingInput()      (TerminalApp; UI thread, ticked by the shared SessionScanner's
  └─ per bound, started CLAUDE tab:     liveness probe — alongside _SweepClaudeLiveness / _ObserverProbe)
       ├─ TermControl::ReadPendingInputDraft()                                     TerminalControl
       │    └─ ControlCore::ReadPendingInputDraft()   (read-only, under the read-lock)
       │         ├─ copy the last ~120 rows' text of the ACTIVE buffer — main OR alt (§12)
       │         └─ Agentmaster::DetectPendingInput(rows)   ◄── PendingInput.h (PURE, header-only)
       ├─ clear-debounce (eager show / lazy hide) → effectiveDraft
       ├─ SessionRegistry::SetPendingInput(id, effectiveDraft)
       │    └─ _notify ONLY on the empty<->non-empty FLIP  ─► every window's board card rebuilds
       │                                                       (AgentManagerContent::_MakeCard → "3 dots")
       ├─ _SetTabPending(tab, hasPending)  ─► TabStatus.AgentPendingVisible ─► TabHeaderControl "3 dots"
       └─ log the FLIP as [pending]   (hooks.log)
```

- **`AgentMaster/PendingInput.h`** — the **pure, header-only** detector (no WinRT / TextBuffer / ICU).
  Header-only like `PromptAnchor.h` / `ProfileBootstrap.h`, so it is shared by the `ControlCore` adapter
  (a separate `Microsoft.Terminal.Control` DLL) AND the test harness with no cross-project source/link.
  Source stays **pure-ASCII** (markers as `\u` escapes, code-point notation in comments) — the
  `PromptAnchor.h` convention, since this TU compiles without `/utf-8`. `DetectPendingInput(rows)` +
  `IsPendingRuleRow(row)`. Unit-tested in `AgentMaster/tests/m5_tests.cpp` (`TestPendingInput`).
- **`ControlCore::ReadPendingInputDraft()`** (`ControlCore.{idl,h,cpp}`) — **read-only**. Guards on
  `_initializedTerminal` (a restored-but-never-shown tab has a null `TextBuffer`; same guard as
  `ResolveConversationPromptRows`), locks for reading, copies the last `kPendingScanRows` (120) row texts,
  and runs the pure detector. Returns the draft (`""` = empty box / no box / not-yet-initialized).
  **Mutation-id cache gate** (`_pendingInputScanValid` / `_pendingInputScanMutationId` /
  `_pendingInputScanResult`): the poll is keyed on `TextBuffer::GetLastMutationId()` — an unchanged
  buffer returns the cached answer without the 120-row read + parse. Sound because a draft being typed
  IS buffer output (the TUI echoes it), so any draft change bumps the id — the same invariant the
  summary-jump epoch cache rides.
- **`TermControl::ReadPendingInputDraft()`** (`TermControl.{idl,h,cpp}`) — a pure passthrough to the core.
- **`SessionInfo::pendingInput` (+ `pendingInputUnixMs`, `pendingPasteRefs`)** (`SessionModels.h`) — the
  draft fact trio. **PERSISTED since the §5 pass** (omitted from `sessions.json` when there is no draft,
  so a draft-free file is byte-unchanged): the draft is a transient fact *of the live box*, but its
  persisted copy is the only durable record of an unsent message (claude never restores its own input
  box), so it survives restart as a staleness-labeled MEMORY — see §5 for the archive-keep decision and
  the revalidation story.
- **`SessionRegistry::SetPendingInput(id, text)`** — the field updates every change, but `_notify` fires
  **only on the BOOLEAN hasPending FLIP** (empty↔non-empty) — the "yes pending / no pending" transition.
  This is the **presence-heartbeat cadence**: a draft moves as the user types, so a per-keystroke notify
  would needlessly run the persist / board-rebuild / scheduler cascade, but the appear/clear transitions
  are infrequent (turn-cadence) and are exactly what the animations key on. A text-only edit (still
  non-empty) updates the field **quietly**. Returns true iff the boolean flipped (== whether it notified).
  **Every non-empty set — changed or not — quietly re-stamps `pendingInputUnixMs`** (the "last actually
  observed" clock: age within a few ticks ⇔ a live read; a frozen stamp ⇔ a carried memory), and a clear
  zeroes the stamp + drops `pendingPasteRefs`. `SetPendingPasteRefs` is the quiet, change-gated,
  draft-guarded sibling for the §2b annotation. Unit-tested (`TestRegistry`): appear notifies, a
  text-only edit is quiet, clear notifies, the stamp refreshes quietly, a late paste-resolve can't land
  on a cleared draft.
- **`TerminalPage::_ScanPendingInput()`** (`TerminalPage.AgentObserver.cpp`) — the **UI lane** (the only
  place a control's buffer is readable). Ticked once per scanner liveness pass. For each **bound, started,
  Claude** session it reads the draft, applies the **clear debounce** (below), commits it via
  `SetPendingInput`, and drives **this window's tab-strip pulse directly** (`_SetTabPending` — it holds the
  tab) every tick + idempotently. **Background (unfocused) tabs are scanned too** — the whole point is to
  notice a draft left in a tab the user switched away from. A **NotConnected/dormant tab** (window-restored,
  not yet started) is not readable — the branch leaves the stored draft + streak untouched AND drives the
  dots **from the persisted memory** so a restored draft is visible before its claude ever starts (§5).
  On a draft-text change the scan also kicks the **off-thread §2b paste resolve**, and a QUIET text drift
  (no flip ⇒ no autosave) triggers a **~10s-throttled direct `SaveSessions`** so the persisted memory
  tracks the live box instead of freezing at its appear-flip snapshot (the staleness the `[pending]` log
  deliberately exhibits must not leak into the durable copy). The flip is logged as `[pending] <id> draft
  (chars=N cp: …): <first line>` / `[pending] <id> cleared`.
- **The "3 dots" animation** (the visible indicator) rides two surfaces, both a phase-shifted opacity
  pulse whose **color is a user-configurable LIGHT/DARK contrast PAIR** (see *Dots color* below) so the
  dots are never invisible against the tab/card they sit on:
  - **Tab strip** — `TerminalTabStatus::AgentPendingVisible` (set by `_SetTabPending`) drives a tiny 3-dot
    cluster at the bottom of the status-dot wrap in `TabHeaderControl.xaml` (below the dot), painted with
    `TerminalTabStatus::AgentPendingBrush` (the contrast-picked color, also set by `_SetTabPending`). The
    pulse storyboard is built imperatively and **started/stopped on the flag** (`TabHeaderControl::_Update-
    PendingAnimation`, hooked to `TabStatus.PropertyChanged`) so an **idle fleet animates nothing** — no
    perpetual 60fps compositor wakeups.
  - **Triage-Board cards** — `AgentManagerContent::_MakeCard` appends a 3-dot pulse (`BuildPendingDots`,
    passed the picked color) when `!s.pendingInput.empty()`. The board rebuilds on the flip `_notify` (the
    lens observer), so it appears/clears with the draft, **cross-window** (a session hosted in window A
    animates on window B's GLOBAL board too). The storyboard begins on `Loaded` (runs only while carded; a
    rebuild drops it).

### Dots color — a LIGHT/DARK contrast pair, auto-picked by background

The dots are painted from a **two-color pair** the user sets in the Settings cog (TABS section: *Pending
dots (on dark tabs)* / *Pending dots (on light tabs)* — two `muxc::ColorPicker`s mirroring the flash-ring
swatch idiom). GLOBAL, persisted as `AppSettings::pendingDotsLightColor` / `pendingDotsDarkColor`
(`#AARRGGBB`; defaults gold `#FFE0A92B` on dark, deep amber `#FF5A3E00` on light — the gold preserves the
prior single hardcoded look). The **algorithm** picks which of the pair to use by the **WCAG relative-
luminance** of the background the dots ride on (`AgentStatusColors.h` — `BackgroundIsLight` /
`PendingDotsColorFor`, the same ~0.179 crossover `PreferDarkTextOn` uses for the card title band): a LIGHT
background ⇒ the DARK dots, a DARK background ⇒ the LIGHT dots — so the dots never wash out:
  - **Tab strip** — the background is the tab header = the session's **per-directory tab color** (Rule #12;
    `GetDirColor` ?? `AutoDirColorHex`). `_ScanPendingInput` computes the picked color each tick and hands
    it to `_SetTabPending`, which re-points `AgentPendingBrush` only on a genuine color change (idempotent),
    so a cog color change applies on the next scan tick (~2 s) with no extra plumbing.
  - **Triage-Board card** — the dots sit on the card BODY, which is the always-dark Manager fill
    (`#2E2E2E`), so the pick yields the LIGHT color there; it updates on the next board rebuild. (The DARK
    color is what shows on a light tab strip.)
Applied live + cross-window through the existing settings broadcast (the `flashRingColor` idiom):
`_appSettings` is refreshed in every window's Save handler + `_ApplyBroadcastSettings`, and both surfaces
re-read it on their next tick/rebuild.

### Reliability — "can we reliably say yes then no?" (the clear debounce)

Yes. The boolean is stable between polls (the box text persists until sent), so normal use is two clean
flips per message — appear (you type), clear (you send). The only spurious-flip risk is a buffer read
landing in a **mid-repaint frame** (Claude's Ink TUI redraws the box constantly) reading a momentarily
empty box. `_ScanPendingInput` guards against it with **eager-show / lazy-hide** hysteresis: a non-empty
read shows the dots **immediately**, but an empty read only **clears** after `kPendingClearConfirmTicks`
(2) **consecutive** empty scans (`_pendingClearStreak`). So a one-frame mis-read can't flicker the
indicator off; a real send clears within ~2 ticks (~4 s). Latency to *appear* is ≤ one tick (~2 s).

### Cost

Steady state is cheap: once per ~2 s liveness tick, per **bound Claude tab**, a read of the **last ~120
rows** (not the whole scrollback) under the read-lock → a linear scan of those rows. Microseconds per tab;
Codex tabs and dormant/never-started tabs are skipped. A text-only edit is quiet (no persist/UI churn); a
`_notify` + board rebuild + a `[pending]` line happen **only on the appear/clear flip** (turn-cadence). The
tab/board pulse storyboards run **only while a tab is actually pending** — an idle fleet animates nothing.

## 4. Scope (v1) & known limitations

- **Claude only.** Codex's TUI has no `❯`-in-a-box input; only managed Claude sessions are monitored.
  (The detector itself is agent-agnostic — only the wiring gates on `kind == Claude`.)
- **Placeholder text.** v1 detects any non-whitespace content after `❯ `. If a Claude build shows a **dim
  placeholder** inside an empty box (e.g. a first-launch suggestion), it would read as a draft. This is
  rare during an active session (an empty box is just `❯ ` + cursor) and self-clears the moment the user
  types or sends, so v1 does **not** guess at placeholder strings (which would risk suppressing a real
  message). The robust fix — read the cells' **faint/dim attribute** and treat dim post-marker text as a
  placeholder — is a documented follow-up; `PendingInputDraft` already returns `caretRow`/`bottomRuleRow`
  as the seam for it.
- **Split panes.** `_ControlForSession` returns the first terminal control in a tab's pane tree; a Claude
  pane split beside a shell could read the wrong pane (a missed detection, never a false positive). The
  single-pane case (the norm) is correct.
- **Side borders.** The caret scan now **skips ONE leading vertical border char** (U+2502/2503/2551), so
  a future framed style (`│ ❯ text │`) still *detects*; body-line side borders would ride the extracted
  text un-stripped until a real sighting motivates full framed extraction (degraded-not-dark, by design).
- **A draft first-line shaped exactly like a preview-menu option** (`N. …` AND a mid-line `│` with text
  after it) is rejected as a menu — the deliberate, vanishingly-rare §2a trade-off.
- **A pasted Claude box inside a GUTTER-LESS external editor** (§12) reads as a box. Suppose the Ctrl+G
  editor draws lines flush-left, and the draft it shows contains a copied Claude input box (a rule, a
  `❯` line, a rule). Then that snippet passes the detector. Microsoft Edit, Claude's default Ctrl+G
  editor, is immune: its `N │ ` gutter keeps the caret from being a row's first glyph (pinned by
  `TestPendingInput` case 29).

## 5. Persist + reload on startup — the draft as a durable MEMORY

The user's ask: *"persist and load on startup the message."* Claude Code itself **never restores its
input box** across a restart, and Phase-0 forensics proved it persists no draft state anywhere on disk
(the exhaustive `~/.claude` sweep — `state/` holds titles, `sessions/<pid>.json` carries no draft field;
only the §2b paste-cache exists, and only for pastes). So **our record is the only durable copy** of an
unsent message, and the design embraces that honestly:

- **Schema** — the trio `pendingInput` + `pendingInputUnixMs` + `pendingPasteRefs` persists on the
  session record (`Persistence.cpp`; all omitted when there is no draft, so a draft-free
  `sessions.json` is byte-unchanged). The **observation stamp** is the load-bearing honesty device:
  re-stamped quietly on every live read, so its age tells a LIVE draft (≤ a few ticks) from a carried
  MEMORY (frozen at the last real observation before shutdown/close/death).
- **Freshness** — the flip notify rides the normal autosave; a QUIET text drift additionally triggers a
  ~10s-throttled direct save (`_pendingDraftSaveMs`), and the teardown/close archive persists once more
  — so a graceful exit always lands the final text and a crash loses at most the throttle window.
- **Archive KEEPS the draft** (the decision the user should know about, with its trade-off): the
  liveness sweep's "dead → archived" path **no longer clears `pendingInput`** — a claude that died with
  an unsent message on screen is exactly the case worth remembering, and clearing there would defeat
  the whole feature on every app restart (teardown archives the fleet). The trade-off: a **resumed**
  session's fresh claude has an EMPTY box, so the memory briefly shows for a tab whose live box no
  longer holds it — accepted because (a) the tooltip labels it (*"last seen \<ago\> — remembered from
  before …"*), and (b) **revalidation erases it honestly**: once the tab starts, the first live reads
  either confirm the draft or the 2-tick clear debounce removes it (+ the flip persist wipes the disk
  copy). (The **§10 re-fill** now closes that window when enabled — the default: the pump types the
  memory back in and the live reads then genuinely confirm it.) The ONE eager clear kept is the
  **restart-tab swap** (`_RestartTabIntoFreshSession`) — the
  user watches that screen be destroyed, so a 5-second dots flash for a box that visibly no longer
  exists would be noise, not memory.
- **Restore display** — a restored record's draft shows the dots **before its tab ever starts**: the
  scan's NotConnected branch drives `_SetTabPending` from the stored value (the board card reads the
  registry directly and needs nothing). Since RENDERING.md §4c (in source 2026-10-03, not built yet)
  these remembered-draft dots are **still** (no pulse) until the tab's claude starts — the tab header
  reads the dormant half-hollow dot (`AgentStatusHalfVisible`), the board and side-panel cards read
  `SessionInfo::started` — and every pulse runs only while its header or card is on screen and Windows'
  "Animation effects" is on. A running pulse keeps XAML and DWM working on every display frame, and the
  release had 50 dormant tabs carrying a remembered draft. The board tip appends *"last seen \<ago\> — remembered from
  before this session's tab (re)started; it clears automatically once the live input box reads empty"*
  once the stamp's age passes ~15s (a live draft's stamp never ages that far), plus the §2b paste-refs
  annotation naming the cached content file(s).
- **What reload gives the user**: reopen Agentmaster → the tab/card show the dots + the labeled draft
  text (copyable from the tip) *before* focusing the tab; focusing it starts claude — and the **§10
  RE-FILL** (built, default ON) then **types the memory back into the fresh box** via the standby
  lane's `BuildPromptFill` + `ReadPendingInputDraft` verify, so the draft is really there again, one
  Enter away. With the cog's `restoreDraftOnResume` OFF (or on a refused/abandoned fill) the classic
  behavior holds: the empty box clears the memory within ~5s, and the draft's paste content stays
  recoverable forever via the named paste-cache file.

## 6. Verification

- ✅ **Detector**: `TestPendingInput` (engine harness) — the original 14 cases (single/multi-line, empty
  box, no box, sent-prompt-vs-box, menu rejection, rule classifier, marker-without-space, `›`,
  trailing-blank trim, blank-after-top-rule, cursor artifacts) **plus the §2a hardening suite**: the
  NBSP separator (draft + empty box), REAL-capture fixtures lifted byte-for-byte from live buffers
  (2.1.217 empty box; 2.1.218 collapsed-paste-only, multi-line, truncated-marker drafts — NBSP bytes
  and all), the preview-menu false positive (floating-border AND anchored-rule variants both rejected),
  the numbered-draft non-rejection, the pasted-marker upward-continue, the anchor discriminator, and
  the framed-future border skip.
- ✅ **Paste resolver**: `TestPendingPaste` — marker grammar (both forms, fragments, singular "line",
  non-markers ignored, two-per-line), both counting conventions against the live 274-segment
  unterminated shape, the REAL truncated arithmetic (head seg 8 + tail seg 266 + offset 258),
  off-by-one refusal, thin-anchor refusal, unique-match resolution, two-file ambiguity refusal,
  expansion (collapsed whole-line, typed-text refusal, truncated segment-substitution, wrong-file
  refusal), and the impure adapter against a temp cache dir (annotation format incl. the
  `(by line count)` tier label; missing dir ⇒ unresolved, never a throw).
- ✅ **Registry**: appear notifies / text-edit quiet / clear notifies / unknown-id no-op + the stamp
  lifecycle + the quiet paste-refs guard (`TestRegistry`). **Persistence**: the trio round-trips and is
  omitted when empty (`tests_persistence`). **Re-fill (§10)**: the `RestoredDraftSessionTakenOver`
  latch matrix (historical-never-blocks / Running-now / at-after-arm push+pull / the `>=` race edge /
  the NeedsApproval seed), the pure `ExpandDraftPasteMarkers` all-or-refuse suite (multi-marker
  stitch, unresolved/ambiguous/same-line/fragment refusals, the live truncated arithmetic), the
  `ExpandPendingDraftPastesIn` temp-cache adapter (+ missing-dir no-throw), and the
  `restoreDraftOnResume` settings round-trip.
- ✅ **Compose-box rule (§8b)**: the full 6-way relation table (incl. both newline-flavor folds, the
  trailing-slack trim, interior whitespace as content, contained-but-not-a-prefix ⇒ Divergent) + the
  offer policy (empty box / continuation auto-take; **box-ahead never offered**; Divergent offered on
  containment or on the 80%-over-50-chars ratio and NOT otherwise) + the containment **cost cap** in
  both directions (an oversized pair is skipped ⇒ no offer; a short box in a long draft still offered)
  + the **provenance guard** (`TextContinuesSeed`: unchanged / extended / the user's own text / nothing
  seeded / a box shortened below the seed / the newline fold) + the similarity metric itself
  (identical / disjoint / one-char-of-ten / prefix share / no double-counting of an overlapping run).
- ✅ **Draft cache (§8c)**: the store keys' names, a multi-line draft + its stamp round-tripping, the
  upsert replacing both halves, the redundant-upsert dedup, the clear removing BOTH keys while leaving
  the title intact, a stampless draft, a draft-only record being deleted when emptied, and the new
  batched `SetSessionStoreFieldsIn` (set + remove in one write, all-unchanged no-op, bad-id / empty-batch
  rejection). **2,954/2,954 checks pass.**
- ✅ **Full chain compiles**: `TerminalControlLib` + `TerminalAppLib` both green.
- ✅ **LIVE (out-of-band, 2026-07-23)**: the fixed detector run against the running fixture session
  ('wow sess', Claude 2.1.218, pid-resolved via the presence heartbeat) extracts the draft **clean**
  (1,036 chars, leading cp `0079` — the NBSP gone), and the resolver run against the real 268-file
  paste-cache resolves its `[...Truncated text #2 +258 lines...]` to **`bf8eefafa3e80676.txt`**
  (unique match) and expands the 1,036-char draft to the full **18,647-char** briefing. The fleet sweep
  (17 attachable screens, 2.1.217+218) detected the box **17/17** with zero false pendings.
- ⏳ **In-app runtime** (the deployed pulse + persistence round-trip through a real restart) rides the
  next deploy cycle — gated on the user's build/deploy permission. Once deployed, verify: type a draft
  in tab A → dots; close the app → `sessions.json` carries the trio; relaunch → the reopened tab shows
  the dots + the staleness tip pre-start; focus it → the memory clears within ~5s of claude starting.
  For **§8b/§8c** additionally verify: pull a draft into the compose box → edit it further **in the tab**
  → refocus the box ⇒ it **extends** to the full draft (no button); rewrite the prompt differently in the
  tab ⇒ the **pull button appears** (and does NOT for an unrelated draft, nor when the box is ahead);
  and `session-store/<sid>.json` grows a `draft` + `draftAtUnixMs` within ~10s of typing, loses both the
  moment the draft is sent, and holds the **expanded** text for a draft carrying a paste placeholder.

## 7. The "3 dots" indicator (built)

The detection NOTIFIES reliably on both transitions (§3 "Reliability"), so the indicator is a
**3-dot opacity pulse** ("typing"/waiting cue) on two surfaces, both driven by the observer's detection.
Its color is a **user-configurable LIGHT/DARK pair** auto-picked by the background luminance so the dots are
never invisible (see *Dots color* in §3):

- **Tab strip** — a tiny cluster **below** the status dot (`TabHeaderControl.xaml` `HeaderPendingDots`,
  bound to `TerminalTabStatus::AgentPendingVisible`, painted via `AgentPendingBrush`). Driven by
  `TerminalPage::_SetTabPending` straight from the UI-lane scan (the hosting window holds the tab). The
  pulse storyboard is **started/stopped on the flag**, so idle tabs animate nothing.
  **Contrast follows the SELECTED state, not just the dir color.** WT renders a colored tab very
  differently by state (`Tab::_ApplyTabColorOnUIThread`): a **selected/focused** tab shows the full per-dir
  color, but a **deselected/unfocused** tab shows it at **30% opacity over the dark tab-row color** — far
  darker — and recomputes the tab's own black/white text for each. So the dots can't contrast against the
  full color alone: on an unfocused colored tab they'd wash out. `TerminalPage::_PendingDotsColorForTab`
  reads `Tab::CurrentEffectiveTabBackground(perDirColor)` — the actual header background *as rendered right
  now* (selected ⇒ full color over row; deselected ⇒ 30% over row; mirrors `_ApplyTabColorOnUIThread`
  exactly, using the real `_tabRowColor`) — and feeds it to the same `PendingDotsColorFor` luminance pick.
  The per-tick scan does this when showing the dots, and `_RefreshPendingDotsContrast` (called from
  `_OnTabSelectionChanged`, no buffer read) re-picks immediately on a tab switch so the now-deselected and
  now-selected pending tabs flip light/dark without the ~2s scan lag.
  **Layout — a reserved band, not the old fixed wrap.** `HeaderAgentStatusDotWrap` is a 2-row Grid: an
  18px **glyph cell** (flash ring · favorite star · status dot · dormant half-dot · crown — geometry
  unchanged) over a **fixed 4px band** that holds the dots (total 22px, within the tab's content height so
  the strip doesn't grow). It used to be one fixed 18px Grid, which left **no room below the dot** — the
  17px favorite **star** fills the cell, so a draft's dots overflowed the wrap and the tab header **clipped
  them** (the worse, the lower they were pushed to clear the star). Reserving the band as a **fixed** row
  (not `Auto`) means the glyph cell never reflows when a draft appears/clears, so the **status dot stays
  stationary** and the dots just fade in/out in their band. Because the band sits strictly **below** the
  glyph cell, the dots clear the star's lower points with **no per-favorite vertical offset** (an earlier
  +3px star-only nudge is gone — it was what pushed the dots out of the wrap to begin with).
- **Triage-Board cards** — a pulse at the top of the card body (`AgentManagerContent::_MakeCard` →
  `BuildPendingDots`, passed the picked color), driven by the flip `_notify` rebuilding the board, so it
  works **cross-window** (a draft in window A shows on window B's GLOBAL board). The storyboard begins on
  `Loaded` (runs only while carded).

## 8. "Copy Current Prompt" — the draft as a COPYABLE value (built)

The draft is not only an indicator: **every copy menu can hand it to you**. All three session copy menus
— the per-tab overlay's copy button (`AgentTabOverlay::_BuildActionsRow`), the Triage Board /
Explorer-tree **Copy ▸** submenu (`AgentManagerContent::_MakeSessionMenu`), and the WT tab menu's
**Copy ▸** (`Tab::_CreateContextMenu`) — carry a **`Copy Current Prompt`** item that puts the session's
UNSENT input-box text on the clipboard, **verbatim and whole** (multi-line drafts included; no
truncation, no annotation — you paste exactly what was typed).

All three route through the ONE shared `CopySessionField` (`AgentCopyActions.h`, code **7**), so the
menus can never drift. **Claude only** — Codex's TUI has no `❯` rule-wrapped box, so no draft is ever
monitored for it; the item is omitted for a Codex session in all three menus (and `CopySessionField`
has a kind backstop).

**Two sources, one pure rule.** The item resolves through `PickCurrentPromptText` (`PendingInput.h`,
unit-tested), whose whole job is choosing between:

| | source | freshness | available when |
|---|---|---|---|
| 1 | **LIVE** — the input box read off the terminal buffer at click time (`TerminalPage::_ReadLiveDraftForSession` → `ControlCore::ReadPendingInputDraft` → `DetectPendingInput`) | this instant | the **calling window HOSTS the tab** and its claude has started |
| 2 | **REMEMBERED** — the observer's `SessionInfo::pendingInput` (§3) | ≤ one liveness tick, or a persisted MEMORY (§5) | always (it is registry state, shared process-wide) |

The rule: **a non-empty LIVE read wins; anything else falls back to the remembered value.** Deliberately,
an *empty* live read does **not** erase the fallback — the "3 dots" are driven by the remembered value
through the 2-tick clear debounce, so while the tab/card still says "this session holds an unsent
message" the copy must hand over that message rather than silently copying nothing.

**The live read is WRAPPED, because all of its failure modes are ordinary** and none may cost the user a
copy: the session's tab may live in **another window** (a different UI thread — the Manager board and
tree span the whole fleet, so this is the common case there), the tab may be **dormant** (window-restored,
claude never started — no buffer), the control may be **torn down mid-click**, or the buffer may not be
initialized. `_ReadLiveDraftForSession` catches everything (`AgentLogCaughtException`, Rule #18) and
returns `""`; the provider itself is optional, and `CopySessionField` guards the call too. Every one of
those paths lands on source 2 — the fallback is the *design*, not an error path.

**Wiring** (the provider is per-caller, because only the hosting window can read a buffer):

```
overlay copy menu  -> AgentTabOverlay::_onReadLiveDraft   (SetLiveDraftHandler,  _AttachClaudeOverlay)
Manager Copy >     -> AgentManagerContent::_liveDraftProvider (SetLiveDraftProvider, _WireAgentManagerContent)
WT tab Copy >      -> the CopySessionFieldRequested handler's lambda (TerminalPage.cpp)
                        \_ all three -> TerminalPage::_ReadLiveDraftForSession(sessionId)
```

**Observability** — the `[nav]` line is the intent (`copy current-prompt <sid8>`, from the shared action's
existing nav log), and a `[pending]` mechanism line names the source that answered:
`[pending] <sid8> copy current prompt: live|remembered chars=N`. With neither source there is **no
clipboard write and no chime** (an honest no-op, like copying an empty branch) plus
`[pending] <sid8> copy current prompt: nothing (box empty, no remembered draft)` — so a "why did nothing
happen?" is answerable straight from hooks.log rather than being a silent dead click.

**Pastes are expanded (§8e).** A draft containing a `[Pasted text #N +M lines]` /
`[...Truncated text #N +M lines...]` placeholder copies as the **pasted text itself**, resolved from the
paste-cache off-thread through the shared `CopyPromptTextExpandingPastes`, all-or-refuse — a placeholder
that can't be proven is copied **as rendered**, never guessed. (v1 copied every placeholder as rendered.)

### 8a. Pull it into the Auto-Testing compose box (built)

The Manager's **Auto Testing** compose box (the textarea you queue the next prompt in) takes the same
draft **on click**. The case it serves: you typed a prompt into a Claude tab's input box, never sent it
(its "3 dots" are pulsing), and came to the Manager to **queue** it instead — retyping it would be absurd.

**Trigger — click / tab into the box, with EDIT INTENT, while it is EMPTY.** `GotFocus` gated on
`FocusState::Pointer | Keyboard` (the path-picker idiom: the box is *also* focused **programmatically**
after every queue / Send-now — `_FocusPromptBox` — and pulling a draft in there would fight the user),
plus a `Tapped` handler for the "already focused, clicked again" case that raises no `GotFocus`. Both
**defer to a clean dispatcher tick**: inserting mid-click would let the pointer's release re-place the
caret inside the text just written (the box was empty when the press landed); one tick later the caret
parks at the end, ready to keep typing. Every guard is re-checked in the deferred body.

**Source — identical to §8**, through the same pure `PickCurrentPromptText`: the LIVE buffer read via
the content's `_liveDraftProvider` (→ `TerminalPage::_ReadLiveDraftForSession`, wrapped; `""` when the
tab is hosted in another window, dormant or unreadable), else the observer's `SessionInfo::pendingInput`
— which is also exactly what the "3 dots" the user is looking at are showing. So the compose box and the
copy menus can never disagree about what "the current prompt" is.

**Guards — every one is "never fight the user":**
- **managed CLAUDE only** (Codex renders no input box, so it never has a draft);
- **only an EMPTY box** (whitespace-only counts as empty, via `pending_detail::AllWhitespace`) — text
  already composed is never clobbered;
- **ONE-SHOT per (session, draft text)** (`_promptPrefillSessionId` / `_promptPrefillText`): after the
  user **queues** the pulled-in prompt (which clears the box) or deletes it, clicking back in must NOT
  silently re-insert it — that would **double-queue** the same prompt. A CHANGED draft, or another
  session, offers itself normally.

**It is a COPY, not a move** — reading the buffer never writes to it (Rule #13), so the draft stays in the
tab's input box; nothing is lost if you decide to send it there after all (and the dots keep pulsing until
you do).

**Discoverability** is the box's own **placeholder**, which is visible exactly when the feature applies
(an empty box): while the selected session holds a pullable draft it reads *"click to pull in this
session's unsent prompt…"*, otherwise the plain *"queue a prompt for the selected session…"*. It is
suppressed once that draft has already been pulled in (the latch would refuse the click), so the
placeholder never promises something the click won't do. Driven from `_RebuildPlan` off the **remembered**
value (a rebuild must never do a live buffer read) and change-gated. The box's tooltip spells the
behaviour out; a pull logs `[nav] compose pull-draft <sid8> src=live|remembered how=fill|extend|button chars=N`.

### 8b. Continuation check + the conditional pull button (built)

§8a only fires into an **empty** compose box. The case that follows immediately: you pull a prompt in
here, go **back to the tab**, keep editing it **there**, then refocus this box. Two texts now exist and
the UI must decide which is "the prompt" — **without ever destroying something you typed**.

**The pure rule** is `EvaluateDraftPull(box, draft)` (`AgentMaster/PendingInput.h`, beside the detector +
`PickCurrentPromptText`; `ClassifyDraftAgainstBox` is the relation alone). Both sides are normalized for
the comparison only: **newline flavors are folded** (a UWP `TextBox` reports a typed newline as `\r`
while `DetectPendingInput` joins body rows with `\n` — without folding, *every* multi-line draft would
read as unrelated to the same text in the box) and **trailing** whitespace is dropped (render slack /
a trailing cursor cell is never intent). Interior text, leading indent included, is content.

| relation | condition | meaning | what happens |
|---|---|---|---|
| `NoDraft` | the draft is empty/whitespace | nothing unsent | button hidden |
| `BoxEmpty` | the box is empty/whitespace | the §8a case | **filled** on focus/tap |
| `Same` | equal after normalizing | in sync | button hidden |
| `Continuation` | the draft **starts with** the box, and is longer | you kept typing in the terminal | **extended in place** on focus/tap — non-destructive, the box is a strict prefix — **but only if the box is text WE put there** (below) |
| `BoxAhead` | the **box** starts with the draft, and is longer | the box already holds everything the draft has, plus your addition | **never offered** — taking it would DELETE that addition and gain nothing |
| `Divergent` | neither is a prefix of the other | possibly a different prompt entirely | button shown **only if related** (below) |

**Only the two relations that cannot lose composed text are applied automatically** (`BoxEmpty`,
`Continuation`) — overwriting composed text has to be an explicit act, which is what the button is for.

**Provenance guard on the automatic extend** (found in self-review). A strict prefix loses no
*characters*, but rewriting a box the user typed **themselves** is still a mutation they did not ask
for: type `fix the` by hand while the session's draft happens to read `fix the tests`, click to place
your caret, and the box grows under you with the caret jumping to the end. So an extend only ever
**continues text we ourselves put there** — `TextContinuesSeed(box, _promptPrefillText)` plus a matching
session id (the pure helper normalizes the same way, and an empty seed is never "ours"). A continuation
of the user's own text is not applied; it is **offered by the button** instead, which costs one explicit
click and loses nothing.

**The button** (`_pullDraftBtn`, glyph `\xE896` "download" = *bring it down here*) appears **only while
there is something worth offering that taking would not destroy**, and its tooltip says outright that it
**replaces** what is in the box. For a `Divergent` draft that means the two must be demonstrably the
*same prompt evolved* — the user's rule, *"only if it [is] contained within the new prompt or similar by
at least 80% (and >50 chars)"*:

- the box appears **verbatim inside** the draft (containment, not merely a prefix — e.g. a word prepended
  in the terminal): nothing in the box is lost, so no length floor applies. ⚠ The probe is
  **cost-capped** (`kDraftContainmentMaxProduct`, 4M): `std::wstring::find` is a naive
  O(needle × haystack) scan and this runs **per keystroke**, so two 20 KB texts would be ~4e8 character
  comparisons per key — a visible stall, and unbounded per-tick work is exactly what froze the app once
  (SUMMARY_JUMP.md §4a). Over the cap the probe is skipped and the linear ratio below decides alone, so
  the failure mode is the conservative one (no offer). Every realistic shape stays well under it — a
  100-char box inside a 20 KB draft is 2e6; **or**
- both texts exceed `kDraftSimilarityMinChars` (50) **and** score `kDraftSimilarityPercent` (80) or more
  on `DraftSimilarityPercent`.

`DraftSimilarityPercent` is deliberately **not** an edit distance: it is the share of the longer text the
two agree on at their **edges** (common prefix + the common suffix of what is left). Two reasons, both
load-bearing. **Cost** — this runs on the box's `TextChanged`, i.e. once per keystroke, where a
Levenshtein matrix over multi-KB prompts would be milliseconds of UI-thread work per key; this is one
linear pass, no allocation. **Direction of error** — editing only the middle costs exactly
`prefix + suffix`, so `distance <= max - (prefix + suffix)`: the value is a **lower bound** on the
edit-distance ratio, hence "edges >= 80%" *implies* ">= 80% similar". It can never over-report, only
under-report (an edit at *both* ends), and under-reporting merely **hides** the button — the conservative
direction, since the alternative is silently offering to overwrite composed text on a weak signal.

**Where it is evaluated.** Three triggers, only one of which may read a buffer:
- **focus / tap** (`_MaybePrefillPromptFromDraft`) — the user's actual flow (edit in the tab, come back),
  and the one place a **live** read is affordable. Applies `BoxEmpty` / `Continuation`, then refreshes
  the button.
- **the box's `TextChanged`** — button **visibility only**, against the **remembered** draft. Never a
  live buffer read (and never a disk read) per keystroke.
- **`_RebuildPlan`** — beside the §8a placeholder update, so a NEW draft (the appear/clear flip notify)
  reaches the pane.

**Known staleness limit:** `SetPendingInput` notifies only on the empty↔non-empty **flip** (§3), so a
*text-only* edit in the terminal is quiet — the button cannot appear on its own while you are editing
over there. The focus/tap live read is what makes the flow work; a page→content nudge on a selected
session's quiet text drift would close that gap and is a deliberate non-goal for now (it is
same-window-only, since the board spans the whole fleet).

**Placement — why it shares column 2 with the templates (paper) icon, side by side.** A control that
appears and disappears must not resize the compose row. The row's height is set by its tallest child, and
the icon strip in column 0 is **horizontal** (one button tall) — so stacking a second button *under* the
paper icon would grow the whole row by a button every time this one showed up. Beside it, column 2 stays
one button tall and only its **width** changes: the textarea's right edge moves, its left edge (where the
caret and the text are) never does, and the paper icon keeps its place at the far right.

**Guards** are §8a's, restated for the non-empty box: managed **Claude** only; the EXTERNAL scope never
offers (observe-only); the **one-shot latch** still blocks a *silent* re-insert of a draft already pulled
in (which would double-queue it), while the **button bypasses the latch** — it is user-initiated, and
re-arms it. A click re-reads live (the box may have moved on since the button was painted) and focuses
the box afterwards. It is still a **copy**: the draft stays in the terminal.

### 8c. The draft CACHE — a durable per-session mirror (built)

*"Cache the prompt in memory and if the observer finds out there was a change then empty the cache or
upsert the cache in the per session persistence."*

The in-memory cache is `SessionInfo::pendingInput` (§3) — already per-session, already observer-written,
and already persisted on the **fleet record** (§5). §8c adds the **per-session** mirror: two keys on the
durable `SessionStore` KV (`<profile>/session-store/<sid>.json`, the same file that carries the session's
title / favorite / tags):

| key | value |
|---|---|
| `draft` | the unsent draft, paste-**expanded** where the cache resolves it (below) |
| `draftAtUnixMs` | when it was last observed — the same honesty device as `pendingInputUnixMs`: its **age** is what separates a live draft from a carried memory |

Both are ONE fact, so they are written and cleared in **one** read-modify-write
(`SetStoredSessionDraftIn` → the new batched `SetSessionStoreFieldsIn`): a crash between two separate
writes can never leave a draft with no stamp, or a stamp with no draft. An empty draft **removes both
keys**, and a session whose only datum was a draft loses its file — the store stays sparse.

**One writer, at the seam that already knows** (`TerminalPage::_ScanPendingInputImpl`, beside the
existing throttled `SaveSessions`), so the fleet copy and the per-session copy cannot disagree:

```
flip -> empty              => _PersistSessionDraft(id, "")            // "empty the cache" (never throttled)
textChanged && hasPending  => _PersistSessionDraft(id, effectiveDraft) // "upsert" (~10s, PER SESSION)
```

The upsert throttle mirrors the §5 sessions.json drift save (the draft moves per keystroke; the durable
copy only has to stay within a throttle window of the live box) but is **per session**, so one busy
session cannot starve another's. The first observation of a new draft is never throttled (the counter
starts at 0), and the **clear is not throttled at all** — it is the correctness-relevant half, since a
stale stored draft would keep describing a message that was already sent. **Known limit:** unlike
sessions.json, which the teardown/close archive flushes one last time, nothing re-writes the store after
the final throttled edit — so a draft the user stops editing can sit up to one throttle window (~10s of
typing) behind. The authoritative copy on the fleet record is complete either way; a teardown flush for
the mirror is a cheap future addition if a reader ever depends on the last keystroke.

`_PersistSessionDraft` is **detached + off-thread** (the expansion reads the whole paste-cache directory,
and no state write belongs on the UI thread) and carries the `SetPendingPasteRefs` **ordering guard**: the
registry, not the coroutine's argument, is the authority on what the draft IS, so each write re-reads it
and proceeds only while it still agrees. Two in-flight writes for one session (a drift, then a clear a tick
later) therefore cannot land out of order and resurrect a sent draft.

**Paste expansion — the stored value is the WHOLE prompt.** A draft reading `[Pasted text #3 +258 lines]`
on screen is not the full message, so the markers are resolved against the paste cache before storing
(`ExpandPendingDraftPastes` — content-anchored, all-or-refuse, §2b). A **refused** expansion stores the
**rendered** text instead: the store's job is to not lose the memory, and the placeholder form is still an
honest record of what was typed. (§9's "never re-type a placeholder" rule governs *filling a box*, not
*remembering a draft*.) A marker-free draft costs one scan and no cache IO. An expansion that actually
substituted content logs `[pending] <sid8> stored draft: expanded N paste(s), chars=… (rendered …)`; a
routine upsert is deliberately silent (it rides the scan cadence and would be noise), and a failed write
logs `[persist-fail]` — the state-loss case.

**Lifecycle** follows §5 exactly: **archive KEEPS the draft** (a claude that died holding an unsent
message is the case worth remembering), and the **one eager clear** is the restart-tab swap
(`_RestartTabIntoFreshSession`), which now clears the store alongside the record.

**Precedence — the registry is AUTHORITATIVE; the store is not a fallback tier for today's callers.**
This is a deliberate deviation from the original design sketch. Every current consumer of "the current
prompt" (§8's three copy menus, §8a/§8b's compose box) resolves a session the **registry knows** — the
copy action returns early for an unknown id, and the compose box only acts on a live selected session.
For such a session an empty `pendingInput` is an authoritative **"there is no draft"**, never a
"don't know", so falling back to the stored copy could only ever surface a **stale** draft (the one just
sent, in the window before the async clear lands). The read accessors (`GetStoredSessionDraft` /
`GetStoredSessionDraftAt`) therefore exist and are tested, but nothing reads them yet: their payoff is a
caller the registry **cannot** answer for — a closed / never-managed session row on the Sessions page, or
the `agentmaster` CLI — which is where they should be wired next.

### 8d. Queue it straight from the tab — the overlay's MAIL button (built)

The third consumer of the same draft, and the one that needs no Manager visit at all: the per-tab
badge's **mail** button (`AgentTabOverlay::_QueueCurrentPrompt`, row 1 between the folder and copy
buttons — TAB_OVERLAY.md §13j) **queues the unsent draft into this session's own Auto-Testing queue**
in one click, and **clears it out of the input box** — the draft is *moved* to the queue. §8a answers
"I want to queue this, but in the Manager"; this answers "I want to queue this, from right here."

**Same source, same rule** — the wrapped LIVE read (`_onReadLiveDraft` →
`TerminalPage::_ReadLiveDraftForSession`) else `SessionInfo::pendingInput`, chosen by the one pure
`PickCurrentPromptText`. Unlike the Manager's copy of this decision, the **live** read is the one that
normally answers here: the overlay sits inside the session's own pane, so its window always hosts the tab.

**Same append as the envelope** — one `SessionRegistry::Update` pushing a default `QueuedPrompt`
(`Pending` / `OnTurnComplete`, the 56-char flattened label) onto `s.queue`, byte-for-byte what
`AgentManagerContent::_OnAddPrompt` builds, so the scheduler cannot tell them apart.

**Shown only where the queue means something:** `Profiles::IsDevOrDebugPackage()` (the Tests Autorunner
subsystem is dev-or-debug; in a release the `Scheduler` never starts, so the button would append prompts
nothing would ever send) **and Claude only** (no `❯` box on Codex ⇒ never a draft), with a kind backstop
in the handler.

**Enabled only while a draft exists** (`AgentTabOverlay::_RefreshQueueButtonEnabled`, from `_Refresh`): the
button is greyed **disabled** on an empty box and clickable exactly while the session holds an unsent draft,
so a prominent always-present toolbar button never fires a *silent* no-op — it tracks the same
`pendingInput` signal as the "3 dots", and reliably, because `SetPendingInput` notifies on precisely the
empty↔non-empty flip this turns on (a text-only edit doesn't notify but can't change the boolean either).
The empty-box handler branch below stays as the backstop.

**MOVE by default, COPY on Shift+Click.** A plain click **removes** the draft from Claude's input box
after queueing it — the prompt is *moved* to the queue, so you are not left with a duplicate to clear by
hand. **Shift+Click** passes `clearBox == false` and **keeps** the draft in the box (the historical copy,
Rule #13 — the read never wrote to the box; the terminal draft and its pulsing "3 dots" stay). Shift is
read at click time via the overlay's shared `IsShiftDown()` (the summary-panel Shift-resize helper).

**The removal is a verified DISCARD, deliberately NOT the swap's clear ladder**
(`TerminalPage::_ClearLiveDraftForSession`, run AFTER the queue append succeeds — a failed/empty queue
never touches the box). The first cut *did* reuse §9's ladder, whose first rung is `Ctrl+S` — and that
shipped a real bug: **Claude's stash is not a discard** (the §9 caution below — a stashed draft is
auto-restored into the box ~0.4 s after the *next* submit), so a stash-and-walk-away clear planted a
scheduled re-paste, and the moved draft reappeared in the box seconds after its own queued copy was
delivered (the "Second pass please" incident, session `d373b992` — it read exactly like a double-send,
because the stash content and the queued prompt are the same bytes by construction). The MOVE-clear now
runs the **discard ladder** (`DecideDraftDiscard`, PendingInput.h — each rung probe-verified live):
per-round **`End` + `Ctrl+U`** (End first: `Ctrl+U` kills only caret→line-start, so a mid-line caret
leaves a tail without it; ~a line per effective round, tolerating the measured no-op press between
lines — only TWO consecutive no-change rounds judge the rung dead), then per-round **`End` +
backspaces** (blind backspaces stall permanently once the caret hits position 0 with lines still below —
measured; a `[Pasted text #N]` placeholder deletes atomically on one press), then give up. Both rungs
are **true discards** (a killed draft never returns across a submit; the kill-ring keeps a manually
recoverable copy — `Ctrl+Y`) and both are **mid-turn safe** (a streaming turn survives untouched;
`Esc`, by contrast, *interrupts* a running turn even with text in the box, which is why it is not a
rung). Mechanics otherwise unchanged: lock the control read-only so the user's keystrokes can't
interleave, each round settle + re-read against the **live** box (`ControlCore::ReadPendingInputDraft`
on a 60 ms poll — the direct buffer read, never the scanner's cached copy, so the loop exits within one
poll tick of the box actually emptying), **no send and no restore**, and the lock is handed back the
**instant** the outcome is known — the unlock precedes every piece of bookkeeping (registry write, log
append), and is released on every path. It shares the swap's box-mutex (`_draftSwapsInFlight`) so a
concurrent send declines rather than injecting alongside it, opens the delivery gate's reserved clear
tag, and is a no-op when the box isn't readable here / is already empty. On a **verified** clear it
calls `SetPendingInput(id, "")` — the draft is no longer unsent, so the "3 dots" drop and the MAIL
button disables **now** (not after the scanner's ~2-tick debounce), which also closes the window where
a fast re-click would re-queue the still-remembered draft. If the box **won't** empty (the ladder gives
up), the prompt is still queued and the draft is simply left in the box (degrading to the Shift+Click
outcome), logged. Traced `[draft-clear] <sid8> … via End+Ctrl+U discard`.

**No one-shot latch** on the queue itself (unlike §8a's silent focus-pull): the click is explicit, so
clicking a present draft twice queues twice, like the Manager's envelope (a latch is also
unblockable-in-practice, since a text-only draft change raises no notify to re-arm one) — though with the
default move-clear the box empties after the first click, so the button disables and a second queue needs
a fresh draft. Nothing to queue ⇒ nothing queued, **no chime**, no clear, and `[pending] <sid8> queue
current prompt: nothing (box empty, no remembered draft)`; a success chimes and logs `[nav] queue <sid8>
"<label>" (overlay draft, clear box|keep box)` plus `[pending] <sid8> queue current prompt:
live|remembered chars=N`.

### 8e. Copy hands over the PASTED TEXT, not its placeholder (built)

> **2026-09-25:** the expansion now resolves through the §2c **paste journal** first (`ResolvePendingDraftCarry`,
> LIVE policy — the journal snapshot is taken on the UI thread via the new optional `registry` argument of
> `CopyPromptTextExpandingPastes`), the paste-cache second. Before this, a FRESH paste could never expand
> (current Claude Code writes the cache only at submit), so the copy handed over the label. A mid-line label
> nothing backs is now kept as written (a quote), claude's bare `[Pasted text #N]` is recognized, and the
> span-based substitution no longer refuses a placeholder that shares its line with typed words.

**The report.** In the Manager's Auto Testing, right-click → **Copy** on a queued prompt that held a large
paste put the literal `[Pasted text #1 +31 lines]` on the clipboard — a label that means nothing outside
the one input box that rendered it.

**Why a queued prompt holds a label at all.** Claude Code shows a large paste in its box as a one-line
placeholder (§2b), and both draft consumers queue the box's text *as rendered*: the overlay's MAIL button
(§8d) verbatim, and the compose box (§8a) through whatever was pulled into it. So the Auto-Testing row
stores `[Pasted text #N +M lines]`, not the paste — measured in the release fleet: five mail-queued rows
consisting of nothing but a placeholder (`[Pasted text #1 +11 lines]`, `[Pasted text #2 +6 lines]`, …).

**The fix — ONE shared "copy a prompt" action.** `CopyPromptTextExpandingPastes` (`AgentCopyActions.h`,
implemented beside `CopySessionField` in `AgentTabOverlay.Actions.cpp`) now backs BOTH prompt-copy
surfaces, so they can never disagree about what a prompt holds:

| surface | text | expanded? |
|---|---|---|
| Auto Testing row menu → **Copy** (`_MakePromptMenu`), row origin **Autorun** (we queued it) | `QueuedPrompt::text` | yes |
| Auto Testing row menu → **Copy**, row origin **Typed** | `QueuedPrompt::text` | **no — verbatim** |
| every **Copy Current Prompt** (overlay / Manager `Copy ▸` / WT tab `Copy ▸`, `CopySessionField` code 7) | the §8 live-else-remembered draft | yes |

- **No placeholder** (the common case): one cheap pure `FindPasteMarkers` scan, then the historical
  synchronous verbatim copy — no cache IO, no thread hop, byte-for-byte unchanged.
- **A placeholder:** the text is newline-**folded** (`FoldCrToLf` — the compose `TextBox` stores a newline
  as `\r` while the resolver's line model is `\n`-split, so unfolded, a placeholder alone on its line reads
  as mid-line and is refused; pinned by `TestPendingPaste` §8), expanded **off the UI thread** by the §10
  whole-draft adapter `ExpandPendingDraftPastes` (it reads the whole paste cache — ~6.7 MB / 415 files on
  the dev box, capped at 64 MiB — which a click must never pay on the UI thread), and the clipboard write
  hops back through the window's `DispatcherQueue` (so the copy lands a moment after the click; the copy
  chime marks it).
- **All-or-refuse, never a wrong expansion.** The same rules as §10: a collapsed label must be the only
  visible content of its line and match exactly ONE cached file by line count; a truncated label must
  match its head/tail content anchors at the exact segment offset; one unproven placeholder refuses the
  whole text. A refusal (a cache file swept, two same-length pastes, typed words beside a collapsed
  label), a throw, or a missing dispatcher copies the text **as rendered** — never nothing. The one path
  that copies nothing: the window closed while its expansion was still reading the cache (traced).
- **Typed rows are copied verbatim, deliberately.** A Typed row is recorded from the `UserPromptSubmit`
  hook, i.e. AFTER claude expanded its own placeholders (measured: a delivered 25-char label arrived as a
  437-char `[ups]` prompt), so a placeholder inside it is literal text the user actually sent — quoting
  one — and "expanding" it would change what they sent.
- **A newer copy wins.** Every call into the shared copy actions (`CopySessionField` too) takes a new
  process-wide generation; an expansion that lands after the user already copied something else drops its
  write (`superseded`), so the slow write can never bury the newer clipboard.

**The decision the old follow-up left open — may a copy differ from the screen?** Yes. The placeholder is
Claude's DISPLAY compression of one input box; outside it the label is inert — pasting it back into claude
does not re-bind the content, it submits the literal label (the §9/§10 rule). So a copy yields what the
prompt MEANS. Nothing on screen changes: the Auto-Testing row and the tab still show the label.

**Limits (known, accepted for a copy).** The collapsed tier is only as strong as "exactly one cached paste
has that many lines": a placeholder merely QUOTED alone on its own line whose count happens to match a
cache file would be expanded (visible in the paste, and recoverable — never a SEND decision). A collapsed
label with typed words on its own line refuses (the resolver replaces whole lines and never eats typed
text) — which is also what keeps a handover brief that quotes a placeholder mid-sentence intact; every
real draft observed puts the typed text on the NEXT line, which expands.

**Observability.** `[nav] copy auto-testing-prompt <sid8> (auto|typed)` (the Auto-Testing Copy intent; Copy
Current Prompt keeps its `[nav] copy current-prompt <sid8>`), then, only when a placeholder was present,
one `[pending] <sid8> copy <what>: …` mechanism line naming the outcome — `expanded N paste
placeholder(s) from the paste-cache chars=A->B` · `paste placeholder(s) NOT expanded (no proven paste-cache
match) - copied as rendered` · `paste expansion threw (see the [exc] line) - copied as rendered` ·
`superseded by a newer copy …` · `the window closed before the paste expansion finished (nothing copied)`.

**Not fixed here — the SEND side of the same label** (see Follow-ups): a queued placeholder is also what
the Tests Autorunner DELIVERS. In the release log all five placeholder-only mail-queued prompts were
delivered as the bare 25–26-char label (`[delivered] … verified=exact`), with two outcomes, both wrong:
- **four** — claude expanded the label through its still-bound paste (the MOVE-clear's discard leaves the
  binding), so the `[ups]` echo carried the full text (437 / 650 / 1073 / 1622 chars) and was recorded as a
  separate **Typed** row; our row never saw its echo, so after the turn the Enter-retry watchdog pressed
  three times into an empty box and marked it **`Failed` — and PAUSED the session's autorunner**;
- **one** (`d373b992`, the Ctrl+S-stash-era clear, which carried the binding away) — `[ups] chars=26 ->
  echo consumed`: the model received the **literal label**, and the pasted content was silently lost.

## 9. The DRAFT SWAP — sending a prompt without eating your unsent draft (built)

**The bug.** A queued prompt is delivered as a bracketed paste plus a submit CR
(`BuildPromptSubmission`). A paste lands **at the cursor**, so if the input box already held an unsent
draft, the CR submitted **draft + prompt as one message** — one the user never wrote and never pressed
Enter on — and the draft was gone with it. Nothing in the state machine could prevent this: a draft is
deliberately a transient **fact**, never `SessionState` (Rule #7/#13), so a session holding one still
reads `Idle` / `WaitingForInput` — exactly the "ready to send" set `DecideAdvance` fires on. (The
`pauseOnHumanInput` toggle that looks like it should have covered this has never been wired: its
`lastHumanInputUnixMs` gate has zero feeders, which the cog says out loud in its own label.)

**The fix.** Take the draft out of the way, send, put it back. Every step is **verified by re-reading
the box** — nothing is assumed to have worked — and the whole sequence is bracketed by a read-only
window so the user's own keystrokes cannot interleave with it.

| # | Step | What it does |
|---|------|--------------|
| 1 | **READ** | `PickCurrentPromptText(live, remembered)` — the §8 rule: the live buffer read wins, the observer's remembered draft is the fallback. An empty result is re-read once (the TUI repaints constantly; a single missed frame must not read as "no draft"). |
| 2 | **LOCK** | `TermControl::SetReadOnly(true)`. The user still **sees** everything happening in the tab; they just cannot type into it for the ~1 s it takes. Silent — WT short-circuits the read-only check for key events, so there is no warning dialog per keystroke. Only released if **we** took it (a read-only the user set themselves is left alone). |
| 3 | **CLEAR** | `Ctrl+S` — **Claude's own stash** — then re-read until the box is *confirmed* empty. `DecideDraftClear` escalates: stash → `Ctrl+U` kill-ring → `DEL` (0x7F) backspaces → give up. Each rung gets a **settle** (300 ms) before it is judged, so a slow repaint is never mistaken for an unbound key. |
| 4 | **ABORT** | If the box never confirms empty: **send nothing**, roll the prompt back to `Pending`, restore the box, unlock. A prompt sent late is recoverable; a mangled message is not. |
| 5 | **SEND** | The unchanged `BuildPromptSubmission` recipe — so the echo dedup, the pickup guard and the Enter-retry watchdog all still apply to it. |
| 6 | **AWAIT** | Wait for the prompt to actually *leave* the box: a turn-started signal (the three `DecideEnterRetry` trusts) **and** an empty box. If it is still sitting there un-submitted we do **not** restore — yanking then would merge into it, the very bug this exists to prevent — and the draft stays in the kill-ring (one manual `Ctrl+Y`) and in the registry's memory, both logged. |
| 7 | **RESTORE** | Through **whichever rung emptied the box** — `Ctrl+S` again for a stash, `Ctrl+Y` for a kill — and the result is **compared against the draft we read**. Neither channel is guaranteed (a multi-press or mixed clear, or a submit that flushed the ring, can hand back the wrong text), so on a mismatch the box is emptied again and the draft is re-pasted verbatim with `BuildPromptFill` — the /handover-standby channel: a bracketed paste with **no** submit CR. Never both: the paste only runs on a box verified still empty. |
| 8 | **UNLOCK** | Release the read-only window and re-record the draft (`SetPendingInput`), so the "3 dots" stay honest throughout. |

**Why `Ctrl+S` is the default rung.** It is Claude's **stash** — a toggle over one slot: pressed with
text in the box it lifts the **whole box** aside and empties it; pressed on an empty box it restores.
Both of our presses land on the right side of that toggle by construction (we clear only when the box
has text, restore only when it is *verified* empty), and unlike `Ctrl+U` it is **not line-scoped and not
cursor-relative** — which is exactly why it replaced the kill-ring pair as the default. `Ctrl+U` only
killed the current line and restored badly when the cursor sat mid-text, so it is now the fallback
(`AppSettings::draftSwapUseCtrlS`, the cog's *"Use Ctrl+S to stash my draft aside while it sends"*,
**checked by default**).

⚠ **One slot.** A stash the *user* had already made is discarded by ours. That is unavoidable — the
slot is not inspectable — and worth knowing; their **live** draft is never at risk, only a previously
stashed one.

⚠ **The stash is not a discard — claude AUTO-RESTORES it at the next submit** (measured 2026-08-07 on
a live PTY: a stashed draft survives idle indefinitely — the status line reads `› stashed` while the
slot is loaded — and is popped back into the input box **~0.4 s after the next message submission**,
whoever submits it; the same behavior RC4 observed from the other side as "a Ctrl+S stash popping
back"). The swap is safe *because it always consumes the slot*: its restore pops it back deliberately,
or the auto-pop beats it and the verify-first restore leaves the box untouched. Any clear that stashes
and **walks away** — no restore step — plants a scheduled re-paste for whichever submit comes next;
that was the §8d mail-move bug ("Second pass please" reappearing right after its queued copy
delivered). A MOVE-style clear must use the discard ladder (`DecideDraftDiscard`), never this rung.

⚠ **Never press the toggle blind.** `Ctrl+S` on a box that still has text *stashes* it rather than
restoring, so `_RestoreDraftAfterSwap` re-reads and **skips the press unless the box is verified empty**,
and the mismatch path clears with `Ctrl+U` rather than `Ctrl+S` (a stash press there would push the
leftover into the slot — the one place the user's original may still be sitting). `kMaxDraftStashPresses`
is **1** and must stay 1: a second press un-stashes.

**Why the TUI's own channel is tried first.** It returns Claude's *internal* state, so a draft holding a
`[Pasted text #N +M lines]` placeholder still refers to the real paste-cache content afterwards. That is
also why the paste fallback is **refused** when the draft contains a placeholder (`FindPasteMarkers`):
re-typing `[Pasted text #1 +50 lines]` would put that label in the box as literal text and silently drop
the content behind it. In that case the box is left empty and the draft kept in memory, loudly logged —
worse than a perfect restore, much better than a corrupted one.

**One seam, four callers.** The swap lives behind `SessionRegistry::SubmitPrompt`, and **every** path
that sends a queued prompt goes through it, so they can never disagree: the autorunner's auto-send
(`Scheduler::_process`), its SemiAuto `Confirm`, the Manager's **Send-now**, and the /handover paste
pump. The hosting window registers a `PromptSubmitter` next to its injector (only that window can read
the control's box or block its keyboard); with none bound `SubmitPrompt` *is* the historical
`Inject(BuildPromptSubmission(...))`, so a host that never registers one behaves exactly as before.
`_AcceptPromptSubmission` returns "accepted" synchronously — the swap is asynchronous, and an accepted
submission that later aborts rolls the prompt back **itself** (`RollbackPromptToPending`), so Rule #4
holds on every path. `refundAutoSend` is true only for the autorunner paths, which spent an
`autoSendsThisRun` slot; a Send-now never did.

**Interaction with the scan lane.** `_ScanPendingInput` **skips** a session whose swap is in flight:
mid-swap the box is deliberately empty, and letting the clear debounce see that would erase the very
draft the swap is carrying (and drop the dots for a second). The swap re-records the draft when it
finishes.

**Off-switches.** `AppSettings::preserveDraftOnSend` (cog → **Tests Autorunner** → *"Preserve my unsent
draft when a prompt is sent"*), **default ON**, and beneath it `AppSettings::draftSwapUseCtrlS`
(*"Use Ctrl+S to stash my draft aside while it sends"*), **also default ON**, which picks the stash rung
over the kill-ring fallback. Off restores the historical merge behavior verbatim. A
session with an empty box takes the same fast path either way — there is nothing a paste could merge
into, so the injection is byte-identical to before.

**Logging** (`hooks.log`, `[draft-swap] <sid8> …`): the hold + lock, an ABORT with the rung counts, a
declined overlapping swap, the deferred restore, and the outcome (`restored by KILL-RING yank` /
`by PASTE` / `could NOT be put back verbatim`). So "why did my draft change?" is answerable off the log.

**Coverage.** The pure half is unit-tested in the engine harness (`TestPendingInput`, §26): the four
control-code builders + the round cap, the emptiness rule, **`Ctrl+S` is pressed at most once** (it is a
toggle — a second press would un-stash), the hand-over to `Ctrl+U` when a stash changes nothing, the
setting-off path starting at the kill rung, the shrink-driven kill rung, the fall-through to backspaces,
every rung cap, "never goes back to an earlier rung", and a **termination** proof under both settings —
an unresponsive TUI reaches `GiveUp` in a bounded number of steps having pressed `Ctrl+S` at most once.
Both settings round-trip in the `AppSettings` persistence test. The in-app behaviour rides
the next deploy cycle.

## 10. RE-FILL ON RESUME — the remembered draft typed back into the box (built)

The §5 memory made an unsent draft *survive* a restart; this makes it *come back*. Claude never
restores its own input box (Phase-0 forensics, §5), so when a session whose record carries a
non-empty `pendingInput` is REOPENED — a Sessions-browser **Resume here**, a window-restore rehome, a
re-fork; every path funnels through `_LaunchClaudeSession` — Agentmaster types the remembered draft
back into the fresh claude's box itself, through the machinery /handover-standby already proved:
**`Inject(BuildPromptFill(text))`** (the bracketed paste with **NO submit CR** — nothing runs until
the user presses Enter) followed by a **read-back verify** (`ReadPendingInputDraft`; no echo ever
confirms a fill). Gated by the cog's **`restoreDraftOnResume`** (TESTS AUTORUNNER, beside the §9
draft-swap toggles; **default ON** — OFF restores the display-only memory verbatim).

**The pipeline** (`TerminalPage.AgentObserver.cpp`):

- **ARM** (`_ArmDraftRestore`, fired from the launch seam when the reopened record's `pendingInput`
  is non-empty): capture the **arm instant** (system clock — the takeover baseline, taken BEFORE any
  await so a racing prompt always reads as *after* arming), then prepare the FILL text. A draft whose
  text carries `[Pasted text #N +M lines]` / truncated markers must NOT be re-typed literally — the
  label would render indistinguishably from a real placeholder but **submit as junk text**, silently
  dropping the pasted content (the §9 draft-swap refusal rule) — so the memory is **EXPANDED** against
  the paste-cache first (`ExpandPendingDraftPastes` → the pure all-or-refuse
  `ExpandDraftPasteMarkers`, off-thread like the §2b resolve: any unresolved/ambiguous/refused marker
  refuses the WHOLE draft and nothing arms; markers substitute LAST-first so earlier markers' recorded
  lines stay valid). An expanded fill is *better* than the original render: claude re-collapses a
  large paste at paste time and re-binds it in its own live state, so the restored draft is
  submit-faithful. A refusal logs and leaves the classic §5 behavior (memory → revalidation clears).
- **PUMP** (`_PumpDraftRestores`, ticked with the other lanes; self-marshals to the UI thread): waits
  for `SessionInfo.started` + injector — **unbounded pre-start** (a background-restored tab starts on
  a HUMAN's first visit, the `_SweepHandoverDeletes` lesson; the entry is a few bytes) — then a 1.5s
  settle, then fills + verifies exactly like the standby lane: box non-empty ⇒ **VERIFIED** (the scan
  re-reads it live next tick and re-stamps the memory); still empty past 12s ⇒ the TUI ate the paste
  pre-raw-mode ⇒ re-fill, ≤2 attempts, then give up (memory left to revalidation, logged); a
  started-but-never-readable box is capped at 10 min so a wedged entry can't hold the scan off
  forever.
- **HANDS-OFF latch** — `RestoredDraftSessionTakenOver` (SessionModels.h, pure + unit-tested), the
  **resume twin** of `StandbySessionTakenOver`: that latch's `!= 0` test is only correct for a fresh
  successor, while a resumed session carries HISTORY (a same-run close→resume keeps
  `turns.lastPromptUnixMs` in the registry record; the observer refills `convLastActivityUnixMs` with
  the conversation's whole past), so it would read *every* restored session as taken over. The restore
  latch keys on the **arm instant** instead: hands off iff Running NOW, or a prompt / real transcript
  line activity landed **at/after arming** (user, autorunner consuming a reopened queue, /handover) —
  historical values are strictly before it and never block, and a crash-preserved at-rest
  `NeedsApproval` seed deliberately doesn't either ("you were answering this when the app died" is
  exactly a box worth re-filling). A pre-fill box already holding ANY text also drops the entry —
  the user typed their own draft, theirs wins, and the scan records it as the new memory.
- **SCAN HOLD** — while an entry is armed/in flight, `_ScanPendingInput` **skips** that session (the
  §9 swap-skip idiom) and drives the dots from the memory: the freshly resumed box is empty until the
  pump fills it, and letting the 2-tick clear debounce see those first empty reads would erase the
  very memory being delivered. The hold releases the moment the entry resolves (verified / refused /
  abandoned), and the next tick revalidates against the live box as usual — so the indicator never
  flickers across the restart→fill hand-off, and every give-up path degrades to the honest §5 story.

**Emergent win**: a /handover-standby briefing the user never sent, the scan recorded as a pending
draft, and the app restart lost from the box — comes back through this same lane on resume (the
memory doesn't care who typed the draft).

**Logs** (`hooks.log`, `[draft-restore] <sid8> …`): armed (chars + expanded-placeholder count), the
refusal, typed-back attempt k/2, RESTORED + VERIFIED, and every drop with its reason (session gone /
setting off / memory cleared / taken over / live draft wins / gave up) — so "why did (or didn't) my
draft come back?" is answerable straight off the log.

**Coverage**: `RestoredDraftSessionTakenOver` (historical-never-blocks, Running-now, at/after-arm on
both channels, the `>=` race edge, the NeedsApproval-seed case — tests_commands.cpp beside its
standby twin) + `ExpandDraftPasteMarkers` / `ExpandPendingDraftPastesIn` (marker-free passthrough,
multi-marker whole-draft stitch, unresolved/ambiguous/same-line/fragment refusals, the live truncated
arithmetic, temp-cache adapter round + missing-dir no-throw — TestPendingPaste) + the
`restoreDraftOnResume` settings round-trip. The pump itself is the standby lane's recipe verbatim
(page-side, WinRT — exercised by that feature's live verification).

**2026-09-25:** the arm now resolves through the §2c journal-aware resolver (STRICT policy) — so a same-run
close→resume or a Deactivate→wake brings back a draft holding a FRESH paste whole (its content is in the
journal), and claude's bare `[Pasted text #N]` / an `[Image #N]` is recognized and refused instead of being
re-typed as a literal label. After an app restart the journal is empty and the paste-cache is the only tier,
exactly as before. The fill shape is unchanged (one paste).

## 11. The FORK CARRY — the unsent prompt comes along into a fork (built 2026-09-25)

**The ask.** *"When I fork a session and it opens into a new tab, have the prompt written in claude code's
prompt box copied over. Make sure the [Pasted text #1 +31 lines] are being expanded."*

**Which forks.** The two seams every fork funnels through: `_ForkManagedSessionById` (a tab's **Fork
session** / Ctrl+Shift+D / its model submenu, and the Manager board/tree **Fork**) and `_ForkSessionFromDisk`
(the Sessions page's **Fork here**, its detail-pane Fork, the double-click Resume/Fork dialog, the launch
bar's Fork). Claude only (Codex has no ❯ box). An adopt-external fork is not carried (a foreign host's box
is unreadable). The carry is keyed on the fork INTENT: a never-prompted source still forks (fresh) and its
draft still comes along.

**The pipeline.**
1. **CAPTURE** (`_CaptureDraftForFork`, before the launch — the prompt as it stood at the click):
   `PickCurrentPromptText` over the source's LIVE box (this window hosts it — the tab-menu case) else the
   observer's remembered draft (a source in another window — ≤ one scan tick stale — a dormant tab, or a
   closed session's persisted memory), plus a copy of the source's §2c journal.
2. **LAUNCH** — unchanged (`_LaunchClaudeSession`, `--resume <src> --fork-session`).
3. **ARM** (`_CarryDraftIntoFork` → `_ArmDraftRestore` with `DraftFillOrigin::Fork`): resolve (LIVE policy,
   off-thread when a placeholder is present); refused ⇒ `[draft-fork] … REFUSED: <why>` and the fork starts
   empty. Else SEED the fork's `pendingInput` with the source's rendered draft (the "3 dots" show on the fork
   tab + board card at once; persisted, so a fork never visited before an app restart re-arms through §10),
   JOURNAL each block under the fork (its box will show claude's own placeholders for them), and arm the
   PIECEWISE plan in the §10 pump's map (so the scan HOLD, the started wait, the settle, the 10-min cap and
   the hands-off latch are all shared — a fork is the latch's easy case: a fresh record whose own transcript
   doesn't exist until its first turn).
4. **FILL — piecewise** (`BuildDraftFillPastes` + `_DeliverDraftFillPastes`). One paste of the whole expanded
   draft would make claude collapse the typed words into the same placeholder (`[Pasted text #1 +32 lines]`
   — right content, a box that no longer shows what you wrote). Instead: typed text in chunks claude keeps
   INLINE (≤2 newlines, ≤800 chars — never cut through a surrogate pair or a kept quote), each block pasted
   ALONE so claude mints its own bound `[Pasted text #K +M lines]` for it. The fork's box then reads exactly
   like the source's (ids may differ — claude numbers its own). Each piece is a bracketed paste with NO
   submit CR, sent only after the previous one VERIFIABLY landed (`VerifyDraftFillPastes` over the prefix —
   claude's classic paste handler reads React props, so two pastes in one input chunk could mint colliding
   ids). The delivery owns the box like the mail-clear: the §9 box-mutex, the delivery gate under the
   reserved `kDeliveryGateFillTag` (an autorunner send declines instead of merging in), and a read-only
   lock handed back first on every path.
5. **VERIFY** (the pump): Verified ⇒ `draft COPIED + VERIFIED`; an EMPTY box with no piece ever seen ⇒ claude
   ate the first paste ⇒ ONE re-type from piece 1 (safe: empty box, cursor at its start); anything else — a
   box you cleared/edited after the copy landed, foreign text, a copy that stopped part-way — is left EXACTLY
   as it is, logged. Never typed onto text it can't account for; never submitted.

**The source keeps its draft** (a copy, not a move). **You win every race**: text typed into the fork before
the copy lands keeps the fork's box yours (pre-fill check, in-delivery re-check, the arm's seeded-memory
check); a prompt sent from the fork hands the lane off.

**Setting.** `AppSettings::carryDraftOnFork` (default ON), cog **SESSIONS ▸ "Copy my unsent prompt into a
forked session"** — deliberately not on the TESTS AUTORUNNER tab beside §10's toggle: that tab is
dev-or-debug only, and forks are a release feature.

**Known limits.** Inherited from the detector (§2): a SOFT-wrapped line reads with a `\n` where the box
wrapped it (Ink wraps the input itself, so the buffer can't tell a wrap from a newline) — the fork gets a
hard line break there (same width ⇒ same look; the submitted text has a newline instead of a space). The
§2c gaps (drag-dropped text, pre-journal pastes) fall to the cache tier, which refuses rather than guesses.
In a Claude tab under 12 rows claude collapses 2-newline chunks too (content intact, collapsed).

**Logs** (`hooks.log`, `[draft-fork] <fork8> …`): `copying the unsent prompt from <src8> (live box|remembered
draft, chars, journal=N)` → `armed (chars, pieces, placeholders: J journal / C cache / K kept)` or `REFUSED:
<why>` → `typed N piece(s)` / `delivery stopped: <why> (typed, seen)` → `COPIED + VERIFIED` or the give-up
reason. The fork's own `[nav] fork-managed-*` / `sessions fork-*` lines are unchanged.

**Coverage.** `TestDraftCarry` (engine harness): claude's normalization / count / threshold, the scan (all
five forms, soft-wrap, whole-line), the journal tier (chronological same-M, newest-wins, mid-line by span,
the bare form), the policy (live quote kept, strict refusal, image + duplicate-id refusals, `cacheMightHelp`),
the cache tier (unique whole-line only), the truncated middle (the live 274-segment fixture), the plan
(chunking, no surrogate/quote split, blocks alone), the verifier (re-minted ids, soft wrap, Eaten, Partial at
a boundary vs Foreign mid-paste, inlined block, exact quotes, the delivery's prefix mode), the registry
journal (normalize, collapse-only, de-dupe, cap, move, clear, remove) and the adapter (journal-only needs no
cache, the cache fallback, a missing dir). `carryDraftOnFork` round-trips in `TestAppSettings`. The page-side
delivery + pump are WinRT (verified live on the next deploy).

### Follow-ups (non-blocking)

- **§11 / §2c:** journal DRAG-AND-DROPPED text (subscribe to the control's `StringSent` for managed panes);
  persist the journal beside the §5 memory (so a restore after an app restart resolves fresh pastes too);
  make the §2b `paste-refs` annotation journal-aware (the board tip still says `unresolved` for a paste the
  journal can expand); let the §10 re-fill use the PIECEWISE fill (restored multi-line drafts would come back
  inline instead of collapsed); the SEND side of §8e (expand a queued placeholder at queue time) can now use
  the journal.

- **Off-switch**: an `AppSettings` flag to disable the pulse (like `showTabOverlay`); v1 is always-on.
- **Placeholder/dim filtering** (§4) — read the cells' faint attribute so a dim placeholder never reads as
  a draft.
- **The out-of-band probe** (`tests/pending_probe.cpp` + `tests/_run-pending-probe.bat`): the
  AttachConsole ground-truth oracle used for the live verification + the fleet capture sweeps — run it
  against any live claude pid to see exactly what the shipped detector would extract, without the app.
- **Tests Autorunner tie-in**: largely **superseded by §9** — a send no longer eats a draft, so the
  original motivation is gone. What remains optional is the *politeness* half: `pauseOnHumanInput` could
  still consult "has a pending draft" to **defer** an auto-send while the user is visibly mid-compose,
  rather than swapping around them. (It would also finally give that dead toggle a feeder — its
  `lastHumanInputUnixMs` gate has none.)
- ~~**Expand pastes on copy**~~ — **built as §8e** (`CopyPromptTextExpandingPastes`: the Auto-Testing row
  Copy + every Copy Current Prompt; off-thread, all-or-refuse, as rendered on refusal).
- **Expand pastes at QUEUE time — the SEND side of §8e.** The overlay MAIL button (§8d) and the compose
  envelope (§8a → `_OnAddPrompt`) still store the RENDERED draft, so a queued `[Pasted text #N +M lines]`
  is what the Tests Autorunner delivers — measured: 4/5 such sends ended `Failed` + the autorunner PAUSED
  (claude expanded the label via a still-bound paste, so the echo never matched), and 1/5 reached the model
  as the literal label (content lost) — §8e *Not fixed here*. The shape: queue as today (the click, the
  label and the MOVE-clear ordering stay synchronous), then expand off-thread with the same
  `ExpandPendingDraftPastes` and swap the row's `text` in place — by prompt id, only while it is still
  `Pending` and still holds the exact rendered text (never under an in-flight delivery). An expanded text
  is submit-faithful (claude re-collapses the large paste and re-binds it — the §10 property; the delivery
  verify is already collapse-tolerant, R4), so the echo then matches and the row goes `Sent` normally. A
  refusal keeps the label; there, the honest option is a visible warning on the row rather than a send
  that loses the content.
- **Wire the §8c store's READ side** — the accessors (`GetStoredSessionDraft` / `GetStoredSessionDraftAt`)
  are built + tested but unread, deliberately: for a registry-known session the registry is
  authoritative, so a fallback there could only surface a stale draft (§8c *Precedence*). The sound
  consumers are the ones the registry cannot answer for: a **Sessions-page** row (show that a closed /
  never-managed session was left holding an unsent message — it is the one surface that lists sessions
  with no record) and the **`agentmaster` CLI** (`show` could report the draft + its age offline).
- **A quiet-drift nudge for §8b** — a text-only draft edit raises no notify (§3), so the pull button only
  re-evaluates when the user touches the Manager. A single page→content call on the SELECTED session's
  drift would make it appear on its own; same-window only, since the board spans the whole fleet.
- **Explorer-tree row** + the per-tab overlay HUD could carry the same pulse (the board + tab cover the
  primary surfaces).

## 12. Claude Code's FULLSCREEN renderer — the alt buffer is Claude's OWN screen (fixed 2026-09-26)

### What broke

In release **0.6.8.12** every Claude tab read as "no input box" (reported 2026-09-26 on session
`986b7ff2`: text sitting in the prompt box, and the overlay's mail button would not queue it). The
effects:

- **Draft detection.** The last `[pending]` line in hooks.log was at 01:00. The build had started at
  00:36, and nothing has been detected in any tab since.
- **The mail button (§8d)** stayed disabled, and **Copy Current Prompt (§8)** found nothing.
- **The §10 restore re-fill** logged `gave up 10 min after start (box never readable)`. The §11 fork
  carry was the only reader still working (see below).
- **The Tests Autorunner delivered nothing.** Every advance logged
  `[advance-skip] <sid> (input box not visible)`. The last `[delivered]` line predates the deploy.

### Why

Two changes met:

1. **Claude's side.** `~/.claude/settings.json` sets **`CLAUDE_CODE_NO_FLICKER=1`**. On Claude Code
   2.1.282 that selects the **fullscreen renderer**. The same renderer is `/tui fullscreen` for a single
   session, and a server-side "feature of the week" flag can turn it on too, so it can appear without
   any local setting. It opens with `ESC[?1049h` and draws its whole UI in the terminal's **alternate
   screen buffer**, and it turns mouse tracking on (`?1000h ?1002h ?1003h ?1006h`).
   - The input box itself is unchanged: two full-width flush-left rules around the `❯` + NBSP caret row,
     with the 2-space continuation indent.
   - It sits at the bottom of a viewport-sized screen, with a footer row (`⏵⏵ bypass permissions on …`)
     below the bottom rule.
   - The conversation above it is virtual-scrolled by claude, so the terminal holds no scrollback of it.
2. **Our side.** `132e19b5d` (2026-09-18, the Ctrl+G / `edit.exe` investigation) made
   `ControlCore::ReadPendingInputDraft` answer **NoBox** for ANY alt-buffer screen, without reading it.
   It assumed "Claude renders inline in the MAIN buffer, never the alt buffer", so the alt buffer had
   to be a child full-screen app. Release 0.6.8.11's binaries were built on 2026-09-18 before that
   commit, and 0.6.8.12 was the first release to carry it. The dev instance had it from 2026-09-18,
   and dev's last `[pending] … draft` line is from that afternoon.

### Measured proof (2026-09-26)

- **The raw stream.** A fresh 2.1.282 session on a real ConPTY (the `claude-wrap-ConPTY` driver, nothing
  submitted) emits `ESC[?1049h` once, before its first frame. Its rendered screen is TestPendingInput's
  case 28 fixture.
- **The live session.** `tests/pending_probe.exe`, run out-of-band against `986b7ff2`'s claude (pid
  72364, a 183×51 screen), found the user's 9-char draft with the shipped detector (`boxFound=1`). So
  the detector reads the fullscreen box fine; only the skip hid it.
- **The one reader that kept working.** In the same hour, the §11 fork carry logged
  `draft COPIED + VERIFIED`. It verifies through `ReadInputBoxProbe`, which never had the skip.

### The fix

`ReadPendingInputDraft` reads the **active** buffer, main or alt. A child full-screen app (the Ctrl+G
editor, a pager) is told apart by **content**. That is the only discriminator left once Claude itself
can own the alt buffer, and it is the one the delivery probe (DELIVERY.md §11) has always relied on.

- **What counts as a box.** `DetectPendingInput` accepts only a `❯`/`›` caret that is a row's first glyph
  (at most one vertical border before it), with anchored rules directly above and below. An editor or
  pager screen reads NoBox, so the autorunner holds exactly as the skip intended.
- **Cost.** One bounded 120-row read per scan tick for a redrawing full-screen app, the same as a busy
  classic tab.
- **The cache across buffer switches.** The mutation-id cache stays correct, because each `TextBuffer`
  instance seeds its own id range 2^32 apart (`textBuffer.cpp`), so a switch always re-scans.
- **`Terminal::IsInAltBuffer()`** stays, with a corrected comment. It must never gate reading Claude's
  box.

### Pinned

`TestPendingInput` holds real 120×40 captures:

- **Case 28:** the fullscreen screen with an empty box (must read **Empty**, caret above the footer), and
  with a 3-line draft (must read **Draft**, text exact).
- **Case 29:** Microsoft Edit 1.2.1 (Claude's default Ctrl+G editor) showing that draft, and Edit showing
  a draft that CONTAINS a pasted Claude box. Both must read **NoBox**. Edit's `N │ ` gutter keeps the
  caret from ever being a row's first glyph, even under a rule-dense, anchored pasted rule.

### Residual limitation

**A GUTTER-LESS external editor** showing a pasted Claude box would draw it flush-left, and it would read
as a box (§4). That was equally true before `132e19b5d`, and for `ReadInputBoxProbe` all along. It needs
three things at once: that editor, that paste, and a queued prompt at rest while the editor is open.

### Elsewhere under fullscreen (not fixed here)

- **The summary-panel JUMP** (SUMMARY_JUMP.md §5) can only reach prompts on claude's current screen.
  The alt buffer has no scrollback to center on.
- **Mouse tracking** is on, so a plain drag goes to claude. Shift+drag is the terminal's own selection.

### Workaround on a build without the fix

- Remove `CLAUDE_CODE_NO_FLICKER` from `~/.claude/settings.json`, or set
  **`CLAUDE_CODE_DISABLE_ALTERNATE_SCREEN=1`**. Claude's own message says that variable "forces the
  classic renderer any time".
- Then restart the affected sessions, because claude chooses its renderer at start.
- **Where the setting lives.** Claude's settings schema describes the renderer preference as:
  "fullscreen" uses the flicker-free alt-screen renderer with virtualized scrollback, equivalent to
  `CLAUDE_CODE_NO_FLICKER=1`. `/tui default` saves the classic renderer to claude's preferences and
  restarts and resumes the session.
- **Unverified precedence.** Whether that saved preference beats a set `CLAUDE_CODE_NO_FLICKER=1` has not
  been checked (claude's own messages call the variable an override), so drop the variable first.
