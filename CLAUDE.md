# Agentmaster

> A fork of **Windows Terminal** (`microsoft/terminal`, MIT) that turns it into a manager
> for multiple **Claude Code** sessions.

## What we're doing & our aim

**Aim:** one Windows Terminal–based app that manages **N Claude Code sessions across M
working directories** — letting you **fully interact** with each session *and* giving the
app **100% programmatic control** (inject prompts, read output, drive state).

Each session is a real `claude.exe` on a **ConPTY** connection: a full-fidelity terminal
with a shared stdin (you and the orchestrator coexist), output tapped for the UI, and
semantic state taken from **Claude Code hooks** — never screen-scraping.

**Converged design:**
- **Design A — Native Graft:** the management "brain" lives *inside* the fork (C++/WinRT).
- **Manager tab (C1 "Linked Lenses"):** a pinned, leftmost, non-closable tab (tab 0, open
  by default), split into three selection-synced regions:
  - **Triage Board** (top) — sessions as cards in state columns (Running · Waiting-for-you
    · Needs-approval · Error).
  - **Explorer Tree** (bottom-left) — the M working directories → their N sessions.
  - **Flight Plan** (bottom-right) — a per-session prompt queue + **Autopilot**.
- **Flight Plan / Autopilot:** queue prompts; on **turn-complete** (`Stop` hook) the next
  prompt is auto-sent. Approvals and clarifying-questions are handled separately.
