# HANDOVER — The Tab-Strip Tooltip (rich, session-aware hover card)

> Everything about the **tab tooltip** mechanism: what it is, where the code lives, the full
> open/close/refresh lifecycle, the content builders, the off-thread summary loader, the XAML-Islands
> gotchas, and — most importantly — the **crash history** (6 distinct crashes, all in this one
> feature) with each root cause and the fix that shipped for it. If you touch the tab tooltip, read
> the **Crash history** and **Invariants** sections FIRST.

Author's note: this WAS the single most crash-prone surface in the fork — seven distinct crashes. The
resolution came in TWO parts (§9): **(1)** 2026-07-01 — the hand-rolled **manual-open** path (fast-open/
dismiss timers + driving `IsOpen` + a cross-tab cooldown) was removed; the rich tooltip is now
**framework-managed** (§4), the SAME lifetime model as the plain default tab tooltip. That killed the
use-after-free class. **(2)** 2026-07-02 — a full-memory dump's decoded stowed backtrace (crash #7, the
first PROVEN stow stack) showed the `0xC000027B` fail-fasts originate in the **card content's
`ScrollViewer`** (DirectManipulation activation on popup-open), independent of who drives open/close — so
the ScrollViewer was removed too (line truncation + a clipping Grid). The history (§9) is kept because it
explains WHY the design is what it is and what NOT to reintroduce; the comments in the code remain
load-bearing.

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
| Open/close | **framework-managed** (`ToolTipService` auto opens/closes on hover) | **framework-managed** now too (was manually driven — timers + `IsOpen`; that was the crash source, §9) |
| Crashes? | **never** (framework owns the lifetime) | the crash history below is the OLD manual path; the current path shares the default's crash-free lifetime model |

There is also a **third, degenerate variant** — the **observe badge tooltip** (`TtBuildObserveCard`):
a tiny `○ <kind> · unlinked` card shown on a non-managed but observed tab (pwsh / cmd / an
unprompted claude / external codex). It is pushed via `SetAgentToolTip` too (so it rides the same
manually-driven lifecycle), by `_SetTabActivityBadge`.

**Key insight that drove the rewrite:** the default tooltip is framework-managed and never crashes; the
rich tooltip WAS manually driven (for fast-open + reliable-close under Islands) and crashed six times. The
fix was to make the rich tooltip framework-managed too — accepting the slower system-hover open in exchange
for the default tooltip's crash-free lifetime model (see §4 and §9).

---

## 2. Architecture — who owns what

```
TerminalPage (owns SessionInfo + the registry)            Tab (owns the ToolTip XAML lifecycle)
--------------------------------------------------        ---------------------------------------
_UpdateTabAgentToolTip(tab, sessionId)                    SetAgentToolTip(UIElement content, hstring sig)
  reads SessionInfo -> formats STRINGS (TtSpan/…)           stores content+sig, _agentToolTipActive=true
  builds the card element (TtBuildTooltipCard)               -> _UpdateToolTip -> _UpdateAgentToolTip
  computes a content SIGNATURE (skip if unchanged)         _UpdateAgentToolTip: create + host on the reused ToolTip
  impl->SetAgentToolTip(card, sig) ----------------------->  _WireAgentToolTipUnload: detach on owner recycle
  kicks _EnsureTabTooltipSummary (off-thread body)         (ToolTipService owns open/close on hover)
                                                           ClearAgentToolTip: revert to the default tooltip
```

**Split of responsibility (do not blur this):**
- **TerminalPage side** (`TerminalPage.AgentObserver.cpp`) owns the *content*: it has the
  `SessionInfo`, formats the strings, builds the XAML card, computes the change signature, and runs
  the off-thread transcript summary load. It knows nothing about *when* the popup opens.
- **Tab side** (`Tab.cpp`) owns the *lifecycle*: creating + hosting the element on ONE framework-managed
  `ToolTip`, swapping its `Content` only while closed, and detaching it on an owner recycle. It knows
  nothing about the session; it just hosts whatever element it's handed. It does NOT own open/close —
  `ToolTipService` does (post-rewrite; the old manual timers + `IsOpen` are gone, §9).

This split is why a content change is a cheap "push a new element + signature," and why all the
crashiness lives on the Tab side (the lifecycle), not the TerminalPage side (the content).

---

## 3. File & symbol map

