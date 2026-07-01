# HANDOVER — The Tab-Strip Tooltip (rich, session-aware hover card)

> Everything about the **tab tooltip** mechanism: what it is, where the code lives, the full
> open/close/refresh lifecycle, the content builders, the off-thread summary loader, the XAML-Islands
> gotchas, and — most importantly — the **crash history** (6 distinct crashes, all in this one
> feature) with each root cause and the fix that shipped for it. If you touch the tab tooltip, read
> the **Crash history** and **Invariants** sections FIRST.

Author's note: this is the single most crash-prone surface in the fork. It manually drives a XAML
`ToolTip` under XAML Islands, and that combination is inherently fragile (see *Why this is hard*).
Every "obvious" simplification here has a landmine; the comments in the code are load-bearing.

---

## 0. Scope & what this is NOT

- **This doc = the TAB-STRIP tooltip**: the hover card that appears when you hover a **tab header**
  in the tab strip. For a managed Claude/Codex session it's a rich, dark, summary-style card; for a
  shell/pwsh/cmd/Manager tab it's the plain title+key-chord tooltip.
- **NOT the per-tab overlay** (the in-terminal HUD badge + the pencil-toggled summary panel that
  live *inside* the terminal pane). That is `AgentTabOverlay` — see
  [`TAB_OVERLAY.md`](TAB_OVERLAY.md). The two are **separate mechanisms** that happen to show
  overlapping session facts. Don't conflate them.
- **NOT `AgentTipHelpers.h`** — that's the shared hover-tooltip recipe for the Manager tab's cards
  and the Sessions/Archive pages (page elements). The tab tooltip is its own code on `Tab` because
  its owner is a MUX-virtualized `TabViewItem` (which is exactly why it's more fragile — see
  *Crash history*). `AgentTipHelpers` is a useful reference for the same class of guards, though.

---

## 1. The two tab tooltips

Every tab header can carry ONE of two tooltips, mutually exclusive, gated by `Tab::_agentToolTipActive`:

| | **Default tooltip** | **Rich agent tooltip** |
|---|---|---|
| Shown on | shell/pwsh/cmd tabs, the Manager tab, an agent tab before its first push | a **managed** (bound) Claude/Codex session tab |
| Content | title + key-chord (`TextBlock`) | dark summary card (state dot, title, folder/branch, state line, model·effort·mode, divider, transcript Summary body) |
| Built in | `Tab::_UpdateToolTip` (Tab.cpp) | `TerminalPage::_UpdateTabAgentToolTip` builds it, `Tab::SetAgentToolTip` hosts it |
| Open/close | **framework-managed** (`ToolTipService` auto opens/closes on hover) | **manually driven** (our timers + `IsOpen`) |
| Crashes? | **never** (framework owns the lifetime) | **the entire crash history below** |

There is also a **third, degenerate variant** — the **observe badge tooltip** (`TtBuildObserveCard`):
a tiny `○ <kind> · unlinked` card shown on a non-managed but observed tab (pwsh / cmd / an
unprompted claude / external codex). It is pushed via `SetAgentToolTip` too (so it rides the same
manually-driven lifecycle), by `_SetTabActivityBadge`.

**Key insight that recurs in every crash:** the default tooltip is framework-managed and never
crashes; the rich tooltip is manually driven and does. The manual driving exists for real reasons
(fast open, reliable close under Islands — see *Why this is hard*), but it is the crash source.

---

## 2. Architecture — who owns what

```
TerminalPage (owns SessionInfo + the registry)            Tab (owns the ToolTip XAML lifecycle)
--------------------------------------------------        ---------------------------------------
_UpdateTabAgentToolTip(tab, sessionId)                    SetAgentToolTip(UIElement content, hstring sig)
  reads SessionInfo -> formats STRINGS (TtSpan/…)           stores content+sig, _agentToolTipActive=true
  builds the card element (TtBuildTooltipCard)               -> _UpdateToolTip -> _UpdateAgentToolTip
  computes a content SIGNATURE (skip if unchanged)         _UpdateAgentToolTip: create/host on the reused ToolTip
  impl->SetAgentToolTip(card, sig) ----------------------->  _WireAgentToolTipHover: open/close on hover
  kicks _EnsureTabTooltipSummary (off-thread body)         ClearAgentToolTip: revert to the default tooltip
```

