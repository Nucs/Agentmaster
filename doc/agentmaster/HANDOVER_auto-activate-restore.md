# Handover — Auto-activate restored tabs (gradual background ramp)

> **Status:** designed + once fully implemented, then **reverted** (a concurrent agent was editing
> the same files and held source locks mid-edit — see §9). This document is the complete,
> implementation-ready design so it can be re-applied cleanly. Nothing of this feature is currently
> in the tree.
>
> **Owner of the foundation:** commit `e340621fb` *"feat(triage): half-hollow dormant dot + Activate
> Tab / Activate All Tabs (eager-init)"* — deployed + verified live on the dev instance. This feature
> sits **directly on top of it** and reuses its actuator.

---

## 1. Requirement (verbatim)

> *"Can we have the restoration be handled by a background thread or interval on ui thread (if a must)
> that activates 4 tabs at a time, with offset of 500ms, every 10 seconds. Make 4 and every 10
> configurable in settings."*

**Interpretation (the cadence):** a reopened window's managed tabs come back **dormant** (their
`claude.exe` hasn't started). A gentle background ramp wakes them so the workspace resumes itself
*without a spawn storm*:

- **Batch** = `4` tabs (configurable), each activation **staggered 500 ms** apart within the batch
  (tab 1 at t≈0, tab 2 at +500 ms, tab 3 at +1000 ms, tab 4 at +1500 ms).
- **Interval** = a new batch **every `10` s** (configurable).
- The **500 ms** intra-batch stagger is **fixed** (only `4` and `10` are configurable, per the ask).
- A fleet of 20 dormant tabs therefore wakes over ~50 s (5 batches), 4 at a time.

**"background thread or interval on UI thread (if a must)":** it **is** a must — the actuator
(`TermControl::InitializeWithSize` → `ControlCore::Initialize` + the DXGI swap chain) is
**UI-thread-only**. A background thread would have to marshal *every* activation onto the UI thread
anyway, so a UI-thread `DispatcherTimer` is the correct and simplest tool. Use it.

---

## 2. Why this is needed (the lazy-restore problem)

A Windows Terminal background tab spawns its child process **lazily** — only when the tab's
`SwapChainPanel` gets its first non-zero layout, which happens when the tab is first **shown**. So a
window-restored / re-homed managed tab (created by `TerminalPage::_RestoreWindowTabs`) sits
**dormant**: no `claude.exe`, no hooks, no autopilot, until the user clicks it. The
`e340621fb` work made that state legible (half-hollow status dot) and manually fixable (**Activate
Tab** / **Activate All Tabs (N)**). **This feature automates that** — a throttled auto-activate so a
reopened workspace comes back alive on its own.

