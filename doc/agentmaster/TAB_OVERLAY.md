# Agentmaster — Tab Overlay (the per-session "link badge")

> A small, glanceable HUD pinned to the **top-right of every Claude session tab's terminal**,
> showing the **connection between that tab and Agentmaster**: its hook-driven status, whether
> (and how) Autopilot is driving it, and what's queued — with on-demand controls. This is the
> per-tab counterpart to the Manager tab's Triage Board: the Board is the *fleet* view; this is
> the *here-and-now* view, visible while you actually work inside a session.
> Companions: [`DESIGN.md`](./DESIGN.md) · [`IMPLEMENTATION.md`](./IMPLEMENTATION.md) ·
> [`HOOKS.md`](./HOOKS.md) · [`PERSISTENCE.md`](./PERSISTENCE.md).

---

> **Status — SHIPPED, and beyond this design.** Phase 1 (the badge) and Phase 2 (hover-expand +
> controls) shipped, plus substantial additions this design did not foresee: the badge now shows on
> **every classified tab** (a dim `○ <kind> · unlinked` *observe badge* — `pwsh` / `cmd` /
> unprompted-`claude` / `codex` — that flips in place as activity changes, **not** "no badge" as §8
> originally said); it is enriched with `model · effort · kind` and carries a **second
> `<workdir>/<branch>` row**; an always-shown action row carries a **folder Open-Path + a copy menu** (Session Id /
> Path / Branch / the real Claude·Codex launch CLI / Summary / Transcript, with a chime) **+ a
> pencil** that toggles a **second overlay, the SUMMARY PANEL**. A matching **tab-strip status dot**
> rides every tab header. These are written up in **§13** below; [`../../CLAUDE.md`](../../CLAUDE.md)
> is the authoritative current behavior.

## 1. The two asks (verbatim)

1. **Phase 1 — the badge.** *"Each tab in Agentmaster must get an overlay on the top-right side,
   small font, summarizing the connection between this tab and the Agentmaster. This way the user
   can see if Agentmaster is handling — or how it's handling — the queued messages, autopilot
   mode, and whatever fits well there."*
2. **Phase 2 — the controls.** *"Later on: add buttons to the tab overlay for autopilot control,
   show/hide the queued messages, and whatever fits well there."*

This document finalizes both, plus a small Phase 3 of deferred ideas.

## 2. Decisions (locked)

| Decision | Choice | Notes |
| --- | --- | --- |
| **Resting content** | **1 compact line** | `◐ waiting · Full · ⏳3 · ⛓linked` — state + autopilot mode + Pending count + link mark. |
| **Interaction** | **Dim, expand on hover/click** | Resting badge is dim + compact; hover (or click) expands a panel with buttons + a queue peek. |
| **Phase-2 buttons** | Autopilot cycle · Send next now (!) · Show/hide queued · Jump to Manager | Confirm / Skip appears **contextually** in SemiAuto (not an opt-in). |
| **Default visibility** | **On, dim until hover** | ~55 % opacity at rest, 1.0 on pointer-over; a Settings-cog toggle (`showTabOverlay`) turns it off. |

Everything below is the implementation of these four choices.

## 3. The "connection" the badge surfaces

The whole point is to make the **tab ⇄ Agentmaster relationship** legible at a glance. Three
orthogonal facts, all already in the model:

