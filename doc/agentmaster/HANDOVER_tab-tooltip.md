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

> **2026-10-05 — the lifecycle is no longer inside `Tab`.** It moved, verbatim, into
> **`AgentSessionToolTipHost`** (`src/cascadia/TerminalApp/AgentSessionToolTip.{h,cpp}`), so a second
> owner can run the same code: the **side-panel session card** in the panel's "Sum" OFF mode
> ([`SIDE_PANEL.md`](SIDE_PANEL.md) §2d). `Tab` owns one host for its header and keeps its old public
> methods as forwarders, so no caller changed. The content side split the same way: the old body of
> `_UpdateTabAgentToolTip` is the shared `_PushSessionToolTipCard`, and the tab and the card are thin
> "surfaces" over it. Old name → current name, for the history sections below:
>
> | in this doc (Tab, before) | now (`AgentSessionToolTipHost`) |
> |---|---|
> | `_agentToolTipActive` / `_agentToolTipContent` / `_agentToolTipSig` / `_agentToolTip` | `_active` (`Active()`) / `_content` / `_sig` (`Sig()`) / `_toolTip` |
> | `_agentToolTipUnloadWired` (+ the Loaded half) | `_lifetimeWired` (`_WireOwnerLifetime`) |
> | `_agentToolTipHoverWired` / `_agentToolTipHoverCb` / `_agentToolTipWheelCb` | `_hoverWired` / `_hoverCb` / `_wheelCb` |
> | `_UpdateAgentToolTip()` | `Update()` |
> | `_WireAgentToolTipUnload()` | `_WireOwnerLifetime()` (Unloaded → `Detach`, Loaded → `Update`) |
> | `_DetachAgentToolTip()` | `Detach()` (its catch now LOGS — it was the last bare `catch (...)` here, Rule #18) |
> | `SetAgentToolTip` / `ClearAgentToolTip` / `EnsureAgentToolTipHoverHook` (Tab, public) | still on `Tab`, forwarding to `SetContent` / `Clear` / `EnsureHoverHook` |
> | the `PlacementRect` write inside `_UpdateAgentToolTip` | `_ApplyPlacement` (+ a per-owner placement provider) and `RefreshPlacement` |

---

## 0. Scope & what this is NOT

- **This doc = the TAB-STRIP tooltip**: the hover card that appears when you hover a **tab header**
  in the tab strip. For a managed Claude/Codex session it's a rich, dark, summary-style card; for a
  shell/pwsh/cmd/Manager tab it's the plain title+key-chord tooltip.
- **NOT the per-tab overlay** (the in-terminal HUD badge + the pencil-toggled summary panel that
  live *inside* the terminal pane). That is `AgentTabOverlay` — see
  [`TAB_OVERLAY.md`](TAB_OVERLAY.md). The two are **separate mechanisms** that happen to show
  overlapping session facts. Don't conflate them.
- **NOT `AgentTipHelpers.h`** — that's the shared hover-tooltip recipe for the Agent Manager page's
  cards and the Sessions page (page elements). The rich tooltip is its own code,
  `AgentSessionToolTipHost`, because its first owner is a MUX-virtualized `TabViewItem` (which is
  exactly why it's more fragile — see *Crash history*). Since 2026-10-05 it has a second owner, the
  side-panel session card ([`SIDE_PANEL.md`](SIDE_PANEL.md) §2d). The two mechanisms must not share
  an element: the `AgentTipHelpers` host attaches ITS tip to the hovered element on open and detaches
  it on close, which would replace and then remove a host's `ToolTip`. So an element that carries a
  host has its floating tips silenced (`AgentSuppressTip`). `AgentTipHelpers` is a useful reference
  for the same class of guards, though.

---

## 1. The two tab tooltips

Every tab header can carry ONE of two tooltips, mutually exclusive, gated by the tab's host
(`Tab::_agentToolTipHost->Active()`):

| | **Default tooltip** | **Rich agent tooltip** |
|---|---|---|
| Shown on | shell/pwsh/cmd tabs, an agent tab before its first push (the Agent Manager is a page behind the Home button, not a tab, so it has neither) | a **managed** (bound) Claude/Codex session tab |
| Content | title + key-chord (`TextBlock`) | dark summary card (state dot, title, folder/branch, state line, model·effort·mode, tags, divider, transcript Summary body) |
| Built in | `Tab::_UpdateToolTip` (Tab.cpp) | `TerminalPage::_UpdateTabAgentToolTip` → the shared `_PushSessionToolTipCard` builds it; the tab's `AgentSessionToolTipHost` hosts it (`SetContent`) |
| Open/close | **framework-managed** (`ToolTipService` auto opens/closes on hover) | **framework-managed** now too (was manually driven — timers + `IsOpen`; that was the crash source, §9) |
| Crashes? | **never** (framework owns the lifetime) | the crash history below is the OLD manual path; the current path shares the default's crash-free lifetime model |

There is also a **third, degenerate variant** — the **observe badge tooltip** (`TtBuildObserveCard`):
a tiny `○ <kind> · unlinked` card shown on a non-managed but observed tab (pwsh / cmd / an
unprompted claude / external codex). It is pushed via `SetAgentToolTip` too (so it rides the same
framework-managed host lifecycle), by `_SetTabActivityBadge`.

**Key insight that drove the rewrite:** the default tooltip is framework-managed and never crashes; the
rich tooltip WAS manually driven (for fast-open + reliable-close under Islands) and crashed six times. The
fix was to make the rich tooltip framework-managed too — accepting the slower system-hover open in exchange
for the default tooltip's crash-free lifetime model (see §4 and §9).

---

## 2. Architecture — who owns what

```
TerminalPage (owns SessionInfo + the registry)          AgentSessionToolTipHost (owns the ToolTip XAML lifecycle)
------------------------------------------------        ---------------------------------------------------------
SURFACES (thin):                                        one per OWNER element:
  _UpdateTabAgentToolTip(tab, sessionId)                  Tab::_agentToolTipHost   (owner = the TabViewItem)
  _UpdateSidePanelCardToolTip(id)  [Sum OFF]              AgentSidePanelCardView::tipHost (owner = the card Border)
        |  resolve the session, gone/!live -> Clear
        v                                                SetContent(UIElement, sig, swapWhileOpen)
_PushSessionToolTipCard(host, surfaceKey, s, …)            stores content+sig, active=true -> Update()
  formats STRINGS (TtSpan/…), builds the card             Update(): owner guard; create + SetToolTip once;
  (TtBuildTooltipCard), computes the SIGNATURE              closed-only placement + Content swap
  host.SetContent(card, sig, …) ------------------------>  _WireOwnerLifetime: Unloaded -> Detach, Loaded -> Update
  kicks _EnsureTabTooltipSummary (off-thread body,        EnsureHoverHook: PointerEntered -> re-anchor + build;
  per SESSION; the arrival re-hosts every waiting           PointerWheelChanged -> the page's body scroll
  surface)                                                (ToolTipService owns open/close on hover)
                                                          Clear / Detach / Shutdown
```

`Tab` keeps its public tooltip API (`SetAgentToolTip`, `ClearAgentToolTip`, `EnsureAgentToolTipHoverHook`,
`AgentToolTipSig/Attached/Open/HoverWired`, plus `AgentToolTipHost()`) as one-line forwarders over its host;
`ClearAgentToolTip` additionally rebuilds the DEFAULT tooltip (`_UpdateToolTip`), which only a tab has.

**Split of responsibility (do not blur this):**
- **TerminalPage side** (`TerminalPage.AgentObserver.cpp`) owns the *content*: it has the
  `SessionInfo`, formats the strings, builds the XAML card, computes the change signature, and runs
  the off-thread transcript summary load. It knows nothing about *when* the popup opens.
- **Host side** (`AgentSessionToolTip.cpp`, formerly `Tab.cpp`) owns the *lifecycle*: creating +
  hosting the element on ONE framework-managed `ToolTip`, swapping its `Content` only while closed,
  placing it (a per-owner placement provider, else the tab default), and detaching it on an owner
  recycle. It knows nothing about the session; it just hosts whatever element it's handed. It does NOT
  own open/close — `ToolTipService` does (post-rewrite; the old manual timers + `IsOpen` are gone, §9).

This split is why a content change is a cheap "push a new element + signature," and why all the
crashiness lives on the host side (the lifecycle), not the TerminalPage side (the content).

---

## 3. File & symbol map

**`src/cascadia/TerminalApp/AgentSessionToolTip.h` / `.cpp`** — the lifecycle, class
`AgentSessionToolTipHost` (framework-managed post-rewrite — the old manual-open members/methods listed in §9's
history are DELETED). One host per OWNER element, held in a `std::shared_ptr` (every handler it wires captures
a `weak_ptr` to it); the owner itself is held WEAKLY (its real owner — the `Tab`, the side-panel card view —
keeps it alive). No destructor touches XAML: owners call `Shutdown()` / `Detach()` on the UI thread.
- Members:
  - `_owner` (`weak_ref<FrameworkElement>`) — the `TabViewItem`, or a side-panel card `Border`.
  - `_active` — is the rich tooltip active (vs none / the tab's default)? `Active()`.
  - `_content` — the card element last handed to us by the page. `_sig` — its fingerprint (`Sig()`).
  - `_toolTip` — the ONE reused ToolTip. Framework-managed (we never drive `IsOpen`); `Content` swapped only
    while closed; **detached + nulled** on owner recycle / `Clear` / `Shutdown`. `{ nullptr }` when none
    (`Attached()`).
  - `_placementProvider` — optional; where the card opens (§4b). None = the tab default (`Bottom` under the
    owner's bounds).
  - `_lifetimeWired` / `_hoverWired` — the owner's Unloaded+Loaded pair and its PointerEntered+wheel pair are
    each wired ONCE per host. `_hoverCb` / `_wheelCb` — the page's callbacks, dropped by `Shutdown`.
  - `_swapOpenOnce` — the one-shot open-swap grant (the summary body landing in the open card).
  - `_wheelLogLabel` (`"item"` for a tab, `"card"` for a side-panel card) + the precomputed `[exc]` context
    strings, so no catch handler has to allocate.
  - *(DELETED in the rewrite: `_agentToolTipOpenTimer`, `_agentToolTipDismissTimer`, the first
    `_agentToolTipHoverWired`, and the file-scope `g_lastAgentToolTipCloseTick` / `kAgentToolTipReopenCooldownMs`
    cross-tab cooldown.)*
- Methods:
  - `Create(owner, wheelLogLabel, logContext)` — the only way to make one (`enable_shared_from_this`).
  - `SetPlacementProvider(provider)`.
  - `SetContent(content, sig, swapWhileOpen)` — the page calls this. Early-out only when the signature is
    unchanged AND a tooltip is attached (§4d-bis); else store, mark active, `Update()`.
  - `Clear()` — detach + forget the content; returns whether a rich card was active (the tab then rebuilds its
    default tooltip).
  - `Update()` — owner guard; create (if null) + `SetToolTip` + `_WireOwnerLifetime`; while CLOSED, apply the
    placement and swap `Content` (`IsOpen()` is a safe READ; we never SET it).
  - `RefreshPlacement()` — the closed-only re-anchor, run from the hover hook before the page's build.
  - `_WireOwnerLifetime()` — wire (once) the owner `Unloaded` → `Detach` and `Loaded` → `Update` (re-attach).
  - `Detach()` — `SetToolTip(owner, nullptr)` + null the ref (no `IsOpen`, no timers, no cooldown). Its catch
    LOGS (the old Tab version was bare).
  - `EnsureHoverHook(onHoverBuild, onWheel)` — wire (once per host) the owner's `PointerEntered` → re-anchor +
    the page's build-now callback **and** its `PointerWheelChanged` → the page's card-scroll callback
    (§4b-bis). Returns true only the ONE time it wires (the caller's arm-build cue). The wheel handler READS
    `IsOpen()` to gate, and marks the notch `Handled` only when the page reports the card took it.
  - `Shutdown()` — `Detach` + drop both callbacks (a tab's `Shutdown`; a side-panel card leaving the list).
  - *(DELETED: `_WireAgentToolTipHover`, `_SafeSetAgentToolTipOpen`, `_ForceCloseAgentToolTip`, `_ArmAgentToolTipDismiss`.)*

**`src/cascadia/TerminalApp/Tab.cpp` / `Tab.h`** — the first owner: `std::shared_ptr<AgentSessionToolTipHost>
_agentToolTipHost` (created lazily by `AgentToolTipHost()`, label `"item"`, context `"Tab"`), plus:
- `_UpdateToolTip()` — the entry that decides default vs rich: an active host → `host->Update()`; else build the
  default tooltip inline.
- `SetAgentToolTip(content, sig, swapWhileOpen)` / `EnsureAgentToolTipHoverHook(…)` — forward to the host.
- `ClearAgentToolTip()` — `host->Clear()`, then `_UpdateToolTip()` to bring the default tooltip back.
- `AgentToolTipSig()` / `AgentToolTipAttached()` / `AgentToolTipOpen()` / `AgentToolTipHoverWired()` — read-only.
- `Shutdown()` → `host->Shutdown()`.

**`src/cascadia/TerminalApp/TerminalPage.AgentObserver.cpp`** — the content:
- TU-local string helpers (anon namespace): `TtNowMs`, `TtSpan`, `TtStateLabel`, `TtJoin`, `TtPermIsBypass`.
- TU-local element builders: `TtFill` (brush), `TtBuildSummaryBody` (text → monospace TextBlocks + `\x1F` →
  full-width `Border` rules), `TtBuildTooltipCard` (the rich card; takes the card's `maxCardWidth`),
  `TtBuildObserveCard` (the `○ … unlinked` twin).
- `_UpdateTabAgentToolTip(tab, sessionId, swapWhileOpen, kickSummary)` — the TAB surface: guards, the
  gone/`!live` → `ClearAgentToolTip` branch, the tags parse (`AgentTagsSpec`), then the shared push.
- `_PushSessionToolTipCard(host, surfaceKey, s, tagsSpec, tagChips, roomWidth, swapWhileOpen, kickSummary,
  rehost)` — the shared orchestrator (the old body of `_UpdateTabAgentToolTip`): formats, builds, signs,
  `host.SetContent`s, adopts the scroll pieces, kicks the off-thread body loader (registering `rehost` as this
  surface's waiter).
- `_EnsureTabTooltipSummary(sessionId, codex, codexId, cwd)` — `fire_and_forget`, per SESSION: off-thread
  resolve+stat+analyze of the transcript → render the Summary box → cache → re-host every waiting surface.
- `_SetTabActivityBadge(tab, wt, kind)` — pushes the observe-badge tooltip on a non-managed tab.
- `_ArmTabAgentToolTipHover(tab)` — arms the lazy hover build **and** the wheel hook (one
  `EnsureAgentToolTipHoverHook` call, two callbacks); the hover callback also resets the scroll to the
  top of the body (only for a hover that starts closed).
- **Body scrolling** (§4b-bis): `_ScrollTabAgentToolTip(tab, delta)` (the tab's wheel entry; sets
  `_tooltipWheelClaimed`) → the shared `_ScrollSessionToolTipBody(surfaceKey, sessionId, delta, src)` →
  `_SyncTabTooltipScrollBar(surfaceKey, offset, opacity)` (the render-only apply) +
  `_ResetTabAgentToolTipScroll(surfaceKey)` / `_ArmTabTooltipScrollBarFade(surfaceKey)` /
  `_OnTabTooltipScrollBarFadeTick()` (the auto-hide). The card's pieces come back from
  `TtBuildTooltipCard`'s `TtCardScrollParts& scrollOut`.

**`src/cascadia/TerminalApp/TerminalPage.AgentSidePanel.cpp`** — the second owner (SIDE_PANEL.md §2d): one host
per card (`AgentSidePanelCardView::tipHost`, label `"card"`), `_UpdateSidePanelCardToolTip` (the card surface),
`_SidePanelArmCardToolTip`, `_OnSidePanelCardToolTipHover`, `_ScrollSidePanelCardToolTip`,
`_SidePanelCardToolTipPlacement` (the placement provider), `_SidePanelCardToolTipRoomWidth`,
`_SidePanelSuspendCardToolTip`, `_SidePanelDropCardToolTip`, `_SidePanelApplyCardTipMode`.

**`src/cascadia/TerminalApp/TerminalPage.h`** — the content cache:
- `struct _AgentTooltipSummary { std::wstring path; int64_t mtime; int64_t lastCheckMs; winrt::hstring body; }`
- `std::unordered_map<std::wstring,_AgentTooltipSummary> _tabTooltipSummary` — per-SESSION Summary cache
  (shared by every surface of the session).
- `std::unordered_set<std::wstring> _tabTooltipSummaryInFlight` — one background load per session.
- `_tabTooltipSummaryWaiters` (session → surface key → re-host callback) — who to re-host when it lands.
- `std::unordered_map<std::wstring,std::wstring> _tabTooltipSig` — last signature per SURFACE key (the session
  id for a tab; `"\x1F" "sidepanel:" <id>` for a side-panel card).
- `winrt::fire_and_forget _EnsureTabTooltipSummary(...)`.
- `struct _AgentTooltipScroll { weak viewport/content/thumb; contentShift; thumbScale; thumbShift; offset; }`
  + `std::unordered_map<std::wstring,_AgentTooltipScroll> _tabTooltipScroll` — each surface's card scroll
  state (§4b-bis), re-adopted on every rebuild; **weak** element refs so a superseded card is never pinned.
- `_tabTooltipScrollFadeTimer` / `_tabTooltipScrollFadeSession` (a surface key) / `_tabTooltipScrollFadeHolding`
  — the ONE per-window scrollbar auto-hide (hold, then step-down fade).

---

## 4. The lifecycle, end to end (CURRENT behavior — framework-managed, post-rewrite)

### 4a. Content push (page → host)
1. A SURFACE runs — for a tab, `_UpdateTabAgentToolTip(tab, sessionId, …)` (§5 lists when); for a
   side-panel card, `_UpdateSidePanelCardToolTip(id, …)`.
2. It null/registry/impl-guards, then: **if the session is gone or `!live`** → the tab calls
   `impl->ClearAgentToolTip()` + drops the cached Summary/sig (this is how an archived tab reverts to the
   default tooltip); the card calls `host->Clear()` and forgets its surface state. Return.
3. Else it resolves the tags (the tab: its `AgentTagsSpec`; the card: its card model) and calls the shared
   `_PushSessionToolTipCard(host, surfaceKey, s, …)`, which formats the header strings from `SessionInfo`
   (state + "ago" via `TtSpan`; why-needs-you; `⚠ unread`; `claude/codex · model · effort · mode`;
   folder/branch), pulls the cached Summary body (may be empty on first sight), and computes a `sig`
   (state+title+folderBranch+meta+tags+bodyMtime + the height and width budgets on a 50 px grain).
4. If `sig` changed since this surface's last push, or nothing is attached, and the card is closed (or this
   push carries the open-swap grant) → `host.SetContent(TtBuildTooltipCard(...), sig, swapWhileOpen)`.
5. With `kickSummary`, it then throttle-kicks `_EnsureTabTooltipSummary` (≤ every 4s per session,
   mtime-gated, one load per session) to load / refresh the Summary body off-thread, after registering this
   surface's re-host callback as a WAITER; on completion every waiting surface re-hosts (the body now
   differs → new sig → re-host), with the open-swap grant and `kickSummary=false`.

### 4b. Host (`SetAgentToolTip` → `SetContent` → `Update`)
- `SetContent` returns early if `sig` matches the last one AND a tooltip is attached (no XAML churn). Else
  stores content+sig, sets `_active=true`, records the one-shot open-swap grant, calls `Update`. (For a tab,
  `Tab::_UpdateToolTip` → `host->Update()` when the host is active; else it builds the default tooltip.)
- `Update`:
  1. **Owner guard**: if the owner is null / `!IsLoaded()` / no `XamlRoot()` → return (never
     build/mutate for a detached owner). The one-shot grant is consumed FIRST, even on this early-out.
  2. `if (!_toolTip)` → **create** the ONE reused `ToolTip`: `RequestedTheme(Dark)`,
     `Placement(Bottom)`, `IsHitTestVisible(false)`, `ToolTipService::SetToolTip(owner, it)`, then
     `_WireOwnerLifetime()` (no-op after the first time). From here **`ToolTipService` owns
     open/close** — hover opens (after the system delay), pointer-exit closes. We wire nothing else.
  3. If `_toolTip.IsOpen()` and no open-swap grant → return (a safe READ — do NOT swap `Content` while the
     framework is showing it). Else — while CLOSED — apply the **placement** (`_ApplyPlacement`): with no
     provider (a tab), **`PlacementRect` = the owner's own bounds** (`{0,0,ActualWidth,ActualHeight}`; a
     hover-opened AUTOMATIC tooltip otherwise places itself relative to the **pointer**, so
     `Placement(Bottom)` alone read "below the cursor" — the explicit rect anchors the card centered under
     the TAB; re-asserted each refresh since tab widths drift); with a provider (a side-panel card), the
     provider's mode + rect. Then `_toolTip.Content(_content)`.
- **The hover re-anchor (new, 2026-10-05):** the `PointerEntered` hook runs `RefreshPlacement()` (the same
  closed-only placement write) BEFORE the page's build, so a tooltip whose content did not change still
  opens at the owner's CURRENT geometry (a tab that moved, a side-panel width drag or dock flip). Before the
  extraction the tab re-anchored only when a content push re-hosted.
- **Where a Left/Right card lands** (WinUI `ToolTip::PerformNonMousePlacementWithPopup` +
  `ToolTipPositioning`): the rect is mapped to window coordinates, inflated by the 20 px default mouse
  offset on each side, the card is docked outside it and CENTERED on it along the other axis, then shifted
  back inside the window; a side with too little room is skipped for another one.

### 4b-bis. Input = the WHEEL, read on the TAB HEADER (the body scrolls)

The card's Summary body is a **viewport** onto a much taller document, scrolled by the **mouse wheel
only** (no drag, no keyboard, no touch/manipulation). The card itself takes **no input at all** — it
stays `IsHitTestVisible(false)` and inert, per invariant 6a — so the wheel is read on the owner
`TabViewItem`, which is under the cursor for the tooltip's entire life anyway:

1. The host's `EnsureHoverHook` (for a tab: `Tab::EnsureAgentToolTipHoverHook`) wires
   `PointerWheelChanged` beside the `PointerEntered` build hook (one wiring, once per host). The handler
   only ever **reads** `IsOpen()` (invariant 2) and forwards the notch to the page callback.
2. `TerminalPage::_ScrollTabAgentToolTip(tab, delta)` resolves the tab's **current** session (re-home
   safe, like the hover hook) and hands the notch to the shared `_ScrollSessionToolTipBody(surfaceKey,
   sessionId, delta, src)`, which advances that surface's stored offset by `kTtScrollStepPx` per 120 units
   and calls `_SyncTabTooltipScrollBar`. (A side-panel card's wheel callback, `_ScrollSidePanelCardToolTip`,
   calls the same function with its own surface key and `src=card`.)
3. `TerminalPage::_SyncTabTooltipScrollBar(surfaceKey, offset, opacity)` shifts the body's
   `TranslateTransform`, re-fits the slim scrollbar (`ScaleTransform` length + `TranslateTransform`
   position), paints it, and returns the max scrollable offset. **Every write is render-only** — never
   `Height`/`Margin` — because this also runs from the viewport's own `SizeChanged` (i.e. from inside a
   layout pass), where a layout write is the popup `E_LAYOUTCYCLE` fail-fast the tag badges had to be
   rewritten around (`TabHeaderControl::_PositionTagBadgesNow`).
4. The notch is marked `Handled` **only when the open card has something to scroll** — even at an end
   stop, because the card IS the scroller under the pointer — so a card whose whole body fits leaves the
   wheel to the tab strip's own horizontal scroll (or, for a side-panel card, to the card list).
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
lines at all ⇒ the wheel never reached the tab strip.** A side-panel card writes the same pair with
`card: cb=… tip=… open=…` and `src=card`.

The per-card pieces live in `TerminalPage::_tabTooltipScroll[surfaceKey]` (**weak** element refs + the
three transforms; the key is the session id for a tab, `"\x1F" "sidepanel:" <id>` for a side-panel card),
re-adopted on every card rebuild. Weak, because the card is rebuilt on every content
change — strong refs would pin every superseded tree (the leak class `AgentTipHelpers` exists to avoid).

**Two non-obvious rules keep the state and the screen in lockstep** — both fall out of `PointerEntered`
being a BUBBLING event, so it re-fires as the pointer crosses the header's own inner elements (icon →
title → close button), i.e. repeatedly *while the tip is already open*:

- **`_PushSessionToolTipCard` does not rebuild while the tip is open** (unless this push is the
  `swapWhileOpen` grant). The host's `Update` would refuse to host it anyway (invariant 3), so
  the page would end up adopting the scroll pieces of a card that is NOT on screen and the wheel would
  silently drive an unhosted tree. Nothing is lost by skipping: the next `PointerEntered` rebuilds
  fresh, and the staged card's "ago" line would have gone stale by then regardless.
- **The scroll reset only runs on a hover that starts CLOSED.** Otherwise every mouse nudge inside the
  tab header would snap the reader back to the top of the body.

Both gate on the host's `Open()` (`Tab::AgentToolTipOpen()` for a tab) — a safe **read** of `IsOpen`
(invariant 2 forbids writing it).

### 4c. Open / close = the FRAMEWORK
There are no open/dismiss timers, no pointer-loss handlers, no manual `IsOpen`, and no cross-tab
cooldown anymore — `ToolTipService` opens the tip on hover (at the system hover delay) and closes it on
pointer-exit / window-deactivate, exactly as it does for the plain default tab tooltip (which never
crashed). We only ever *read* `IsOpen()` (to gate the content swap); we never *set* it. This is what
removes the entire crash class (§9) — there is no manually-opened popup, and no reused object we drive
across an open/close cycle.

### 4d. Detach = drop (`Detach`, formerly `_DetachAgentToolTip`)
Runs from the owner `Unloaded` handler (§4e), `Clear` (session gone), `Shutdown` (a tab's `Shutdown`; a
side-panel card leaving its list), and — side-panel cards only — a tag ribbon's hover
(`_SidePanelSuspendCardToolTip`, SIDE_PANEL.md §2b). It:
1. Returns early if `!_toolTip` (nothing of ours attached — leaves any default tooltip alone).
2. `ToolTipService::SetToolTip(owner, nullptr)` (**detach** from the owner; try/catch that LOGS — Rule #18).
3. `_toolTip = nullptr` (**drop** our strong ref).

It drives **no `IsOpen`** — the framework closes any open popup itself on exit/recycle
(`UnregisterToolTip`) — so this path can't fail-fast. Dropping the ref means the next `Update` rebuilds a
FRESH tooltip bound to the live (reloaded) owner (the crash #4 lesson, kept without the manual machinery).

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
- owner `Loaded → Update()` when active-but-unattached (the missing other half of the Unloaded detach),
  so a recycled tab is re-armed the moment it re-enters the tree (now `_WireOwnerLifetime`, per host).
- `SetContent`'s early-out (formerly `SetAgentToolTip`'s) also requires `_toolTip` to exist, so a
  same-signature push still re-attaches.
- `_PushSessionToolTipCard` (formerly `_UpdateTabAgentToolTip`) pushes when `!host.Attached()` even if the
  signature is unchanged. The side-panel card's tag-ribbon detach relies on exactly this: the next hover
  over the card's body re-attaches it.

**Lesson to keep:** for a service-owned tooltip, *attachment* — not content, not `IsOpen` — is the
state that determines whether it can ever open. Anything that detaches (recycle, `ClearAgentToolTip`)
must have a matching re-attach that runs **outside** a dwell.

### 4e. Owner-recycle safety (`_WireOwnerLifetime`, wired once per host)
MUX `TabView` virtualizes/recycles its item containers (heavy during a multi-tab window restore), which
tears down the ToolTip's native peer while our WinRT strong ref keeps resolving non-null — a zombie. If
the SAME owner later RELOADS, reusing that stale ref to swap `.Content()` would read freed memory (crash
#4). So the owner's `Unloaded → Detach()`: on unload we detach + drop, and the owner's `Loaded → Update()`
(§4d-bis) re-attaches a fresh one. The `Update` owner-loaded guard is the complementary front door (never
build/mutate for a detached owner).

A side-panel card is not recycled by a virtualizing panel, but it does leave the visual tree (a list
reset, a removal) and is then DROPPED for good. `Unloaded` is a deferred event that may
run after the card view (and its host) is gone, so the side panel never relies on it alone: it calls
`Shutdown()` itself before a card leaves the list (`_SidePanelDropCardToolTip`). A `ToolTip` left attached
to a dropped element is the islands leak class (AgentTipHelpers.h). A card MOVED inside the list keeps its
tooltip: `UIElementCollection::Move` raises no `Unloaded` / `Loaded`.

---

## 5. Who calls what (invocation points)

The tooltip is **LAZY**: a live session's card is built on HOVER, never per tick or per registry
notify (the CPU fix — with 19+ active sessions the per-notify rebuild was a steady UI-thread burn).
`_UpdateTabAgentToolTip(tab, sessionId, swapWhileOpen, kickSummary)` is called from (all
`TerminalPage.AgentObserver.cpp`):
1. **The hover hook** (`_ArmTabAgentToolTipHover`'s `PointerEntered` callback) — the build with fresh
   state + a fresh "ago", kicking the mtime-gated Summary load; the scroll resets to the top only for a
   hover that starts closed.
2. **The arm-build** — the first time `_ArmTabAgentToolTipHover` wires a tab, a HEADER-ONLY card
   (`kickSummary=false`) so the first hover finds a tooltip attached.
3. **The Summary arrival** — `_EnsureTabTooltipSummary` re-hosts every waiting surface; the tab's waiter
   calls `_UpdateTabAgentToolTip(t, id, swapWhileOpen=true, kickSummary=false)`.
4. **On bind** (`_BindClaudeSessionToTab`) — replace any `○ … unlinked` observe tooltip with the rich
   managed HEADER card the instant a session binds (`kickSummary=false`), then arm the hover.
5. **A tag toggle** — the tag-chip row rides the tab's `AgentTagsSpec` (sig-gated, `kickSummary=false`).
6. **The `!live` edge** of the registry-observer push (`_UpdateTabAgentDot`) — the gone/archived branch
   clears to the default tooltip and drops the cached Summary.

The **liveness sweep** (`_SweepClaudeLivenessImpl`, ~2.5 s) no longer builds anything: it only re-arms the
hover hook for a tab that entered `_claudeTabs` without passing the bind funnel (a window-restored dormant
tab, a re-home) — one bool probe per tab per tick.

The rich card reaches the tab's host directly: `_PushSessionToolTipCard` calls `SetContent` on the host
`impl->AgentToolTipHost()` returns. `Tab::SetAgentToolTip` is left with `_SetTabActivityBadge` (the observe
card). `ClearAgentToolTip` is called from `_UpdateTabAgentToolTip`'s gone/`!live` branch and the liveness
sweep's dead-tab branch. The side-panel card surface has its own call sites
(SIDE_PANEL.md §2d): its hover hook, its arm-build, its Summary waiter, and the "Sum" mode switch.

**Threading:** every one of these runs on the window's UI thread. The observer/registry callbacks
that originate off-thread marshal onto the UI thread (`resume_foreground` / `DispatcherQueue`) BEFORE
touching the tooltip. The `Tab` forwarders assert `ASSERT_UI_THREAD()`; the host's own entries run from
them or from XAML events of the owner's thread. Never call tooltip code off the UI thread.

---

## 6. The content builders (`Tt*`)

- `TtBuildTooltipCard(accent, title, folderBranch, stateText, metaText, bodyText, …, maxCardHeight,
  maxCardWidth, scrollOut)` → a dark `Border` (bg `#FF202020`, 1px `#40FFFFFF` border, corner radius 4,
  padding 10/8, **MaxWidth = `maxCardWidth`**: the flat `kTtCardMaxWidth` = 660 for a tab, the room beside
  the panel clamped to [`kTtCardMinWidth` 320, 660] for a side-panel card; `<= 0` = 660) wrapping a vertical
  `StackPanel`:
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
  spends 100 per side). `kTagLineBudget` (the tag-row greedy packer) is derived from the card's ACTUAL
  budget (`maxCardWidth`), so it can't drift. A side-panel card passes the room beside the panel instead
  (`_SidePanelCardToolTipRoomWidth`, clamped to [320, 660]); the width rides the re-host signature on the
  same 50px grain as the height.
- ⚠ **The `ToolTip` caps every card at 320 px.** WinUI 2.8's ToolTip style
  (`microsoft-ui-xaml` release/2.8, `dev/CommonStyles/ToolTip_rs5_themeresources.xaml`, loaded by the app's
  `XamlControlsResources ControlsResourcesVersion="Version2"`) sets `MaxWidth = ToolTipMaxWidth = 320` and
  `Padding = 9,6,9,8` on the `ToolTip` itself, and the template's `ContentPresenter` takes
  `MaxWidth="{TemplateBinding MaxWidth}"`. Nothing in this app sets a ToolTip `MaxWidth`, so — per that
  source — the card renders at most ~300 px wide on a tab and on a side-panel card alike, and the
  460 → 660 change above never showed. Found 2026-10-05 by reading the source; not yet measured on
  screen. If the wider card is wanted, set the host's `ToolTip.MaxWidth` locally (a local value outranks
  the style setter) — that is a visible change to every tab's card, so it is a decision, not a fix to
  slip in. Note that `kTagLineBudget` is sized for the budget, not the cap, so a long tag row can run past
  the visible width.
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

- `fire_and_forget`, UI → worker → UI, per SESSION (`_EnsureTabTooltipSummary(sessionId, codex, codexId,
  cwd)` — it no longer takes a tab). Resolves the transcript path, `GetFileAttributesExW` for the mtime,
  and — only when the mtime grew (or first sight) — `AnalyzeSessionTranscript` + `RenderSessionSummaryBox`
  (Claude) / `ReadCodexRolloutInfo` + `RenderCodexSummaryBox` (Codex) on the **worker thread**, then
  marshals back, stores into `_tabTooltipSummary[id]`, takes the session's entry out of
  `_tabTooltipSummaryWaiters`, and calls each waiting surface's re-host callback (the tab's re-calls
  `_UpdateTabAgentToolTip`, a side-panel card's `_UpdateSidePanelCardToolTip`), each inside its own
  try/catch (one failed surface never costs the others), all with the open-swap grant and
  `kickSummary=false`.
- **mtime-gated** (a quiet tab costs one stat) + **in-flight-guarded** (`_tabTooltipSummaryInFlight`,
  one load per session, however many surfaces wait) + **throttled** by the caller (`lastCheckMs`,
  ≤ every 4s). A surface that asks while a check is DUE (no body yet, or the last check is > 4 s old)
  registers as a waiter even when a load is already in flight, so hovering the side-panel card while the
  tab's first load runs (or the reverse) still gets the body. A surface that asks within 4 s of a check
  that already has a body shows that body; its next hover re-checks.
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
3. **Reuse ONE object; swap `Content` only while closed.** Create the host's `_toolTip` once and
   `SetToolTip` once; on refresh, `if (_toolTip.IsOpen() && !swapOpenOnce) return;` before `.Content(...)` —
   swapping under the pointer flickers, and re-`SetToolTip`ing every refresh would replace the framework's
   open tip. Placement is a closed-only write too (`_ApplyPlacement` / `RefreshPlacement`).
4. **Never build/mutate the tooltip for a detached owner.** Keep the `IsLoaded()`+`XamlRoot()` guard at
   the top of the host's `Update` (and of `RefreshPlacement`).
4a. **Every detach needs a re-attach that runs OUTSIDE a dwell.** A tooltip that is not attached cannot
   open, and one attached mid-hover does not open for that hover (§4d-bis). Keep the owner
   `Loaded → Update` re-host (`_WireOwnerLifetime`), and keep `host.Attached()` in the push conditions —
   a signature-only gate will silently leave recycled tabs (and suspended side-panel cards) tooltip-less.
5. **Detach + drop on owner recycle.** Keep the owner `Unloaded → Detach` handler
   (`SetToolTip(owner, nullptr)` + null the ref). Without it, a recycle→reload reuses a zombie ref on the
   next `.Content()` swap — exactly crash #4. `Detach` drives NO `IsOpen` (the framework closes any open
   popup itself), so it can't fail-fast.
5a. **An owner that can be DROPPED shuts its host down first.** `Unloaded` is deferred and may run after
   the owner's view is gone, and a `ToolTip` attached to a dropped element is the islands leak class — so
   the side panel calls `Shutdown()` (`_SidePanelDropCardToolTip`) before a card leaves its list or the list
   resets, and `Tab::Shutdown` does the same for a tab. A new owner must do the same.
5b. **Never put an `AgentTipHelpers` floating tip on an element that has a host.** That host attaches its
   own tip to the hovered element on open and detaches it on close — it would REPLACE the host's
   `ToolTip`, then remove it. Silence the element's floating tips first (`AgentSuppressTip`, which keeps
   the "already wired" marker so they can be given back), as the side panel's "Sum" OFF mode does.
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
9. **The signature gate is a correctness feature, not just perf.** `SetContent` no-ops on an unchanged
   `sig` while a tooltip is attached, so every hover, the bind, a tag toggle and the Summary arrival can
   all re-assert without XAML churn. Keep the sig covering everything the card renders
   (state/title/folderBranch/meta/tags/bodyMtime + the height and width budgets), and keep it per SURFACE
   (`_tabTooltipSig[surfaceKey]`) — two surfaces of one session host two different cards.
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

- Rich card built: the shared `TerminalPage::_PushSessionToolTipCard` (`TerminalPage.AgentObserver.cpp`), called
  by the tab surface `_UpdateTabAgentToolTip` and the side-panel card surface `_UpdateSidePanelCardToolTip`.
- Hosted + lifecycle (framework-managed): `AgentSessionToolTipHost` (`AgentSessionToolTip.{h,cpp}`) —
  `SetContent` / `Update` / `_WireOwnerLifetime` / `Detach` / `EnsureHoverHook` / `Shutdown`; `Tab` forwards
  (`SetAgentToolTip`, `ClearAgentToolTip`, `EnsureAgentToolTipHoverHook`). Second owner: the side-panel card
  ([`SIDE_PANEL.md`](SIDE_PANEL.md) §2d).
- The one rule: `ToolTipService` owns open/close — we never drive `IsOpen`; swap `Content` only while closed.
- Body scrolling (§4b-bis): wheel-only, read on the OWNER (the host's `PointerWheelChanged`) →
  `TerminalPage::_ScrollTabAgentToolTip` / `_ScrollSidePanelCardToolTip` → the shared `_ScrollSessionToolTipBody`
  → `_SyncTabTooltipScrollBar` (render-only writes: a `TranslateTransform` on the body, a
  `ScaleTransform`+`TranslateTransform` on the slim bar, which auto-hides a couple of seconds after the last
  notch). The card stays inert and hit-test-invisible — still NO ScrollViewer (invariants 6a/6b).
- Owner-recycle safety: the owner `Unloaded → Detach` / `Loaded → Update` pair + the `Update` owner-loaded
  guard; a droppable owner calls `Shutdown` first (invariant 5a).
- Off-thread body: `_EnsureTabTooltipSummary` (per session) + `_tabTooltipSummary` / `_tabTooltipSummaryWaiters` /
  `_tabTooltipSig` (per surface) / `_tabTooltipSummaryInFlight`.
- Shared palette: `AgentStatusColorFor` (`AgentStatusColors.h`).
- Commits: `918b720b7`, `44beaea7e`, `be1d63b7f`, `9792161b8` (observer nets), `6601c7532`,
  `1d1edbe18`, `9c026822d` (the six manual-open crashes) → the framework-managed rewrite (this change).
- Diagnostics: the `debug-dumps` skill + `tools/dumpexc|dumpourscan|dumpwalk2|hangwalk`.