**Split of responsibility (do not blur this):**
- **TerminalPage side** (`TerminalPage.AgentObserver.cpp`) owns the *content*: it has the
  `SessionInfo`, formats the strings, builds the XAML card, computes the change signature, and runs
  the off-thread transcript summary load. It knows nothing about *when* the popup opens.
- **Tab side** (`Tab.cpp`) owns the *lifecycle*: hosting the element, the hover open/close timers,
  and the popup's create/open/close/drop. It knows nothing about the session; it just hosts whatever
  element it's handed and toggles visibility.

This split is why a content change is a cheap "push a new element + signature," and why all the
crashiness lives on the Tab side (the lifecycle), not the TerminalPage side (the content).

---

## 3. File & symbol map

**`src/cascadia/TerminalApp/Tab.cpp` / `Tab.h`** — the lifecycle (all `Agentmaster`-marked):
- Members (`Tab.h` ~243–259):
  - `bool _agentToolTipActive` — is the rich tooltip active (vs the default)?
  - `winrt::…UIElement _agentToolTipContent` — the card element last handed to us by the page.
  - `winrt::hstring _agentToolTipSig` — the content fingerprint (skip re-host if unchanged).
  - `winrt::…Controls::ToolTip _agentToolTip` — the ToolTip object. **Created fresh per open,
    nulled on close** (post-`1d1edbe18`; see Crash history). `{ nullptr }` when none.
  - `winrt::…DispatcherTimer _agentToolTipOpenTimer` — one-shot fast-open timer (~1/3 system hover).
  - `winrt::…DispatcherTimer _agentToolTipDismissTimer` — one-shot 8s auto-dismiss backstop.
  - `bool _agentToolTipHoverWired` — the hover handlers are wired ONCE per tab; this guards that.
- File-scope (Tab.cpp, anonymous namespace, ~34):
  - `std::atomic<uint64_t> g_lastAgentToolTipCloseTick` — the **cross-tab reopen-cooldown** stamp
    (post-`9c026822d`). Atomic because each window runs on its own UI thread.
  - `constexpr uint64_t kAgentToolTipReopenCooldownMs = 150`.
- Methods:
  - `_UpdateToolTip()` — the entry that decides default vs rich; builds the default tooltip inline.
  - `SetAgentToolTip(content, sig)` — public; the page calls this. Stores + `_UpdateToolTip`.
  - `ClearAgentToolTip()` — public; revert to the default tooltip (session gone/archived).
  - `_UpdateAgentToolTip()` — create (if null) + host the card on the reused ToolTip.
  - `_WireAgentToolTipHover()` — wire (once) the open/dismiss timers + pointer handlers on the tvi.
  - `_SafeSetAgentToolTipOpen(bool)` — `_agentToolTip.IsOpen(open)` inside try/catch.
  - `_ForceCloseAgentToolTip()` — stop timers + close + **detach + drop** + stamp cooldown.
  - `_ArmAgentToolTipDismiss()` — (re)start the 8s dismiss backstop (Stop+Start).

**`src/cascadia/TerminalApp/TerminalPage.AgentObserver.cpp`** — the content:
- TU-local string helpers (anon namespace ~81–195): `TtNowMs`, `TtSpan`, `TtStateLabel`, `TtJoin`,
  `TtPermIsBypass`.
- TU-local element builders (~197–411): `TtFill` (brush), `TtBuildSummaryBody` (text → monospace
  TextBlocks + `\x1F` → full-width `Border` rules), `TtBuildTooltipCard` (the rich card),
  `TtBuildObserveCard` (the `○ … unlinked` twin).