### 3a. Link state — *can* Agentmaster act here?
Derived from whether a stdin **injector is bound** to the session id (Correctness Rule #3, #9):

| Link state | When | Badge mark | Controls |
| --- | --- | --- | --- |
| **Managed** | We launched it (`_LaunchClaudeSession`) | `⛓ linked` | full |
| **Adopted + bound** | Hand-typed `claude`, correlated to its ConPTY (`_AdoptExternalSession`) | `⛓ linked` | full |
| **Observe-only** | `external` session with no matching connection (a `claude` hosted outside this app) | `⛓̸ observe` (dimmed) | disabled, with a tooltip |

Source of truth: a new `SessionRegistry::HasInjector(id) const` (trivial — the map already
exists at `_injectors`). The overlay reads it on each refresh. *Don't* key this off
`SessionInfo::external` alone — an adopted session is `external==true` yet fully controllable
once bound.

### 3b. Autopilot relationship — *how* is it driving?
From `SessionInfo::autopilot.mode`. The expanded panel spells out the relationship in words so
"how is it handling this" is unambiguous:

| Mode | Chip | Expanded sub-text |
| --- | --- | --- |
| `Off` | `Manual` | "You drive — Agentmaster only watches." |
| `SemiAuto` | `Semi` | "Agentmaster proposes the next prompt; you confirm." |
| `Full` | `Full` | "Agentmaster auto-sends on each turn-complete." |

Plus the transient annotations that explain *why nothing is moving right now*:
`confirm?` (SemiAuto armed — `pendingConfirmPromptId` non-empty), `held: question`
(`lastMessageWasQuestion` + the question-guard), `⏸ paused` (scheduler global pause), and the
backstop `n/maxAutoSends`. An `error` state (Crimson) already signals a `stopOnError` halt.

### 3c. Status — *what* is the session doing?
The hook-driven `SessionState`, color-matched **exactly** to the Triage Board so the two views
speak one language. Reuse the existing `StateColor` / `StateGlyph` / `StateLabel` palette
(`AgentManagerContent.cpp:59-117`):

```
Idle ○ Gray   Running ● DodgerBlue   WaitingForInput ◐ Goldenrod
NeedsApproval ⚠ OrangeRed   Error ✕ Crimson   Done ✓ MediumSeaGreen
```

## 4. Visual spec

### Resting (1 compact line, dim)
```
 terminal output…       ┌────────────────────────────────┐
 > _                    │ ◐ waiting · Full · ⏳3 · ⛓linked │   ← ~55% opacity
                        └────────────────────────────────┘
```

### Expanded (hover or click)
```
                        ┌─────────────────────────────────┐
                        │ ◐ waiting · Full        ⛓ linked │
                        │ Agentmaster auto-sends on turn-  │
                        │ complete.                        │
                        │ ⏳ 3 queued · next: "run tests"  │
                        │ ┌─────────────────────────────┐  │   ← queue peek
                        │ │ ⏳ run tests                 │  │     (toggled by
                        │ │ ⏳ commit                    │  │      [Queue])
                        │ │ ⛔ deploy (held: question)   │  │
                        │ └─────────────────────────────┘  │
                        │ [Auto: Full] [Send !] [Queue]    │
                        │ [Jump to Manager]                │
                        └─────────────────────────────────┘
```

Placement & feel:
- **Top-right**, with a **~20 px right margin** so it clears the terminal's vertical scrollbar
  (the `ScrollBarSize` is 16 — see `TermControl.xaml`). A small top margin (~4 px).
- **Theme-aware** brushes pulled from `Application::Current().Resources()` like the Manager
  (`UnfocusedBorderBrush` background, subtle border, rounded corners ~4 px). Monochrome
  geometric glyphs only (the set above) — **no wide color emoji**, to match the Manager and
  render cleanly in Cascadia.
- **Opacity:** 0.55 at rest, 1.0 on `PointerEntered`, back to 0.55 on `PointerExited` (skip the
  fade while expanded/pinned). Small font (~11–12 px).
- **Hit-testing:** the resting badge is click-through-friendly except its own bounds; the
  expanded panel captures taps (a handled `Tapped`, like the Manager's modal cards) so a click
  inside it never reaches the terminal. The badge **never takes keyboard focus** — it must not
  steal focus from the ConPTY (no `IsTabStop`).

## 5. Where it attaches (visual-tree seam)

A terminal pane is `Pane::_borderFirst.Child(_content.GetRoot())`, and for a terminal
`TerminalPaneContent::GetRoot()` returns the **bare `TermControl`** (`TerminalPaneContent.cpp:51`).
To float a HUD over it we wrap that control:

- **`TerminalPaneContent::GetRoot()` returns a cached `Grid { _control, _overlaySlot }`** instead
  of the bare control. `_control` fills the cell; `_overlaySlot` (a `ContentControl` /
  `Border`, `HorizontalAlignment=Right`, `VerticalAlignment=Top`) sits on top, **empty and
  `Collapsed` by default** → **zero visual change for non-Claude panes**.
- This wrapper **travels with the content** across split / zoom / re-parent (Pane re-reads
  `_content.GetRoot()` at `Pane.cpp:1454, 1958`) — unlike appending to `Pane::_root`, which is
  rebuilt on split and would orphan the overlay.
- A non-projected setter `TerminalPaneContent::SetAgentOverlay(FrameworkElement)` fills the slot
  (and toggles its visibility). It is called via `winrt::get_self<implementation::TerminalPaneContent>(…)`
  — **no `.idl` / MIDL change** (same trick as `_GetTabImpl`; mirrors the "no `.idl`" note in
  IMPLEMENTATION.md §Integration).

> **Why always-wrap (even non-Claude panes):** adoption. A hand-typed `claude` starts life in an
> ordinary `+` tab whose pane is a plain `TerminalPaneContent`. Because the slot is *already
> present*, `_AdoptExternalSession` can drop the badge in with `SetAgentOverlay(...)` and **no
> re-parenting**. The cost is one lightweight `Grid` + one `Collapsed` element per terminal pane
> — negligible.

Risk to verify on first build: the extra `Grid` must not disturb `TermControl` sizing
(it is `Stretch`/`Stretch`, so it fills), the SwapChainPanel, or drag-drop (`DragOver`/`Drop`
are handlers *on the control*, unaffected). Low risk, but it touches every terminal pane, so
sanity-check a vanilla split/zoom.

## 6. Who builds & owns it — `AgentTabOverlay`

A **new additive control**, `src/cascadia/TerminalApp/AgentTabOverlay.{h,cpp}`, built
**imperatively** (no IDL/XAML markup), exactly like `AgentManagerContent`. It is **not** an
`IPaneContent` — it's a plain `FrameworkElement`-producing helper the app installs into the
pane's overlay slot.

Session knowledge stays in the **app layer** (`TerminalPage`), never in the generic
`TerminalPaneContent`. The page constructs an `AgentTabOverlay` and wires it at the two seams
that already establish a managed session:

- **`_LaunchClaudeSession(dir, title, restored)`** — after `_CreateNewTabFromPane` + the
  `_claudeTabs[id] = tab` mapping (`TerminalPage.cpp:1099-1103`), reach the new pane's
  `TerminalPaneContent` and `SetAgentOverlay(overlay)`.
- **`_AdoptExternalSession`** — it already has the matching `TerminalPaneContent` in hand
  (`term` at `TerminalPage.cpp:1740`); install the overlay there once bound.

The overlay's wiring mirrors `_WireAgentManagerContent` (reuse, don't duplicate, the existing
seams):

| Overlay needs | Wired to | Reuses |
| --- | --- | --- |
| `sessionId` | set at construction | — |
| read state/queue/mode | shared `SessionRegistry` (`SetRegistry`) | same `shared_ptr` as the Manager |
| **Autopilot cycle** | registry `Update` of `autopilot.mode` (Off→Semi→Full) | logic of `AgentManagerContent::_CycleAutopilot` |
| **Send next now (!)** | page callback `sendNextNow(id)` | the Manager's `_DoSendNow` inject path (atomic mark-Sent — Rule #4) |
| **Confirm / Skip** | page callback → `Scheduler::Confirm(id, bool)` | same as the Flight Plan confirm |
| **Jump to Manager** | page callback: focus `_managerTab` + `AgentManagerContent::SelectSession(id)` (new public entry) | the inverse of `_ActivateClaudeSession` |
| link state | `SessionRegistry::HasInjector(id)` (new) | — |
| global pause | provider `() -> _scheduler->GlobalPaused()` | — |

> Cycling Autopilot to `Full`/`SemiAuto` while the session sits `Idle`/`WaitingForInput`
> naturally kicks the plan: the registry `Update` fires observers → the scheduler's `OnObserved`
> requests an advance (the same change-driven start that lets idle plans begin — Correctness
> Rule #1). No special-casing needed in the overlay.

## 7. Live updates

Mirror the Manager's proven pattern (`AgentManagerContent.cpp:390-416`) but **id-filtered** and
**self-detaching**:

- On `SetRegistry`, `AddObserver([weak, disp, myId](const SessionInfo& s, HookEvent){ if (s.id==myId) disp.TryEnqueue(refresh); })`. Keep the returned `ObserverToken`; **`RemoveObserver` on teardown** (the overlay's `Close`/dtor) — same discipline as the Manager lens (Rule #10), so a closed tab's badge never pins a dead `DispatcherQueue`.
- N sessions ⇒ N observers, each doing an O(1) id check per change — fine at this scale.
- **Global pause** is scheduler state, not a registry event: read it via the provider on each
  `refresh`, and have the page **nudge every open overlay** when Pause-all toggles (iterate
  `_claudeTabs`) so the `⏸ paused` mark updates immediately rather than on the next session event.
- Refresh is **snapshot-driven** (rebuild the line from `registry->Get(id)`), like the Manager —
  no incremental diffing.

## 8. Scope — which tabs get a badge

| Tab | Badge? |
| --- | --- |
| Managed Claude **or Codex** session (launched / restored) | **Yes** (full linked badge) |
| Adopted Claude session — bound | **Yes** (full) |
| Adopted Claude session — observe-only | **Yes** (observe style, controls disabled) |
| Plain terminal (`pwsh`, `cmd`), a started-but-unprompted `claude`, or an external `codex` | **Yes — a dim `○ <kind> · unlinked` observe badge** that flips kind in place as activity changes, and is promoted to the full linked badge the instant a `claude` resolves a conversation id. *(Originally "No — slot stays empty"; superseded — see §13a.)* |
| The Manager tab | **No** (it *is* the manager; and it's `AgentManagerContent`, not a `TerminalPaneContent`) |

When a session is **archived in place** by the liveness sweep (claude exited, tab left open —
`_SweepClaudeLiveness`), the record flips `live=false` and the injector unbinds; the badge
refreshes to a muted `done · not linked` (or hides) so a dead tab doesn't look live.

## 9. Settings — the off switch

Add one field to `AppSettings` (`SessionModels.h`):

```cpp
bool showTabOverlay{ true }; // ON => show the per-tab link badge; default reproduces "always on"
```

Wiring (all existing seams):
- (De)serialize in `Json.h` / `Persistence` (a missing key ⇒ `true`, the no-op default — matches
  the "every default reproduces prior behavior" rule for `settings.json`).
- A `ToggleSwitch _setShowTabOverlay` in the Settings cog overlay (`_BuildSettingsOverlay` /
  `_ShowSettings` / `_SaveSettings`), labeled "Show per-tab status badge."
- On change, the page applies it to every open overlay (show/hide) — same fan-out as the global
  pause nudge.

## 10. Phasing

- **Phase 1 — read-only badge.** §5 attach seam + §6 `AgentTabOverlay` (compact 1-line:
  state + mode + Pending count + link mark) + §7 live updates + §8 scope + §9 off-switch +
  dim-until-hover. Delivers ask #1.
- **Phase 2 — expand + controls.** Hover/click expand → next-prompt preview + the four buttons
  (Autopilot cycle, Send now, Queue peek, Jump to Manager) + contextual Confirm / Skip. Delivers
  ask #2.
- **Phase 3 — deferred.** Approve / Deny on `NeedsApproval` (today approvals go through the
  in-terminal y/n; surfacing them on the badge means routing through the Approval Policy, not the
  prompt queue — Rule #1); a live transcript "peek" (`SessionInfo::lastAssistantText`, already
  captured by the scanner); a per-session pause distinct from global pause.

## 11. Correctness rules to respect (do not regress)

- **#1** Autopilot readiness / question-guard / approval routing — the overlay only *reflects*
  scheduler state and *requests* the same actions the Manager does; it never invents a new send
  path. Approve/Deny stays out of the prompt queue.
- **#2** A badge button **never** writes a stray CR. "Send now" goes through the inject path that
  marks `Sent` atomically; "Jump to Manager" only focuses a tab.
- **#3 / #9** Control is bound to the **session id** with a real injector; observe-only sessions
  show disabled controls and never bind "the active tab."
- **#4** "Send now" is idempotent (atomic mark-`Sent` before inject) — reuse `_DoSendNow`.
- **#7** Everything shown is **hook-derived** (`SessionState`, queue) — never scraped from the
  Ink TUI.
- **#10** The overlay detaches its registry observer on teardown (token discipline), like the
  Manager lens on the shared (process-wide) engine.
- **#11 / #12** The badge **reads** title/color but is not a second source of truth for either;
  it doesn't rename or recolor.

## 12. New / touched files (summary)

| File | Change |
| --- | --- |
| `AgentTabOverlay.{h,cpp}` | **new** — the imperative badge control (Phase 1 + 2). |
| `TerminalPaneContent.{h,cpp}` | small touch — `GetRoot()` returns the wrapper `Grid`; add non-projected `SetAgentOverlay(...)`. |
| `TerminalPage.{h,cpp}` | install + wire the overlay in `_LaunchClaudeSession` / `_AdoptExternalSession`; the `sendNextNow` / `jumpToManager` callbacks; global-pause + setting fan-out. |
| `AgentMaster/SessionRegistry.{h,cpp}` | add `bool HasInjector(const std::wstring&) const`. |
| `AgentManagerContent.{h,cpp}` | add public `SelectSession(id)` (Jump-to-Manager target); a `_setShowTabOverlay` toggle in the cog. |
| `AgentMaster/SessionModels.h` + `Json.h`/`Persistence` | `AppSettings::showTabOverlay` (+ (de)serialize). |
| `TerminalAppLib.vcxproj` | register `AgentTabOverlay.{h,cpp}`. |
| this doc + `CLAUDE.md` / `DESIGN.md` index links | docs. |

## 13. Shipped additions beyond the original design

The badge shipped (Phase 1 + 2) and then grew well past this spec. The sections above are the design
seed; this section records what actually ships today (authoritative: [`../../CLAUDE.md`](../../CLAUDE.md)).

### 13a. An observe badge on *every* classified tab (supersedes §8's "No")
A non-bound tab is no longer badge-less. The `_ObserverProbe` UI lane reads the Fleet Observer's
correlation/activity tables and shows a registry-LESS **observe badge** — `○ <kind> · unlinked`
(`pwsh` / `cmd` / `codex` / a `claude` that is started-but-not-yet-prompted, §11d-style) — via
`AgentTabOverlay::ShowActivity`. It **flips kind in place** as the tab's activity changes (a `pwsh`
tab → `claude` the moment you run it) and is **promoted** to the bound linked badge the instant a
claude resolves a conversation id (its first prompt). `_DropPendingOverlay` collapses/releases it
when the tab binds, the agent exits, or the tab leaves the window's roster.

### 13b. Enrichment + Codex
The Fleet Observer enriches the badge with **`model · effort · kind`** (O6). A managed **Codex**
session wears the full badge at its 3-state floor (Running / Waiting / Idle — Codex has no
hook-derived NeedsApproval/Error via PULL); an external codex shows `○ codex · <model>`.

### 13c. Second row — `<workdir folder>/<branch>`
The linked badge carries a dim second line (`AgentTabOverlay::_subline`) — the session's root
working-dir folder + its **live** git branch (`ReadGitBranchForDir`, read from `.git/HEAD`, handling
a worktree/submodule `.git` FILE + a detached HEAD → short SHA; distinct from a transcript's
historical first-seen branch). Hidden when there is no dir/branch, and on observe badges.

### 13d. **Action row** (row 2, left) — Open Path + copy menu
A linked badge's action row is **always visible** (originally the hover-only "row 3"; now it sits on
**row 2, to the LEFT of the dir/branch label**): a **folder** button (Open Path → the working dir via
`explorer.exe`, off-thread) + a **copy menu** + a **pencil**. The copy menu yields `Session Id` ·
`Copy Path` · `Copy Branch Name` · `Claude Launch CLI` · `Codex Launch CLI` (each the **REAL** full
command — the live process commandline from the PEB, or the builder Launch/Restore would use, *not*
a toy `--resume <id>`) · `Summary` (the full textual session box) · `Transcript` (the whole
conversation, user + assistant TEXT only via `ReadConversationText`). Every copy / Open Path plays a
short confirmation chime (`PlaySoundW`). Built only for a LINKED session (never an observe badge); the
buttons stay laid out, while the whole badge is dim at rest and brightens on hover.

### 13e. The SUMMARY PANEL (the pencil → a second overlay)
The pencil toggles a **second overlay** stacked **below the badge** (`TerminalPaneContent::SetAgentSummaryOverlay`,
capped to ≤20 % of the pane width), shown while the **GLOBAL** `AppSettings.showSummaryPanel` is on
(default ON; the pencil hands off to `TerminalPage::_ToggleSummaryPanel` — a freshest-disk
read-modify-write + a live broadcast to every linked overlay in the window; the cog Save preserves
it). It renders the `~/.claude/hooks/session-end.js` box — a faithful C++ port in
`ProcessInspect::AnalyzeSessionTranscript` (user messages [deduped, noise-filtered via
`SeIsCommandNoise`], files read / **created** / edited, branch, first/last timestamps, tasks, plan
signals, parent/plan) — analyzed **off-thread** (`_LoadSummaryAsync`) and reloaded only when the
transcript **mtime grows** (a quiet tab costs one `GetFileAttributesEx`). The displayed panel is a
**TRIMMED** view (omits what the badge already shows); the copy menu's `Summary` yields the COMPLETE
box. A live **times bar** ticks *age · last user msg · last activity*. It is **resizable** (left /
bottom / corner grips; size persisted GLOBALLY as pane fractions, or Shift-drag for a per-tab
ephemeral size), with a **wrap-line toggle** (↵: literal `\n` vs real newlines) and a **truncate
toggle** (…: cap long messages, default ON) at the right of the times bar — both GLOBAL + persisted.
Section separators fill border-to-border (a `StackPanel` of monospace `TextBlock`s interleaved with
full-width `Border` rules driven by a `\x1F` sentinel). Codex renders a reduced box
(`RenderCodexSummary`: model/effort + prompts).

### 13f. Tab-strip status dot (companion, not the overlay)
Independently of the in-terminal badge, every classified tab's header reads `[icon] ● <title>`: a
state-colored `Ellipse` (thin black stroke) in `TabHeaderControl.xaml`'s indicator row — a managed
session in its Triage-Board state color, an observed-but-unmanaged tab a dim gray, the Manager tab
none. The palette lives once in `AgentStatusColors.h` (board dot + this overlay + the tab-strip dot
all read it).

### 13g. Settings
`AppSettings` gained, beyond `showTabOverlay`: `showSummaryPanel`, `summaryPanelWrapNewlines`,
`summaryPanelTruncate`, and `summaryPanelWidthFraction` / `summaryPanelHeightFraction` — all GLOBAL,
persisted, and written by the overlay's own toggles/grips (not the cog form), with the cog Save
preserving them freshest-from-disk.