**Rule #6 is preserved.** A *fresh app launch* opens to just the Manager tab (everything archived,
nothing restored — `CLAUDE.md` Correctness Rule #6). There are no dormant tabs at fresh startup, so
the ramp **never runs there**. It only fires on a **window REOPEN** (the toolbar *Reopen Windows*
button, or the Emperor's startup multi-window reopen), where the user is explicitly bringing a saved
workspace back and wants its sessions live. This is why defaulting it **ON** does not violate the
"startup archives, never auto-launches" invariant.

---

## 3. Foundation already in the tree (from `e340621fb`) — DO NOT rebuild these

The ramp is pure orchestration over primitives that already exist and are deployed:

| Symbol | File | What it does |
|---|---|---|
| `bool TermControl::InitializeWithSize(double w, double h, float scale)` | `TerminalControl/TermControl.{idl,h,cpp}` | Eager-init a dormant control **in place** with a placeholder size (AV-safe `_core.Initialize()`→`conn.Start()` order; the real size self-corrects via `_SwapChainSizeChanged` when shown). Returns `true` if it started one. |
| `bool TerminalPage::_ActivateDormantSession(const std::wstring& id)` | `TerminalPage.AgentObserver.cpp` | The actuator the ramp calls per tab. Resolves the control via `_ControlForSession(id)`, no-ops if not `NotConnected`, else `InitializeWithSize(_tabContent size)` + `SetStarted(id,true)`. **Returns `false` on failure** (see the loop-guard, §4.3). |
| `int TerminalPage::_ActivateAllDormantTabsLocal()` | `TerminalPage.AgentObserver.cpp` | Wakes ALL of this window's dormant tabs at once. The ramp does **not** call this (it throttles instead) — listed so you don't confuse the two. |
| `winrt::…::TermControl _ControlForSession(const std::wstring& id)` | `TerminalPage.AgentObserver.cpp` | `_claudeTabs[id]` → tab → first `TermControl`, or null. |
| `SessionInfo.started` (transient) + `SessionRegistry::SetStarted(id,bool)` | `AgentMaster/SessionModels.h`, `SessionRegistry.{h,cpp}` | "Has this session's control left `NotConnected`." Drives the half-hollow dot + the Activate-All count. Maintained by the ~2s liveness sweep + `_TrackSessionStarted`. The ramp's `_ActivateDormantSession` already flips it. |
| `_claudeTabs` (`std::unordered_map<wstring, weak<Tab>>`) | `TerminalPage.h` | This window's `sessionId → tab` map (Claude **and** Codex). The ramp enumerates it to find dormant tabs. |
| `_RestoreWindowTabs()` | `TerminalPage.AgentWindowRecord.cpp` | Re-homes a reopened window's tabs (dormant). **The ramp's start hook goes at the END of this.** |

**Dormant predicate (the stable signal):**
`control && control.ConnectionState() == winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::NotConnected`.
`NotConnected` ⟺ never started. This is already trusted by `_JumpEligibilityInSession`
(`AgentObserver.cpp`) and the liveness sweep.

---

## 4. Design

### 4.1 Two UI-thread `DispatcherTimer`s, per window

Each `TerminalPage` (window) owns its own ramp — controls live on their window's UI thread, so a tab
can only be woken in the window that hosts it.

- **`_autoActivateBatchTimer`** — interval = `restoreActivateIntervalSeconds` (10 s). On each tick it
  **starts a batch** (sets the budget = `restoreActivateBatchSize`, then drains the first immediately).
- **`_autoActivateStaggerTimer`** — interval = **500 ms (fixed)**. While a batch is draining it wakes
  **one** more dormant tab per tick until the batch budget is spent, then stops itself (the batch
  timer fires the next batch 10 s later).

So per 10 s window: ≤4 tabs woken at 0 / 0.5 / 1.0 / 1.5 s, then idle until the next batch tick.

### 4.2 First batch is immediate

`_StartAutoActivateRestored()` starts the batch timer **and** calls `_OnAutoActivateBatchTick()` once
right away, so the ramp visibly begins within ~500 ms of restore rather than waiting a full 10 s.
Subsequent batches are timer-driven (every 10 s). *(Decision — see §7.)*

### 4.3 Loop-guard — `_autoActivateTried` (REQUIRED, not optional)

`_ActivateDormantSession` returns **`false`** if `InitializeWithSize` fails (e.g. an off-tree
swap-chain hiccup, §8.1). A failed tab **stays `NotConnected`**, so a naïve "find next dormant" would
return the *same* tab forever → an infinite 500 ms loop hammering one broken tab.

**Fix:** a per-window `std::unordered_set<std::wstring> _autoActivateTried`. `_AutoActivateDrainStep`
inserts the id **before** activating; `_NextDormantSessionId()` **skips** tried ids. So each tab is
attempted at most once per ramp; a failure is left dormant (the user can still click it / Activate
Tab). Cleared at each `_StartAutoActivateRestored()` (a new reopen is a fresh attempt).

### 4.4 Stop conditions

The ramp self-terminates (`_StopAutoActivateRestored()` stops both timers) when:
- `_NextDormantSessionId()` is empty (the fleet is awake / all remaining were tried), or
- `autoActivateRestoredTabs` is turned **OFF** live (checked each batch tick + `_OnAutoActivateSettingsChanged`).

It is restarted on the next `_RestoreWindowTabs` (another reopen) or by `_OnAutoActivateSettingsChanged`
if re-enabled while dormant tabs exist.

### 4.5 Composition with the manual paths

`_ActivateDormantSession` is idempotent. If the user manually clicks a tab or hits **Activate All**
mid-ramp, those tabs leave the dormant set (or join `_autoActivateTried`), and the ramp simply skips
them. No coordination needed.

### 4.6 Multi-window note

Each reopened window ramps independently → aggregate spawn rate is `batchSize × (reopened windows)`
per interval. For a 3-window reopen at the defaults that's 12 spawns / 10 s — a moderate ramp. If
that's too much, the user lowers the batch size. (Cross-window global throttling is **out of scope**;
per-window is the natural fit and matches "4 tabs at a time" without a global coordinator.)

