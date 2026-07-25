# Pending-input monitor — detect an UNSENT draft in a Claude tab's input box

> Status: **complete + BULLETPROOFED (2026-07-23) — detection + the "yes pending / no pending" observer
> NOTIFY + the "3 dots" animation on BOTH surfaces, PLUS: the cross-version hardening pass (§2a — the NBSP
> separator fix, the anchored-rule + menu-shape rejectors that killed the six live AskUserQuestion
> false positives, the candidate upward-continue), the PASTE-CACHE resolver (§2b — a draft's
> `[Pasted text #N +M lines]` placeholder resolves to its real on-disk content, content-anchored +
> arithmetic-verified), and PERSISTENCE (§5 — the draft survives an Agentmaster restart/crash as a
> staleness-labeled memory and revalidates against the live box on resume), and the draft as a COPYABLE
> value (§8 — a **`Copy Current Prompt`** item in all three session copy menus: a LIVE buffer read where
> the window hosts the tab, falling back to the observer's remembered draft).** Pure detector + resolver +
> the registry notify-on-flip/stamping + the persistence round-trip are unit-tested (engine harness
> 2,671/2,671 incl. REAL-capture fixtures from live 2.1.217/2.1.218 buffers); the full chain lib-compiles
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

## 3. Architecture (one pure brain, a thin adapter, a UI-lane poll)

```
TerminalPage::_ScanPendingInput()      (TerminalApp; UI thread, ticked by the shared SessionScanner's
  └─ per bound, started CLAUDE tab:     liveness probe — alongside _SweepClaudeLiveness / _ObserverProbe)
       ├─ TermControl::ReadPendingInputDraft()                                     TerminalControl
       │    └─ ControlCore::ReadPendingInputDraft()   (read-only, under the read-lock)
       │         ├─ copy the last ~120 buffer rows' text
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
  copy). The ONE eager clear kept is the **restart-tab swap** (`_RestartTabIntoFreshSession`) — the
  user watches that screen be destroyed, so a 5-second dots flash for a box that visibly no longer
  exists would be noise, not memory.
- **Restore display** — a restored record's draft shows the dots **before its tab ever starts**: the
  scan's NotConnected branch drives `_SetTabPending` from the stored value (the board card reads the
  registry directly and needs nothing). The board tip appends *"last seen \<ago\> — remembered from
  before this session's tab (re)started; it clears automatically once the live input box reads empty"*
  once the stamp's age passes ~15s (a live draft's stamp never ages that far), plus the §2b paste-refs
  annotation naming the cached content file(s).
- **What reload gives the user**: reopen Agentmaster → the tab/card show the dots + the labeled draft
  text (copyable from the tip) *before* focusing the tab; focusing it starts claude, whose empty box
  then clears the memory within ~5s. The draft's paste content stays recoverable forever via the named
  paste-cache file. (A future re-fill — typing the memory back into the resumed box via the standby
  lane's `BuildPromptFill` + `ReadPendingInputDraft` verify — is the natural next step; deliberately
  NOT in this pass, since writing to a session's stdin on restore is a behavior change the user should
  opt into. See Follow-ups.)

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
  omitted when empty (`tests_persistence`). **2,671/2,671 checks pass.**
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

**Not expanded (v1).** A draft containing a `[Pasted text #N +M lines]` placeholder copies **as rendered**,
placeholder included — the §2b resolver identifies the backing cache file (and the board tip names it),
but substituting content into a copy is a separate, deliberate step (see Follow-ups).

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
behaviour out; a pull logs `[nav] compose pull-draft <sid8> src=live|remembered chars=N`.

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

### Follow-ups (non-blocking)

- **Off-switch**: an `AppSettings` flag to disable the pulse (like `showTabOverlay`); v1 is always-on.
- **Placeholder/dim filtering** (§4) — read the cells' faint attribute so a dim placeholder never reads as
  a draft.
- **Re-fill on resume** (§5): offer to TYPE a restored draft memory back into the resumed session's
  empty box — the /handover-standby lane already owns the exact machinery (`BuildPromptFill` paste with
  no submit CR + the `ReadPendingInputDraft` verify + the taken-over guard). Deliberately not in the
  persistence pass: writing to a session's stdin on restore is a behavior change the user should opt
  into (a cog switch, or a per-toast/tooltip action).
- **The out-of-band probe** (`tests/pending_probe.cpp` + `tests/_run-pending-probe.bat`): the
  AttachConsole ground-truth oracle used for the live verification + the fleet capture sweeps — run it
  against any live claude pid to see exactly what the shipped detector would extract, without the app.
- **Tests Autorunner tie-in**: largely **superseded by §9** — a send no longer eats a draft, so the
  original motivation is gone. What remains optional is the *politeness* half: `pauseOnHumanInput` could
  still consult "has a pending draft" to **defer** an auto-send while the user is visibly mid-compose,
  rather than swapping around them. (It would also finally give that dead toggle a feeder — its
  `lastHumanInputUnixMs` gate has none.)
- **Expand pastes on copy** (§8): "Copy Current Prompt" copies the draft as rendered, so a
  `[Pasted text #N +M lines]` placeholder rides along as a placeholder. The §2b brain can already
  substitute the real content (`ExpandPasteMarker`, verified-only), but it needs an impure adapter that
  expands a whole draft (today `ClaudeSpawn` only exposes the annotation form `ResolvePendingPasteRefs`)
  and an off-thread copy path like the Transcript/Summary cases — worth doing, deliberately not in this
  pass. A refusal must keep the placeholder (never a wrong expansion).
- **Explorer-tree row** + the per-tab overlay HUD could carry the same pulse (the board + tab cover the
  primary surfaces).