**`src/cascadia/TerminalApp/Tab.cpp` / `Tab.h`** — the lifecycle (all `Agentmaster`-marked; framework-managed
post-rewrite — the old manual-open members/methods listed in §9's history are DELETED):
- Members (`Tab.h` ~238–252):
  - `bool _agentToolTipActive` — is the rich tooltip active (vs the default)?
  - `winrt::…UIElement _agentToolTipContent` — the card element last handed to us by the page.
  - `winrt::hstring _agentToolTipSig` — the content fingerprint (skip re-host if unchanged).
  - `winrt::…Controls::ToolTip _agentToolTip` — the ONE reused ToolTip. Framework-managed (we never
    drive `IsOpen`); `Content` swapped only while closed; **detached + nulled** on owner recycle /
    `ClearAgentToolTip` / `Shutdown`. `{ nullptr }` when none.
  - `bool _agentToolTipUnloadWired` — the owner-`Unloaded`→detach handler is wired ONCE per tab; guards that.
  - *(DELETED in the rewrite: `_agentToolTipOpenTimer`, `_agentToolTipDismissTimer`, `_agentToolTipHoverWired`,
    and the file-scope `g_lastAgentToolTipCloseTick` / `kAgentToolTipReopenCooldownMs` cross-tab cooldown.)*
- Methods:
  - `_UpdateToolTip()` — the entry that decides default vs rich; builds the default tooltip inline.
  - `SetAgentToolTip(content, sig)` — public; the page calls this. Stores + `_UpdateToolTip`.
  - `ClearAgentToolTip()` — public; revert to the default tooltip (session gone/archived); `_DetachAgentToolTip` first.
  - `_UpdateAgentToolTip()` — owner-guard; create (if null) + `SetToolTip` + `_WireAgentToolTipUnload`;
    swap `Content` only when `!IsOpen()` (a safe READ; we never SET IsOpen).
  - `_WireAgentToolTipUnload()` — wire (once) the owner `TabViewItem.Unloaded` → `_DetachAgentToolTip`.
  - `_DetachAgentToolTip()` — `SetToolTip(tvi, nullptr)` + null the ref (no `IsOpen`, no timers, no cooldown).
  - `EnsureAgentToolTipHoverHook(onHoverBuild, onWheel)` — public; wire (once per tab) the owner's
    `PointerEntered` → the page's build-now callback **and** its `PointerWheelChanged` → the page's
    card-scroll callback (§4b-bis; `_agentToolTipWheelCb`, cleared in `Shutdown` beside the hover one).
    Returns true only the ONE time it wires (the caller's arm-build cue). The wheel handler READS
    `IsOpen()` to gate, and marks the notch `Handled` only when the page reports it scrolled.
  - *(DELETED: `_WireAgentToolTipHover`, `_SafeSetAgentToolTipOpen`, `_ForceCloseAgentToolTip`, `_ArmAgentToolTipDismiss`.)*

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
- `_ArmTabAgentToolTipHover(tab)` — arms the lazy hover build **and** the wheel hook (one
  `EnsureAgentToolTipHoverHook` call, two callbacks); the hover callback also resets the scroll to the
  top of the body.
- **Body scrolling** (§4b-bis): `_ScrollTabAgentToolTip(tab, delta)` (the wheel entry) →
  `_SyncTabTooltipScrollBar(sid, offset, opacity)` (the render-only apply) +
  `_ResetTabAgentToolTipScroll(sid)` / `_ArmTabTooltipScrollBarFade(sid)` /
  `_OnTabTooltipScrollBarFadeTick()` (the auto-hide). The card's pieces come back from
  `TtBuildTooltipCard`'s `TtCardScrollParts& scrollOut`.

**`src/cascadia/TerminalApp/TerminalPage.h`** — the content cache (~730–744):
- `struct _AgentTooltipSummary { std::wstring path; int64_t mtime; int64_t lastCheckMs; winrt::hstring body; }`
- `std::unordered_map<std::wstring,_AgentTooltipSummary> _tabTooltipSummary` — per-session Summary cache.
- `std::unordered_set<std::wstring> _tabTooltipSummaryInFlight` — one background load per session.
- `std::unordered_map<std::wstring,std::wstring> _tabTooltipSig` — last signature per session.
- `winrt::fire_and_forget _EnsureTabTooltipSummary(...)`.
- `struct _AgentTooltipScroll { weak viewport/content/thumb; contentShift; thumbScale; thumbShift; offset; }`
  + `std::unordered_map<std::wstring,_AgentTooltipScroll> _tabTooltipScroll` — the hovered card's scroll
  state (§4b-bis), re-adopted on every rebuild; **weak** element refs so a superseded card is never pinned.
- `_tabTooltipScrollFadeTimer` / `_tabTooltipScrollFadeSession` / `_tabTooltipScrollFadeHolding` — the ONE
  per-window scrollbar auto-hide (hold, then step-down fade).

---

## 4. The lifecycle, end to end (CURRENT behavior — framework-managed, post-rewrite)

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
     build/mutate for a detached owner). The `owner` is captured in the `if`-init and reused below.
  2. `else if (!_agentToolTip)` → **create** the ONE reused `ToolTip`: `RequestedTheme(Dark)`,
     `Placement(Bottom)`, `IsHitTestVisible(false)`, `ToolTipService::SetToolTip(owner, it)`, then
     `_WireAgentToolTipUnload()` (no-op after the first time). From here **`ToolTipService` owns
     open/close** — hover opens (after the system delay), pointer-exit closes. We wire nothing else.
  3. If `_agentToolTip.IsOpen()` → return (a safe READ — do NOT swap `Content` while the framework is
     showing it). Else — closed-only mutations: re-assert **`PlacementRect` = the tab's own bounds**
     (`{0,0,ActualWidth,ActualHeight}`; a hover-opened AUTOMATIC tooltip otherwise places itself relative
     to the **pointer**, so `Placement(Bottom)` alone read "below the cursor" — the explicit rect anchors
     the card centered under the TAB; re-asserted each refresh since tab widths drift), then
     `_agentToolTip.Content(_agentToolTipContent)`.

### 4b-bis. Input = the WHEEL, read on the TAB HEADER (the body scrolls)

The card's Summary body is a **viewport** onto a much taller document, scrolled by the **mouse wheel
only** (no drag, no keyboard, no touch/manipulation). The card itself takes **no input at all** — it
stays `IsHitTestVisible(false)` and inert, per invariant 6a — so the wheel is read on the owner
`TabViewItem`, which is under the cursor for the tooltip's entire life anyway:

1. `Tab::EnsureAgentToolTipHoverHook` wires `PointerWheelChanged` beside the `PointerEntered`
   build hook (one wiring, once per tab). The handler only ever **reads** `IsOpen()` (invariant 2) and
   forwards the notch to the page callback.
2. `TerminalPage::_ScrollTabAgentToolTip(tab, delta)` resolves the tab's **current** session (re-home
   safe, like the hover hook), advances the stored offset by `kTtScrollStepPx` per 120 units, and calls
   `_SyncTabTooltipScrollBar`.