---

## 5. Settings (3 new `AppSettings` fields)

`4` and `10` are the required configurables; a **master on/off** is added so the behavior can be
disabled without a sentinel (default **ON** — the user asked for the feature).

### 5.1 `AgentMaster/SessionModels.h` — `struct AppSettings`, after `recentDirsLimit`

```cpp
// Agentmaster (auto-activate restored tabs): a reopened window's managed tabs come back DORMANT —
// each tab's claude spawns lazily, only when the tab is first SHOWN (WT lazy background tabs) — so
// a restored workspace sits idle (no claude, no hooks, no autopilot) until you click each tab. When
// ON (default), each window runs a gentle background RAMP after restore that eager-inits its dormant
// tabs IN PLACE: `restoreActivateBatchSize` tabs every `restoreActivateIntervalSeconds` seconds, each
// staggered 500ms apart — so the whole fleet resumes itself without a spawn storm. OFF leaves them
// dormant until you Activate them (the half-hollow dot + Activate Tab / Activate All Tabs). This only
// affects window REOPENS (a fresh app launch opens to just the Manager, Rule #6 — nothing to ramp).
bool autoActivateRestoredTabs{ true };
// Tabs woken per batch. 0 / garbage falls back to 4. (The 500ms intra-batch stagger is fixed.)
uint32_t restoreActivateBatchSize{ 4 };
// Seconds between batches. 0 / garbage falls back to 10.
uint32_t restoreActivateIntervalSeconds{ 10 };
```

### 5.2 `AgentMaster/Persistence.cpp` — JSON round-trip

In `ToJson(AppSettings)` next to `recentDirsLimit` (helpers: `json::Value::MkBool` / `MkNum`):

```cpp
o.Set(L"autoActivateRestoredTabs", json::Value::MkBool(s.autoActivateRestoredTabs));
o.Set(L"restoreActivateBatchSize", json::Value::MkNum(s.restoreActivateBatchSize));
o.Set(L"restoreActivateIntervalSeconds", json::Value::MkNum(s.restoreActivateIntervalSeconds));
```

In `AppSettingsFromJson` next to `recentDirsLimit` (helpers: `BoolAt` / `U32At`):

```cpp
s.autoActivateRestoredTabs = v.BoolAt(L"autoActivateRestoredTabs", true); // absent => ON
s.restoreActivateBatchSize = v.U32At(L"restoreActivateBatchSize", 4);
s.restoreActivateIntervalSeconds = v.U32At(L"restoreActivateIntervalSeconds", 10);
```

> ⚠️ `Persistence.cpp` is heavily edited by other work — locate the `recentDirsLimit` anchors by
> content (grep), not line number.

### 5.3 Cog UI (`AgentManagerContent.{h,cpp}`)

Mirror the existing numeric/toggle settings exactly (`_setServerCache` / `_setRecentDirsLimit` =
`TextBox`; `_setShowTabCloseButton` = `ToggleSwitch`). The cog is six top-tab panels; put these in
**`behaviorPanel`** (the "BEHAVIOR" group, near `_setServerCache`).

**Members** (`AgentManagerContent.h`, beside `_setServerCache` etc.):
```cpp
winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setAutoActivateRestored{ nullptr };
winrt::Windows::UI::Xaml::Controls::TextBox _setRestoreBatchSize{ nullptr };
winrt::Windows::UI::Xaml::Controls::TextBox _setRestoreInterval{ nullptr };
```