- `_UpdateTabAgentToolTip(tab, sessionId)` (~649) — the orchestrator: reads `SessionInfo`, formats,
  builds, signs, `SetAgentToolTip`s, kicks the off-thread body loader.
- `_EnsureTabTooltipSummary(...)` (~787) — `fire_and_forget`: off-thread resolve+stat+analyze of the
  transcript → render the Summary box → cache → re-host.
- `_SetTabActivityBadge(tab, wt, kind)` (~2295) — pushes the observe-badge tooltip on a non-managed tab.

**`src/cascadia/TerminalApp/TerminalPage.h`** — the content cache (~730–744):
- `struct _AgentTooltipSummary { std::wstring path; int64_t mtime; int64_t lastCheckMs; winrt::hstring body; }`
- `std::unordered_map<std::wstring,_AgentTooltipSummary> _tabTooltipSummary` — per-session Summary cache.
- `std::unordered_set<std::wstring> _tabTooltipSummaryInFlight` — one background load per session.
- `std::unordered_map<std::wstring,std::wstring> _tabTooltipSig` — last signature per session.
- `winrt::fire_and_forget _EnsureTabTooltipSummary(...)`.

---

## 4. The lifecycle, end to end (CURRENT behavior, post all 6 fixes)

### 4a. Content push (page → tab)
1. Something (bind / sweep / activity — see §5) calls `_UpdateTabAgentToolTip(tab, sessionId)`.
2. It null/registry/impl-guards, then: **if the session is gone or `!live`** → `impl->ClearAgentToolTip()`
   + drop the cached Summary/sig, return. (This is how an archived tab reverts to the default tooltip.)
3. Else it formats the header strings from `SessionInfo` (state + "ago" via `TtSpan`; why-needs-you;
   `⚠ unread`; `claude/codex · model · effort · mode`; folder/branch), pulls the cached Summary body
   (may be empty on first sight), and computes a `sig` (state+title+folderBranch+meta+bodyMtime).
4. If `sig` changed since last push → `impl->SetAgentToolTip(TtBuildTooltipCard(...), sig)`.
5. It then throttle-kicks `_EnsureTabTooltipSummary` (≤ every 4s per session, mtime-gated) to load /
   refresh the Summary body off-thread; on completion that re-calls `_UpdateTabAgentToolTip` (the body
   now differs → new sig → re-host).

### 4b. Host (`SetAgentToolTip` → `_UpdateToolTip` → `_UpdateAgentToolTip`)
- `SetAgentToolTip` returns early if `sig` matches the last one (no XAML churn). Else stores
  content+sig, sets `_agentToolTipActive=true`, calls `_UpdateToolTip`.
