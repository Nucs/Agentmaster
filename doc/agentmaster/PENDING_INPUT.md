# Pending-input monitor — detect an UNSENT draft in a Claude tab's input box

> Status: **complete — detection + the "yes pending / no pending" observer NOTIFY + the visible "3 dots"
> animation on BOTH the tab strip and the Triage-Board cards.** Pure detector + the registry notify-on-flip
> are unit-tested (engine harness, 1354 checks green); the full chain lib-compiles green
> (TerminalControlLib + TerminalAppLib). The fact is recorded on the session, logged (`[pending]`), and
> drives the indicator. Runtime verification (the live pulse + `[pending]` trace) needs a deploy — gated on
> the user's build/deploy permission.

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

1. it is the **BOTTOM-MOST** line whose first non-space glyph is the prompt marker `❯` (U+276F; `›`
   U+203A is also accepted), and
2. it is **WRAPPED by rule rows** — a plain `─` rule **directly above** the `❯` line (within 2 rows,
   tolerating one intervening blank), and another rule **below** the body.

Fact 2 is what separates the live input box from the two other things that also start with `❯`:

| looks like | example | rejected because |
|---|---|---|
| a **SENT prompt** in scrollback | `❯ a message I sent earlier` | rendered INLINE, no surrounding box (no rule directly above + below) |
| a **menu selection** cursor | `❯ 1. Yes` under "Do you want to proceed?" | the line directly above the `❯` is the question text, not a rule |

A **rule row** is a row *dominated by* box-drawing characters (U+2500..U+257F): ≥ 6 of them AND ≥ 80% of
the row's visible (non-space) chars. The 80% floor rejects a *labeled* divider (`── 3 files ──`, used
elsewhere by Claude) while accepting a plain or corner-framed rule.

**Extraction**: the body rows `[caret, bottomRule)` — strip the `❯` marker (+ one following space) from the
first line and the 2-space continuation indent from the rest, join with `\n`, drop trailing blank lines.
An empty box (`❯ ` + cursor) yields empty text → **no pending draft**. (Reconstruction is best-effort for
display; the load-bearing output is "is there any non-whitespace content".)

The buffer position is independent of the user's **scroll**: Claude renders inline, so the box always sits
at the bottom of the buffer (`GetLastNonSpaceCharacter`), and the adapter reads a bounded window of the
**last rows** regardless of where the WT scrollback is scrolled.

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
- **`TermControl::ReadPendingInputDraft()`** (`TermControl.{idl,h,cpp}`) — a pure passthrough to the core.
- **`SessionInfo::pendingInput`** (`SessionModels.h`) — the transient draft fact. **Never persisted**
  (`Persistence.cpp` writes an explicit field list that omits it; cleared when a session is archived).
- **`SessionRegistry::SetPendingInput(id, text)`** — the field updates every change, but `_notify` fires
  **only on the BOOLEAN hasPending FLIP** (empty↔non-empty) — the "yes pending / no pending" transition.
  This is the **presence-heartbeat cadence**: a draft moves as the user types, so a per-keystroke notify
  would needlessly run the persist / board-rebuild / scheduler cascade, but the appear/clear transitions
  are infrequent (turn-cadence) and are exactly what the animations key on. A text-only edit (still
  non-empty) updates the field **quietly**. Returns true iff the boolean flipped (== whether it notified).
  Unit-tested (`TestRegistry`): appear notifies, a text-only edit is quiet, clear notifies.
- **`TerminalPage::_ScanPendingInput()`** (`TerminalPage.AgentObserver.cpp`) — the **UI lane** (the only
  place a control's buffer is readable). Ticked once per scanner liveness pass. For each **bound, started,
  Claude** session it reads the draft, applies the **clear debounce** (below), commits it via
  `SetPendingInput`, and drives **this window's tab-strip pulse directly** (`_SetTabPending` — it holds the
  tab) every tick + idempotently. **Background (unfocused) tabs are scanned too** — the whole point is to
  notice a draft left in a tab the user switched away from. The flip is logged as `[pending] <id> draft
  (chars=N): <first line>` / `[pending] <id> cleared`.