**Build** (in the settings-form builder, behaviorPanel, after `_setServerCache` is appended):
```cpp
_setAutoActivateRestored = ToggleSwitch{};
_setAutoActivateRestored.Header(winrt::box_value(L"Auto-activate restored tabs"));
AgentSetTip(_setAutoActivateRestored, L"When a window reopens, gradually start its restored Claude/Codex sessions in the background instead of waiting until you click each tab.");
panel.Children().Append(_setAutoActivateRestored);

_setRestoreBatchSize = TextBox{};
_setRestoreBatchSize.Header(winrt::box_value(L"…tabs at a time"));
_setRestoreBatchSize.PlaceholderText(L"4");
AgentSetTip(_setRestoreBatchSize, L"How many restored tabs to wake per batch (each staggered 500ms apart). Blank/0 resets to 4.");
panel.Children().Append(_setRestoreBatchSize);

_setRestoreInterval = TextBox{};
_setRestoreInterval.Header(winrt::box_value(L"…every N seconds"));
_setRestoreInterval.PlaceholderText(L"10");
AgentSetTip(_setRestoreInterval, L"Seconds between batches. Blank/0 resets to 10.");
panel.Children().Append(_setRestoreInterval);
```

**Load** (in `_ShowSettings` / the form-populate section, next to `_setServerCache.Text(...)`):
```cpp
if (_setAutoActivateRestored) { _setAutoActivateRestored.IsOn(_appSettings.autoActivateRestoredTabs); }
if (_setRestoreBatchSize)     { _setRestoreBatchSize.Text(winrt::hstring{ std::to_wstring(_appSettings.restoreActivateBatchSize) }); }
if (_setRestoreInterval)      { _setRestoreInterval.Text(winrt::hstring{ std::to_wstring(_appSettings.restoreActivateIntervalSeconds) }); }
```