- `_UpdateToolTip`: `_agentToolTipActive` → `_UpdateAgentToolTip`; else build the default tooltip.
- `_UpdateAgentToolTip`:
  1. **Owner guard**: if the `TabViewItem` is null / `!IsLoaded()` / no `XamlRoot()` → return (never
     build/mutate for a detached owner).
  2. If `!_agentToolTip` → **create** a fresh `ToolTip`: `RequestedTheme(Dark)`,
     `Placement(Bottom)`, `IsHitTestVisible(false)`, `ToolTipService::SetToolTip(tvi, it)`.
  3. `_WireAgentToolTipHover()` (no-op after the first time).
  4. If the tip **IsOpen** → return (do NOT swap `Content` while you're reading it).
  5. `_agentToolTip.Content(_agentToolTipContent)`.

### 4c. Hover open/close (`_WireAgentToolTipHover`, wired once per tab)
- **openTimer** (one-shot, ~1/3 the system hover time, e.g. ~133ms). On tick:
  1. `Stop()` (one-shot). Guard `!_agentToolTipActive` → return.
  2. Owner guard (`IsLoaded()`+`XamlRoot()`) → return.
  3. **Cooldown guard** (§4e): if an agent tooltip closed within 150ms → re-arm the one-shot + return.
  4. `_UpdateAgentToolTip()` (creates a FRESH tooltip since it's null after the last close) →
     `_SafeSetAgentToolTipOpen(true)` → `_ArmAgentToolTipDismiss()`.
- **dismissTimer** (one-shot, 8s). On tick → `_ForceCloseAgentToolTip()` (close + drop).
- **PointerEntered**: guard `!_agentToolTipActive`; start openTimer if idle.
- **PointerMoved**: guard `!_agentToolTipActive`; if `_agentToolTip && IsOpen()` → re-arm dismiss
  (keep-alive while genuinely hovering); else start openTimer (re-open after a stationary dismiss / a
  missed enter). The `_agentToolTip &&` null-check matters — it's null after a close (drop-on-close).
- **PointerExited / PointerCanceled / PointerCaptureLost** (shared `closeHandler`) → `_ForceCloseAgentToolTip()`.
- **TabViewItem().Unloaded** → `_ForceCloseAgentToolTip()`.

### 4d. Close = drop (`_ForceCloseAgentToolTip`) — the crux of the whole design
Runs from EVERY close path (the 3 pointer-loss handlers, the dismiss backstop, Unloaded, and
`Tab::Shutdown`). It:
1. Captures `hadAgentToolTip = (bool)_agentToolTip`.
2. Stops both timers.
3. `_SafeSetAgentToolTipOpen(false)` (close the popup while the peer is still valid).
4. `ToolTipService::SetToolTip(tvi, nullptr)` (**detach** from the owner).
5. `_agentToolTip = nullptr` (**drop** our strong ref).
6. If `hadAgentToolTip` → `g_lastAgentToolTipCloseTick = GetTickCount64()` (start the cooldown).

The **drop** (steps 4–5) is the entire point: after a close there is no reused object for the next
open (or a deferred framework pass) to touch. The next open creates a brand-new one.

### 4e. Cross-tab reopen cooldown (`g_lastAgentToolTipCloseTick`, post-`9c026822d`)
Jumping the cursor between tab A and tab B opens B's tooltip while A's popup is still in the
framework's **async teardown**. Two agent popups colliding in one render/composition pass is what
fail-faults. So: any close that tore down a real tooltip stamps the shared timestamp; the open-tick
**defers** (re-arms) while a close is within 150ms. A settled hover with no recent close opens
immediately (fast path preserved). A bare pointer-exit with no open tooltip does **not** stamp (so
sweeping the cursor across the strip never throttles).

---

## 5. Who calls what (invocation points)

`_UpdateTabAgentToolTip(tab, sessionId)` is called from (all `TerminalPage.AgentObserver.cpp`):
1. **The registry/observer refresh** (~645) — the per-change + per-tick path that repaints the tab dot
   also refreshes the tooltip (and reverts it to default when the session went `!live`).
2. **`_EnsureTabTooltipSummary` completion** (~883) — after the off-thread Summary body loads, re-host
   with the fuller card.
3. **On bind** (`_BindClaudeSessionToTab`, ~2613) — replace any `○ … unlinked` observe tooltip with the
   rich managed one the instant a session binds.
4. **The liveness sweep** (`_SweepClaudeLivenessImpl`, ~2673) — the ~2s tick refreshes each hosted
   tab's tooltip (the "ago" line ticking + reverting dead tabs).

`SetAgentToolTip` is called only from `_UpdateTabAgentToolTip` (rich card, ~766) and
`_SetTabActivityBadge` (observe card, ~2315). `ClearAgentToolTip` from `_UpdateTabAgentToolTip`'s
gone/`!live` branch (~663) and the liveness sweep's dead-tab branch (~2791).

**Threading:** every one of these runs on the window's UI thread. The observer/registry callbacks
that originate off-thread marshal onto the UI thread (`resume_foreground` / `DispatcherQueue`) BEFORE
touching the tooltip. `SetAgentToolTip`/`_UpdateAgentToolTip` assert `ASSERT_UI_THREAD()`. Never call
tooltip code off the UI thread.

---

## 6. The content builders (`Tt*`)

- `TtBuildTooltipCard(accent, title, folderBranch, stateText, metaText, bodyText)` → a dark `Border`
  (bg `#FF202020`, 1px `#40FFFFFF` border, corner radius 4, padding 10/8, **MaxWidth 460**) wrapping a
  vertical `StackPanel`:
  - **Header** (a 2-col `Grid`): left = a 9px state-colored `Ellipse` (`accent`, 1px black stroke) +
    the title (Cascadia Mono 13, semibold, ellipsized, NoWrap); right = `folderBranch` (Cascadia Mono
    11, dim `#B0B0B0`, ellipsized).
  - **State line** (Cascadia Mono 11, colored `accent`): `<state> · <ago> · <why> · ⚠ unread`.
  - **Meta line** (Cascadia Mono 11, dim): `claude|codex · model · effort · <mode/⚡ bypass>`.
  - **Divider** (full-width 1px `Border`) + a **ScrollViewer** (MaxHeight 360) hosting the Summary body.
- `TtBuildSummaryBody(text)` → a `StackPanel` of Cascadia Mono `TextBlock`s; the sentinel line
  `\x1F` becomes a full-width `Border` rule (the same convention the overlay's summary panel uses).
  The text is the `RenderSessionSummaryBox(..., full=false)` output (numbered messages + files; no
  header, since the card already shows state/title/dir).
- `TtBuildObserveCard(kind, title)` → the `○ <kind> · unlinked` line over the (optional) tab title.
- `accent` = `AgentStatusColorFor(state)` from `AgentStatusColors.h` (the ONE shared state→color table).

The card is **rebuilt fresh on every content change** (never mutated in place / cached as an element —
the cache holds only STRINGS). This matters: a XAML element has one parent, so a shared/reused element
set as `Content` on two ToolTips would corrupt. Keep builders returning fresh trees.

---

## 7. The off-thread Summary loader (`_EnsureTabTooltipSummary`)

- `fire_and_forget`, UI → worker → UI. Resolves the transcript path, `GetFileAttributesExW` for the
  mtime, and — only when the mtime grew (or first sight) — `AnalyzeSessionTranscript` +
  `RenderSessionSummaryBox` (Claude) / `ReadCodexRolloutInfo` + `RenderCodexSummaryBox` (Codex) on the
  **worker thread**, then marshals back and stores into `_tabTooltipSummary[id]` and re-calls
  `_UpdateTabAgentToolTip`.
- **mtime-gated** (a quiet tab costs one stat) + **in-flight-guarded** (`_tabTooltipSummaryInFlight`,
  one load per session) + **throttled** by the caller (`lastCheckMs`, ≤ every 4s).
- **Exception containment (commit `be1d63b7f`) — do not remove:** an exception escaping a
  `fire_and_forget` calls `std::terminate()`. The body has an INNER try/catch around the background
  analyze/render (contained on the worker thread so the UI cleanup still runs + the in-flight flag is
  always cleared) and an OUTER try/catch as the terminate-net for a `resume_foreground` teardown-race
  or a `_UpdateTabAgentToolTip` XAML-build throw. `AnalyzeSessionTranscript` self-contains, but the
  `Render*Box`/`ReadCodexRolloutInfo` do NOT — hence the net.

The same terminate-net discipline applies to the **five sibling observer `fire_and_forget`s**
(`_SweepClaudeLiveness`, `_ReconcileClaudeTabs`, `_ObserverProbe`, `_ScanPendingInput`,
`_RefreshObserverData`) — each is a thin shell that `co_await`s an `IAsyncAction` impl inside
try/catch (commit `9792161b8`), because co-awaiting an `IAsyncAction` propagates exceptions to the
awaiter instead of terminating.

---

## 8. Why this is hard (the fundamental fragility)

Under **XAML Islands**, a `ToolTip`:
1. **Doesn't inherit the host theme** — it renders in the popup root, so it must be pinned
   `RequestedTheme(Dark)` explicitly (`ToolTipService` theme propagation is unreliable here). This is
   why both the default and rich tooltips set Dark directly.
2. **Framework auto-dismiss is unreliable** — a tip opened on hover routinely OUTLIVES the pointer
   leaving. That's the whole reason for the **manual driving** (our open/dismiss timers + `IsOpen`),
   and the manual driving is the crash source. The plain default tooltip accepts the framework's
   (working-enough) behavior and never crashes; the rich one wanted fast-open + reliable-close and
   pays for it.
3. **Is dual-driven** — attaching via `ToolTipService::SetToolTip` AND manually calling `IsOpen`
   means both the framework and our code toggle the popup. `IsHitTestVisible(false)` stops the popup
   stealing pointer events, but the dual-drive + rapid hover is the deep race behind the later crashes.
4. **Owner is a MUX `TabViewItem`** — MUX `TabView` **virtualizes/recycles** its item containers, so
   the owner (and the ToolTip's native peer) can be torn down out from under our still-valid WinRT
   strong ref → a "zombie" peer. This is what makes the tab tooltip far more crash-prone than the
   page tooltips `AgentTipHelpers` drives.
5. **Fail-fasts + AVs bypass `try/catch`** — a XAML stowed exception (`0xC000027B`) is a
   `RaiseFailFastException`; a CFG violation (`0xC0000409` subcode `0xA`) and an access violation
   (`0xC0000005`) are not C++ exceptions under `/EHsc`. So `_SafeSetAgentToolTipOpen`'s try/catch
   catches the *hresult_error* cousins but NOT the fail-fast/AV. And several of these fire in a
   **deferred framework render/input pass** — OFF our call stack — where NO guard we add can catch
   them. The only defense against those is to never leave the popup in a state that pass chokes on.

---

## 9. CRASH HISTORY (read this before changing anything)

All six were the rich tab tooltip. Diagnosed from WER Event 1000 + full crash dumps in
`%LOCALAPPDATA%\CrashDumps\`, walked with the repo's dump tools (§11) — there is no cdb/WinDbg on the
build box.

| # | Commit | Exception | Trigger | Root cause | Fix |
|---|--------|-----------|---------|-----------|-----|
| 1 | `918b720b7` | `0xC000027B` stowed fail-fast in **Windows.UI.Xaml** | tooltip fade / mouse-exit | manual `IsOpen` on an owner-less/re-entrant tooltip | route every `IsOpen` through `_SafeSetAgentToolTipOpen` (try/catch) + `IsHitTestVisible(false)` |
| 2 | `44beaea7e` | `0xC0000005` **READ @0x0** in **Microsoft.UI.Xaml** (MUX), deferred/off-stack | owner `TabViewItem` recycled during a multi-tab restore | orphaned open popup touched by MUX's deferred input/placement pass | force-close on `TabViewItem.Unloaded` + `Shutdown`; guard the open-tick on `IsLoaded()`+`XamlRoot()` |
| 3 | `be1d63b7f` | (potential) `std::terminate` | any throw from the off-thread Summary load | `_EnsureTabTooltipSummary` `fire_and_forget` had no exception containment | inner (bg) + outer (terminate-net) try/catch |
| 4 | `6601c7532` | `0xC0000005` **READ freed** (`Rcx=0xDDDD…`) in put_Content, from the sweep · AND `0xC0000409` CFG (`subcode 0xA`) in put_IsOpen, from the open-tick | owner recycled, then the tab **reloaded** | the reused `_agentToolTip` was kept after the peer was torn down → **zombie** reuse (owner-guard didn't help: the owner had recovered) | on close-for-unload, **detach + null** `_agentToolTip` (don't keep the zombie) |
| 5 | `1d1edbe18` | `0xC0000005` **READ freed** (`Rax=0xDDDD…`) in put_IsOpen, from the open-tick | **rapid tab↔tab tooltip swapping** (no unload) | reusing ONE object across an open/close cycle: `IsOpen(false)` starts async teardown, a rapid re-hover's `IsOpen(true)` races it | **create-fresh-on-open + drop-on-close** — never reuse across a close (all close paths → `_ForceCloseAgentToolTip`; the open-tick recreates) |
| 6 | `9c026822d` | `0xC000027B` stowed fail-fast in a **render/composition pass** (GPU driver on the stack), deferred/off-stack, **zero of our frames** | jumping the cursor between the focused tab and a just-activated (dormant) tab | **cross-tab** race: opening B's fresh tooltip while A's popup is still in async teardown → two agent popups collide in one render pass | **cross-tab reopen cooldown** (`g_lastAgentToolTipCloseTick`, 150ms) — defer B's open until A's teardown drains |

**Patterns to internalize:**
- Every crash reduces to *"a method (`IsOpen`/`Content`) reached a torn-down / racing ToolTip peer."*
  The `0xDDDD…` register value is the MSVC debug-CRT freed-block fill — a dead giveaway of
  use-after-free; look for it in the faulting registers (`dumpexc`).
- The fixes progressed: guard the call → guard the owner → drop the zombie on unload → never reuse
  across close → serialize across tabs. Each closed a specific window; the *class* is the manual-open
  reused-ToolTip pattern under Islands.
- **`try/catch` is not a fix here.** Two of the six (2 and 6) fire in a deferred pass off our stack;
  fail-fasts and AVs aren't caught by `catch(...)` under `/EHsc` anyway. The real fixes are all
  *structural* (never leave a bad popup for a later pass to touch).

**Current status:** #6's cooldown is **MODERATE confidence, not proven** — the fail-fast is deferred
and off-stack, so 150ms *serializes/shrinks* the collision window rather than provably eliminating it.
If a 7th appears, do NOT add a 7th point-patch — go to the fallback (§12).

---

## 10. Invariants — DO NOT REGRESS

1. **UI thread only.** All tooltip code runs on the window's UI thread; off-thread callers marshal
   first. Keep the `ASSERT_UI_THREAD()`s.
2. **Never reuse a ToolTip across an open/close cycle.** Every close path goes through
   `_ForceCloseAgentToolTip` (close + detach + **null**); every open recreates fresh via
   `_UpdateAgentToolTip`. Do not "optimize" by keeping `_agentToolTip` alive across a close — that is
   exactly crash #5.
3. **Detach on drop.** When you null `_agentToolTip`, also `SetToolTip(tvi, nullptr)` — otherwise the
   owner's attached property still points at the (soon-zombie) object and the framework's own
   auto-open can fire it (crash #4/#6 territory).
4. **Never build/mutate the tooltip for a detached owner.** Keep the `IsLoaded()`+`XamlRoot()` guard
   at the top of `_UpdateAgentToolTip` and in the open-tick.
5. **Don't swap `Content` while open.** Keep the `if (_agentToolTip.IsOpen()) return;` before
   `.Content(...)` — swapping under the pointer flickers and races.
6. **Keep the cross-tab cooldown.** Any close that tore down a real popup stamps
   `g_lastAgentToolTipCloseTick`; the open-tick defers within `kAgentToolTipReopenCooldownMs`. It's
   atomic (per-window UI threads).
7. **`fire_and_forget`s must contain exceptions.** `_EnsureTabTooltipSummary` (and the 5 observer
   lanes) must keep their terminate-nets. An escaping exception = `std::terminate`.
8. **Builders return FRESH element trees.** Never cache/share a XAML element as `Content` across
   ToolTips; the cache holds strings only.
9. **The signature gate is a correctness feature, not just perf.** `SetAgentToolTip` no-ops on an
   unchanged `sig`, so the ~2s sweep + per-change observer + bind can all re-assert without XAML churn.
   Keep the sig covering everything the card renders (state/title/folderBranch/meta/bodyMtime).
10. **One title value.** The tooltip title comes from `SessionInfo.title` (Rule #11 elsewhere); the
    tooltip never renames anything. Read-only.

---

## 11. Diagnostics — how these were found (no cdb/WinDbg on the box)

There is a **`debug-dumps` skill** (commit `bf0ba663f`) and standalone tools under `tools/`:
- **`dumpexc.exe <dmp>`** — the exception record + params + **faulting registers**. This is where you
  see `0xDDDD…` (freed) / the fastfail subcode / the AV data address. START HERE.
- **`dumpourscan.exe <dmp> <bindir>`** — scans the faulting thread's stack for OUR-module
  (TerminalApp.dll) return addresses, symbolized via DIA against the matching PDB, + a module
  histogram + exception params. This is a *scan* (includes stale frames) — corroborate ordering.
- **`dumpwalk2.exe <dmp> <bindir>`** — DbgHelp `StackWalkEx` ordered walk (breaks when unwind info is
  missing, e.g. deep in MUX/composition — expect `<no-sym>` there).
- **`hangwalk.exe <dmp> <imageDir> <pdbDir> <tid|ALL> SCAN`** — per-thread scan for a hang dump.
- Build them with `tools/_build_diag.bat` / `_build_ourscan.bat` / `_build_walk.bat` (DIA SDK +
  `dbghelp.lib`). On Git Bash prefix Windows-path args with `MSYS_NO_PATHCONV=1`.

**Workflow that worked every time:** WER Event 1000 (module + exception code + offset + package) →
`dumpexc` (registers → is it freed `0xDDDD`? a fastfail? which subcode? read vs write?) →
`dumpourscan` (which of our frames + the module histogram → is it OUR stack or a deferred framework
pass?). A histogram dominated by `Windows.UI.Xaml`/`Microsoft.UI.Xaml`/`CoreMessaging`/GPU with
**zero of our frames** == a deferred/off-stack fail-fast (uncatchable; needs a structural fix).

**Crash-vs-fix timeline check (critical):** always compare the crashed `TerminalApp.dll` build time
(WER gives the faulting path; `ls` the DLL) against the fix commit time. Several times a crash was on
a binary that PREDATED the relevant fix (moot), and once (crash #6) it POSTDATED it (proving the fix
insufficient). Don't diagnose without this check.

---

## 12. The fallback if it crashes AGAIN

If a 7th distinct crash appears, stop point-patching and do ONE of:
- **Framework-managed rich tooltip (recommended):** keep the rich card content but let
  `ToolTipService` own open/close — remove the manual `IsOpen` driving, the open/dismiss timers, and
  the cooldown. This is the SAME mechanism as the plain default tooltip, which has NEVER crashed.
  Cost: opens at the system hover delay (~0.5–1s, no `InitialShowDelay` in this SDK) instead of
  instantly, and the original "sticky tooltip under Islands" annoyance may return (cosmetic, not a
  crash). Content refresh: set `toolTip.Content(newCard)` only when `!toolTip.IsOpen()`.
- **Off-switch:** a setting to disable the rich tab tooltip and fall back to the default title
  tooltip (robust). Loses the rich card unless re-enabled.

The user, as of this handover, chose to keep the fast manual tooltip (hence commit `9c026822d`), with
the framework-managed path held as the escape hatch.

---

## 13. Quick reference

- Rich card built: `TerminalPage::_UpdateTabAgentToolTip` (`TerminalPage.AgentObserver.cpp`).
- Hosted + lifecycle: `Tab::SetAgentToolTip` / `_UpdateAgentToolTip` / `_WireAgentToolTipHover` /
  `_ForceCloseAgentToolTip` (`Tab.cpp`).
- The one function that fixes reuse: `_ForceCloseAgentToolTip` (close + detach + null + stamp).
- The one guard that keeps the fast path: the cooldown check in the openTimer tick.
- Off-thread body: `_EnsureTabTooltipSummary` + `_tabTooltipSummary`/`_tabTooltipSig`/`_tabTooltipSummaryInFlight`.
- Shared palette: `AgentStatusColorFor` (`AgentStatusColors.h`).
- Commits: `918b720b7`, `44beaea7e`, `be1d63b7f`, `9792161b8` (observer nets), `6601c7532`,
  `1d1edbe18`, `9c026822d`.
- Diagnostics: the `debug-dumps` skill + `tools/dumpexc|dumpourscan|dumpwalk2|hangwalk`.