- **The "3 dots" animation** (the visible indicator) rides two surfaces, both a phase-shifted goldenrod
  opacity pulse:
  - **Tab strip** — `TerminalTabStatus::AgentPendingVisible` (set by `_SetTabPending`) drives a tiny 3-dot
    cluster at the bottom of the status-dot wrap in `TabHeaderControl.xaml` (below the dot). The pulse
    storyboard is built imperatively and **started/stopped on the flag** (`TabHeaderControl::_UpdatePending-
    Animation`, hooked to `TabStatus.PropertyChanged`) so an **idle fleet animates nothing** — no perpetual
    60fps compositor wakeups.
  - **Triage-Board cards** — `AgentManagerContent::_MakeCard` appends a 3-dot pulse (`BuildPendingDots`)
    when `!s.pendingInput.empty()`. The board rebuilds on the flip `_notify` (the lens observer), so it
    appears/clears with the draft, **cross-window** (a session hosted in window A animates on window B's
    GLOBAL board too). The storyboard begins on `Loaded` (runs only while carded; a rebuild drops it).

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
- **Side borders.** v1 targets the current horizontal-rule-only box style (no `│` sides / corners, as the
  user's Claude renders). A future framed style would need the caret/rule detection to skip a leading
  vertical border char.

## 5. Verification

- ✅ **Detector**: `TestPendingInput` (engine harness) — single-line, multi-line (continuation indent +
  internal blank line), empty box, no box, a sent prompt in scrollback + an empty box below (only the box
  is taken), menu rejection (bare + rule-wrapped-with-a-question-above), the rule classifier (pure rule
  vs labeled divider vs text vs too-short), marker-without-space, the secondary `›` marker, trailing-blank
  trim, a blank row between the top rule and the marker.
- ✅ **Registry notify**: `SetPendingInput` — appear **notifies** (the boolean flip), a text-only edit is
  **quiet**, clear **notifies**, unknown-id no-op (`TestRegistry`). 1354/1354 checks pass.
- ✅ **Full chain compiles**: `TerminalControlLib` (IDL projection + `ControlCore` + `TermControl`) and
  `TerminalAppLib` (the registry + `TerminalPage._ScanPendingInput` + `TerminalTabStatus.AgentPendingVisible`
  + the `TabHeaderControl` pulse + the `AgentManagerContent` card pulse) both build green.
- ⏳ **Runtime**: the live pulse + `[pending]` trace need a deploy (close → build → relaunch). Once
  deployed, verify: type a multi-line draft in tab A → the tab-strip dots pulse + its board card shows the
  pulse; switch to tab B → `[pending] <A> draft …` logged and the pulse persists (background tab); send it
  → both pulses clear within ~2 ticks and `[pending] <A> cleared` is logged.

## 6. The "3 dots" indicator (built)

The detection NOTIFIES reliably on both transitions (§3 "Reliability"), so the indicator is a goldenrod
**3-dot opacity pulse** ("typing"/waiting cue) on two surfaces, both driven by the observer's detection:

- **Tab strip** — a tiny cluster at the bottom of the status-dot wrap, **below** the dot
  (`TabHeaderControl.xaml` `HeaderPendingDots`, bound to `TerminalTabStatus::AgentPendingVisible`). Driven
  by `TerminalPage::_SetTabPending` straight from the UI-lane scan (the hosting window holds the tab). The
  pulse storyboard is **started/stopped on the flag**, so idle tabs animate nothing.
- **Triage-Board cards** — a pulse at the top of the card body (`AgentManagerContent::_MakeCard` →
  `BuildPendingDots`), driven by the flip `_notify` rebuilding the board, so it works **cross-window**
  (a draft in window A shows on window B's GLOBAL board). The storyboard begins on `Loaded` (runs only
  while carded).

### Follow-ups (non-blocking)

- **Off-switch**: an `AppSettings` flag to disable the pulse (like `showTabOverlay`); v1 is always-on.
- **Placeholder/dim filtering** (§4) — read the cells' faint attribute so a dim placeholder never reads as
  a draft.
- **Autopilot tie-in**: `pauseOnHumanInput` could consult "has a pending draft" to suspend an auto-send
  while the user is mid-compose — the draft fact is exactly the signal `pauseOnHumanInput` was waiting for.
- **Explorer-tree row** + the per-tab overlay HUD could carry the same pulse (the board + tab cover the
  primary surfaces).