3. `TerminalPage::_SyncTabTooltipScrollBar(sid, offset, opacity)` shifts the body's
   `TranslateTransform`, re-fits the slim scrollbar (`ScaleTransform` length + `TranslateTransform`
   position), paints it, and returns the max scrollable offset. **Every write is render-only** — never
   `Height`/`Margin` — because this also runs from the viewport's own `SizeChanged` (i.e. from inside a
   layout pass), where a layout write is the popup `E_LAYOUTCYCLE` fail-fast the tag badges had to be
   rewritten around (`TabHeaderControl::_PositionTagBadgesNow`).
4. The notch is marked `Handled` **only when something actually scrolled**, so a card that fits leaves
   the wheel to the tab strip's own horizontal scroll.
5. The bar exists **only while you scroll**: invisible at rest (even on a card that *can* scroll), lit
   on the first notch, then one per-window `DispatcherTimer` holds `kTtScrollBarHoldMs` and steps the
   opacity back to 0 (`_ArmTabTooltipScrollBarFade` / `_OnTabTooltipScrollBarFadeTick`). It is feedback
   for the gesture, not chrome — a bar sitting permanently on a hover card is just noise.

**Two handlers, deliberately.** The per-tab one is the precise path; a **belt** on the whole tab ROW
(`_WireTabStripTooltipWheel`) catches a notch that MUX's tab-strip internals swallowed before it reached
the item, and scrolls whichever tab's card is open (only one can be — the tip dies on pointer-exit).
Both use `AddHandler(..., handledEventsToo: true)`; `_tooltipWheelClaimed` de-dupes them (the per-tab
handler is deeper in the bubble, so it always runs first and claims the notch).

**Diagnostics — `[tooltip-wheel]` in `hooks.log`** (throttled to one line per ~400ms gesture, and only
for a managed tab). This chain's first link is invisible from the outside, so "the scroll didn't
register" would otherwise be indistinguishable between *the notch never arrived*, *the card wasn't
open*, and *the body had nothing to scroll*. The lines separate exactly those:
`item: cb=1 tip=1 open=0/1` (the notch reached the tab), `<sid> src=item|row delta=… off=A->B max=M`
(it scrolled, and by how much), `row: no open card` (it reached the strip but no card was up). **No
lines at all ⇒ the wheel never reached the tab strip.**

The per-card pieces live in `TerminalPage::_tabTooltipScroll[sessionId]` (**weak** element refs + the
three transforms), re-adopted on every card rebuild. Weak, because the card is rebuilt on every content
change — strong refs would pin every superseded tree (the leak class `AgentTipHelpers` exists to avoid).

**Two non-obvious rules keep the state and the screen in lockstep** — both fall out of `PointerEntered`
being a BUBBLING event, so it re-fires as the pointer crosses the header's own inner elements (icon →
title → close button), i.e. repeatedly *while the tip is already open*:

- **`_UpdateTabAgentToolTip` does not rebuild while the tip is open** (unless this push is the
  `swapWhileOpen` grant). `Tab::_UpdateAgentToolTip` would refuse to host it anyway (invariant 3), so
  the page would end up adopting the scroll pieces of a card that is NOT on screen and the wheel would
  silently drive an unhosted tree. Nothing is lost by skipping: the next `PointerEntered` rebuilds
  fresh, and the staged card's "ago" line would have gone stale by then regardless.
- **The scroll reset only runs on a hover that starts CLOSED.** Otherwise every mouse nudge inside the
  tab header would snap the reader back to the top of the body.

Both gate on `Tab::AgentToolTipOpen()` — a safe **read** of `IsOpen` (invariant 2 forbids writing it).

### 4c. Open / close = the FRAMEWORK
There are no open/dismiss timers, no pointer-loss handlers, no manual `IsOpen`, and no cross-tab
cooldown anymore — `ToolTipService` opens the tip on hover (at the system hover delay) and closes it on
pointer-exit / window-deactivate, exactly as it does for the plain default tab tooltip (which never
crashed). We only ever *read* `IsOpen()` (to gate the content swap); we never *set* it. This is what
removes the entire crash class (§9) — there is no manually-opened popup, and no reused object we drive
across an open/close cycle.