**Save** (in the Save handler, next to the `_setServerCache` digit-parse; the codebase parses digits
manually rather than `std::stoul`):
```cpp
if (_setAutoActivateRestored) { _appSettings.autoActivateRestoredTabs = _setAutoActivateRestored.IsOn(); }
if (_setRestoreBatchSize)     { _appSettings.restoreActivateBatchSize     = ParseU32OrDefault(_setRestoreBatchSize.Text(), 4); }
if (_setRestoreInterval)      { _appSettings.restoreActivateIntervalSeconds = ParseU32OrDefault(_setRestoreInterval.Text(), 10); }
```
…where `ParseU32OrDefault` is the same blank/0/garbage→default digit loop used for `serverCacheMinutes`
/ `recentDirsLimit` (inline it if there's no shared helper).

---

## 6. `TerminalPage` wiring

### 6.1 `TerminalPage.h` — members + declarations (after `_promptNavRefreshTimer`)

```cpp
// Agentmaster (auto-activate restored tabs; AppSettings.autoActivateRestoredTabs): a per-window
// gradual ramp that eager-inits this window's DORMANT restored tabs after a window reopen. Two
// UI-thread DispatcherTimers (activation is UI-thread-only): _autoActivateBatchTimer fires every
// restoreActivateIntervalSeconds and starts a batch; _autoActivateStaggerTimer drains up to
// restoreActivateBatchSize tabs, one per 500ms. Self-stops when no dormant tabs remain.
winrt::Windows::UI::Xaml::DispatcherTimer _autoActivateBatchTimer{ nullptr };
winrt::Windows::UI::Xaml::DispatcherTimer _autoActivateStaggerTimer{ nullptr };
int _autoActivateBatchRemaining{ 0 };                 // tabs left in the CURRENT batch
std::unordered_set<std::wstring> _autoActivateTried;  // ids attempted this run (loop-guard, §4.3)
void _StartAutoActivateRestored();
void _StopAutoActivateRestored();
void _OnAutoActivateBatchTick();
void _OnAutoActivateStaggerTick();
void _AutoActivateDrainStep();
void _OnAutoActivateSettingsChanged();
std::wstring _NextDormantSessionId();
```
*(`<unordered_set>` is already included by TerminalPage.h.)*

### 6.2 The ramp implementation — `TerminalPage.AgentObserver.cpp` (next to `_ActivateDormantSession`)

```cpp
// First dormant managed session (Claude or Codex) hosted in THIS window not yet tried, or empty.
std::wstring TerminalPage::_NextDormantSessionId()
{
    for (const auto& [id, weakTab] : _claudeTabs)
    {
        if (_autoActivateTried.count(id)) { continue; }
        const auto control = _ControlForSession(id);
        if (control && control.ConnectionState() == TerminalConnection::ConnectionState::NotConnected)
        {
            return id;
        }
    }
    return {};
}

// Begin (or restart) this window's gradual ramp. No-op when disabled or nothing is dormant.
void TerminalPage::_StartAutoActivateRestored()
{
    if (!_appSettings.autoActivateRestoredTabs) { return; }
    if (_NextDormantSessionId().empty()) { return; }
    _autoActivateTried.clear();
    const uint32_t sec = _appSettings.restoreActivateIntervalSeconds ? _appSettings.restoreActivateIntervalSeconds : 10;
    if (!_autoActivateBatchTimer)
    {
        _autoActivateBatchTimer = WUX::DispatcherTimer{};
        _autoActivateBatchTimer.Tick([weak = get_weak()](const IInspectable&, const IInspectable&) {
            if (auto self = weak.get()) { self->_OnAutoActivateBatchTick(); }
        });
    }
    _autoActivateBatchTimer.Interval(std::chrono::seconds(static_cast<int64_t>(sec)));
    if (!_autoActivateStaggerTimer)
    {
        _autoActivateStaggerTimer = WUX::DispatcherTimer{};
        _autoActivateStaggerTimer.Interval(std::chrono::milliseconds(500)); // fixed stagger
        _autoActivateStaggerTimer.Tick([weak = get_weak()](const IInspectable&, const IInspectable&) {
            if (auto self = weak.get()) { self->_OnAutoActivateStaggerTick(); }
        });
    }
    ::Agentmaster::AppendStateLog(L"hooks.log", L"[auto-activate] window " + _windowId + L" ramp start (batch="
        + std::to_wstring(_appSettings.restoreActivateBatchSize ? _appSettings.restoreActivateBatchSize : 4)
        + L" every " + std::to_wstring(sec) + L"s)\n");
    _autoActivateBatchTimer.Start();
    _OnAutoActivateBatchTick(); // first batch now (don't wait a full interval)
}

void TerminalPage::_StopAutoActivateRestored()
{
    if (_autoActivateStaggerTimer) { _autoActivateStaggerTimer.Stop(); }
    if (_autoActivateBatchTimer)   { _autoActivateBatchTimer.Stop(); }
    _autoActivateBatchRemaining = 0;
}

void TerminalPage::_OnAutoActivateBatchTick()
{
    if (!_appSettings.autoActivateRestoredTabs || _NextDormantSessionId().empty())
    {
        _StopAutoActivateRestored(); // disabled or fully drained
        return;
    }
    const uint32_t bs = _appSettings.restoreActivateBatchSize ? _appSettings.restoreActivateBatchSize : 4;
    _autoActivateBatchRemaining = static_cast<int>(bs);
    _AutoActivateDrainStep(); // wake the first now; the stagger timer drains the rest
}

void TerminalPage::_OnAutoActivateStaggerTick()
{
    _AutoActivateDrainStep();
}

void TerminalPage::_AutoActivateDrainStep()
{
    const auto id = _NextDormantSessionId();
    if (id.empty()) { _StopAutoActivateRestored(); return; } // fleet awake -> done
    _autoActivateTried.insert(id);   // mark BEFORE activating: a failed init must not loop the ramp
    _ActivateDormantSession(id);
    _autoActivateBatchRemaining--;
    if (_autoActivateBatchRemaining > 0 && !_NextDormantSessionId().empty())
    {
        if (_autoActivateStaggerTimer && !_autoActivateStaggerTimer.IsEnabled())
        {
            _autoActivateStaggerTimer.Start(); // more in this batch -> keep staggering at 500ms
        }
    }
    else
    {
        if (_autoActivateStaggerTimer) { _autoActivateStaggerTimer.Stop(); } // batch spent
        if (_NextDormantSessionId().empty()) { _StopAutoActivateRestored(); } // nothing left at all
    }
}

// Live AppSettings change (cog Save / cross-window broadcast): disable => stop; interval => re-arm.
void TerminalPage::_OnAutoActivateSettingsChanged()
{
    if (!_appSettings.autoActivateRestoredTabs) { _StopAutoActivateRestored(); return; }
    if (_autoActivateBatchTimer && _autoActivateBatchTimer.IsEnabled())
    {
        const uint32_t sec = _appSettings.restoreActivateIntervalSeconds ? _appSettings.restoreActivateIntervalSeconds : 10;
        _autoActivateBatchTimer.Interval(std::chrono::seconds(static_cast<int64_t>(sec)));
    }
    else if (!_NextDormantSessionId().empty())
    {
        _StartAutoActivateRestored(); // just enabled with dormant tabs present
    }
}
```

> Idiom notes (all verified against the existing tree):
> - `WUX::DispatcherTimer` — `WUX` alias is already in `AgentObserver.cpp` (`_agentFlashTimer = WUX::DispatcherTimer{}`).
> - `.Tick([weak = get_weak()](const IInspectable&, const IInspectable&){…})` — the existing flash-timer pattern.
> - `std::chrono::seconds/milliseconds` implicitly convert to `Windows::Foundation::TimeSpan` for `.Interval()` (`<chrono>` is in scope).
> - `TerminalConnection::ConnectionState::NotConnected` — that alias is in scope in this TU.

### 6.3 Call sites

1. **Start the ramp** — the **last** statement of `TerminalPage::_RestoreWindowTabs()`
   (`TerminalPage.AgentWindowRecord.cpp`), after the tabs are re-homed:
   ```cpp
   _StartAutoActivateRestored(); // Agentmaster: gradually wake the just-restored dormant tabs
   ```
   `_RestoreWindowTabs` runs on the UI thread from `_OnFirstLayout` (gated on a claimed record), so
   the dormant Claude/Codex tabs already exist in `_claudeTabs` when this fires. Shell ("Other") tabs
   aren't in `_claudeTabs`, so the ramp ignores them — correct.

2. **Live-apply** — call `_OnAutoActivateSettingsChanged();` right after `_appSettings` is assigned in
   **both** settings-apply paths in `TerminalPage.AgentEngine.cpp`:
   - the cog Save handler `SetSettingsHandler` (after `self->_appSettings = s;`), and
   - `_ApplyBroadcastSettings` (after `_appSettings = settings;`).

3. **Teardown** — in `~TerminalPage` (or the existing engine-detach cleanup), `_StopAutoActivateRestored();`
   alongside the other `*Timer.Stop()` calls.

---

## 7. Decisions taken (defaults; change if the product owner disagrees)

| Decision | Choice | Rationale |
|---|---|---|
| Master on/off toggle | **Add it, default ON** | The user asked for the feature (ON), but a behavior change this size must be disableable. |
| First batch timing | **Immediate** (then every interval) | Restoration shouldn't wait a full 10 s to start; "every 10 s" governs the *cadence between* batches. |
| Scope | **Per window** | Controls are per-window-UI-thread; "4 at a time" maps naturally to the reopened window. |
| 500 ms stagger | **Fixed (not configurable)** | The ask names only `4` and `10` as configurable. |
| Default `4` / `10` | as specified | — |

---

## 8. Gotchas / risks (learned while building it)

### 8.1 Off-tree swap-chain init — the one thing to watch on a live run
The ramp drives `InitializeWithSize` on **background (off-tree)** controls *en masse*. That calls
`ISwapChainPanelNative2::SetSwapChainHandle` on a panel not yet in the visual tree
(`TermControl::_AttachDxgiSwapChainToXaml`). This was flagged "validate live" in the `e340621fb`
work — the connection still drains into the buffer (so `claude.exe` starts + hooks fire even
unpainted), but painting may defer until the tab is shown. **This feature is the stress test for
that path.** Watch `~/.agentmaster-dev/hooks.log` for the woken sessions' `[SessionStart]` lines and
for any AV (`0xC0000005` in `Microsoft.Terminal.Control.dll`) under load. If an off-tree init proves
unstable, the `_autoActivateTried` guard already prevents a retry loop; the fallback design is to
make `_ActivateDormantSession` realize each control off-screen for one layout pass instead of pure
off-tree init (more invasive — only if needed).

### 8.2 The loop-guard is mandatory — see §4.3. Don't drop `_autoActivateTried`.

### 8.3 Resource ramp across multi-window reopen — see §4.6. `batchSize × windows` per interval.

### 8.4 `started` / dot interplay
`_ActivateDormantSession` → `SetStarted(id,true)` flips the half-hollow dot to full as each tab wakes,
so the board/strip visibly "fill in" during the ramp — a nice progress indicator, no extra work.

### 8.5 Don't reuse `_ActivateAllDormantTabsLocal()` for the ramp
It wakes everything at once (the exact spawn storm we're avoiding). The ramp drains one tab per
`_AutoActivateDrainStep`.

---

## 9. ⚠️ Multi-agent hazard (operational, not design)

This repo currently has a **second agent actively editing and building the same working tree**.
During the first implementation attempt:
- its `cl.exe` held source files open → my edits failed with `EPERM: operation not permitted, rename …`;
- it shifted `Persistence.cpp` by ~40 lines mid-edit (line numbers are unreliable — locate anchors by grep);
- some unrelated files carry its uncommitted work and a repo-wide **license-header rewrite (MIT→AGPL)**.

**Therefore, when re-implementing:**
1. Before editing, check no foreign build is mid-compile: `Get-CimInstance Win32_Process -Filter "Name='cl.exe'"` → wait until **0** (idle `MSBuild.exe` reuse nodes are fine — they don't hold `.h` files).
2. **Never** `git checkout`/`git stash` these files to revert — they hold other people's uncommitted work. Reverse specific edits surgically.
3. Stage commits with an **explicit file list** (never `git add -A`) — `git add -A` from either agent sweeps the other's changes into the wrong commit (it already happened once: this feature's would-be neighbors' engine files landed in a concurrent commit).
4. Hold the `build-launch` mutex for any full/lib build (`tools/am-lock.sh`).

---

## 10. Implementation checklist

- [ ] `SessionModels.h`: 3 `AppSettings` fields (§5.1).
- [ ] `Persistence.cpp`: JSON serialize + deserialize (§5.2).
- [ ] `TerminalPage.h`: 2 timers + `_autoActivateBatchRemaining` + `_autoActivateTried` + 7 method decls (§6.1).
- [ ] `TerminalPage.AgentObserver.cpp`: the 7 method bodies (§6.2).
- [ ] `TerminalPage.AgentWindowRecord.cpp`: `_StartAutoActivateRestored()` at the end of `_RestoreWindowTabs` (§6.3.1).
- [ ] `TerminalPage.AgentEngine.cpp`: `_OnAutoActivateSettingsChanged()` in both settings-apply sites (§6.3.2).
- [ ] `~TerminalPage`: `_StopAutoActivateRestored()` (§6.3.3).
- [ ] `AgentManagerContent.{h,cpp}`: 3 cog controls — declare, build, load, save (§5.3).
- [ ] Lib compile-check: `TerminalAppLib.vcxproj` (no `.idl` touched → no `TerminalControlLib` regen needed; the 3 settings live in plain engine headers).
- [ ] Deploy dev + verify: reopen a multi-tab window, watch the half-hollow dots fill 4-at-a-time every 10 s, and `[auto-activate] … ramp start` + the woken sessions' `[SessionStart]` in `~/.agentmaster-dev/hooks.log`. Toggle OFF in the cog → reopen → tabs stay dormant.

---

## 11. Verification / acceptance

1. Reopen a saved window with ≥6 Claude/Codex tabs → within ~0.5 s the first 4 begin waking
   (half-hollow → full dots), staggered ~500 ms; the remaining 2 wake at +10 s.
2. `restoreActivateBatchSize=2`, `restoreActivateIntervalSeconds=5` → 2 tabs every 5 s.
3. Toggle **Auto-activate restored tabs** OFF → reopen → all tabs stay dormant (manual Activate
   Tab / Activate All still work).
4. No retry loop on a tab that fails to init (force-test by temporarily making `InitializeWithSize`
   return false; the ramp moves on and stops).
5. Multi-window reopen → each window ramps its own tabs; no cross-window double-activation.
6. Engine harness still green; `TerminalAppLib` compiles clean; dev deploy runs without AV.