- **Per-tab link badge (overlay) — built ([`TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md)):**
  **every terminal tab the Fleet Observer classifies** carries a small **top-right terminal HUD**
  that makes the tab ⇄ Agentmaster link legible *while you work inside the session*. A **linked**
  Claude session shows the full badge — status (color-matched to the Triage Board) + the Fleet
  Observer's `model · effort · kind`, Autopilot mode (**Manual/Semi/Full**), queued count, and link
  state **⛓ linked**. Any **other** tab shows a dim **observe badge** `○ <kind> · unlinked` (kind =
  `pwsh` / `cmd` / `claude` *started-but-not-yet-prompted* (§11d) / `codex`) that **flips in place**
  as the tab's activity changes — a `pwsh` tab → `claude` the moment you run it → the full linked
  badge on its first prompt; cleared when claude exits or the tab closes — so a started-but-unprompted
  claude (no transcript id yet) is never invisible. Dim until hover; hover/click **expands** controls
  (Autopilot cycle · Send-now · queue peek · Jump-to-Manager) + a contextual SemiAuto confirm.
  Off-switchable (`AppSettings.showTabOverlay`).
  The per-tab *here-and-now* lens, complementing the Manager's *fleet* view.

Full design: [`doc/agentmaster/DESIGN.md`](doc/agentmaster/DESIGN.md).
Milestones & build: [`doc/agentmaster/IMPLEMENTATION.md`](doc/agentmaster/IMPLEMENTATION.md).
Hooks bridge: [`doc/agentmaster/HOOKS.md`](doc/agentmaster/HOOKS.md).
Workspace persistence (window layer): [`doc/agentmaster/PERSISTENCE.md`](doc/agentmaster/PERSISTENCE.md).
Release/dev identities + per-install state profiles: [`doc/agentmaster/PROFILES.md`](doc/agentmaster/PROFILES.md).
Per-tab link badge (overlay): [`doc/agentmaster/TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md).
Fleet Observer (pull correlation + activity): [`doc/agentmaster/OBSERVER.md`](doc/agentmaster/OBSERVER.md).
Sessions browser + the `~/.claude` storage map: [`doc/agentmaster/SESSIONS.md`](doc/agentmaster/SESSIONS.md).
Observer-owned session state (the PULL state engine — design, pre-implementation): [`doc/agentmaster/STATE.md`](doc/agentmaster/STATE.md).

## Status

**All milestones M0–M8 + session restore are complete, built, deployed (until the next deploy
cycle, still under the pre-split `Agentmaster` loose registration — the dev identity is now
`AgentmasterDev`, see *Deploy & run* migration), and verified running.** The engine passes
**666/666** standalone
checks (`AgentMaster/tests/`), and the full pipeline has been exercised end-to-end in the
deployed package: Launch → real `claude.exe` on a ConPTY → `--settings` hooks → PowerShell
forwarder → named pipe → registry → state machine → UI, plus `claude --resume` restore on
reopen (traces in `~/.agentmaster/hooks.log`).

**Workspace persistence ([`PERSISTENCE.md`](doc/agentmaster/PERSISTENCE.md)) — the window layer is
COMPLETE + live-verified.** (The original **M9–M14** milestone ladder is **superseded** by
PERSISTENCE.md §13's Increments 1–4: M9 + M10 shipped, **M11 obviated** — the lens rides the
`WindowRecord` autosave, not a persist-hook — M12/M13 delivered AS the Increments, and **M14
migration is moot** (no legacy format). The lone remaining item is one **cosmetic** refinement —
exact left-to-right tab interleave on reopen, noted at the end of this block.) **M9 (singleton
engine) is complete — compiles + unit-tested:** the
registry/bridge/scheduler are now ONE process-wide `SharedEngine` (`AgentMaster/Engine.{h,cpp}`),
so the WindowEmperor's many windows share one fleet over one pipe instead of two registries
racing on `\\.\pipe\agentmaster.<pid>` and clobbering `sessions.json`. **M10's data layer is
done — unit-tested:** the per-window `WindowRecord` schema + `windows/<id>.json` (de)serialize,
shaped as **Option 1** — per-window *UI state only* (geometry + Manager lens + ordered tab
*refs*); `sessions.json` + `SessionInfo.live` stay the single session source of truth, never
duplicated. **M10 capture + per-window lens restore is now done — built + live-verified**
(PERSISTENCE.md §13.5): each window **claims** its `windows/<id>.json` at engine init
(`Engine::ClaimWindowRecord`, or mints a fresh GUID), `_CaptureWindowRecord` reads live
geometry (the `PersistState` recipe) + ordered tab refs + the Manager lens, a debounced
autosave (`_saveWindowRecordThrottled`, fed by tab/resize/recolor/lens-push triggers + a
one-shot save at end of startup) persists it, and a claimed record **seeds the lens**
(`AgentManagerContent::Get/SetManagerState` + a lens-changed push). Verified in the deployed
package: the record carries real geometry + the real splitter layout, and a relaunch **reuses
the same windowId** (claim → restore round-trip). **Increment 2 (geometry re-apply) is also
done + live-verified:** a relaunched window reopens at its saved position/size/launch-mode via
the `TerminalWindow` startup seam (`GetInitialPosition`/`GetLaunchDimensions`/`GetLaunchMode` —
WT's own persisted layout is OFF in our DefaultProfile mode; note WT only calls
`TerminalPage::PersistState()` when `firstWindowPreference != DefaultProfile`, which our mode is
*not*, so geometry capture rides the autosave, not a close-flush). **So single-window workspace
restore — geometry + lens — fully works.** **Multi-window reopen (Increment 3) is also done +
live-verified — including both refinements.** On startup the `WindowEmperor` reopens the
**open-at-exit** set: it scans `windows/*.json` (sorted by filename, matching `LoadWindowRecords` so
`-s <idx>` is canonical), reads the **`open-windows.json` manifest** (a byte-scan for braced `{guid}`
tokens — it links `TerminalApp.dll`, not the `TerminalAppLib` static lib, so it can't call the JSON
helpers), intersects, and dispatches `wt -w new -s <idx>` only for the records that were open at last
exit; each window resolves `records[idx]` for geometry (TerminalWindow) and claims it by id
(TerminalPage), so geometry + lens agree. A **decide-prompt** (Yes/No "Reopen your N previous windows?")
gates the reopen when >1; a lone record / first run is silent. The **manifest** is owned by the
process-wide `SharedEngine` (NOT the Emperor enumerating the projected surface — the engine already has
every window's id): each `TerminalPage` `RegisterLiveWindow`s its `_windowId` at engine init and
`UnregisterLiveWindow`s in `~TerminalPage`, and every change rewrites `open-windows.json` = the live id
set **except** a change that empties it (skip-empty preserves the final snapshot; a hard shutdown that
kills the threads before they unregister leaves the full set). So a window closed mid-session is
**pruned** from the manifest (won't be re-offered) while its record stays on disk. The Manager's
**"Reopen Windows (N)"** recover button (next to Archived, shown when N>0 == records-minus-live,
`Engine::RecoverableWindows`) is the "if I answered No" path: it reopens each not-currently-open record
via `<our alias> -w -1 -s <idx>` (`TerminalPage::_ReopenSavedWindows` ShellExecutes
**`_AgentmasterReopenTarget()`** — the per-IDENTITY execution alias **by name**, `agentmaster.exe`
release / `agentmasterdev.exe` dev, so side-by-side installs never reopen into each other; not the
upstream `wt.exe`, which doesn't exist for our packages; unpackaged falls back to the neighbor
`WindowsTerminal.exe` — and the
single-instance handoff routes it back to the Emperor) — the runtime analog of the Emperor loop. A
window claimed-then-closed THIS session is **re-claimed** (its real id + lens) from a second
**reclaimable pool** (`Engine::reclaimableWindowRecords`, fed by `UnregisterLiveWindow`, drawn by id
ONLY), not minted as a lens-less duplicate; the no-arg front-pop claim never draws from it, so a plain
"+ new window" never adopts a closed window's layout. Verified end-to-end: 2 records + a manifest
naming only one → exactly that one reopens (silent) at its geometry; a fake id is dropped + the second
never added (register rewrote to the live set); the manifest survives app-close (skip-empty); a
gracefully closed window is pruned; and the recover path reopens a not-open record at its saved geometry.
**Window-grouped restore is now shipped + live-verified** — a reopened window re-homes its **whole
workspace**, not just geometry + lens. `TerminalPage::_RestoreWindowTabs` (run from `_OnFirstLayout`
after `_RestoreClaudeSessions`, gated on a *claimed* record) walks the record's ordered tab refs and
rebuilds them in place: each **Claude** ref resumes its session INTO this window (`_LaunchClaudeSession`,
lazy-start safe — no eager `connection.Start()`); each **Other** ref replays its captured WT startup
actions (`WindowLayout::FromJson` → one `ProcessStartupActions`) to recreate the shell tab with its
title + color + cwd. Capture fills the Other ref's `actionsJson` from `BuildStartupActions(Persist)` →
`WindowLayout::ToJson` in `_CaptureWindowRecord` (so `windows/<id>.json` now carries the pwsh/cmd tabs,
not just Claude refs). The Manager's **Archived** UI (now the full-window Archive page, C1 UI) is **grouped by window** (`_GatherArchiveRows`
over `Engine::RecoverableWindows`): each not-currently-open record is a "Saved window" card with a
per-window **Reopen window** (`_ReopenSavedWindow(idx)` → `agentmaster -w -1 -s <idx>`) over its session
rows (**Restore here** = cherry-pick one into the current window); sessions in no record fall under
"Other archived sessions". A clobber guard keeps a not-yet-laid-out window (no tabs AND no geometry, or
pre-Initialized) from overwriting a good record on disk (`_FlushWindowRecord`), and the close-flush
captures the final state before the gap-#1 teardown clears `_claudeTabs` (`CloseWindow`). A reopened
(claimed-record) window also **suppresses the default startup tab** — `_OnFirstLayout` gates
`ProcessStartupActions` on `!_windowRecordClaimed`, because in DefaultProfile mode a reopen's `wt -w new
-s <idx>` (where `ShouldUsePersistedLayout` is off) falls back to a default `newTab` that would otherwise
append a spurious pwsh tab to every reopened window and **accumulate one shell per reopen cycle** in the
record; a reopened window's content comes from the re-home, not the default action. Live-verified:
a window with 1 Claude + 3 pwsh tabs closed → reopened at its geometry (1466×780 @ 14,173) with the
Claude session resumed + all three pwsh tabs (title/cwd) replayed; the record round-tripped intact.
The reopened window also **re-selects the tab that was focused at close** — persisted by STABLE
IDENTITY (`WindowRecord::selectedSessionId` = the Claude conversation id; a shell tab, which has no
cross-restart id, falls back to `selectedTabIndex`; Manager / none ⇒ both unset). `_RestoreWindowTabs`
resolves the focused tab's live position and applies it deterministically — a trailing `SwitchToTab`
appended to the shell `ProcessStartupActions` batch (so it runs AFTER the async shells exist), or a
synchronous select when there are no shells. **Still deferred:** exact left-to-right interleave of
Claude vs Other tabs on reopen (Claude tabs land first, then shells — see PERSISTENCE.md §13.5).

**The Fleet Observer (O1–O7, [`OBSERVER.md`](doc/agentmaster/OBSERVER.md)) is complete — built,
deployed, and live-verified.** It is the **PULL** half of the state engine: a process- +
transcript-driven survey that detects + correlates + enriches **every** Claude session and tab
activity **out-of-band** (reading each `claude.exe`'s PEB — cwd / cmdline / env — and its
transcript), so a hand-typed `claude` that fires **zero hooks** (a shell `claude` function/alias
shadows the PATH shim) is still detected, bound, and driven — with **no hooks, no shim, no
settings, and nothing the user can feel** (all reads are out-of-band; it **never** writes to a
shell). It is the always-correct floor beneath the lossy hook **push** and subsumes the old per-tab
Toolhelp walk + the global `projects/` discovery scan. Live-verified end-to-end in the deployed
package: a no-hook `claude.exe` typed after a `cd` lands a Triage-Board card + per-tab overlay
within ~3 s, classified `Agentmaster`; two claudes in one cwd bind to their **own** conversations;
real-WindowsTerminal claudes are classified external + never bound; steady-state cost is µs.

**The Archive UI is now a full-window page; the round-2 audit's 13 fixes are deployed, and a round-3
audit (10 more fixes) + a three-commit informativeness batch are built + lib-verified on top — they ride
the next deploy cycle.**
The **Archived** button opens a **full-window Archive page** (dense sortable + searchable table left;
detail — metadata + read-only Flight Plan + **Restore here** / **Reopen its window** — right; multi-select
**bulk Restore**) replacing the old in-content modal, mounted over `TerminalPage`'s Root content rows with
**every pointer handler deferring** its tree mutation (a synchronous mid-click tree change AVs the
XAML-Islands hit-test — pinned from two crash dumps, fixed + live-exercised crash-free). A read-only sweep
of that page, the resume/restore path, and the `WindowRecord` layer then **fixed 13 correctness issues**
(commit `b5768081e`): quit-all now flushes the window record, a fleet-load **barrier**
(`Engine::restoreMutex`) stops a reopened window racing its tab re-home against a half-loaded registry, the
`live=true` revive is gated on a changed pid, `SessionRegistry::Remove` notifies observers, inject-rollback
covers every send path, and `ProcessAlive` uses a wait-based liveness test. A **round-3 audit** then
confirmed-in-code and fixed **10 more** (commit `6d463af2a`): `SessionInfo.branch` had **no live writer**
(the Branch column + the search's branch term were permanently empty) — now **backfilled off-thread** from
each transcript's first user line (quiet-update + ONE `SaveSessions`); an open page was a **stale
snapshot** — a registry **observer** (token, detached in `~TerminalPage`) behind an atomic visibility
mirror + a 400 ms trailing throttle keeps it **live**; the per-keystroke synchronous rebuild +
transcript-read storm got a **200 ms search debounce** + an **(id, mtime)-validated detail cache**; a
recoverable window with NO archived sessions was **invisible** here — now a synthetic, checkbox-less
**"Saved window" row** (sentinel id `window:<guid>`, never collides with a session UUID); the
gathered-but-never-rendered sent/total counts became a sortable **Plan column**; plus ago-phrasing
("just now", `mo`/`y` units), full-Unicode lowercasing (`LCMapStringEx`) + a slash-flipped dir term in
the search haystack, bulk restore in **view order** (was unordered_set hash order), and wider fixed
columns (the sort arrow was ellipsized off the default sort column). The **informativeness batch** on
top: hover **tooltips** on every truncating/abbreviating cell — absolute local datetimes behind
Created/Active, full title/path/branch, and the **W{n} chip tip** ("W2 · 4 tabs (2 claude, 2 shell) ·
1466×780 @ 14,173", `ArchiveWindowTip`, gather-stamped) (`af7bc5879`); **detail-pane depth**
(`2e8e45b7f`) — the truncated conversation **id + Copy id / Copy path / Open transcript** (off-thread
ShellExecute, Explorer `/select` fallback; all read-only — the no-delete design), an **always-on
Conversation section** (the transcript's human prompts, which the queue's else-fallback used to hide),
and **"Last assistant reply"** = where the conversation left off (a 64 KB off-thread **tail read**,
`ArchiveReadFileTail` → `ParseTranscriptDelta`, cached by (id, mtime), empty results cached too); and
**plan-text search** (`e2522e448`) — a gather-built per-row `searchBlob` (title · dir + slash-flipped
twin · branch · the **session id** · every queued prompt's label+text; window-only rows index their chip
tip) with **whitespace-tokenized AND-matching**, so "remember that prompt I queued" — or a UUID pasted
from `hooks.log` — finds its session. Detail: *C1 UI* + the audit bullets under *Persistence*.

**The Sessions browser ([`SESSIONS.md`](doc/agentmaster/SESSIONS.md)) is implemented — engine + UI,
lib-compiled green + engine-tested (the 604-check harness incl. a live-corpus smoke); it rides the
next deploy cycle.** A **"Sessions"** toolbar button (right after Archived) opens a full-window page
(the Archive page's structure + its deferred-pointer-handler discipline) listing **EVERY on-disk
Claude Code session** (`~/.claude/projects/*/<uuid>.jsonl` — not just managed ones) in a selectable
window: the `[1 month]` button click-cycles 1d/3d/7d/14d/1mo/3mo, hover opens a **From/To range
popup** (plain text boxes — islands-safe). The search bar `[ search ] (👤)(🤖)(📁)(📄)(F)` runs
**two-phase**: FAST = in-memory over per-session **sidecar indexes**
(`~/.agentmaster/sessions-index/<sid>.json` — `(size,mtime)`-invalidated, **incrementally**
re-accumulated from the stored byte offset; built by `TranscriptStore`) + the **`history.jsonl`
accelerator**; SLOW = **ripgrep**-prefiltered transcript content (`rg -il`, PATH-resolved,
batched under the cmdline cap, full in-process fallback), every match **scope-attributed
in-process** (👤 typed prompts vs 🤖 assistant text/thinking + tool inputs/results — rg can't tell
them apart, `ClassifyTranscriptLine` can), generation-cancelled on re-type. Both message scopes
OFF ⇒ title+directory only; 📁/📄 match the **directories/files a session's tool calls touched**
(`TranscriptStats::pathsAccessed`) and **default ON** (fast-phase-only — in-memory over the
sidecar, no rg/transcript IO; 👤/🤖/(F) default OFF — either message scope flips on the SLOW
content scan); (F) fuzzy has identical rg/in-process semantics
(`BuildSearchRegex`/`MatchesQueryText`, tested as a pair). The query parses into
**whitespace-split AND terms** (`ParseSessionQuery` — every term must hit, each may hit a
different field): `"quoted phrase"` = ONE **exact** contiguous term ((F) never applies inside
quotes), and a bare **whole session-id GUID** token ({braces} tolerated, never fuzzied) also
matches the session's **identity** — its id + fork-parent id — so a pasted id finds the session
and its forks; in the content phases a guid term **scopes** hits to that session, and a
guid-only query is answered by the fast phase alone. Rows carry fork-aware **created** (a
fork duplicates its parent's lines verbatim with `forkedFrom` stamps — file birth is the truth),
**line-derived last-activity** (file mtime lies: measured median ~1 h, max ~43 days —
SESSIONS.md §5), the title precedence `customTitle > aiTitle > legacy summary > first REAL
prompt`, the msgs·tools weight, and the session's **per-dir tab color** as a chip (solid = OPEN
here, dim = on-disk) with a **presence ring** when claude's own heartbeat reports busy/idle/
waiting. Detail = metadata + scope-tagged match snippets + the numbered prompt list (off-thread,
(id,mtime)-cached); actions: **Jump** (OPEN here), **Resume here** (`_ResumeSessionFromDisk`: an
unknown sid gets a minimal archived-shaped record, then the SAME transcript-gated `--resume` seam
— title pinning, dir color, hook correlation all reused), **Fork here** (`_ForkSessionFromDisk` —
the duplicate-tab fork's recipe: `claude --resume <parent> --fork-session --session-id <new>`, the
new id minted by us so hooks/registry correlate from the first event; offered on EVERY row
**including a LIVE one** — a fork writes its OWN transcript, so the adopt path's two-writers
hazard doesn't apply; transcript-gated → fresh; titled `"<title> (fork)"`, logged
`[sessions-page->fork]`), **Open New Session Here**; double-click = resume. **Presence
integration (§7-Q5's separation):** `TranscriptStore::ReadSessionPresence`
owns the raw `~/.claude/sessions/<pid>.json` read; the **observer** validates rows against its
process snapshot (stale/PID-reuse dropped) and publishes a `Presence()` table + the transient
`SessionInfo.presenceStatus` fact through `ObserveClaude` (**never** `SessionState` — Rule #13).
The same pass also fixed engine bugs: `ReadTranscriptInfo` now honors `ai-title`/legacy `summary`
+ skips sidechain/compact-summary lines, and the shared **noise filter** (`IsNoiseUserPrompt`)
keeps interrupt markers / command echoes / task notifications out of titles, prompt lists, AND
the scanner's Flight-Plan back-fill (STATE.md §8 bug-2 fixed). **Both full-window pages (Archive +
Sessions) now share generic chrome:** a window-level **overlay registry** — each page
`_RegisterAgentPageOverlay`s its host + atomic visibility mirror + an optional dismiss hook (the
Sessions page closes its range Popup there: popups render in the popup ROOT, a collapsed host
does NOT hide them) and the tab-switch seam (`TabManagement.cpp`) calls
`_DismissAgentPageOverlays()` — every page closes on tab switch with no page named there, so a
future page binds automatically by registering. **Up/Down navigate the visible (sorted +
filtered) rows** in both pages — nothing selected ⇒ Down picks the FIRST row, Up the LAST; then
±1 **wrapping** at the ends — via `PreviewKeyDown` on the page host (tunneling, so it beats the
focused search box), deferred to a clean tick, recolor-only highlight
(`_UpdateArchive/SessionsSelectionHighlight`) + `StartBringIntoView`; both pages **focus their
search box on show** (keyboard events only route through the page when focus is INSIDE it —
typing filters immediately, arrows work from the first keystroke). The button +
header-declaration wiring (AgentManagerContent, TerminalPage.h, ProcessObserver.cpp,
m5_tests.cpp, the Archive-TU registration/key-hook) rides the in-flight working tree alongside
the concurrent UIA work; the tree as a whole builds green.

**The tab strip itself now carries the state dot.** Every classified tab's header reads
`[icon] ● <title>`: a state-colored **Ellipse** (thin black stroke for contrast on any tab
chrome; dipped **1px below slot-center** via an asymmetric `0,1,6,-1` Margin — dead-center read
optically high against the title, and the +1/−1 pair keeps the 10px slot so the header row
doesn't grow) in `TabHeaderControl.xaml`'s indicator row right before the title — one more
`x:Bind`'ed element over `TerminalTabStatus` (two new observable properties,
`AgentStatusVisible`/`AgentStatusBrush`; `Tab.idl` already projects `TabStatus{get;}`, so no
`Tab.{h,cpp}` changes — the page drives it idempotently via `_SetTabAgentDot(tab, color?)`). A
**managed** session's tab wears its Triage-Board state color (Running blue · Waiting goldenrod ·
NeedsApproval orange-red · Error crimson · Done green · Idle gray); an observed-but-unmanaged tab
(pwsh / cmd / unprompted-claude / codex) a **dim gray** dot; the Manager tab none. Deliberately
NOT a title prefix — the one-title invariant (Rule #11: Explorer name == tab title == persisted
title) must never carry presentation glyphs through renames/persistence. The state palette now
lives ONCE in **`AgentStatusColors.h`** (the overlay's hand-synced copy folded in — board dot,
per-tab overlay, and tab-strip dot read the same table).

**Release/dev separation + per-install state PROFILES ([`PROFILES.md`](doc/agentmaster/PROFILES.md))
is implemented — lib-compiled green + engine-tested (604/604 incl. new profile checks); it rides the
next deploy cycle (and needs the one-time `Remove-AppxPackage`/re-register migration documented in
Deploy & run).** The GitHub release now ships its OWN identity (`Package-Rel.appxmanifest`:
`Agentmaster`/`agentmaster.exe`, selected by `/p:AgentmasterPackageIdentity=Release`) while the dev
loose layout becomes **`AgentmasterDev`**/`agentmasterdev.exe`/"Agentmaster Dev" — so both install
side by side; `GetWtExePath`-class launchers and the reopen dispatch (`_AgentmasterReopenTarget`) pick
the alias by PFN prefix (Dev first). ALL persisted state — engine files AND Terminal's own
settings.json/state.json (a `GetBaseSettingsPath` redirect to `<profile>\terminal\`) — lives in ONE
**profile folder** per install: resolved env `AGENTMASTER_PROFILE` > `.portable` marker
(`<exedir>\profile`, true-portable zips now pass `-PortableMode`) > the per-install slot in
`~/.agentmaster.profiles` > per-identity default (`~/.agentmaster` release+unpackaged /
`~/.agentmaster-dev` dev). An install's **first launch shows a picker** (TaskDialog command links —
comctl32 v6 dep added to `WindowsTerminal.manifest`): **Production / Development / Browse…** (+ a
"copy existing data from `~/.agentmaster`" checkbox; skips `locks/`+`shim/`+`bridge.json`, never
clobbers), runs from `WindowEmperor::HandleCommandlineArgs` AFTER the single-instance handoff and
BEFORE any state read (`-Embedding` defterm activations resolve silently); the choice persists
per-package-family, is changeable from the cog's new **PROFILE** row (applies on restart), and a
kernel **profile mutex** warns if two live instances point at one folder. The generated hook
forwarder's bridge discovery is now per-profile too (`BuildForwarderScript(stateDir)` — was a
hardcoded `~/.agentmaster/bridge.json`, a cross-instance hook-routing bug). Engine code is
otherwise untouched: `AgentmasterStateDir()` simply resolves through `ProfileBootstrap.h`, so
unpackaged/test runs keep the historical `~/.agentmaster`.

What works, by area:
- **Engine (M5, `AgentMaster/`; M9 process singleton).** Thread-safe `SessionRegistry` (single
  source of truth; **token-based** observers — `AddObserver`→token + `RemoveObserver` — and
  multiple `AddAdoptionHandler`s, so a closing window detaches its lens observer + adoption
  handler cleanly instead of dangling on the shared registry), `HooksBridge` (local named-pipe
  server `\\.\pipe\agentmaster.<pid>`), `ClaudeSpawn` (spawn/`--resume` recipe + the shared hooks
  config + PowerShell forwarder). **M9:** one process-wide **`SharedEngine()`** (`Engine.{h,cpp}`)
  owns the registry/bridge/scheduler for ALL windows (the WindowEmperor is one process, N windows
  on N threads); each `TerminalPage` copies the shared `shared_ptr`s and its Manager tab is a
  per-window *lens* over the one fleet. The engine also carries the per-window **activate sinks**
  (`Engine::RegisterWindowActivateHandler` / `ActivateSessionInOtherWindows` — the cross-window
  Activate fan-out; see *C1 UI*), registered at engine init and token-detached in `~TerminalPage`
  like the rest. The `<pid>` pipe is unambiguous *because* there is exactly
  one bridge; one writer for `sessions.json`; restore loads process-once
  under a load **barrier** (`Engine::restoreMutex` — a 2nd window blocks until the fleet is fully loaded, then
  skips, so it can't double-load NOR race its tab re-home against a half-loaded registry).
  Hooks → wire line → registry → hook-driven `SessionState` (Correctness Rule #1). The wire
  line carries a 7th **`tabToken`** field (the hosting `WT_SESSION`, for adopting a hand-typed
  `claude` — see *Adopt any `claude`* below), an 8th, **escaped `prompt`** field on
  `UserPromptSubmit` (`WireEscape`/
  `WireUnescape`: `\ \t \r \n`), so the registry records **every** message a session got — a
  prompt typed straight into the ConPTY becomes a `Sent`/`Typed` Flight-Plan entry, while the
  `UserPromptSubmit` echo of a prompt WE injected is recognized (text + a recency window + the
  transient `QueuedPrompt::echoed` flag) and NOT double-recorded — and a trailing 9th **`ts`**
  field (the hook's FIRE time, stamped by the forwarder before its slow Stop-path transcript
  work; old 8-field lines parse with ts=0 → arrival order). `ts` drives the **ordered state
  machine** (`NextSessionStateOrdered` + `SessionInfo.turns`, HOOKS.md *State machine*): a
  **stale Stop** (fired before the newest prompt — the slow Stop forwarder lands it after the
  next turn's `UserPromptSubmit`) keeps state + suppresses its question-bit/advance, and a
  **type-ahead** prompt (`UserPromptSubmit` at Enter-time mid-turn; the queued batch then runs
  as the next turn with NO further hook) is counted so that turn's `Stop` stays **Running**
  instead of stranding the whole follow-on turn in `WaitingForInput` — the "second turn never
  shows Running" bug. The scanner's synthesized missed-Stop is `quiescentStop` (≥2s-quiet
  transcript): always lands `WaitingForInput`, never stale, never held by the queue. Hook `ts`
  also refreshes `lastActivityUnixMs` monotonically (real hooks previously never updated the
  Waiting→Idle decay anchor — it only moved on synthesized events).
- **Adopt any `claude` — observe + control of sessions we did NOT Launch.** A `claude` you
  type yourself into any tab (the WT `+` button → `cd` → `claude`) is managed too, not just
  Manager-Launched ones. At engine init we export `CCMGR_HOOK_PIPE` into the app's process env
  and prepend a transparent **`claude` PATH shim** (`<profile>\shim\` — the ACTIVE profile dir,
  default `~/.agentmaster/shim/`; `claude.cmd` for
  cmd/PowerShell + a POSIX `claude`) that injects `--settings <ours>` then execs the real claude
  (`ResolveRealClaude`, resolved BEFORE the PATH prepend so it never finds the shim); every new
  tab is *meant* to inherit this env so a bare `claude` self-wires for hooks (but WT's env
  regeneration breaks that for `+` tabs — see the caveat below). (Launch's direct
  `CreateProcessW("claude …")` resolves `claude.exe` with no PATHEXT, bypassing the `.cmd` shim
  — no double-wiring.) The forwarder takes the session id from the hook **payload** and emits
  the hosting **`WT_SESSION`** as the `tabToken` wire field, falling back to a **`bridge.json`**
  discovery file when it didn't inherit the pipe env. The registry **adopts** an unknown session
  on `SessionStart` (flagged `SessionInfo::external`) and fires every window's adoption handler (`AddAdoptionHandler`, fanned out — whichever window hosts the `+` tab binds it);
  `TerminalPage::_AdoptExternalSession` matches the `tabToken` to a live ConPTY
  (`ITerminalConnection::SessionId`) and **binds a stdin injector** — promoting it to full
  observe+control (Autopilot can drive it). A claude hosted outside this app (no matching
  connection) stays observe-only (Rule #9). **Caveat — this hook fast-path is *degraded* for
  hand-typed `+`-tab claudes:** WT regenerates a `+`-tab's child env from the registry, dropping
  these runtime-only vars (`AM_SESSION` / `CCMGR_HOOK_PIPE` / the PATH shim — see Gotchas), so a bare
  `claude` there fires **zero** hooks (a Manager-Launched session is unaffected — it sets env
  explicitly via `spec.env`). The **Fleet Observer** (below) is the reliable detection + bind path
  that needs none of this env — it keys on the **roster correlation**, not the shim/pipe.
- **Fleet Observer — PULL correlation + activity (O1–O7, `OBSERVER.md`; `AgentMaster/ProcessInspect.{h,cpp}`,
  `ProcessObserver.{h,cpp}`, `Activity.h`).** The always-correct floor BENEATH the lossy hook push — the
  one path no shell function / alias / PATH quirk can shadow. Empirically grounded (we measured it live):
  **`WT_SESSION` is the correlation key** (== `ITerminalConnection::SessionId()`, stable across a claude
  close→`cd`→reopen), enumeration (~10 ms Toolhelp) is the only cost while PEB reads are µs, and Claude
  closes its `.jsonl` after each write so correlation is by **cwd → newest transcript**, not an open handle.
  - **Primitives (`ProcessInspect`).** ONE `SnapshotProcesses()` per survey (reused for census + every tab
    tree) + µs PEB reads via the x64 `RTL_USER_PROCESS_PARAMETERS` offsets WT itself reads (centralized +
    commented, WOW64-guarded): `ReadProcessCwd` (the claude PROCESS cwd — tracks `cd` across a relaunch;
    PowerShell never syncs ITS process cwd, but spawns claude with the right one), `ReadProcessCommandLine`
    (exposes `--resume`/`--session-id`/`--model`/`--effort`/`--permission-mode`), `ReadProcessEnv`
    (`WT_SESSION` + `AM_SESSION` + `CLAUDE_*`); `ProcessStartUnixMs`/`ProcessAlive`; pure tree helpers
    (`FindDescendantByImage`/`ChildrenOf`, replacing the old `FindClaudeDescendantPid`); `ReadClaudeFacts`
    (cmdline+env parse); `ClassifyRunningApp`; transcript resolution (`EncodeCwdToProjectDir` — every
    non-`[A-Za-z0-9]` → `-`; `ResolveSessionId` — see Rule #14); and **transcript content** —
    `TranscriptTimes` (cheap ctime/mtime stat = conversation age + last activity) + `ReadTranscriptInfo`
    (head or full read → the first-prompt **title**, `gitBranch`, and the human prompts, parsed like
    `ParseTranscriptDelta`; recent transcripts carry NO `summary` — verified 0/1842 over 90 days — so the
    first prompt is the title). A denied/elevated/WOW64 PEB read ⇒ observe-only, never mis-bound.
    (Promoted out of `ClaudeSpawn`'s anon namespace; `ClaudeCwdForShell` now delegates here.)
  - **`AM_SESSION` ownership stamp (O2, `Engine`).** A per-process GUID exported into the app env at
    engine init (alongside `CCMGR_HOOK_PIPE`); a Manager-Launched claude additionally carries
    `AM_SESSION=<guid>:<windowId>` (via `spec.env`, the launching window appended). `ClassifyRunningApp`
    matches on the GUID **prefix**: ours → `Agentmaster`, a bare `WT_SESSION` (no `AM_SESSION`) → the real
    `WindowsTerminal`, neither → `Other`. **This is the *census* signal, REFINED by the roster:** a
    hand-typed `+`-tab claude does NOT reliably inherit `AM_SESSION` (WT regenerates a tab's env — see
    Gotchas), so a claude correlated to one of OUR roster tabs is `Agentmaster` *regardless* (Rule #13);
    `AM_SESSION` only classifies the NON-rostered remainder (a background daemon we spawned, telling our
    instance from another's, or a real `WindowsTerminal` → external, observe-only). Per-process GUID ⇒ two
    Agentmaster instances stay cleanly separable.
  - **Registry feed (O3, `SessionRegistry::ObserveClaude`).** A provenance-aware upsert: first sight of a
    claude we did NOT Launch creates an `external` record + fires adoption; thereafter it idempotently
    ENRICHES the transient `SessionInfo` facts (pid / liveCwd / model / effort / permissionMode /
    background / runningApp / amSession / ownerWindowId, plus the transcript timing
    `convCreated/convLastActivityUnixMs` that drives the per-session timing adornment — all runtime-only,
    NOT persisted) but **NEVER** sets `SessionState` (push hooks + the transcript tail own state, Rule
    #1/#7). The timing is refreshed silently (like `lastObservedUnixMs`) — mtime ticks constantly, so it
    must not drive the change cascade; the UI recomputes the "ago" live on any rebuild. A steady-state re-observe
    with unchanged facts is a no-op (no observer / persist / UI churn). `hookWired`/`lastHookUnixMs` are set
    in `OnHookEvent` for provenance.
  - **S-lane (`ProcessObserver`).** A process-wide worker (next to the scanner, thread/condvar shape
    mirrored) on a ~2 s heartbeat + on-roster-change `Wake()`: ONE snapshot → `ReadClaudeFacts` + classify
    EVERY claude (the census, logged `[observer] census claudes=N ours=A wt=W other=O rostered=R`) → merge
    every window's published tab roster → per roster tab, `FindDescendantByImage` the shell's claude,
    resolve its id, and feed `ObserveClaude`. **A claude correlated to a tab in OUR roster is OURS
    regardless of `AM_SESSION`** (Rule #13). Publishes two copy-under-lock tables (Correlation + Activity).
  - **UI lane (`TerminalPage::_ObserverProbe`, replaces `_DiscoverClaudeTabsByCwd`).** The one WinRT
    thread: each scanner tick it builds THIS window's roster `{WT_SESSION = SessionId(), shell PID =
    GetProcessId(ConptyConnection::RootProcessHandle()), bound}`, `PublishRoster`s it (both reads are µs),
    then — after a short settle so the publish-triggered survey lands — reads the Correlation table and
    binds each of our unbound, id-resolved tabs via the **unchanged** `_BindClaudeSessionToTab` (injector +
    overlay + title + per-dir color). Detached via `_observer->UnpublishWindow(_windowId)` in
    `~TerminalPage` (Rule #10).
  - **Activity + adornments (O6).** Full `TabActivity` taxonomy (Powershell / Cmd / ClaudeCode / Codex
    (bare acknowledge, image+pid only) / Other), enriched `[activity]` events, `model · effort · kind`
    adornments on the Manager cards + per-tab overlay, and an **External (N)** group. The external
    census now includes **cmd-/console-hosted** claudes too (the `Other` bucket — previously counted but
    hidden), not only real-WindowsTerminal, and each `ExternalClaudeRow` is **enriched from its
    transcript** (worker-thread title cache so the head-read happens once): resolved `sessionId`, a
    **title** (first prompt), `gitBranch`, host kind (`wt` / `cmd` / shell leaf), and `created/lastActivity`
    timing. Observe-only — surfaced on the board AND the Explorer Tree's **EXTERNAL** scope, where a row's
    **Open New Session Here** / **Adopt** lives on the right-click menu and a **left-click → a read-only Flight Plan**
    of the conversation.
  - **Hardening (O7).** Steady-state is µs: the survey skips the Toolhelp snapshot when the roster is
    byte-identical to last tick AND every correlated `(pid, start-time)` pair is still alive (the start time
    is paired — per the rule that PIDs reuse — so a recycled PID can't masquerade as alive), except on the
    slow heartbeat. Retires the now-dead `SessionScanner` transcript-DISCOVERY sweep (`ArmDiscovery` /
    `RecentTranscripts`, which the observer subsumes; the scanner keeps its state-reconcile tail). A
    denied / elevated / WOW64 PEB read is guarded → observe-only, never a misread. 24-h soak: no leak / wedge.
- **C1 UI (M6, `AgentManagerContent`).** Triage Board + Explorer Tree + Flight Plan,
  imperative and snapshot-driven from the registry (cross-thread refresh via
  `DispatcherQueue`), bidirectional selection + directory scope. Each **Triage-Board column**
  is a fixed-width box at full board height with a **pinned header over a vertically-scrolling
  card list** (`_MakeBoardColumn`), so a tall column (e.g. a large **External** census) scrolls
  within the board instead of clipping past the bottom edge (the board's own ScrollViewer scrolls
  only horizontally). The Board header's **"Show all"** (clears the directory scope) is shown only
  when a directory IS scoped — it auto-hides (`_showAllBtn`, kept in sync by `_RebuildBoard`) while
  already showing all directories. A managed **board card** mirrors the Explorer-Tree row's
  interactions (one card/row, one action set): single-click selects, **double-click Activates**
  (jump to the live tab), and **right-click opens the SAME context menu** as the tree session row
  (`_MakeSessionMenu` — Rename… / Archive… / Open New Session Here; a board-invoked Rename first
  makes the tree row renderable — un-collapses its dir, widens a LOCAL scope to GLOBAL for a
  session hosted elsewhere — since the in-place editor lives in the tree). **Activate is
  cross-window**: the board and the tree's GLOBAL scope show the WHOLE fleet, but a session's tab
  lives in exactly one window, so `_ActivateClaudeSession` selects locally when this window hosts
  the tab and otherwise fans out through the engine's per-window **activate sinks**
  (`Engine::RegisterWindowActivateHandler` / `ActivateSessionInOtherWindows`; registered at engine
  init, token-detached in `~TerminalPage`, Rule #10) — the (single) hosting window hops to its UI
  thread, re-checks its `_claudeTabs`, selects the tab, and **foregrounds itself**
  (`_FocusClaudeSessionTab`: restore-if-minimized + `SetForegroundWindow` + the
  `SwitchToThisWindow` fallback — same-process, so the hand-off is permitted). Board/tree
  double-click, tree `Enter`, the Flight-Plan eye, and the Sessions page's Jump all ride this one
  seam. **Rename is cross-window too**: the rename writes the shared registry; the window hosting
  the tab re-pins its title via the registry observer (`_SyncClaudeTabTitleFromRegistry`, riding
  the tab-dot push — equality-guarded both directions, so the settled case is a no-op; Rule #11).
  The Board/Tree show only
  **OPEN** (`live`) sessions; closed ones are **ARCHIVED** (shut down, restorable) and opened from the
  **Archived (N)** toolbar button (the toolbar's rightmost, after the cog) — a **full-window Archive page**
  (`_BuildArchivePageShell`/`_ShowArchivePage`, mounted over `TerminalPage`'s Root content rows, ← Back to
  dismiss; it REPLACES the old in-content modal). LEFT = a dense, **sortable + searchable** table of archived
  sessions (Title · Directory · Branch · Created · Active · **Plan** `sent/total` · a saved-**window** chip),
  each row a checkbox for **multi-select bulk Restore** (restores in **view order**); a recoverable window
  with NO archived sessions still appears — a synthetic, checkbox-less **"Saved window" row** (sentinel id
  `window:<guid>`, tab composition + record-file timing) so every saved window is visible + reopenable from
  the page; **every truncating/abbreviating cell carries a hover tooltip** (full title/path/branch, absolute
  local datetime behind the relative ages, "N of M prompts sent", and the **W{n} chip's** what-window-is-this
  tip — tab composition + geometry + launch mode, `ArchiveWindowTip`, built once per record at gather). The
  **search box** (200 ms debounced) matches a gather-built per-row **`searchBlob`** — title · dir + a
  slash-flipped twin (`k:/source` matches `k:\source`) · branch · the **session id** · every queued prompt's
  label+text — with **whitespace-tokenized AND-matching** (every token must hit, order-free); the **Branch
  column backfills off-thread** (`_BackfillArchiveBranches`: `SessionInfo.branch` had no live writer — the
  transcript's first user line carries it; quiet-update all + ONE save, then poke the page). An **open page
  stays live**: a registry observer (token, detached in `~TerminalPage`) behind an atomic visibility mirror
  (`_archivePageVisible`) + a 400 ms trailing throttle re-gathers when a session archives/restores/renames
  anywhere. RIGHT = the selected row's **detail** — metadata, the truncated conversation **id** with
  **Copy id / Copy path / Open transcript** mini-actions (off-thread ShellExecute, Explorer `/select`
  fallback — all read-only, the no-delete design), a read-only Flight Plan, an **always-on Conversation
  section** (the transcript's human prompts — head-read, (id, mtime)-cached), **"Last assistant reply"**
  (where the conversation left off — 64 KB off-thread tail read via `ParseTranscriptDelta`, (id, mtime)-
  cached, empty results too), and **Restore here** / **Reopen its window** (resume via `claude --resume`,
  transcript-gated; Reopen re-resolves the live record index from the stable `windowId` at click time). The
  two halves are
  divided by a **draggable splitter** (the Manager-tab `_MakeSplitter` recipe, self-contained in the archive
  TU: drag state in a `shared_ptr` the handlers capture — no `TerminalPage` members; pointer deltas read
  relative to `nullptr` so no ancestor element is captured into a delegate cycle; star-width writes are
  layout-property changes, safe synchronously in pointer handlers — the defer-rule below is about tree
  mutations). The split is **persisted GLOBALLY and window-size-RELATIVE**: drag release normalizes the
  columns to `(f, 1-f)` STAR weights (a proportion, so a window resize keeps the ratio) and read-modify-writes
  `AppSettings::archiveSplitFraction` into `settings.json` (freshest-disk merge of just this field — the
  `treeSort` pattern — plus the window's in-memory copy, so a later cog Save can't regress it); every window's
  shell seeds its columns from it at build (sane-band clamped on load, like the Manager layout fractions).
  XAML-Islands hard
  rule: every pointer handler **defers** its visual-tree mutation to the dispatcher (a synchronous tree change
  mid-click AVs the hit-test), so row-select is highlight-only and open/sort/restore/back post to a clean tick. Explorer `Enter`=Activate /
  `Del`=archive (never injects — Rule #2). The tree's **scope toggle is 3-way — LOCAL · GLOBAL ·
  EXTERNAL** (this window's sessions · all windows · the Fleet Observer's observe-only externals);
  the **Triage Board header carries a 2-way LOCAL/GLOBAL twin** (`_boardScopeBtn`, same style) over
  the **same ONE state** — `_SetTreeScope` is the single mutator behind both buttons, the board
  reads EXTERNAL as GLOBAL (it has no External mode; a click there flips the shared scope to
  LOCAL), and in LOCAL the board filters its five state columns to this window's sessions (the
  External census column is never scoped — externals belong to no window). The scope is **persisted
  per window in the lens** (`ManagerState.treeScope`, riding the `WindowRecord` autosave — no
  longer in-memory-only; an older record without the field reads LOCAL, the prior default). The
  board's old `[all directories]` placeholder is **gone** (it was display-only); the `[scope: …]`
  label now appears only while a directory IS scoped, next to "Show all". After the tree's scope
  toggle comes a **sort toggle — NEWEST · OLDEST · MOST ACTIVE · A–Z · BY PID** (`_CycleTreeSort` /
  `_UpdateTreeSortButton`) that orders **both the directory groups and the rows within each, in every
  scope** (a dir's rank is an aggregate over its sessions: NEWEST/OLDEST by conversation ctime, MOST
  ACTIVE by recency with a currently-**Running** session pinned to the top, A–Z by name, **BY PID** by
  the **host window/shell pid** — externals: `ExternalClaudeRow::hostPid`, the same key as the
  color-coded pid underline, so same-window rows group together; managed: the claude pid — **then by
  most active** within each pid group; ctime/mtime via the same transcript timing as the adornment,
  `SortKey`/`MakeSortKey`/`SortKeyLess`). Unlike the
  per-window scope (lens-persisted, above), the sort is a **GLOBAL, persisted** setting (`AppSettings::treeSort` →
  `settings.json`, via the same settings sink the cog uses) — it survives restart and seeds every
  window (the changing window re-sorts live; others adopt it on next launch). After the sort comes a
  **↻ refresh button** (`_treeRefreshBtn`) that **reloads the data for the current scope**: it redraws
  immediately (re-pulls the registry snapshot + recomputes the live "ago" timing) and fires
  `_refreshHandler` → the page's `_RefreshObserverData` — which `Wake()`s the Fleet Observer for an
  immediate full survey (re-enrich the registry + recompute the External/Correlation tables, bypassing
  the O7 debounce), re-runs `_ObserverProbe`, then forces one `RefreshNow()` redraw once it lands — so
  LOCAL/GLOBAL re-pull and the EXTERNAL census both refresh on demand instead of waiting for the next
  tick. EXTERNAL/GLOBAL keep their local-first grouping, with the sort applied **within** each group:
  **EXTERNAL** (`_RebuildExternalTree`) lists every claude we do NOT manage — both real-WindowsTerminal
  **and cmd-/console-hosted** (the `Other` census bucket, previously hidden) — grouped by cwd. Each row is
  **enriched out-of-band from the transcript**: a real **title** (the conversation's first prompt — recent
  transcripts carry no `summary`), a **host** tag (`wt` / `cmd` / the shell leaf), `gitBranch`,
  `model · effort · pid`, and the timing adornment. The **pid number carries a color-coded underline**
  keyed by its **host window/shell** (`ExternalClaudeRow::hostPid` = the claude's parent shell pid,
  filled by the observer census; `WindowKeyColor` maps it through a stable palette) — claudes running in
  the **same terminal window/tab share a host shell**, so they get the **same underline color** and are
  easy to identify at a glance, even across cwd groups (note: real WT runs single-process, so this
  groups by host **tab/shell**, the finest reliable unit — it never falsely merges distinct windows).
  **Left-click selects** an external — from the
  Explorer-Tree EXTERNAL row **OR a Triage-Board External card** (the whole card is the click target;
  there is no inline observe pill / Adopt button) — → the Flight Plan shows its conversation
  **read-only** (`_RebuildExternalPlan` — the human prompts, read from the transcript on a
  **background thread** and cached; observe-only — we host no ConPTY, so it is never drivable, no
  queue/Autopilot). Selecting an external is **Linked-Lenses-synced** (`_SelectExternal`): it switches
  the tree to **EXTERNAL** with the row highlighted, highlights the board card, and renders the
  read-only plan — so a board click behaves exactly like a tree click. **Right-click** (on either the
  tree row or the board card — both use `_MakeExternalTreeMenu`) offers **Adopt** (resume its
  conversation into a managed, controllable tab — `_AdoptExternalClaude` resolves the id via
  `ResolveSessionId`, then `claude --resume`s it, leaving the original external running — Rule #13),
  **Open New Session Here** (spawn a managed session in that cwd, a new independent conversation),
  and, **as the last item, Bring Window To Front** (surface the external's HOSTING window:
  `_BringExternalToFront` resolves the row's `hostPid`/`sessionId`/title from the latest snapshot
  and hands the OS work to a background thread — `ProcessInspect::BringClaudeWindowToFront` walks
  the claude's ancestor chain to the nearest visible window (conhost children cover a classic
  console; a Win11 default-terminal HANDOFF console falls back to scanning foreign WT-class windows
  for a confident tab match), restores it when minimized, foregrounds it, and — when the host is a
  Windows Terminal-class window (`CASCADIA_HOSTING_WINDOW_CLASS`, real WT or our fork) — **also
  selects the claude's tab** via UI Automation. The pick is the pure, unit-tested `PickClaudeTab`
  over the window's tab names: per-tab score = max of the title tiers (`ScoreClaudeTabName`: exact
  title 100 > "claude" 90 > OSC status glyph 80 > containment 70 > title head 60 > cwd leaf 40 —
  hints are the row title, the transcript's **custom title** (`TranscriptInfo::customTitle`, the
  LAST `custom-title` line; it is also now preferred for the external row's display title), and the
  first prompt) and a **token-overlap tier** for hand-renamed tab labels (`ScoreTabNameTokens` over
  a 2 MB transcript read: every ≥3-char tab-name token present in the conversation corpus as a
  whole word or word-PREFIX — "act" ~ "actions"; tokens hitting the title+**first-prompt** HEAD
  corpus score 50 over 45 for later-prompt-only hits, so a purpose-named tab outranks an incidental
  word match; and a tab whose token set is a **strict subset** of a sibling's ("npyiter pr" beside
  "npyiter perf") is token-disqualified — a generic prefix would "uniquely" match any conversation
  sharing the word). The tab is selected **only on a UNIQUE strict-best score** — a tie or no
  signal keeps the window's current tab (a wrong-tab flip is worse than none; verified against the
  live fleet via `tests/uia_probe.cpp` + `_run-uia-probe.bat`, the ad-hoc dry-run diagnostic). A
  non-WT host (cmd console / ConEmu / VS Code) is just foregrounded. Window activation only — never
  input into the foreign session, upholding the Rule-#13 invariant). **Open New
  Session Here is offered in EVERY scope** — it is also the **last item** on the LOCAL/GLOBAL
  session-row menu (`_MakeSessionMenu`, after Rename / Archive), spawning in that session's working
  dir. With no external selected the
  Flight Plan reads **nothing-selected**. Every card/row (board, tree LOCAL/GLOBAL/EXTERNAL) carries a dim
  **timing adornment** `-createdAgo/activeFor/-lastActivityAgo` (e.g. `-2m7d/12h/-2h30m` — created ago /
  active span / last-activity ago; `m`=month or minute by position, tooltip-explained;
  `FormatSessionTiming`/`FormatSpan`) from the transcript's ctime/mtime (managed:
  `SessionInfo::convCreated/convLastActivityUnixMs`; external: `ExternalClaudeRow` times, falling back to
  the process start). **A session's title is one value** — the
  Explorer-tree row, the WT **tab** title, and the persisted `SessionInfo.title` are the same
  thing: it's **pinned** onto the tab at launch/restore (`Tab::SetTabText`, so it stops floating
  with claude's OSC title) and renaming from **either** side syncs the other + persists (Explorer
  right-click **Rename…** → `_RenameClaudeSession`; a WT tab rename → `_SyncClaudeTitleFromTab`;
  Rule #11). A launched session's default name is **smart-derived from its cwd**
  (`DeriveSessionTitle`: walk up past generic `bin/obj/Debug/...` segments to the first meaningful
  folder, then **≤16 chars** as-is / **>16 mixed-case** → its capitals only / **>16 all-lower** →
  as-is truncated past 30 with `...`), and each tab is **colored per working directory** (a stable
  auto palette color or the dir's persisted one; recoloring one tab recolors every tab in that dir
  and persists — Rule #12). Flight Plan: a **compose row** — three top-left icon buttons
  (**eye** = Focus / jump to the live tab · **!** = Send now, *which now confirms first* ·
  **envelope** = Add to the queue) beside a **multiline textarea** that grows as you type;
  per-message actions moved off a button strip onto a **right-click menu over the messages**
  (`_MakePromptMenu`: **Move up / Move down / Delete** on upcoming rows, **Archive session** on
  any). **Autopilot** is now a **toggle in the FLIGHT PLAN header** (mirrors the Explorer Tree
  LOCAL/GLOBAL/EXTERNAL toggle) — a colored state dot, gray ○ Off / amber ◐ Semi / green ● Full, that
  **cycles** Off → Semi-auto → Full on click (`_CycleAutopilot` / `_UpdateAutopilotButton`,
  replacing the old combo); the **Templates** row (save / apply / apply-to-dir) is collapsed
  behind a **paper icon** at the textarea's top-right (kept inline — NOT a Flyout — so its name
  box still takes keypresses; the XAML-Islands text-input trap). It still reflects **all**
  messages a session got, not just queued ones: a chronological **SENT** summary (each row
  tagged **flight** = we queued+injected it vs **typed** = you typed it into the terminal) over
  the **UPCOMING** queue (Pending/Held). `Focus()` focuses the cwd `TextBox`. The Launch cwd box has a
  focus-triggered **path-picker drop-down** (`Primitives::Popup`): up to 5 recent dirs (the
  current one excluded) over the subfolders of the current path + a `..` up-nav; click a row
  to navigate, and it re-lists. Working dirs are grouped/scoped with a filesystem-aware
  `PathEq`, so case-variant spellings collapse to one Explorer Tree root (Rule #8).
- **Autopilot (M7, `Scheduler`).** Pure `DecideAdvance()` + a worker thread on the registry
  advance seam. Turn-complete → auto-send next Pending (Full) / one-click confirm (SemiAuto)
  / Held by the question-guard (transient) / skip Manual gate. Backstops: pause-on-human-
  input, maxAutoSends, stopOnError, global Pause-all. Idempotent (atomic mark-Sent before
  inject). Advances fire on **two** triggers: the `Stop` seam (turn-complete) AND observed
  changes (`OnObserved`), so a session sitting **Idle** (freshly launched / just `--resume`d —
  it never emits a `Stop`) with a queued plan + autopilot **starts** consuming instead of
  waiting forever. `DecideAdvance` treats `Idle` as *ready* alongside `WaitingForInput`; a
  time-bounded **pickup guard** (the `echoed` flag + `kPickupGuardMs`) holds the next send until
  the just-injected prompt is picked up, so a change-driven advance never drains the queue (one
  prompt per turn). `RequestAdvance` dedups; a failed inject (no injector bound yet, e.g.
  mid-restore) rolls the prompt back to `Pending` rather than stranding a phantom `Sent` — on **every** inject
  path: auto-send, the SemiAuto `Confirm`, and the Manager's Send-now (Rule #4).
- **Persistence + archive/restore (M8, `Json.h`/`Persistence`).** Sessions + named plan
  templates + the path-picker's recent-dirs MRU (de)serialize to JSON under the **ACTIVE
  PROFILE** dir (`AgentmasterStateDir()` — default `%USERPROFILE%\.agentmaster\`, dev package
  `…\.agentmaster-dev\`; PROFILES.md); sessions autosave on change. **Lifecycle = Open ⇄ Archived**
  (the transient `SessionInfo::live` flag, never persisted): Open == has a live tab/claude this
  run (on the Board); Archived == shut down but kept restorable (behind the Archived button).
  On startup `_RestoreClaudeSessions()` loads each saved session into the registry as
  **Archived** and does **NOT** auto-launch it (Rule #6) — the app opens to just the Manager
  tab; the prior fleet comes back from the **Archive page** (per-row **Restore here** or **multi-select bulk Restore**). Closing a session's tab
  (the X, the tree `Del`, the Manager's Delete/Archive, or the Flight-Plan **Archive** button)
  all route through the ONE archive seam (`_HandleCloseTabRequested`→`_ArchiveAndCloseClaudeTab`):
  a single consequence confirm (gated by `confirmBeforeKill`), then `live=false` + clear injector
  + persist + close the tab — the record is **kept**, so it lists under Archived. **There is no
  discard**: archive is terminal, and the Claude transcript on disk is never touched. Restore
  re-launches in the working dir + reloads the Flight Plan + autopilot; resume is
  **transcript-gated**: `claude --resume <id>` only when Claude actually has a conversation for
  that id, otherwise a **fresh** session (new id, same dir + queue) — and the stale archived
  record is dropped. A never-prompted session has no transcript and a blind `--resume` would die
  with "No conversation found" (Rule #6). `Sent` prompts are never replayed. Templates: save a
  session's queue, apply it, or broadcast to a whole directory.
  - **Lifecycle coverage — audit of the add-tab / launch / close / archive / resume paths
    end-to-end. #1 & #2 are ✅ FIXED since the audit; #3 (by design), #4 (mitigated), #5 (moot),
    #6 (cosmetic) remain — all low-severity.** The tab-X archive seam above is sound; the *non-tab-X*
    exits were the risk. **(1, HIGH — ✅ FIXED) closing a _window_ now archives its sessions.**
    `_ArchiveWindowSessionsOnTeardown` (`TerminalPage.cpp:1825`, commit `4fa9fb7bc`) mirrors
    `_ArchiveAndCloseClaudeTab`'s bookkeeping for every hosted session — minus the dialog + `tab.Close()`
    (the tabs go with the window): flip `live=false`, clear the injector (releases the ConptyConnection →
    claude.exe exits), drop the per-window maps, persist once. It runs from the deterministic close seam
    (`CloseWindow:4605`, after the confirm) for immediate phantom-clearing AND idempotently from
    `~TerminalPage:254` as the catch-all for quit / any other teardown. So a window-chrome ✕ (still the
    _primary_ window exit — the non-closable Manager tab means `_RemoveTab`'s `size()==0` path can't fire)
    no longer leaves `live=true` **phantom cards** or **leaked injectors/connections** on the other
    windows' Triage Boards. **(2, MED — ✅ FIXED) no-tab archive no longer fake-archives a still-running
    session.** `_ArchiveClaudeSession`'s no-tab branch (`:1730`, commit `0336fa420`) now guards on
    `ProcessAlive(info->pid)`: it refuses to flip `live=false` on a claude that is still alive but not
    hosted in THIS window (an *ours* claude the S-lane carded a tick before its bind completed, or one
    hosted by another window) — which `ObserveClaude` would otherwise bounce back to `live=true` the next
    survey (`SessionRegistry.cpp:339`, Rule #7). Only a claude that has actually EXITED archives there.
    **(3, MED — open, by design) adopt-external = two writers on one transcript** — `_AdoptExternalClaude`
    (`:1877`) `--resume`s the id into a managed tab while the original external keeps running, both
    appending the same `.jsonl` (only a code comment — "the user closes it" — mitigates). **(4, LOW —
    mitigated) `SessionEnd`/`Done` doesn't eagerly archive** — only the liveness sweep
    (`_SweepClaudeLiveness:2334`) does, when the hosting connection reaches Closed; the window-close case
    it once missed (→ gap 1) is now covered by the teardown archive, leaving only ~one slow heartbeat
    (~2 s) of a finished claude lingering as a live card. **(5, LOW — effectively moot) Launch with a null
    tab** — `Upsert(live=true)`+`SetInjector` still run unconditionally after `_CreateNewTabFromPane`
    (`:1213`), but the `pane==null` guard (`:1183`) makes a null `tab` unreachable
    (`_CreateNewTabFromPane` returns null only for a null pane), so there is no phantom in practice; the
    unconditional upsert is a latent defensive nit. **(6, LOW — open) persisted `state` is dead weight** —
    `ToJson(SessionInfo)` writes it but `_RestoreClaudeSessions` forces `Idle` on load (`:1389`); written,
    never read (the resume-gating gotcha already says don't trust it). **Verified clean (not gaps):**
    external WindowsTerminal/Other claudes never enter the registry or `sessions.json` (`ObserveClaude` is
    gated on rostered + resolved id, `ProcessObserver.cpp:427`) — no foreign-claude leak into the Archived
    list; cross-window double-bind is guarded (`HasInjector`; one ConPTY lives in one window); and
    resume-fresh drops the stale archived record (`:1218`).
  - **Archive-page + resume/restore + persistence audit (round 2) — 13 findings, all ✅ FIXED + deployed**
    (commit `b5768081e`; a read-only sweep of the Archive page, resume/restore, and the `WindowRecord`
    layer). *Archive page:* the header count tracks the active filter ("K of N"); **Reopen its window**
    re-resolves its target from the record's stable `windowId` at click time (a gather-time index goes stale
    if the record set shifts while the page is open); `_GatherArchiveRows` indexes archived sessions by id
    ONCE (was O(tabs·sessions)) + dedupes a session referenced by two records to a single row; the
    Restore/Reopen/bulk handlers **defer** their tree mutation (the page's pointer-handler crash class);
    bulk-restore + its "(N)" count act on **checked ∩ visible** only (a check hidden by a later search is
    never silently restored) and stale checks are pruned each gather. *Persistence:* **quit-all now flushes**
    each window's record (`RequestQuit` + the `~TerminalPage` catch-all, latched so the catch-all can't
    clobber a good close-seam flush after `_claudeTabs` is cleared) — previously only the 750 ms debounce
    saved it, losing trailing geometry/tab-order/lens on app quit; the process-once fleet load is now a true
    **barrier** (`Engine::restoreMutex`) so a 2nd reopened window BLOCKS until the registry is populated
    before re-homing, instead of skipping its not-yet-loaded sessions and flushing an EMPTY record over its
    workspace; `_CaptureWindowRecord` stops persisting a **dead** per-tab Claude color (the dir-color system
    owns it, Rule #12) and only records a focused shell-tab target that can actually be recreated. *Registry
    / autopilot:* `SessionRegistry::Remove` now `_notify`s (a resume-fresh drop refreshes every window's
    Archive list — no ghost row); `ObserveClaude`'s `live=true` revive is gated on a **different pid** so a
    just-archived session whose claude is briefly still alive isn't bounced back (Rule #7); `Scheduler::Confirm`
    (SemiAuto) + the Manager's Send-now now **roll a failed inject back to `Pending`** like the auto-send path
    (Rule #4). *Primitive:* `ProcessAlive` prefers the unambiguous `WaitForSingleObject` liveness test (avoids
    the `GetExitCodeProcess`==`STILL_ACTIVE`/259 false-alive), falling back to the query path when SYNCHRONIZE
    is denied.
- **Per-window records (M10 data layer, `Persistence`/`SessionModels`).** A `WindowRecord`
  (one file per window: `windows/<windowId>.json`) holds per-window **UI state** — geometry
  (position/size/launch-mode), the Manager **lens** (selection / dir scope / selected prompt /
  collapsed dirs / splitter fractions / the shared tree+board LOCAL·GLOBAL·EXTERNAL scope,
  `treeScope`), and an **ordered list of tab refs** (a Claude tab = just
  its `sessionId`; a non-Claude tab = an opaque WT `actionsJson`). This is **Option 1** — a thin
  layer OVER the archive model: it records tab order + window↔session affinity + geometry/lens
  WITHOUT duplicating session data (`sessions.json` stays the session truth, so there is one copy
  of every session). Schema + (de)serialize + `Save/Load/Delete/LoadWindowRecord` are done and
  unit-tested; the live **capture** (debounced autosave) and **restore** (re-apply geometry/lens,
  claim/re-claim a record by id) are **shipped + live-verified** (see Status + `PERSISTENCE.md` §13.5).
  **Session re-home + Other-tab recreation are now shipped too** (`_RestoreWindowTabs`): a reopened
  window resumes its Claude sessions and replays its shell tabs (title/color/cwd) from the record's tab
  refs, in order — so closing and reopening a window brings the whole workspace back, not just
  geometry + lens. The Manager's full-window **Archive page** (C1 UI) groups closed sessions **by window** with a per-window
  "Reopen window". (Tab `actionsJson` capture, once deferred, is now live in `_CaptureWindowRecord`.)
- **Settings cog (`AppSettings`, `settings.json`).** A `⚙` (toolbar order: Launch · Reopen · `⚙` ·
  Pause Autopilot · Archived · **Sessions** — the cog sits *before* Pause Autopilot / Archived; the
  Sessions browser button comes right after Archived) opens a
  global-settings surface — an **in-content modal overlay** (a dimmed `Grid` over `_root`),
  NOT a `ContentDialog` (a text box inside one gets no keypresses in XAML Islands — see
  Gotchas). Exposes **Claude-session** config — `skipPermissions` (the spawn's
  `--dangerously-skip-permissions`), `model` (== `/model <v>`), `includeCoAuthoredBy`, and a
  global **`env`** (a `;`-delimited `NAME=VALUE` list applied to every session via
  `ParseEnvAssignments`→`spec.env`, `CCMGR_*` filtered) — plus **Autopilot defaults** stamped
  onto NEW sessions (mode / maxAutoSends / stopOnError / pauseOnHumanInput) and **behavior**
  (`confirmBeforeKill` — relabeled "Confirm before archiving" — routes the archive action
  (tab X / Manager Archive / tree `Del`) through the confirm dialog;
  `defaultLaunchDir` seeds the cwd box). A **PROFILE row** (read-only path + **Change profile
  folder…**) shows the ACTIVE per-install profile dir and re-runs the ProfileBootstrap picker —
  deliberately NOT an `AppSettings` field (the profile is the pointer TO `settings.json`, stored
  in the `.agentmaster.profiles` choice file / env, never inside the profile it selects); a change
  applies on the NEXT start and is shown staged as `current → new (after restart)` until then
  (PROFILES.md). It also carries non-cog global state set elsewhere in the
  UI but persisted through the same file: `showTabOverlay`, **`treeSort`** (the Explorer Tree's
  NEWEST/OLDEST/MOST ACTIVE/A–Z sort — written by the tree's sort toggle via the settings sink, NOT
  the cog), and **`archiveSplitFraction`** (the Archive page's table|detail split as the table's
  fraction — written by the splitter's drag release via a read-modify-write of settings.json; star
  ratios, so it scales with the window). Loaded at engine init, seeded via `SetSettings`,
  persisted + re-materialized on Save via `SetSettingsHandler`. Every default reproduces prior
  behavior, so a missing `settings.json` (or any unset field) is a no-op.

Follow-ups (not blocking): the PROFILES.md §5 set (per-identity defterm/shellext CLSIDs — the one
shared seam left between the release and dev packages; distinct dev iconography; profile
export/import); feed `pauseOnHumanInput` from a TermControl input tap;
bracketed-paste for true multi-line prompt bodies; a live buffer "peek" in the Flight Plan;
**bulk Restore** (the Archive page's "Restore selected") re-opens tabs lazily (a non-foreground restored tab starts its `claude` only
when first focused — WT's lazy-background-tab behavior; restore one at a time to force start);
a one-time **"Restore your previous layout?"** launch prompt (offers **all archived sessions**;
decided + **deferred** — it ships *after* the per-window `WindowRecord` capture is wired so it
restores true per-window layouts, not a flat global list — see `PERSISTENCE.md` §6/§6a);
prevent splitting the Manager tab. The **per-tab link badge / overlay**
([`TAB_OVERLAY.md`](doc/agentmaster/TAB_OVERLAY.md) / DESIGN §9.7) is **built** — `AgentTabOverlay`
wrapped in the terminal's slot `Grid` (`TerminalPaneContent::SetAgentOverlay`), driven by
`SessionRegistry::HasInjector` + `AppSettings.showTabOverlay`, attached on bind
(`_AttachClaudeOverlay` / `_claudeOverlays`) and **enriched by the Fleet Observer** with
`model · effort · kind` (O6). It now also shows on **every non-bound tab** as a registry-LESS
**observe badge** (`○ <kind> · unlinked`): the `_ObserverProbe` UI lane reads the observer's
`Correlation()` + `Activity()` tables and `_SetTabActivityBadge`s each tab's kind via
`AgentTabOverlay::ShowActivity` (registry-less — there is no session to observe; keyed by `WT_SESSION`
in `_pendingOverlays`, idempotent on an unchanged kind) — a `pwsh` / `cmd` / `codex` tab, or a
never-prompted `claude` (correlated but no transcript id yet, §11d). The badge flips kind in place as
activity changes and is **promoted to the bound `_AttachClaudeOverlay`** the instant a claude resolves
an id (its first prompt); `_DropPendingOverlay` collapses + releases it when the tab binds, the claude
exits, or the tab leaves the window's roster. Milestones tracked in `doc/agentmaster/IMPLEMENTATION.md`.

## Repo facts

- Forked from `microsoft/terminal` @ `v1.24.2372`; work branch **`agentmaster`**.
- Build entry: **`OpenConsole.slnx`** (slnx format). No git submodules in 1.24
  (`doc/building.md` is stale on that point).
- Toolchain: VS 2022 + C++/UWP workloads + Windows SDK 10.0.22621/26100.
- **TWO package identities, one branding** (`PROFILES.md`): the **release** identity
  **`Agentmaster`** (PFN `Agentmaster_56k4f06dsfp9r`, alias `agentmaster.exe`,
  `Package-Rel.appxmanifest` — selected by `/p:AgentmasterPackageIdentity=Release`, what
  `release.yml` ships and what v0.1.x already shipped) and the **dev** identity
  **`AgentmasterDev`** (PFN `AgentmasterDev_56k4f06dsfp9r`, alias `agentmasterdev.exe`, Start
  menu "Agentmaster Dev", `Package-Dev.appxmanifest` — the default; what the local loose layout
  registers). Distinct identities + per-identity aliases + the per-install state **profiles**
  are what let the released app and the dev build install side by side with zero collision —
  and both are deliberately **distinct from `WindowsTerminalDev`** so they coexist with real
  Windows Terminal. ⚠️ There is a **separate `K:\source\windowsterminal` checkout on this
  machine that owns the `WindowsTerminalDev` identity** — never reuse that identity here (see
  Gotchas). The Publisher is **`CN=Agentmaster` for BOTH** — an honest self-signed identity,
  NOT `CN=Microsoft Corporation` — and it is what **derives the PFN hash `56k4f06dsfp9r`**
  (the hash depends on the Publisher ALONE, which is why both families share it), so the
  public-release self-signed cert can carry that exact subject (an MSIX signature is valid only
  when the cert subject == `<Identity Publisher>`; see *Releasing a public version*). No C++
  hardcodes a PFN: `GetWtExePath` + the reopen dispatch key on the `AgentmasterDev`/`Agentmaster`
  name *prefixes* (**Dev first** — `Agentmaster` is a prefix of `AgentmasterDev`);
  `windowClassName` uses `GetCurrentPackageFamilyName()` at runtime. The dev loose-layout
  register is **unsigned** (no `CascadiaPackage_TemporaryKey.pfx` ⇒
  `AppxPackageSigningEnabled=false` in the wapproj). Hash recompute if a Publisher ever changes:
  SHA-256 the UTF-16LE Publisher, first 8 bytes, base32 over `0123456789abcdefghjkmnpqrstvwxyz`.
  Known shared seam: the defterm/shellext CLSIDs are upstream-Dev GUIDs in BOTH manifests — see
  `PROFILES.md` §1/§5 (don't set the dev build as the OS default terminal).
- Our additions (all marked `Agentmaster`):
  - `src/cascadia/TerminalApp/AgentManagerContent.{h,cpp}` — the Manager tab content (C1 UI).
  - `src/cascadia/TerminalApp/AgentMaster/` — the engine (plain C++, no WinRT; the `.cpp`
    are `<PrecompiledHeader>NotUsing`): `SessionModels.h`, `HookEvents.h`, `HookWire.h`,
    `SessionRegistry.{h,cpp}`, `HooksBridge.{h,cpp}`, `ClaudeSpawn.{h,cpp}`,
    `Scheduler.{h,cpp}`, `Engine.{h,cpp}` (the M9 process-wide `SharedEngine`),
    `SessionScanner.{h,cpp}` (the interval reconciler / PULL transcript tail), the **Fleet
    Observer** — `Activity.h` (data models), `ProcessInspect.{h,cpp}` (PEB / Toolhelp / transcript
    primitives — id resolution + content: title / prompts / ctime·mtime timing),
    `ProcessObserver.{h,cpp}` (the S-lane; also validates + publishes the `sessions/<pid>.json`
    presence heartbeat) — `TranscriptStore.{h,cpp}` (the on-disk Claude-session
    store API for the Sessions browser, SESSIONS.md §6: global transcript enumeration, the
    byte-offset-resumable scan + stats fold, fork-aware quick row facts, the per-session sidecar
    index, the raw presence read, and the shared prompt-noise + title-precedence rules),
    `SessionSearch.{h,cpp}` (the two-phase search: pure regex/match/snippet primitives + the
    history.jsonl accelerator + the rg-prefiltered, scope-attributed content scan with in-process
    fallback), `Json.h`, `Persistence.{h,cpp}`,
    `ProfileBootstrap.h` (header-only, pure Win32 — the per-install state PROFILE: resolution
    [env > portable marker > saved choice > per-identity default], the `.agentmaster.profiles`
    choice file, the first-launch TaskDialog picker + folder Browse, legacy-data migration,
    Terminal-settings seeding, and the one-instance-per-profile kernel mutex; included by the
    engine, AgentManagerContent, TerminalPage AND the WindowsTerminal EXE — PROFILES.md), and
    `tests/` (standalone harness, not in the msbuild — run `tests/run-m5-tests.bat`).
  - `src/cascadia/TerminalApp/AgentTabOverlay.{h,cpp}` — the per-tab link badge (TAB_OVERLAY.md),
    enriched by the observer with `model · effort · kind`; also the registry-less `ShowActivity`
    **observe badge** (`○ <kind> · unlinked`: pwsh / cmd / unprompted-claude / codex) for every non-bound tab.
  - `src/cascadia/TerminalApp/AgentStatusColors.h` — the ONE shared `SessionState` → color table
    (Triage-Board dot, per-tab overlay, and the tab-strip status dot all read it; replaced the
    overlay's hand-synced palette copy).
  - `src/cascadia/TerminalApp/TerminalPage.Agent{Engine,Sessions,Observer,WindowRecord,ArchivePage,SessionsPage}.cpp`
    — the TerminalPage-side Agentmaster *implementation* in six same-class TUs (the upstream
    `TabManagement.cpp` pattern; the original five were split out of `TerminalPage.cpp` as a pure
    move — **SessionsPage** is new code):
    **Engine** (`~TerminalPage`, `_InitAgentmasterEngine`, the Manager tab, `_WireAgentManagerContent`),
    **Sessions** (spawn/launch/restore/archive/adopt-external, tab-title sync, smart naming + per-dir
    tab color), **Observer** (the per-tab overlay/badge, bind/reconcile/liveness, the UI lane
    `_ObserverProbe`), **WindowRecord** (M10 capture/flush/restore + reopen saved windows),
    **ArchivePage** (the full-window Archive page), **SessionsPage** (the full-window Sessions
    browser — SESSIONS.md). Declarations stay in `TerminalPage.h` (C++ has no partial classes).
  - small touches in `TerminalPage.{h,cpp}` (~20 integration seams left in the `.cpp`:
    `_OnFirstLayout` startup, `_MakePane`'s `agentManager` branch, close/quit record-flush +
    teardown-archive, tab-move detach, title/color sync hooks, `_restartPaneConnection` injector
    re-point), `Tab.{h,cpp}` (a
    `TabColorChanged` event + `GetRuntimeTabColor`), `TabManagement.cpp` (incl. the generic
    `_DismissAgentPageOverlays` tab-switch seam), `TabHeaderControl.xaml` +
    `TerminalTabStatus.{h,idl}` (the tab-strip status dot: an `AgentStatusVisible`/
    `AgentStatusBrush`-bound Ellipse in the indicator row); registrations in
    `TerminalAppLib.vcxproj`.
  - `Package-Rel.appxmanifest` + `Package-Dev.appxmanifest` (the two identities; selection in
    `CascadiaPackage.wapproj` via `AgentmasterPackageIdentity`), a comctl32-v6 dependency in
    `WindowsTerminal.manifest` (the profile picker's TaskDialog), the `AGENTMASTER_PROFILE`
    redirect in `TerminalSettingsModel/FileUtils.cpp` (Terminal's own settings →
    `<profile>\terminal\`), the profile bootstrap call in `WindowEmperor.cpp`,
    `doc/agentmaster/`, `tools/Build-Agentmaster.ps1` (incl. `-ReleaseIdentity`),
    `tools/am-lock.sh` (the global build/launch mutex — see Deploy & run → *Concurrency lock*).
  - `.github/workflows/ci.yml` + `.github/workflows/release.yml` — the GitHub Actions build gate
    + public-release pipeline (see *Releasing a public version*). The **only** CI in the repo;
    Windows Terminal's upstream Azure-Pipelines / OneBranch under `build/pipelines/` is
    Microsoft-internal and never runs for this fork.
- **Runtime state dir = the ACTIVE PROFILE** (PROFILES.md; historically — and still, as the
  release/unpackaged default — `%USERPROFILE%\.agentmaster\`; dev-package default
  `%USERPROFILE%\.agentmaster-dev\`; resolution: env `AGENTMASTER_PROFILE` > `.portable` marker >
  the `%USERPROFILE%\.agentmaster.profiles` per-install choice file > the per-identity default;
  picked on an install's FIRST LAUNCH — Production / Development / Browse… — and changeable from
  the cog's PROFILE row, applied on restart). Contents: `hooks-settings.json` +
  `agentmaster-hook.ps1` (the shared hooks config Claude is pointed at via `--settings`),
  `hooks.log` + `autopilot.log` (engine traces), `forwarder-errors.log` (the hook forwarder's
  local silent-drop trace — a delivery that never reached the bridge: no sid / no pipe / a dead
  pipe's connect timeout; the bridge-side hooks.log only sees lines that ARRIVED), `sessions.json` (persisted fleet),
  `templates.json` (saved plans), `recent-dirs.json` (path-picker MRU), `dir-colors.json`
  (per-working-directory tab colors), `sessions-index/<sid>.json` (the Sessions browser's
  per-session search/stats sidecar cache — `(size,mtime)`-keyed, incrementally re-accumulated
  from the stored byte offset), `settings.json`
  (the Settings cog's `AppSettings`), `windows/<id>.json` (M10 per-window UI-state records —
  one file per window; captured + autosaved + restored), `open-windows.json` (the M10 Increment-3
  open-at-exit manifest — the live window-id set the next launch reopens), `bridge.json`
  (live-bridge discovery for the shim), `shim/` (the transparent `claude` PATH shim —
  `claude.cmd` + a POSIX `claude` — that auto-wires hand-typed sessions; see *Adopt any
  `claude`*), and `locks/` (the `build-launch` mutex — `tools/am-lock.sh`; see Deploy & run →
  *Concurrency lock*; the lock dir stays at the FIXED `%USERPROFILE%\.agentmaster\locks\` — it
  guards the ONE build tree, not a profile), and `terminal/` (Terminal's OWN settings.json +
  state.json — the `AGENTMASTER_PROFILE` redirect in `GetBaseSettingsPath`, seeded from the
  install's previous location on first profile claim). Deliberately NOT under `%LOCALAPPDATA%`
  — see Gotchas (MSIX).

## Integration points (1.24 pluggable pane-content model)

- New tab content implements **`IPaneContent`** (like `ScratchpadContent` — no `.idl`).
  Ours: `AgentManagerContent`, compiled via `src/cascadia/TerminalApp/TerminalAppLib.vcxproj`.
- Content dispatch by type string in `TerminalPage::_MakePane` (`TerminalPage.cpp`): we add
  an `else if (paneType == L"agentManager")` branch → `make_self<AgentManagerContent>()`.
- The Manager tab is opened by **`TerminalPage::_OpenAgentManagerTab()`**, called from
  `_OnFirstLayout` *before* startup terminal tabs, so it lands at index 0 (leftmost), and is
  tracked in the `_managerTab` member (nulled on close in `TabManagement.cpp`, mirroring
  `_settingsTab`). It is **non-closable AND non-movable**:
  - *Non-closable* — `CloseButtonVisibility = Never` (→ `IsClosable(false)`) hides the X and
    `DisableCloseAndMoveMenuItems()` greys the context-menu Close/Move. **Critically,**
    `_updateAllTabCloseButtons()` (which re-applies the theme's global close-button setting to
    *every* tab on each theme/settings pass) **skips `_managerTab`** — otherwise it re-enabled
    the X right after we set it (see Gotchas). The only way to "kill" is closing the **window**.
  - *Non-movable* — its `TabViewItem` is `CanDrag(false)`/`AllowDrop(false)`; `_TryMoveTab`
    refuses to move it and reserves index 0 for it; and `_PinManagerTabFirst()` (called from
    `_TabDragCompleted`) snaps it back to 0 if another tab is dropped ahead of it.
- Tab placement primitive: `_CreateNewTabFromPane(pane, insertPosition)` (`TabManagement.cpp`).
- **Engine wiring (`TerminalPage`):** `_InitAgentmasterEngine()` (from `_OnFirstLayout`,
  before the Manager tab) **consumes the process-wide `::Agentmaster::SharedEngine()`** (M9) —
  copies the shared `SessionRegistry` + `HooksBridge` + `Scheduler` + `SessionScanner` +
  `ProcessObserver` `shared_ptr`s and registers THIS window's adoption handler + liveness probe
  (tokens, detached in `~TerminalPage`). The once-per-process wiring (the logging / scheduler /
  persistence observers, the pipe, bridge discovery + hook files + the PATH shim, the
  **`AM_SESSION`** mint + export, and the `ProcessObserver` start) lives in `Engine.cpp` and runs
  on first access. The Fleet Observer's UI lane is `_ObserverProbe()` (ticked by the scanner's
  liveness probe alongside `_ReconcileClaudeTabs` + `_SweepClaudeLiveness`; it replaced
  `_DiscoverClaudeTabsByCwd`): publish this window's tab roster, then bind via the observer's
  Correlation table. `~TerminalPage` calls `_observer->UnpublishWindow(_windowId)` (Rule #10).
  `_WireAgentManagerContent()` hands the content the shared registry + spawn / activate / archive
  / restore / rename / adopt-external / pause / confirm callbacks + the cog's settings seed/persist
  (`SetSettings`/`SetSettingsHandler`). `_LaunchClaudeSession(dir, title, restored)` builds a
  claude `ConptyConnection` (cmdline/cwd/env ours) and opens it as a normal terminal tab via
  `_MakePane(args, …, existingConnection)`; `_SpawnClaudeSession` = fresh,
  `_RestoreClaudeSessions()` = load the persisted fleet **as Archived** (process-once via
  the `Engine::restoreMutex` load barrier — a 2nd window blocks until it's loaded, then skips),
  `_RestoreArchivedSession()` = the on-demand resume.
  `sessionId → Tab` lives in `_claudeTabs` (per window) for Activate / Archive / retitle. A
  session's **title is one value** (Explorer name == tab title == persisted `SessionInfo.title`):
  `_LaunchClaudeSession` **pins** it onto the tab (`Tab::SetTabText`); an Explorer rename routes
  through the `rename` callback → `_RenameClaudeSession` (registry + tab in lockstep); a WT tab
  rename (double-click / right-click **Rename Tab** / `renameTab` action, all via `Tab::SetTabText`)
  flows back through `_UpdateTitle` → `_SyncClaudeTitleFromTab`, which writes the registry (Rule #11).
  `_LaunchClaudeSession`/`_AdoptExternalSession` also **smart-name** an untitled session
  (`DeriveSessionTitle`) and **color the tab per working dir** (`_ApplyDirColorToTab` — the dir's
  persisted color or a stable auto one); a user color change flows `Tab::SetRuntimeTabColor` → the
  new `Tab::TabColorChanged` event → `_OnClaudeTabColorChanged`, which persists it to
  `dir-colors.json` and recolors every live tab in that dir (Rule #12).
- **Shared stdin:** the registry holds a per-session injector bound to that session's
  `ConptyConnection::WriteInput`, so the user's keystrokes and the scheduler's prompts both
  reach the same `claude.exe` stdin (Correctness Rule #3 binds the injector to the id).

## Building FAST

This machine: **i9-13900K — 32 threads (8 P + 16 E cores), 64 GB RAM.** The stock
`Invoke-OpenConsoleBuild` is slow because it runs msbuild with **no `/m`** (projects build
serially → 31 threads idle), builds the **whole** solution (tests/tools/samples), and
re-runs `nuget restore` every call. Per-file `/MP` is already enabled
(`src/common.build.pre.props:145`).

> ⚠️ **Hold the `build-launch` mutex before any full exe build, launch, or deploy** — see
> Deploy & run → *Concurrency lock*. (A lib-only compile-check, #6, doesn't relink the running
> exe and so needs no lock.)

1. **Build on NVMe, not the A400.** `K:` is a DRAM-less SATA SSD; a WT build is tens of
   thousands of tiny files and 32 threads thrash it. Prefer **`Q:` (Kingston Fury
   Renegade, Gen4 NVMe + DRAM, ~546 GB free)** or `C:` (Corsair MP600 PRO).

2. **Use the wrapper** — parallel `/m` + per-file `/MP`, scoped to just the app target
   (`Terminal\CascadiaPackage`), skips the redundant double restore on rebuilds:
   ```powershell
   pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1            # first build
   pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1 -NoRestore # inner loop
   ```
   Raw equivalent:
   `msbuild OpenConsole.slnx /m /p:Configuration=Debug /p:Platform=x64 /t:Terminal\CascadiaPackage /v:m`
   64 GB handles unbounded `/m`; if it ever pages, add `-ClMpCount 6`.

3. **Windows Defender exclusions** (Admin, once — 20–40% on cold builds):
   ```powershell
   Add-MpPreference -ExclusionPath (Resolve-Path .)
   'MSBuild.exe','cl.exe','link.exe','cppwinrt.exe','midl.exe','mc.exe','VBCSCompiler.exe','nuget.exe','tracker.exe' |
     ForEach-Object { Add-MpPreference -ExclusionProcess $_ }
   ```

4. **Iterate incrementally.** The cold build (restore + cppwinrt projection) is the
   expensive one; afterwards `-NoRestore` rebuilds (our edits touch only `TerminalApp`)
   are quick thanks to MSBuild's up-to-date check. Reference incremental times on this box:
   first ~236s, code-change rebuilds ~165–290s.

5. **Optional — MSBuildCache** for clean-rebuild / branch-switch cache hits: add
   `-p:MsBuildCacheEnabled=true` (uses file copies, not hardlinks).

6. **Compile-check without relinking the exe.** `TerminalAppLib` is a **static lib**, so you
   can validate code changes (and catch all our compile errors) while the app is still
   running — build just the lib (after `vcvars64.bat`):
   ```
   msbuild src\cascadia\TerminalApp\TerminalAppLib.vcxproj /m /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=K:\source\Agentmaster\
   ```
   `/p:SolutionDir=` (trailing `\`) is **required** when building a `.vcxproj` directly —
   otherwise `$(SolutionDir)build\rules\*.targets` imports fail (MSB4019). The full exe link
   is `msbuild OpenConsole.slnx /m /p:Configuration=Debug /p:Platform=x64 /t:Terminal\CascadiaPackage`
   (~3–3.5 min on this box; SolutionDir is implicit for the `.slnx`).

## Deploy & run

A packaged app can't be launched by running `WindowsTerminal.exe` directly (WT #926/#4043);
it must be deployed. Deploy the **loose layout** (what VS F5 does) — no signing/cert/admin:

```powershell
# one-time per machine (or after the manifest changes): register the loose layout
Add-AppxPackage -Register "K:\source\Agentmaster\src\cascadia\CascadiaPackage\bin\x64\Debug\AppxManifest.xml" -ForceUpdateFromAnyVersion
```

Launch any of these ways (the loose layout registers the **DEV identity**, `AgentmasterDev` —
the release MSIX owns `Agentmaster`/`agentmaster`; see PROFILES.md):
- execution alias: **`agentmasterdev`**
- Start menu: **“Agentmaster Dev”**
- `Start-Process "shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App"`

⚠️ **One-time migration on this machine** (first deploy after the identity split): the old loose
registration still owns `Agentmaster_56k4f06dsfp9r` (now the RELEASE family) and its on-disk
manifest no longer matches it — `Remove-AppxPackage Agentmaster_56k4f06dsfp9r`, then register as
above. First launch then shows the **profile picker** (current data lives in `~/.agentmaster`:
either Browse… to it, or pick Development + the migrate checkbox to copy it to
`~/.agentmaster-dev`). PROFILES.md §3 has the full recipe.

### Concurrency lock (multi-agent) — REQUIRED before any build, launch, or deploy

There is exactly ONE dev instance and ONE build output tree (`…\CascadiaPackage\bin\x64\Debug`),
so the close→build→relaunch cycle is **process-global and destructive**. Two actors running it at
once — two AI agents, or an agent + a human — collide: one closes the instance the other just
launched, two msbuilds race on the same outputs, and the **exe link fails because a running
`WindowsTerminal.exe` locks the very `WindowsTerminal.exe` being relinked**. **REQUIREMENT: hold
the global `build-launch` mutex for the WHOLE cycle before you build (full exe), launch, deploy,
OR close our instance.** (A lib-only compile-check — Building FAST #6 — doesn't relink the running
exe, so it needs no lock.) The mutex is a filesystem lock (`tools/am-lock.sh`, built on `mkdir(2)`
atomicity) under `%USERPROFILE%\.agentmaster\locks\`, so it **persists across separate Bash calls**
(each Bash tool call is a fresh shell) and is shared by every agent.

```bash
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "deploy $(git rev-parse --short HEAD)") || exit 1
# ... entire close → build → relaunch cycle while holding $TOKEN ...
bash tools/am-lock.sh release --token "$TOKEN"
```
`acquire` prints the token to **stdout** (capture it) and logs to stderr; `--wait 600` **queues**
behind another agent's build (picking up within ~1s of its release) instead of failing. Without
`--wait`, a contended `acquire` exits **3 (`BUSY`)** with the holder printed — back off, don't
spin. A crashed holder can't wedge it: a lock past its TTL (default 30 min) is auto-broken on the
next `acquire`. Token-checked `release` means one agent can't drop another's lock. Helpers:
`status` (who holds it + age), `refresh --token T` (extend a long hold), `acquire --force` (steal
now), `with --wait 600 -- <cmd>` (acquire → run one command → release). `bash tools/am-lock.sh --help`.

**Inner loop.** The loose layout is live (binaries update in place), but you **cannot
relink `WindowsTerminal.exe` while the app is running** — it locks the exe. So: close *our*
dev instance (spare the Store WT), rebuild, relaunch. The user has **standing-authorized
this close→build→relaunch cycle** ("always auto deploy") — run it without prompting; just
never touch the Store WT (it's not under our path — see Gotchas). **Hold the `build-launch`
mutex around the whole cycle** (acquire at step 0, release at step 4 — see *Concurrency lock*).
```bash
# 0. acquire the global mutex — REQUIRED (queues behind another agent's build)
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "deploy $(git rev-parse --short HEAD)") || exit 1
```
```powershell
# 1. close ONLY our dev instance (path filter spares the Store WT — see Gotchas)
Get-CimInstance Win32_Process -Filter "Name='WindowsTerminal.exe' OR Name='OpenConsole.exe'" |
  ? { $_.ExecutablePath -like 'K:\source\Agentmaster\*' } | % { Stop-Process -Id $_.ProcessId -Force }
# 2. build (full exe link)
pwsh -File .\tools\Build-Agentmaster.ps1 -NoRestore      # or: msbuild OpenConsole.slnx /t:Terminal\CascadiaPackage /m /p:Configuration=Debug /p:Platform=x64
# 3. relaunch
Start-Process "shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App"   # or: agentmasterdev
```
```bash
# 4. release the mutex (always — even if a step above failed)
bash tools/am-lock.sh release --token "$TOKEN"
```
Re-register **only** when `Package-Dev.appxmanifest` changes. (VS F5 on `CascadiaPackage`
also builds + deploys.) Runtime/session state lives in the dev install's **profile** (default
`%USERPROFILE%\.agentmaster-dev\`, or wherever the picker pointed it — check
`%USERPROFILE%\.agentmaster.profiles`); tail `hooks.log` there to confirm the engine is live
(`[engine] bridge listening …`) and that spawned sessions' hooks arrive (`[SessionStart]`,
`[Stop]`, …).

## Releasing a public version

**Only when the user agrees / requests it.** Cutting a release builds, self-signs, and publishes
a GitHub Release — it is outward-facing, so confirm the **version** with the user first and don't
do it on your own initiative. (Local dev deploys via *Deploy & run* above; this is the public path.)

Two GitHub Actions workflows are the repo's only CI:
- **`.github/workflows/ci.yml`** — build-only gate on `pull_request` + push to `agentmaster`/`main`
  (+ `workflow_dispatch`): restores vcpkg + nuget (cached) and builds `Terminal\CascadiaPackage`
  x64 Release. No packaging / signing / release. **No path filter**, so a docs-only push still
  triggers a full ~20-min build (add `paths-ignore: ['**.md','doc/**','LICENSE']` if that matters).
- **`.github/workflows/release.yml`** — the public-release pipeline: triggers on a **tag `v*`**
  push OR **`workflow_dispatch`** (a `version` input). No nightly, no Azure, no NuGet *publish*
  (NuGet *restore* stays — it's a build dependency).

**To cut a release (happy path):**
```bash
# agree X.Y.Z with the user, then either tag-push (real release) ...
git tag vX.Y.Z && git push origin vX.Y.Z          # auto-triggers release.yml
# ... or dispatch a DRAFT dry-run without creating a tag:
gh workflow run release.yml --repo Nucs/Agentmaster -f version=X.Y.Z
# find + watch the run (background it; it pings on exit; --exit-status => failure is non-zero):
gh run list --repo Nucs/Agentmaster --workflow release.yml --limit 3
gh run watch <run-id> --repo Nucs/Agentmaster --exit-status
```
The pipeline is **prep** (version from the tag/input, stamped into `Package-Rel.appxmanifest`'s
`Identity Version`) → **build** matrix **x64 + arm64** (Release, `WindowsTerminalBranding=Dev` +
**`/p:AgentmasterPackageIdentity=Release`** = the `Agentmaster` RELEASE identity — never the dev
`AgentmasterDev` manifest, `AppxPackageSigningEnabled=false`) → **bundle**
(`build/scripts/Create-AppxBundle.ps1` merges both arches → `.msixbundle`, then self-signs;
`New-UnpackagedTerminalDistribution.ps1 -PortableMode:$true` makes the portable zips — TRUE
portable: `.portable` marker ⇒ WT settings in `<unzip>\settings`, the Agentmaster profile in
`<unzip>\profile`) → **release** (`softprops/action-gh-release` creates a **DRAFT** GitHub Release
with the `.msixbundle`, `Agentmaster.cer`, and both portable `.zip`s). It lands as a **draft** —
review the assets, then **the user publishes it** (a draft creates no git tag until published; a
`workflow_dispatch` draft is safe to delete). After publishing, refresh the release notes + README
(capabilities / Download) if the feature set moved.

**Signing.** The release self-signs with a **`CN=Agentmaster`** code-signing cert minted at runtime
whose subject is **read from the manifest**, so it always equals the Publisher (this is *why* the
Publisher is `CN=Agentmaster` — see Repo facts). Being self-signed, users must trust
`Agentmaster.cer` once to install the `.msixbundle`; the **portable `.zip` needs no cert** (unzip +
run `WindowsTerminal.exe` — fully self-contained: settings + profile live inside the unzip dir).
To sign with a real cert instead, set repo secrets **`SIGNING_PFX_BASE64`**
+ **`SIGNING_PFX_PASSWORD`** (its subject must still equal the Publisher).

**One-time repo settings** (already done for `Nucs/Agentmaster`; a fresh fork needs them — Actions
is **OFF by default on forks**, and the release job needs write to create the Release):
```bash
gh api -X PUT repos/<owner>/<repo>/actions/permissions -F enabled=true -f allowed_actions=all
gh api -X PUT repos/<owner>/<repo>/actions/permissions/workflow -f default_workflow_permissions=write
```
Verified end-to-end on a stock `windows-2022` runner (UWP workload + Windows SDK + vcpkg all
present): `v0.1.0` ran green — CI ~21 min, release ~23 min (x64+arm64 in parallel). On failure the
build **binlog uploads as an artifact** to diagnose the first run.

## Gotchas (learned the hard way)

- **`nuget.exe` can't parse `.slnx`.** The bundled `dep\nuget\nuget.exe` errors with
  "file type was not recognized" on `OpenConsole.slnx`, and a bare `packages.config`
  restore needs `-PackagesDirectory`. The wrapper restores `dep\nuget\packages.config`
  into `packages\` and is non-fatal; use `-NoRestore` once packages exist. (Stock
  `Invoke-OpenConsoleBuild` has the same latent bug — it only "works" because VS already
  restored.)
- **`Grid`/`Panel` has no `Focus(FocusState)`** in this XAML projection — only
  `Control`-derived types do (this caused error C2039). `IPaneContent::Focus` must focus a
  `Control` child; `AgentManagerContent::Focus` focuses its cwd `TextBox`.
- **`TextBox` has no `VerticalScrollBarVisibility`** in this projection (WPF puts it on the
  TextBox; UWP doesn't) — set the **attached** `ScrollViewer.VerticalScrollBarVisibility`
  instead (`ScrollViewer::SetVerticalScrollBarVisibility(box, …)`; also a C2039, hit on the
  multiline Flight-Plan compose box). Related glyph-alignment quirk: a bare symbol rides
  differently per element — a **`TextBlock`** reserves descent space in its line box (so a
  centered `"!"` sits high), while a **`FontIcon`** centers the glyph's ink. Render compose-bar
  symbols as `FontIcon` (even a text-font one, e.g. `Segoe UI` glyph `"!"`) so an icon row lines
  up; mixing `TextBlock` + `FontIcon` siblings misaligns them.
- **Building a `.vcxproj` directly needs `/p:SolutionDir=K:\source\Agentmaster\`** (trailing
  `\`), else `$(SolutionDir)build\rules\*.targets` imports fail with MSB4019. The `.slnx`
  build sets it implicitly. (See Building FAST #6.)
- **Imperative XAML name clashes:** a `using namespace winrt::Windows::UI;` pulls the nested
  `Text` namespace into scope and collides with a `Text(...)` helper (C2872/C2882) — prefer
  narrow `using`-declarations (`Color`/`ColorHelper`/`Colors`). The `.cpp` can't run-time
  test here, so the compiler is the safety net; build the lib (#6) after UI edits.
- **A text box inside a `ContentDialog` gets no keypresses in XAML Islands.** The dialog's
  PopupRoot sits outside our island's input path, so a hosted `TextBox`/`NumberBox` takes
  focus but receives no typing (first hit with the tree-rename box). So: use a **buttons-only**
  ContentDialog for confirms (`_OnDeleteSession`), and build anything that needs typing **into
  the main visual tree** instead — the in-place rename editor, and the Settings cog's
  **in-content modal overlay** (a dimmed `Grid` over `_root`, `RowSpan`-all; the card swallows
  taps via a handled `Tapped`, a backdrop tap = cancel). Don't reach for a ContentDialog when a
  field needs keyboard input.
- **A pinned tab's `CloseButtonVisibility::Never` is silently undone by
  `_updateAllTabCloseButtons()`.** Setting `Never` on a tab once (as `_OpenAgentManagerTab` does)
  is NOT enough: that method loops EVERY tab and re-applies the theme's *global* close-button
  setting (`tab.CloseButtonVisibility(theme…)` → `IsClosable`), and it runs on every theme/
  settings apply — which happens *after* the Manager tab is created — so the X reappeared. Fix:
  **skip `_managerTab` in that loop.** Note hiding the X (`IsClosable`) is independent of
  drag-reorder: a non-closable tab can still be dragged/torn out, so non-movable needs its own
  guard (`TabViewItem.CanDrag(false)` + `_PinManagerTabFirst()` snap-back + a `_TryMoveTab`
  refusal). Context-menu Close/Move is a third axis (`DisableCloseAndMoveMenuItems()`).
- **Never reuse the `WindowsTerminalDev` package identity.** It belongs to the separate
  `K:\source\windowsterminal` checkout; registering the same identity tries to *replace*
  it and fails with a file-in-use lock (`0x80073CF6 / 0x80070020`) when its
  `OpenConsoleProxy.dll` is loaded. Our distinct `Agentmaster` identity sidesteps this.
- **Upstream "wt identity" assumptions are package-BLIND and break under our rename** (commit
  `404c04f72`; per-identity since the profiles split). Several WT helpers hardcode the
  `wt.exe`/`WindowsTerminal` identity and silently
  misbehave for our packages: (1) **`GetWtExePath()`** (`WtExeUtils.h`) resolves
  `<PFN>\wt.exe`/`wtd.exe`, which **doesn't exist** (our manifests register the **`agentmaster.exe`**
  / **`agentmasterdev.exe`** execution aliases) — so every launcher built on it (`_OpenNewWindow`,
  the Reopen-Windows button, Jump
  List shortcuts) silently no-ops; (2) **`windowClassName`** (`WindowEmperor.cpp` — it seeds BOTH the
  single-instance **mutex** and the **window class**) is built from `WT_BRANDING` only, so our Debug
  "Windows Terminal Dev" branding **shared one single-instance identity with the real
  `WindowsTerminalDev`** (a launch could hand its commandline to the *other* window — defeating the
  whole point of the rename). Fix (both, package-aware): `GetWtExePath` picks the alias by PFN
  prefix — **`AgentmasterDev` FIRST, then `Agentmaster`** (the former contains the latter as a
  prefix; same ordering rule in `_AgentmasterReopenTarget`) —
  and `windowClassName` appends `GetCurrentPackageFamilyName()`
  for packaged builds. So coexistence (with `WindowsTerminalDev` AND between our own release/dev
  pair) needs distinct package identities (above)
  AND package-distinct **runtime** identifiers. Launch the alias by **name** (`agentmaster.exe` /
  `agentmasterdev.exe` — per-identity, so it can never activate the other install) — it
  resolves on PATH + follows the APPEXECLINK reparse and hands off; a full reparse-path
  `CreateProcess`/`Start-Process` bypasses the alias and cascades a fresh window instead
  (ShellExecuteEx on the full `<PFN>\<alias>` path is the verified exception — see `GetWtExePath`).
- **Closing instances to relink.** Auto-closing **our** dev instance for the deploy inner
  loop is standing-authorized ("always auto deploy"): filter by
  `ExecutablePath -like 'K:\source\Agentmaster\*'` (matches our `WindowsTerminal.exe` *and*
  its `OpenConsole.exe` ConPTY hosts), `Stop-Process` them, build, relaunch — no prompt.
  But **never** touch the running **Store** Windows Terminal — it's the user's live session
  (under `Program Files\WindowsApps\…`, not our path). Never blanket-`taskkill` by image
  name; always path-filter so the Store WT is spared.
- **Packaging (`PRI210 / 0x800704c8`) can fail to overwrite `resources.pri`.** The registered
  loose-layout package keeps `src\cascadia\CascadiaPackage\bin\x64\Debug\resources.pri`
  memory-mapped, so MakePri's final overwrite-move dies with `0x800704c8` (ERROR_USER_MAPPED_FILE)
  — and `handle64` shows *no* owning user process (it's a registered-package/kernel mapping). The
  exe/dll already linked by then (your code is in the fresh `TerminalApp.dll`, copied into the
  layout *before* this step), so it's purely the PRI. Fix: **delete that `resources.pri` and
  rebuild** — MakePri then *creates* it fresh instead of overwriting a mapped target. (The Defender
  exclusion in Building FAST #3 also reduces the transient-lock variant.)
- **MSIX virtualizes a packaged app's `%LOCALAPPDATA%`** to the package LocalCache, but the
  spawned **`claude.exe` is external** and resolves paths against the real filesystem. So
  the hooks files + `--settings` path **must** live somewhere un-virtualized that both
  agree on — Agentmaster anchors the **profile defaults under `%USERPROFILE%`**
  (`AgentmasterStateDir()` == the ACTIVE PROFILE: default `~\.agentmaster`, dev package
  `~\.agentmaster-dev`; a Browse…-picked profile inherits this constraint — it must be a real
  filesystem folder, which `FOS_FORCEFILESYSTEM` enforces).
  Using `%LOCALAPPDATA%` here silently breaks hooks for spawned sessions (the app writes to
  LocalCache; Claude reads the empty real path). Verified live: with the fix, a spawned
  session's `SessionStart`/`UserPromptSubmit` reach the registry (`<profile>\hooks.log`).
- **`--dangerously-skip-permissions` also skips the startup "trust this folder" dialog.**
  Spawned sessions run with the flag by default (`AppSettings.skipPermissions`): besides
  auto-accepting tool prompts, permission mode `bypassPermissions` makes claude skip the
  per-folder trust dialog at startup (claude's block is gated on `mode !== "bypassPermissions"`),
  which would otherwise wedge an unattended ConPTY session waiting on a keypress. It does NOT
  suppress the one-time **global** "Bypass Permissions mode" acceptance (`~/.claude.json`
  `bypassPermissionsModeAccepted` — shown once, ever, until accepted). Toggling skipPermissions
  OFF drops the flag and instead pins `permissions.defaultMode:"default"` in the hooks-settings
  file (normal prompts + trust apply). The trust decision itself keys on the **git toplevel**
  of the cwd (forward-slash) under `~/.claude.json` `projects.<dir>.hasTrustDialogAccepted` — a
  trusted ancestor counts; accepting at your **home dir never persists** (so it re-prompts).
- **`claude --resume <id>` dies if there's no conversation.** A session that was opened but
  never prompted has no saved transcript; resuming it exits code 1 ("No conversation found")
  and the tab is dead. **Don't gate on the persisted `SessionState`** — it is overwritten
  with the *live post-restore* state (a just-resumed session reads `Idle` until its first
  new turn), so it's useless as a "was-it-used" signal. Gate on the transcript on disk:
  `ClaudeConversationExists(id)` globs `<CLAUDE_CONFIG_DIR | ~/.claude>/projects/*/<id>.jsonl`
  (ids are unique UUIDs, so no need to reproduce Claude's cwd→dir encoding). No transcript ⇒
  launch fresh (`[restore-fresh]` in `hooks.log`) instead of `--resume` (`[resume]`).
- **A ConPTY connection's process spawns on the control's first non-zero layout — never
  eager-`Start()` it before the control initializes.** `TermControl::_InitializeTerminal`
  (gated on `SwapChainPanel().LayoutUpdated`) is what calls `_core.Connection().Start()`, so a
  background/unfocused tab's `claude.exe` doesn't launch until the tab is first shown (hence
  "Restore all" re-opens lazily). Trying to force a background session to run by calling
  `connection.Start()` early from the app layer **crashes the app**: the connection's output
  then reaches `ControlCore`'s output handler before `_core.Initialize()` (gated on that same
  layout) has run → **AV `0xc0000005` in `Microsoft.Terminal.Control.dll`**, on every startup
  restore. The safe way to start a background session is to let its control initialize first
  (select the tab) — which is *why* startup ARCHIVES instead of auto-launching (Rule #6): no
  startup tabs ⇒ nothing to lazily-not-start, and a user Restore opens one focused tab that
  initializes normally.
- **Working-dir comparison must be filesystem-aware** (else the Explorer Tree forks one dir
  into multiple roots). Windows is case-INsensitive (`C:\…\Desktop` == `…\desktop`) and
  treats `/`≡`\`; POSIX is case-SENSITIVE with `\` a literal char. Route every dir
  grouping/scope/match through `PathEq`/`NormPath` (`#ifdef _WIN32` → `CompareStringOrdinal`
  ignoreCase; `#else` → exact) — never raw `==`/`find`.
- **The hook wire's `prompt` field must be escaped — and the PowerShell escape must mirror
  `WireEscape` byte-for-byte.** The forwarder appends the `UserPromptSubmit` prompt as the
  trailing TAB-separated wire field; a real prompt has embedded TAB/newline, which would break
  the single-line, TAB-split, newline-framed record. Both the PowerShell forwarder and
  `BuildWireLine` escape it identically — `\ → \\` FIRST, then tab/CR/LF → `\t \r \n` — and the
  bridge `WireUnescape`s it. The PowerShell side is the one seam the C++ tests can't cover, so
  verify it with a parity check (`C:\src` → `C:\\src`, `https://` survives, no raw newline/tab
  remains). PowerShell gotcha: `-replace '\\','\\'` (pattern is regex = one backslash;
  replacement is literal = two) does the backslash-doubling, and it MUST run before the
  tab/CR/LF replacements or it would double the backslashes they introduce.
- **Path-picker Popup placement.** Parent the `Primitives::Popup` into the content root
  (top/left aligned) so its `Horizontal/VerticalOffset` is root-relative; the popup's own
  layout anchor already carries the root's offset within the XAML island, so the two compose
  to the box's on-screen position. Open it only on `FocusState::Pointer`/`Keyboard` (not
  `Programmatic`) so it doesn't pop on tab activation, and keep it open across row clicks via
  a **deferred** `LostFocus` check (re-focus the box on pick; bail if it regained focus).
- **Erasing a map node invalidates references to its key — copy the key first.** `_BindClaudeSession-
  ToTab`'s re-home block did `for (const auto& [oldId, w] : _claudeTabs) { … _claudeTabs.erase(oldId);
  _claudeOverlays.erase(oldId); }` — `oldId` is a reference INTO the `_claudeTabs` node, so the first
  erase freed it and the second `erase(oldId)` hashed **freed memory** → AV `0xc0000005` in
  `std::_Fnv1a_append_bytes` (`std::hash<wstring>`). Latent for ages (re-home was rare); the Fleet
  Observer's far more frequent correlation/re-home **exposed it**. Fix: `const std::wstring superseded
  = oldId;` BEFORE any erase. (General rule: never use a `[key, val]` structured-binding ref after
  erasing that element.)
- **A `+`-tab claude does NOT inherit our runtime env** (`AM_SESSION` / `CCMGR_HOOK_PIPE` / the PATH
  shim are all absent), so a hand-typed `claude` there fires **zero hooks** and is classified `wt`
  by `AM_SESSION` alone. WT regenerates a tab's child env from the **registry** (`til::env::regenerate`)
  unless `reloadEnvironmentVariables` is genuinely OFF, which drops these runtime-only vars — so the
  "Adopt any `claude`" hook fast-path is **degraded for `+` tabs** (a Manager-Launched session is fine:
  it sets env explicitly via `spec.env`). This is **why the observer keys binding on the roster
  correlation, not `AM_SESSION`** (Rule #13): a claude under one of our tabs' shells is ours
  regardless. The observer + the transcript-tail reconciler give it full detection + state without
  hooks; restoring the hook fast-path for `+` tabs (fixing the env regeneration) is a separate,
  non-blocking follow-up.
- **Symbolize a crash without cdb/WinDbg via the in-box DIA SDK + DbgHelp.** When the app AVs, the
  WER **Application Error** event already gives `Faulting module` + `Exception code` + `Fault offset`
  (the RVA), and a full dump lands in `%LOCALAPPDATA%\CrashDumps\`. No debugger is installed, but
  `msdia140.dll` + the **DIA SDK** ship with VS 2022 (`…\DIA SDK\include\dia2.h`, `…\lib\amd64\
  diaguids.lib`). A ~60-line C++ tool can `NoRegCoCreate(msdia140.dll)` → `loadDataFromPdb(TerminalApp.pdb)`
  → `findSymbolByRVA(rva, SymTagFunction)` + `findLinesByRVA` to map the fault offset to **function +
  source line** (link `diaguids.lib ole32 oleaut32 advapi32`). For the full call chain, `DbgHelp`'s
  `MiniDumpReadDumpStream` (ExceptionStream → faulting thread id; ThreadListStream → its stack memory;
  ModuleListStream → `TerminalApp.dll` base/size) + scanning the stack for in-module return addresses,
  symbolized through the same DIA session, reconstructs the stack top-down. This is how the re-home
  use-after-free above was pinned exactly.

## Correctness rules (do not regress)

1. **"Waiting-for-you" is three states.** A session is *ready* for an auto-send when it is
   **turn-complete** (`Stop` → `WaitingForInput`) **or** sitting **Idle** with no turn in
   progress (a freshly launched / just-resumed plan must START, not wait for a `Stop` it will
   never emit); `Running` / `NeedsApproval` / `Error` / `Done` are never ready. A change-driven
   advance is held to **one prompt per turn** by the pickup guard (don't regress that — it
   prevents a queue-drain). `Notification(permission)` → Approval Policy (NOT the prompt queue).
   A `Stop` whose last message is a question → **Held** by the question-guard.
2. **Tree `Enter` = Activate** (jump to the session's live tab); never forward it to
   `ITerminalConnection::WriteInput` (that would submit a stray carriage return).
3. **Bind queue → sessionId**, never "the selected session" at send time.
4. **Idempotent sends:** mark `Sent` atomically + persist; survive restart without replay.
5. **Backstops:** stop-on-error, maxAutoSends, global pause/kill, pause-on-human-input.
6. **Startup ARCHIVES, never auto-launches; restore = resume, not replay (transcript-gated).**
   On startup, persisted sessions load into the registry as **Archived** (`live=false`) and are
   **NOT** re-launched — the app opens to just the Manager tab, and the prior fleet is restorable
   as a whole from the **Archived** button (a deliberate reversal of the old auto-reopen). A
   user-initiated **Restore** re-launches one: `claude --resume <id>` (same id ⇒ hooks still
   correlate) **only when Claude has a transcript for that id**, else a fresh session (new id,
   same dir + queue, stale archived record dropped). Queues reload with statuses intact. Closing
   a tab **archives** (keeps the record, `live=false`); there is **no discard** — archive is
   terminal, and the Claude transcript on disk is never deleted. Never decide resume from the
   persisted `SessionState` (it's the live post-restore state) — see Gotchas.
7. **State is hook-derived,** never screen-scraped (the Ink TUI repaints constantly).
8. **Same directory = same path, filesystem-aware.** Group/scope/match sessions by working
   dir through `PathEq` (case-insensitive on Windows, case-sensitive on POSIX), so
   case-variant spellings of one directory never fork the Explorer Tree into two roots.
9. **Adopt only on `SessionStart`; bind by `tabToken`, never guess.** A hook for a session we
   didn't Launch creates an `external` record ONLY on `SessionStart` (any other event for an
   unknown id is ignored — there's no connection to bind). Promotion to control correlates the
   `WT_SESSION` `tabToken` to a live ConPTY and binds the injector to THAT id (Rule #3); if no
   connection matches (a claude hosted outside this app), it stays observe-only — never bind to
   "the active tab".
10. **One engine per process (M9); a closing window must detach; records reference, never copy.**
    Exactly ONE `SessionRegistry` / `HooksBridge` / `Scheduler` for the whole process
    (`SharedEngine()`), shared by every window — never re-create them per `TerminalPage`, and
    never key the bridge on anything that collides across windows (the `<pid>` pipe is fine
    *because* there is one bridge). Each window registers its lens observer + adoption handler +
    activate sink by **token** and detaches them on teardown (`RemoveObserver` in
    `~AgentManagerContent`; `RemoveAdoptionHandler` + `UnregisterWindowActivateHandler` in
    `~TerminalPage`); the fleet loads **process-once**
    under the `Engine::restoreMutex` barrier (a second window blocks until it's loaded, then skips) so it can't
    double-insert NOR race its tab re-home against a half-loaded registry. Per-window persisted UI
    state (geometry + lens + ordered tab refs) is the `WindowRecord` (`windows/<id>.json`) and it
    must **reference** sessions by id, never copy them — `sessions.json` + `SessionInfo.live`
    remain the one session source of truth (Option 1).
11. **A session's title is ONE value — Explorer name == tab title == persisted `SessionInfo.title`.**
    `s.title` is the single source of truth; it is **pinned** onto the WT tab at launch / restore /
    adopt (`Tab::SetTabText`, so the tab stops floating with claude's volatile OSC title). Renaming
    from **either** side writes that one value and persists (the autosave-on-change observer): the
    Explorer-tree **Rename…** routes through the `rename` callback → `_RenameClaudeSession` (registry
    + tab); a WT tab rename (double-click / right-click **Rename Tab** / `renameTab` action — all
    funnel through `Tab::SetTabText` → `PropertyChanged("Title")` → `_UpdateTitle`) flows back via
    `_SyncClaudeTitleFromTab`. The sync is **cross-window**: a rename run in a window that does NOT
    host the tab (the board/tree show the whole fleet) writes the registry, and the hosting
    window's registry observer re-pins its tab (`_SyncClaudeTabTitleFromRegistry`, riding the
    tab-dot push). Equality guards make an already-in-step sync a no-op (no loops); an
    emptied override (`ResetTabText`) re-pins. On **adopt**, a name the user already gave the `+` tab
    wins (mirrored into the registry); else the tab is pinned to the managed name. Don't reintroduce
    a separate tab title or scrape claude's OSC title for the name.
12. **A tab's color is ONE value per working directory.** Every Claude tab in a dir shares one
    color, persisted to `dir-colors.json` (keyed by `NormDirKey` — slash/case/trailing-normalized).
    On launch/restore/adopt `_ApplyDirColorToTab` paints the tab from the dir's persisted color, or
    a stable auto palette color (hash of the dir; persisted so it survives). A user color change
    (`Tab::SetRuntimeTabColor`/`Reset` → `TabColorChanged` → `_OnClaudeTabColorChanged`) **persists
    it for the dir AND recolors every live tab in that dir** (filesystem-aware, Rule #8); a reset
    drops the entry. A de-dupe vs the persisted color makes our own launch/propagation writes
    no-ops — don't regress that, it is what keeps propagation from looping. (The default *name*,
    `DeriveSessionTitle`, only seeds an *untitled* session — a real/renamed title wins, Rule #11.)
13. **Fleet Observer: rostered == ours; correlate by `WT_SESSION`, never guess; provenance: push
    wins for state.** A claude correlated to a tab in OUR published roster (its shell == the ConPTY
    root of one of our tabs) is running in one of our windows, so it is **OURS** (`RunningApp::
    Agentmaster`) and bindable **regardless of `AM_SESSION`** — a hand-typed `+`-tab claude does NOT
    reliably inherit `AM_SESSION` (WT regenerates a tab's env; see Gotchas), so the **roster
    correlation is the authoritative bind signal**, and `AM_SESSION` is only the secondary signal
    for the census of NON-rostered claudes (a background daemon we spawned, or telling our instance
    from another's / a real WindowsTerminal). Correlation keys on the **exact `WT_SESSION`** (==
    `ITerminalConnection::SessionId()`) — never PPID ancestry, never the stale tab cwd, never "the
    active tab". `ObserveClaude` **enriches facts but NEVER sets `SessionState`** — push hooks + the
    transcript tail own state (Rule #1/#7); a hooked and an un-hooked claude converge on the same
    record. The observer **only reads** (PEB / Toolhelp / filesystem) — it must NEVER write to a
    shell's stdin (the invisibility invariant). **Adopting** an external (the Explorer-Tree
    EXTERNAL scope's *Adopt*) upholds this — it never injects into / kills the foreign process; it
    resumes the external's *conversation* into a NEW managed tab (`claude --resume <id>`, id from
    `ResolveSessionId`, transcript-gated → fresh if none) and leaves the original running. The
    transient enrichment fields are **never persisted** (`Persistence.cpp`); the observer
    re-derives them each run.
14. **Conversation identity is by transcript CREATION time ≈ process start, not newest-mtime.** A
    claude's OWN transcript is created when it first writes — at/after its process start.
    `ResolveSessionId` (for a bare claude with no explicit `--session-id`/`--resume <guid>`) picks
    the cwd's transcript whose **creation time is closest to (and not significantly before) the
    claude's start**, and REJECTS transcripts created before it started. This is what keeps **two
    claudes sharing one cwd** bound to their OWN conversations (newest-mtime collapses them onto
    whichever is momentarily most active), and resolves a **never-prompted** claude to `""` (no card
    until its first prompt — §11d) rather than collapsing it onto a stale leftover transcript. An
    explicit `--session-id` / `--resume <guid>` is authoritative and wins (collision-free, known
    before the transcript exists).
15. **One profile per install, resolved ONCE, before ANY state read; everything persists inside
    it.** The WindowEmperor resolves the profile (and shows the first-launch picker) **after**
    winning the single-instance handoff and **before** the first settings/state read — never show
    the picker from a handed-off process or a `-Embedding` (defterm) activation, and never read or
    write persisted state (engine files, Terminal settings, window records, the reopen scan)
    through any path that isn't `AgentmasterStateDir()` / the `AGENTMASTER_PROFILE`-redirected
    `GetBaseSettingsPath()`. The resolution is cached for the process lifetime — a profile change
    (the cog's Change…) applies on restart only; never re-home state mid-run. The ONLY data outside
    a profile: the `.agentmaster.profiles` choice file + the env override (the pointer must live
    outside what it points at) and the fixed `~/.agentmaster/locks/` build mutex (it guards the ONE
    build tree, not a profile). Library-side resolution (`ProfileBootstrap::ResolveProfileDir`)
    must never show UI — headless hosts (tests, tools) silently land on the per-identity default,
    which for unpackaged runs is the historical `~/.agentmaster` (don't regress that — it's what
    keeps the test harness and old tooling stable). Generated artifacts that embed paths or live
    endpoints (the shim, hooks-settings, the forwarder's `bridge.json` discovery) must be derived
    from the ACTIVE profile at engine init — a fixed `~/.agentmaster` literal in generated content
    is a cross-instance routing bug (the forwarder had exactly that).

## Conventions

- Mark our additions with `Agentmaster`. Keep the upstream MIT `LICENSE`/`NOTICE`.
- Keep the diff against upstream minimal where practical (additive files, small touches at
  integration points) so rebasing onto `microsoft/terminal` stays cheap.
- Build artifacts (`bin/`, `packages/`, `Generated Files/`) are gitignored — never commit them.