### 4d. Detach = drop (`_DetachAgentToolTip`)
Runs from the owner `Unloaded` handler (§4e), `ClearAgentToolTip` (session gone), and `Tab::Shutdown`. It:
1. Returns early if `!_agentToolTip` (nothing of ours attached — leaves any default tooltip alone).
2. `ToolTipService::SetToolTip(tvi, nullptr)` (**detach** from the owner; try/catch).
3. `_agentToolTip = nullptr` (**drop** our strong ref).

It drives **no `IsOpen`** — the framework closes any open popup itself on exit/recycle — so this path
can't fail-fast. Dropping the ref means the next `_UpdateAgentToolTip` rebuilds a FRESH tooltip bound to
the live (reloaded) owner (the crash #4 lesson, kept without the manual machinery).

### 4d-bis. The ATTACH-TIMING root cause (2026-07-27) — why the card "only showed sometimes"

**Symptom:** the rich card appeared unreliably, and its wheel scrolling never worked — every single
`[tooltip-wheel]` sample in hooks.log read `open=0`, most of them `tip=0` (our ToolTip object *null*).

**Two plausible theories were both WRONG**, and the framework's own source refuted them
(`microsoft-ui-xaml`, `dxaml/xcp/dxaml/lib/ToolTipService_Partial.cpp` — the same implementation
lineage as the system XAML we run on):
- *"the wheel dismisses the tooltip"* — there is **no wheel handling at all** in `ToolTipService` or
  `ToolTip`. Nothing about scrolling closes a tooltip.
- *"`IsOpen` lies for a service-opened tooltip"* — `OpenAutomaticToolTip` calls
  **`put_IsOpen(TRUE)` on the app's own instance** (the one returned by `GetActualToolTipObjectStatic`
  for the owner). `IsOpen` is truthful.

**The actual cause is attachment timing.** `ToolTipService::RegisterToolTip` — which runs when the
tooltip is **attached** — is what does `add_PointerEntered` on the owner. So:

1. A tooltip attached **during** a dwell never sees that dwell's `PointerEntered` → its open timer
   never starts → **it does not open for that hover**. (Our own code already knew this in passing: see
   the arm-build comment in `_ArmTabAgentToolTipHover`.)
2. `Unloaded → _DetachAgentToolTip` nulls our tooltip on **every MUX container recycle** — constant on
   a many-tab strip, and required (crash #4).
3. Re-attachment only happened on the next **content push**, which — since the card went lazy /
   hover-built — is *mid-dwell*. Worse, `SetAgentToolTip`'s signature early-out could skip that push
   entirely when nothing had changed, leaving the tab with **no tooltip attached at all**.

Net effect: after a recycle, the next hover showed nothing (or a leftover popup from an earlier dwell,
which is why the card *looked* present), while our object was null-or-freshly-created and honestly
closed. The wheel gate was reporting reality.

**The fix — attach before a pointer can arrive, never during:**
- `TabViewItem().Loaded → _UpdateAgentToolTip()` when active-but-unattached (the missing other half of
  the Unloaded detach), so a recycled tab is re-armed the moment it re-enters the tree.
- `SetAgentToolTip`'s early-out now also requires `_agentToolTip` to exist, so a same-signature push
  still re-attaches.
- `_UpdateTabAgentToolTip` pushes when `!AgentToolTipAttached()` even if the signature is unchanged.

**Lesson to keep:** for a service-owned tooltip, *attachment* — not content, not `IsOpen` — is the
state that determines whether it can ever open. Anything that detaches (recycle, `ClearAgentToolTip`)
must have a matching re-attach that runs **outside** a dwell.

### 4e. Owner-recycle safety (`_WireAgentToolTipUnload`, wired once per tab)
MUX `TabView` virtualizes/recycles its item containers (heavy during a multi-tab window restore), which
tears down the ToolTip's native peer while our WinRT strong ref keeps resolving non-null — a zombie. If
the SAME owner later RELOADS, reusing that stale ref to swap `.Content()` would read freed memory (crash
#4). So the sole surviving handler is `TabViewItem().Unloaded → _DetachAgentToolTip()`: on unload we
detach + drop, and the next content refresh rebuilds fresh. The `_UpdateAgentToolTip` owner-loaded guard
is the complementary front door (never build/mutate for a detached owner).

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

- `TtBuildTooltipCard(accent, title, folderBranch, stateText, metaText, bodyText, …, maxCardHeight)` → a
  dark `Border` (bg `#FF202020`, 1px `#40FFFFFF` border, corner radius 4, padding 10/8,
  **MaxWidth `kTtCardMaxWidth` = 660**) wrapping a vertical `StackPanel`:
  - **Header** (a 2-col `Grid`): left = a 9px state-colored `Ellipse` (`accent`, 1px black stroke) +
    the title (Cascadia Mono 13, semibold, ellipsized, NoWrap); right = `folderBranch` (Cascadia Mono
    11, dim `#B0B0B0`, ellipsized).
  - **State line** (Cascadia Mono 11, colored `accent`): `<state> · <ago> · <why> · ⚠ unread`.
  - **Meta line** (Cascadia Mono 11, dim): `claude|codex · model · effort · <mode/⚡ bypass>`.
  - **Divider** (full-width 1px `Border`) + the Summary body in a **wheel-scrollable viewport**,
    line-truncated at `kTtBodyMaxLines` (now `kTtScrollBodyScreenfuls` × a screenful, floored/capped
    192…1000 — the element/text-measure backstop, no longer the visual cut) with the dim
    `"… +K more (see the summary panel)"` marker for anything past it. **Deliberately NOT a
    ScrollViewer** (crash #7, §9 — its DirectManipulation activation on popup-enter is the proven
    fail-fast); the scrolling is hand-rolled out of inert parts:
    - `bodyArea` (`Grid`) — hosts the viewport **and** the overlay scrollbar as siblings, so the bar's
      negative right margin can park it in the card's padding gutter (never over text) instead of being
      cut by the clip.
    - `bodyClip` (`Grid`, `MaxHeight` = the body budget) — the height cap **and** the visual cut; the
      page wires its `SizeChanged` to set an explicit `Clip` rect (nothing is measured at build time,
      since a card is built before it is ever shown).
    - `measureHost` (`StackPanel`) — measures the body with **infinite** height. That is what makes the
      body's `ActualHeight` the true scroll extent and, more subtly, what keeps the body free of a
      **layout clip of its own**: a layout clip lives in the element's own coordinate space, so it would
      travel WITH the render transform and reveal nothing. The clips that matter therefore sit on
      elements we never transform.
    - the body carries a `TranslateTransform` (scroll = a render shift: no layout pass, so a notch can
      never resize or move the open popup), and the slim bar is sized by a `ScaleTransform` + positioned
      by a `TranslateTransform` (same reason — see §4b-bis).

### Sizing — width is flat, height is a WINDOW FRACTION

Height is the dimension that stretches (the card grows downward with the Summary body), so it is the one
budgeted against the window rather than hardcoded — the old flat `MaxHeight(360)` body clip topped the
whole card out near half the screen:

- **Width** — `kTtCardMaxWidth` **660** (was 460; +200, and since the card is centered under its tab that
  spends 100 per side). `kTagLineBudget` (the tag-row greedy packer) is derived from it, so it can't drift.
- **Height** — `_UpdateTabAgentToolTip` measures `Root().ActualHeight()` (the XAML island root == the window
  client area; the tooltip renders in the island's popup root, so the window bounds it regardless — and it
  IS the screen when maximized, the normal case) and passes `kTtCardHeightFraction` (**0.80**) of it as
  `maxCardHeight`. The builder hands the body `maxCardHeight − kTtChromeReserve` (170 — an estimate of the
  header/state/meta/tags/divider/padding rows, since nothing is laid out at build time), floored at
  `kTtMinBodyHeight` (120). An unmeasured root (0) falls back to `kTtCardFallbackHeight` (460 ≈ the old
  fixed card), so a pre-layout build is never worse than before.
- **`kTtBodyMaxLines`** is no longer a constant: it is derived from the body budget with a deliberately
  UNDER-estimated per-line height (`/ 11.0`, clamped 24–160) so it stays a pure **element-count backstop**
  sitting ABOVE what the clip can show — the clip, not the truncation, is the visual cut. That reproduces
  the old hardcoded pair exactly at the old budget (360 / 11 = 32).
- The budget rides the **re-host signature** (coarsened to a 50px grain, the `tagsOpacity` precedent) so a
  window resize re-hosts an already-built card without a drag re-hosting on every pixel.
- `TtBuildObserveCard` (the `○ <kind> · unlinked` twin) keeps its flat **MaxWidth 420** — it is two lines
  and never stretches.
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
   leaving. That unreliability was the ORIGINAL reason for the **manual driving** (open/dismiss timers +
   `IsOpen`) — which then became the crash source. The rewrite gives that up: like the plain default
   tooltip, the rich one now accepts the framework's (working-enough) behavior and never crashes, at the
   cost of the slower system-hover open (§4/§12). The points below are WHY the manual path was so
   fragile — kept as the rationale for NOT reintroducing it.
3. **Manual driving is dual-driving** — attaching via `ToolTipService::SetToolTip` AND manually calling
   `IsOpen` meant both the framework and our code toggled the popup; that dual-drive + rapid hover was
   the deep race behind the later crashes. The rewrite removes our half — only the framework toggles now.
   `IsHitTestVisible(false)` is still set (so the large card never sits under the cursor as a target).
4. **Owner is a MUX `TabViewItem`** — MUX `TabView` **virtualizes/recycles** its item containers, so
   the owner (and the ToolTip's native peer) can be torn down out from under our still-valid WinRT
   strong ref → a "zombie" peer. This is the ONE fragility that survives the rewrite (a reused ref can
   still go stale), so the `Unloaded → _DetachAgentToolTip` handler + the owner-loaded guard remain.
5. **Fail-fasts + AVs bypass `try/catch`** — a XAML stowed exception (`0xC000027B`) is a
   `RaiseFailFastException`; a CFG violation (`0xC0000409` subcode `0xA`) and an access violation
   (`0xC0000005`) are not C++ exceptions under `/EHsc`, and several fired in a **deferred framework
   render/input pass** OFF our call stack where no guard could catch them. That is exactly why the fix
   had to be **structural** (stop driving `IsOpen` at all), not another `try/catch` — the retired
   `_SafeSetAgentToolTipOpen` caught only the *hresult_error* cousins, never the fail-fast/AV.

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
| 6 | `9c026822d` | `0xC000027B` stowed fail-fast in a **render/composition pass** (GPU driver on the stack), deferred/off-stack, **zero of our frames** | jumping the cursor between the focused tab and a just-activated (dormant) tab | **cross-tab** race: opening B's fresh tooltip while A's popup is still in async teardown → two agent popups collide in one render pass — *attribution now SUPERSEDED: see #7* | **cross-tab reopen cooldown** (`g_lastAgentToolTipCloseTick`, 150ms) — defer B's open until A's teardown drains — *insufficient (a 20:00 build with it crashed at 20:02)* |
| 7 | this change | `0xC000027B`, **PROVEN via a full-memory dump's decoded `STOWED_EXCEPTION` backtrace** (the first crash with the actual stow stack) | hovering a tab ~14s after stepping through restored tabs — on the **framework-managed rewrite binary** | `DirectUI::ToolTipService::OpenAutomaticToolTip` → `ToolTip::OpenPopup` → the card's content tree **Enter** walk → **the Summary body's `ScrollViewer`** → `ScrollViewer::OnManipulatabilityAffectingPropertyChanged` → `CDirectManipulationService::ActivateDirectManipulationManager` = **E_INVALIDARG** → stowed → fail-fast. NOT an open/close race at all — the CARD CONTENT was the poison | **remove the `ScrollViewer` from `TtBuildTooltipCard`** (a hit-test-invisible tooltip could never scroll — it was dead weight): line-truncate the body (`kTtBodyMaxLines` 32 + a dim "+K more" marker) inside a plain `MaxHeight(360)` `Grid` (UWP layout-clips overflow; a Grid has no manipulation machinery) |

**Patterns to internalize (REVISED after #7's full-dump proof):**
- The saga had **TWO distinct crash classes**, not one:
  - **Real use-after-frees (#4, #5)** — `0xDDDD…` registers (MSVC debug-CRT freed fill) prove a method
    reached a torn-down ToolTip peer. The manual-path hardening (detach+drop, create-fresh) was genuinely
    needed for these, and the framework-managed rewrite deletes the whole class.
  - **Stowed-exception fail-fasts (#1, #6, #7 + three unanalyzed overnight crashes)** — for six crashes we
    could only see `ProcessUnhandledError → RaiseFailFastException` (deferred, off-stack, zero of our
    frames) and attributed them to open/close races by repro correlation. #7's full dump finally decoded
    the stowed backtrace: the fatal error originates in **DirectManipulation activation for the card's
    `ScrollViewer` during the popup-open Enter walk** — `E_INVALIDARG`, plausibly state-dependent on the
    island's input-site state (which is why "a tab I just activated without entering" correlated). #1/#6
    can't be re-proven (their WER dumps are partial — no heap, no stow structs) but match this shape.
- **A ToolTip's content is part of its crash surface.** The open/close mechanics were only half the story;
  what the popup's Enter walk touches (manipulation, input sites) can fail-fast all by itself. Keep
  tooltip content INERT: text, shapes, panels — no ScrollViewer, no manipulation-capable element.
- **`try/catch` is not a fix here.** Fail-fasts and AVs aren't caught by `catch(...)` under `/EHsc`, and
  several fire in a deferred pass off our stack. The real fixes are all *structural*.
- **Chronic tolerated stows are noise, not the killer.** Both Debug and Release layouts have 34 missing
  PRI `Path` assets (ProfileIcons etc. — `0x80070003` stows on tab-header render); they appear in the
  stowed-history array of any fail-fast dump. Check each stow's **nested-blob FILETIME** to find the one
  that matches the crash instant — that's the fatal one; earlier timestamps are residue.

**RESOLUTION, part 1 (2026-07-01) — the manual-open path was RETIRED.** After crash #6, rather than adding
a 7th point-patch to a fundamentally racy design, the whole manual-open machinery was **removed** and the
rich tooltip made **framework-managed** — the §12 "recommended fallback", now the shipping design (§4).
Deleted: the fast-open one-shot timer, the 8s auto-dismiss backstop, the three pointer-loss close handlers,
`PointerMoved` keep-alive/re-open, `_SafeSetAgentToolTipOpen` (manual `IsOpen`), `_ForceCloseAgentToolTip`,
`_ArmAgentToolTipDismiss`, and the `g_lastAgentToolTipCloseTick` cross-tab cooldown. Kept: the rich card
content, the ONE reused `ToolTip`, the owner-loaded guard, the closed-only `Content` swap, and a single
`Unloaded → _DetachAgentToolTip` handler. `ToolTipService` now owns open/close. **Tradeoff:** opens at the
system hover delay (~0.5–1s; no `InitialShowDelay` in this SDK), and the old "sticky tooltip" annoyance may
occasionally reappear — both cosmetic. This closes the UAF class (#2/#4/#5) by construction.

**RESOLUTION, part 2 (2026-07-02) — the rewrite alone was NOT enough; the card's `ScrollViewer` was the
fail-fast.** The 21:57 build containing the rewrite crashed `0xC000027B` three more times overnight (22:21,
05:22, 05:37). A procdump watcher captured the 05:37 one as a **full-memory dump**, whose decoded
`STOWED_EXCEPTION` backtrace (crash #7 above) proved the fatal chain runs through the **framework's own
`OpenAutomaticToolTip`** into the card content's `ScrollViewer` DirectManipulation activation —
`E_INVALIDARG`, independent of who drives open/close (so the manual-path era's stowed crashes #1/#6 were
plausibly this all along). The fix removed the `ScrollViewer` from `TtBuildTooltipCard` (line-truncation +
a plain clipping `Grid` instead — §6); the tooltip is hit-test-invisible, so the ScrollViewer was never
scrollable and nothing was lost. **Both parts stand:** part 1 killed the UAF class and simplified the
lifecycle; part 2 killed the stowed-fail-fast class at its proven origin. If a NEW crash somehow appears,
decode its stow stack FIRST (§11 — full dump + `STOWED_EXCEPTION` + the MS symbol server); the remaining
escape hatch is the §12 **off-switch**.

---

## 10. Invariants — DO NOT REGRESS

1. **UI thread only.** All tooltip code runs on the window's UI thread; off-thread callers marshal
   first. Keep the `ASSERT_UI_THREAD()`s.
2. **NEVER drive `IsOpen`.** `ToolTipService` owns open/close. Reading `IsOpen()` (to gate the content
   swap) is fine; *setting* it is what caused crashes #1/#5/#6. Do not reintroduce open/dismiss timers,
   pointer-loss close handlers, `_SafeSetAgentToolTipOpen`, or a cross-tab cooldown — that is the retired
   manual-open path (§9).
3. **Reuse ONE object; swap `Content` only while closed.** Create `_agentToolTip` once and `SetToolTip`
   once; on refresh, `if (_agentToolTip.IsOpen()) return;` before `.Content(...)` — swapping under the
   pointer flickers, and re-`SetToolTip`ing every ~2s would replace the framework's open tip.
4. **Never build/mutate the tooltip for a detached owner.** Keep the `IsLoaded()`+`XamlRoot()` guard at
   the top of `_UpdateAgentToolTip`.
4a. **Every detach needs a re-attach that runs OUTSIDE a dwell.** A tooltip that is not attached cannot
   open, and one attached mid-hover does not open for that hover (§4d-bis). Keep the
   `TabViewItem().Loaded → _UpdateAgentToolTip` re-host, and keep `AgentToolTipAttached()` in the push
   conditions — a signature-only gate will silently leave recycled tabs tooltip-less.
5. **Detach + drop on owner recycle.** Keep the `TabViewItem().Unloaded → _DetachAgentToolTip` handler
   (`SetToolTip(tvi, nullptr)` + null the ref). Without it, a recycle→reload reuses a zombie ref on the
   next `.Content()` swap — exactly crash #4. `_DetachAgentToolTip` drives NO `IsOpen` (the framework
   closes any open popup itself), so it can't fail-fast.
6. **Keep it framework-managed — do not "speed it up" back into manual open.** The slower system-hover
   open is the deliberate price of the default tooltip's crash-free lifetime. If it must be faster, that
   is a framework/SDK concern (there is no `InitialShowDelay` here), not a reason to hand-drive `IsOpen`.
6a. **NO ScrollViewer — no manipulation-capable element — inside ToolTip content. Ever.** The popup-open
   Enter walk activates a ScrollViewer's DirectManipulation, which can fail `E_INVALIDARG` under XAML
   Islands → stowed → `0xC000027B` fail-fast (crash #7, the only PROVEN stow stack of the saga). Tooltip
   content must be inert: TextBlocks, panels, shapes, Borders, transforms. Height is capped by line
   truncation + a plain `MaxHeight` Grid. This applies to ANY future popup-hosted content we build, not
   just this card. **The body's wheel scrolling (§4b-bis) does not bend this** — it adds no
   manipulation-capable element and no input on the card at all: the wheel is read on the tab HEADER and
   applied as a `RenderTransform`. If the card ever needs another interaction, that is the pattern to
   copy — drive it from the header, keep the popup inert.
6b. **Card content is mutated only through render-only properties while the tip is open.** Transform
   values, `Opacity` and `Clip` are fine (they cannot re-enter layout); `Height`/`Margin`/`Width` from a
   layout-driven handler (`SizeChanged`/`LayoutUpdated`) inside a popup is the `E_LAYOUTCYCLE`
   (`0x88000FA8`) fail-fast — see `TabHeaderControl::_PositionTagBadgesNow`, which had to become a
   coalescing scheduler for exactly this. This is why the scrollbar is scaled, not resized.
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
insufficient). Don't diagnose without this check. The dump itself carries both halves: `MiscInfoStream`
→ **ProcessCreateTime** (which on-disk binary the process loaded) and the module list's **TimeDateStamp**
(the exact link time of the loaded DLL) — `stowed2`/`dumpstowed` print them.

**The `0xC000027B` breakthrough (crash #7) — decode the STOWED stack; stop guessing.** A stowed fail-fast's
`ExceptionInformation[0]` points at an array of `STOWED_EXCEPTION_INFORMATION_V2*` (`[1]` = count). Each
carries the nested HRESULT **and the ORIGINAL backtrace captured at stow time** — the exact framework call
chain the six earlier crashes never revealed. Requirements + gotchas:
- **A FULL-memory dump** (the stow structs live in heap; WER's default ~30 MB dumps drop them). Capture
  with a `procdump -ma -e` watcher armed BEFORE the crash (the `fulldumps/` recipe): attaching procdump
  does NOT cause the fail-fasts (two of the overnight crashes happened with no debugger attached).
- **The signature is a multi-char constant**: `'SE01'/'SE02'` = `0x53453031/32` stores little-endian as
  bytes `31/32 30 45 53` — check BOTH byte orders (`tools/dumpstowed.cpp`'s original check missed this and
  could never match; the fixed decoder is the scratchpad `stowed2.cpp`).
- **The tid bitfield is `(tid | form)`, not `(tid << 2 | form)`** — Windows TIDs are multiples of 4, so
  `tid = bits & ~3`. All of #7's stows were on the window's own UI thread.
- Each stow's **nested "XAML"-typed blob** (`NestedExceptionType == 0x4C4D4158`) starts with a **FILETIME**
  — the stow instant. Match it against the crash time to find the FATAL stow; earlier ones are tolerated
  residue (e.g. the chronic missing-asset `0x80070003`s).
- **Symbolize the WUX frames via the MS symbol server**: copy `symsrv.dll` (VS Remote Debugger x64 dir) +
  `dbghelp.dll` (System32) NEXT TO the decoder exe, pass
  `srv*<cache>*https://msdl.microsoft.com/download/symbols;<our-pdb-dir>` as the symbol path. Without
  names, an Enter-walk recursion and a DManip activation are unreadable module+offset noise.
- When `StackWalkEx` dies at `RaiseFailFastException` (partial dumps), the scratchpad `scanstack.cpp`
  (unwind-free symbolized stack scan, dbghelp+symsrv) still recovers the shape — e.g. it showed the old
  crashes' `ProcessUnhandledError` envelopes.

---

## 12. Design history + the remaining escape hatch

**IMPLEMENTED (2026-07-01, + the 2026-07-02 content fix) — framework-managed rich tooltip.** This was the
"recommended fallback" and is now the shipping design (see §4, §9 RESOLUTION parts 1+2): `ToolTipService`
owns open/close — the manual `IsOpen` driving, the open/dismiss timers, the pointer-loss handlers, and the
cross-tab cooldown are gone — and the card content lost its `ScrollViewer` (the proven crash-#7 fail-fast;
line truncation + a clipping Grid instead). It is the SAME lifetime mechanism as the plain default tooltip,
which has NEVER crashed, with content that is now inert (no manipulation-capable elements). Cost: opens at
the system hover delay (~0.5–1s, no `InitialShowDelay` in this SDK) instead of instantly, and the "sticky
tooltip under Islands" annoyance may occasionally return (both cosmetic). Content refresh sets
`_agentToolTip.Content(newCard)` only when `!_agentToolTip.IsOpen()`.

**Remaining escape hatch — off-switch.** If a NEW crash somehow appears on this framework path (unlikely —
it is the default tooltip's own path), do NOT hand-drive `IsOpen` again. Add a setting to disable the rich
tab tooltip and fall back to the default title tooltip (fully robust). Loses the rich card unless re-enabled.

History: the user originally chose to keep the fast **manual** tooltip (commit `9c026822d`) with the
framework-managed path held as the escape hatch; after crash #6 that hatch was taken and the manual path
retired (§9 RESOLUTION).

---

## 13. Quick reference

- Rich card built: `TerminalPage::_UpdateTabAgentToolTip` (`TerminalPage.AgentObserver.cpp`) — unchanged by
  the rewrite (the whole content side is untouched).
- Hosted + lifecycle (framework-managed): `Tab::SetAgentToolTip` / `_UpdateAgentToolTip` /
  `_WireAgentToolTipUnload` / `_DetachAgentToolTip` (`Tab.cpp`).
- The one rule: `ToolTipService` owns open/close — we never drive `IsOpen`; swap `Content` only while closed.
- Body scrolling (§4b-bis): wheel-only, read on the tab HEADER (`Tab::EnsureAgentToolTipHoverHook`'s
  `PointerWheelChanged`) → `TerminalPage::_ScrollTabAgentToolTip` → `_SyncTabTooltipScrollBar`
  (render-only writes: a `TranslateTransform` on the body, a `ScaleTransform`+`TranslateTransform` on the
  slim bar, which auto-hides a couple of seconds after the last notch). The card stays inert and
  hit-test-invisible — still NO ScrollViewer (invariants 6a/6b).
- Owner-recycle safety: the `TabViewItem().Unloaded → _DetachAgentToolTip` handler + the `_UpdateAgentToolTip`
  owner-loaded guard.
- Off-thread body: `_EnsureTabTooltipSummary` + `_tabTooltipSummary`/`_tabTooltipSig`/`_tabTooltipSummaryInFlight`.
- Shared palette: `AgentStatusColorFor` (`AgentStatusColors.h`).
- Commits: `918b720b7`, `44beaea7e`, `be1d63b7f`, `9792161b8` (observer nets), `6601c7532`,
  `1d1edbe18`, `9c026822d` (the six manual-open crashes) → the framework-managed rewrite (this change).
- Diagnostics: the `debug-dumps` skill + `tools/dumpexc|dumpourscan|dumpwalk2|hangwalk`.
