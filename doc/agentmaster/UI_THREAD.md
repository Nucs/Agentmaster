# Where the release's CPU goes — the UI thread at fleet scale

Measured 2026-10-03 on the release instance, read-only. No thread of the target was suspended.
This document attributes the "UI thread" row that RENDERING.md §4b left unattributed, and names the
other CPU users of the process.

## 1. Summary

- The release process (`Agentmaster.exe`, pid 160972, 0.6.8.14, layout slot Release-B, started 07:24)
  used **43–60% of one core**. It had 4 windows with 550 tabs (461 + 58 + 28 + 3), 57 initialized
  terminals and 54 `claude.exe` in its process tree.
- The **UI thread** used most of it: **211–614 ms/s** (21–61% of one core) over seven windows.
- Its steady cost is **XAML's 60 Hz frame loop**. Forever storyboards keep the loop running: the
  unsent-draft dots and the cache-warm sparks. Each frame walks the composition tree and ticks every
  running storyboard. Both costs grow with the number of tabs, cards and running animations.
- When sessions are active, two more costs grow with the tab count:
  - Every layout pass raises `LayoutUpdated` to ~1,050 handlers. Each never-initialized terminal has
    one, and each tab header has one.
  - Each tab with bookmark tags re-runs its badge placement after every layout pass. Each run creates
    up to 4 XAML transforms. Releasing each transform costs a linear search in XAML's event manager.
- The engine threads are small: `ProcessObserver` 40–63 ms/s, `SessionScanner` 22–44 ms/s.
- The release held **58 Direct3D devices**. The NVIDIA hang-recovery storms of 09-25 and 10-01 started
  at the 63rd device (RENDERING.md §1). 4 more initialized terminals can start the next one.
- The sessions it hosts are the bigger memory load: the process tree held **42.8 GB** of private
  memory (54 `claude.exe`: 36.6 GB). Machine commit was 179 of 199 GB (90%).

## 2. How it was measured

| Tool | What it gives | Effect on the target |
|---|---|---|
| `tools/threadcensus.cs` | per-thread CPU and wake-ups, grouped by start module; devices; render threads | none (kernel counters only) |
| `tools/stacksnap.cpp` (new) | per-thread CPU over a window; sampled stacks with symbols and inline frames | a microsecond kernel APC per sample |
| Get-Process / Win32_Process deltas | per-process CPU and private memory, the whole process tree | none |
| hooks.log of this run (14,863 lines since 07:24) | stalls, slow tab switches, exceptions | none |

**stacksnap never suspends a thread.** Each sample does three steps:

1. It reads the running thread's registers with `GetThreadContext`. The kernel captures them in a
   special kernel APC, and the thread continues.
2. It copies the used stack (`[RSP, StackBase)`) with one `ReadProcessMemory`.
3. It walks that copy with `StackWalk64` in its own process.

Symbol loads and downloads therefore cost only the sampler's time. `tools/livesample.exe` walks while
the thread is suspended; with a symbol server it froze the release UI for 165 s on 2026-09-27.
stacksnap can use a symbol server safely. Here it used the PDBs in the Release-B layout plus Microsoft's
public symbols, cached in `Q:\tmp\symcache`.

Trade-off: the thread runs between steps 1 and 2 (tens of µs). Some samples have wrong inner frames, or
a walk that stops early. About 30% of the idle window's busy samples ended without reaching the message
pump. Read the percentages as a profile, not as exact values.

## 3. The process, thread by thread

`stacksnap top` and `threadcensus`, 10–20 s windows:

| Thread | CPU (ms/s) | Wake-ups/s | Notes |
|---|---|---|---|
| UI thread (`Agentmaster.exe+0x7E900`, tid 163276) | 211–614 | 254 | all 4 windows share it |
| `ProcessObserver::_worker` | 40–63 | — | full survey every 2 s |
| `SessionScanner::_worker` | 22–44 | — | |
| 57 render threads ("Rendering Output Thread") | 27 total | 82 total | 9 painted in 20 s |
| thread pool (`TppWorkerThread`) | 15–20 total | 90 total | mostly terminals' throttled work (URL patterns, ICU regex) |
| 1,160 NVIDIA threads (`nvwgf2umx.dll`) = 58 devices | 11 total | 3,856 total | 2 of 20 per device poll ~66/s |
| XAML frame thread (`Windows.UI.Xaml.dll+0x6F59C0`) | 2–5 | 64.6 | a continuous 60 Hz loop |

For comparison, the dev instance (1 window, 28 tabs, a Debug build) used 19 ms/s on its UI thread.

## 4. The UI thread

Two profiles, with different load from the sessions. Nobody navigated during either window.

| Share of busy samples | Active window (19:55, 14 s, UI 502 ms/s) | Quiet window (20:03, 180 s, UI 240 ms/s) |
|---|---|---|
| XAML frame tick (`CXcpBrowserHost::OnTick`) | 58% | 60% |
| – composition submit (`SubmitPrimitiveCompositionCommands`) | 18% (~92 ms/s) | 38% (~91 ms/s) |
| – animation tick (`CCoreServices::Tick` → `CTimeManager::Tick`) | 13% | 17% (~42 ms/s) |
| – layout pass, nearly all of it the `LayoutUpdated` raise | 21% (~103 ms/s) | 1.6% (~4 ms/s) |
| Dispatcher-queue callbacks | 21% | 8% |
| – `TabHeaderControl::_PositionTagBadgesNow` | 17% (~84 ms/s) | 1.4% |
| – our periodic lanes (`_ObserverProbeImpl`, `_ReconcileClaudeTabsImpl`, liveness sweep, side panel) | ~3% | ~5% |
| Busy samples | 322 | 2,557 (of 10,329) |

### 4a. The 60 Hz loop and why it never stops

- XAML runs a frame (`CXcpDispatcher::Tick`) on every display refresh while any XAML animation runs.
  The frame thread woke 64.6 times/s for the whole window.
- The dev measurement of commit c77b5fd01 shows the cause: 62 wake-ups/s while one tab's draft dots
  pulsed, 3.2/s after the draft was sent.
- In the release build (no §4c yet), these Forever storyboards run:
  - The draft dots of **51 tab headers**: every open session with a remembered draft, dormant ones
    included (`_UpdatePendingAnimation` starts on the flag only).
  - The draft dots of about **51 side-panel cards**: 45 + 1 + 5 in the three windows whose side panel
    is open. `BuildPendingDots` starts the pulse on `Loaded`. The side panel's card list is a plain
    `StackPanel` in a `ScrollViewer` (`_SidePanelRebuildCards`, TerminalPage.AgentSidePanel.cpp), so all
    cards of the window are loaded: 461 in the big window, scrolled into view or not.
  - The cache-warm sparks of each session at rest within 5 min of its last turn.

### 4b. What one frame costs

- **Composition submit, ~91 ms/s in both windows.** `HWCompTreeNode::UpdateTree` recurses through the
  composition tree. Its time goes to iterating child visual collections (`VisualIterator::MoveNext`,
  `VisualCollection::First`, `get_IsVisible`) and to `CUIElement::IsOffsetIndependentlyAnimating`. The
  cost grows with the number of composition nodes under animated parents. Inferred, not proven: the
  461-card side panel is the largest such collection.
- **Animation tick, ~42 ms/s.** `CStoryboard::ComputeStateImpl` for each running storyboard, plus a
  weak-reference check of each animation target. It grows with the number of running storyboards,
  whether their elements are on screen or not.

### 4c. The `LayoutUpdated` fan-out (active periods)

- After a layout pass, XAML calls every `LayoutUpdated` handler of the thread. Elements outside any
  visual tree are included: the profile shows the dormant terminals' handlers running in the raise.
- ~1,050 handlers are registered:
  - **~493 never-initialized terminals.** `TermControl` subscribes `SwapChainPanel().LayoutUpdated` in
    its constructor (TermControl.cpp:361) and revokes it only after a successful initialization. Each
    call reads `ActualWidth`, `ActualHeight`, `CompositionScaleX` and `CompositionScaleY`, then returns
    because the size is 0. 35% of the raise.
  - **550 tab headers.** `TabHeaderControl` subscribes `HeaderRootGrid().LayoutUpdated` for every
    header (TabHeaderControl.cpp:231). It checks for tags inside the handler. 10% of the raise.
  - XAML's per-handler overhead (`ComPtr` add-ref/release, `GetEventSourceNoRef`): ~40% of the raise.
- In the active window, layout passes ran often enough for the raise to cost ~103 ms/s. Session
  activity (tab-status changes, text updates in some window) drives the passes, not the user.

### 4d. Bookmark-tag badge placement (active periods)

- Each tagged header (7 open tabs) queues `_PositionTagBadgesNow` on every `LayoutUpdated`, loaded or not.
- One run calls `TransformToVisual` up to 4 times (TabHeaderControl.cpp:791, 819, 860, 891).
- Releasing each returned transform runs `CDependencyObject::~CDependencyObject` →
  `CEventManager::RemoveObject` → `xpairlist::RemoveByKey`, a linear search with `AreEqual` per entry.
  The list holds every object with an event registration, which is thousands at this tab count.
- That search was 16.5% of all busy UI samples in the active window.

### 4e. User-visible effect (hooks.log, 07:24–20:10)

- 22 `[tab-switch-settle]` lines: after a tab switch, the UI thread was idle again only after
  0.75–3.4 s, typically ~1 s.
- 4 `[tab-switch-slow]` lines: 260–401 ms, mostly in the content swap.
- One `[ui-stall]` of ~35 s at 11:12, when a dormant tab ("idleon", 26ee0fca) was focused. Its terminal
  got its GPU device 32 s later. No NVIDIA event at that time. No sample covers it; the cause is unknown.
- No `[exc]` or `[wil]` line in the whole run.

## 5. The engine threads

| Thread | Busy-sample share | What it does |
|---|---|---|
| `ProcessObserver` (~49 ms/s in a 120 s window) | `SnapshotProcesses` 42% | a machine-wide process snapshot every 2 s (1,343 processes on this machine) |
| | `_LineDerivedLastActivity` 20% | transcript tail reads |
| | `SessionRegistry::Snapshot` 8% | a FULL copy of all 1,520 records with their queues, every survey, only to collect managed Codex tab tokens (ProcessObserver.cpp:1161) |
| `SessionScanner` (~34 ms/s) | `SessionRegistry::SnapshotLive` 57% | a deep copy of every live session (~550, with queued prompts) on each pass (SessionScanner.cpp:680) |
| | `_reconcileSession` 41% | transcript deltas (`_readDelta` 29%) |

The UI lanes also copy sessions: the liveness sweep calls `SessionRegistry::Get` (a full `SessionInfo`
copy) for each tab on each tick. The `top` snapshot caught the UI thread inside that copy.

## 6. GPU devices

- 1,160 NVIDIA threads ÷ 20 = 58 devices: 57 initialized terminals + XAML's own.
- The storms of 09-25 and 10-01 began at the 63rd device. The previous instance had one on 10-02
  22:00–23:59 (524 `nvlddmkm` Event 153) and more events at 07:19 today, five minutes before this
  instance started.
- Each first visit of a dormant tab, and each new tab, adds a device. §4c releases a hidden terminal's
  device after 60 s hidden; it is built for dev (c77b5fd01), not for the release.

## 7. The hosted sessions and the machine

| Part of the release's process tree (344 processes, 22 s) | CPU (% of one core) | Private memory |
|---|---|---|
| 54 `claude.exe` | 60 | 36.6 GB |
| `Agentmaster.exe` | 52 | 2.4 GB |
| 57 `pwsh` + 57 `OpenConsole` + 56 `conhost` | 0.3 | 2.2 GB |
| 54 `catalog` + 48 `lazymcp` MCP servers | 0 | 1.0 GB |
| a user job (`NumSharp.DotNetRunScript`) | 93 | 0.3 GB |
| **Total** | 205 | **42.8 GB** |

- Machine: 32 logical CPUs at 26% total, run queue 0.7. CPU is not saturated.
- Memory: 8.9 GB free of 63.7 GB; commit 179 of 199 GB (90%). The 11,632 pages/s were file reads
  (0 pages/s written), not paging pressure.

## 8. Fix directions

Ranked by measured cost addressed. Item 1 is done; each of the others needs a build. §10 measures the
release after item 1 and sets the order for the rest.

1. **Ship §4c to the release.** Done: release 0.6.8.15 (2026-10-03). It stops off-screen header
   pulses, shows still dots for dormant tabs (headers, side panel, board), stops hidden terminals from
   painting, and releases a hidden terminal's device after 60 s. It removed the device-limit risk (§6).
   It did not stop the 60 Hz loop (§10).
2. **Virtualize the side panel's card list.** Use an `ItemsRepeater` with a `StackLayout`, or a
   `ListView`. Or start a card's pulse only while the card is in the scroll viewport
   (`EffectiveViewportChanged`). Today all 461 cards of the big window are live, and every card with a
   draft pulses.
3. **Subscribe to `LayoutUpdated` only where it can matter.**
   - `TermControl` (TermControl.cpp:361): subscribe on `Loaded` and revoke on `Unloaded`. A control
     outside a visual tree has no size, so it cannot initialize anyway. The §4c `Loaded` / `Unloaded`
     handlers are the place. Done in code, not built yet (§10a #3: `_UpdateLayoutInitHook`, §10d).
   - `TabHeaderControl` (TabHeaderControl.cpp:231): subscribe only while the header has tags and is
     loaded. Done in code, not built yet (§10a #2: `_UpdateBadgeLayoutHook`).
4. **Badge placement without transforms per pass** (TabHeaderControl.cpp:765). Gate on cheap reads first
   (tab index, strip scroll offset, `ActualOffset`), and call `TransformToVisual` only on a real change.
   Done in code, not built yet (§10a #2: `_BadgePlacementSignature`).
5. **Ambient animations on the Composition API.** Run the pulse and the sparks as composition key-frame
   animations (`ElementCompositionPreview::GetElementVisual`, `IterationBehavior::Forever`) instead of
   XAML storyboards. Composition animations run in the compositor, so XAML's UI-thread loop can stop.
   Verify on dev first: the frame-thread wake rate must drop to ~3/s while a pulse runs.
6. **Engine copies.** Replace the observer's full `Snapshot()` (ProcessObserver.cpp:1161) with a query
   for managed Codex tokens. Give the scanner (SessionScanner.cpp:680) and the liveness sweep a lean
   projection without queues and prompt texts.
7. **The process survey.** `SnapshotProcesses` enumerates all 1,343 processes every 2 s. A longer
   full-survey interval when the roster and the correlated pids did not change would cut most of it.

## 9. How to measure again

```bash
# Build (Git Bash needs the absolute path of the .bat)
cmd //c "K:\\source\\Agentmaster\\tools\\_build_stacksnap.bat"
# Optional OS symbols: copy dbghelp.dll + symsrv.dll from
# "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64" beside stacksnap.exe, then:
export STACKSNAP_SYMSRV='srv*Q:\tmp\symcache*https://msdl.microsoft.com/download/symbols'
# Per-thread CPU + one stack per busy thread (read-only, nothing suspended)
stacksnap.exe top <pid> 10
# Profile threads: the UI thread is the one whose start is Agentmaster!wWinMainCRTStartup in `top`
stacksnap.exe sample <pid> 180 3 <uiTid>        # writes stacksnap_<pid>_<tid>.folded
# Frame-loop rate, devices, render threads
dotnet run tools/threadcensus.cs -- <pid> 20
```

Checks after the fixes:
- XAML frame thread at ~3 wake-ups/s when nothing visible animates.
- `CEventManager::RemoveObject` and `TermControl::_InitializeTerminal` absent from the UI profile.
- NVIDIA threads ÷ 20 ≈ windows + tabs viewed in the last 60 s + 1.

## 10. After §4c: release 0.6.8.15, measured again on 2026-10-04

Release 0.6.8.15 has the RENDERING.md §4c work. It runs from layout slot A as pid 222804, started
2026-10-03 23:13. Its hooks.log has `[render] hidden tabs (settings.json): pause ON, GPU release after
60 s hidden`. The fleet has the same size as in §1: 4 windows, 555 tabs (466 + 58 + 28 + 3), 52
remembered drafts, and the side panel open in 3 windows. Measured 09:08–09:12 with the §2 tools,
read-only: `threadcensus` 20 s, `stacksnap top` 10 s, and `stacksnap sample` 60 s on the UI, scanner
and observer threads. The user worked in the release during the sample.

| Item | 0.6.8.14 (§1–§6) | 0.6.8.15 |
|---|---|---|
| Threads | 1,303 | 137 |
| Initialized terminals | 57 (9 painted in 20 s) | 18 (0 painted in 20 s) |
| D3D11 devices (NVIDIA threads ÷ 20) | 58 | 4 |
| Process CPU | 43–60% of one core | 42% |
| UI thread | 211–614 ms/s | 286–374 ms/s |
| XAML frame thread | 64.6 wake-ups/s | 65.1 wake-ups/s |
| `ProcessObserver` / `SessionScanner` | 40–63 / 22–44 ms/s | 38–41 / 31–44 ms/s |

§4c removed the device risk. It did not stop the 60 Hz loop, so the UI thread did not get cheaper.

UI thread, 60 s sample (374 ms/s, 605 busy samples of 2,011):

| Share of busy samples | Value |
|---|---|
| XAML frame tick (`CXcpBrowserHost::OnTick`) | 49% (~185 ms/s) |
| – composition submit, the tree walk | 46% (~174 ms/s): ~2.7 ms per frame |
| – animation tick (`CTimeManager::Tick`) | 2.5% (~9 ms/s) |
| Walk samples that the sampler could not follow to the frame tick (`UpdateTreeChildren`, `get_Children` → `AllocateItemForPeer`, `VisualIterator`) | 21% (~78 ms/s) more |
| `LayoutUpdated` raise / `_PositionTagBadgesNow` | 0.2% / 0.5% (no session churn in this window) |
| Our lanes (`_ReconcileClaudeTabsImpl`, liveness sweep, observer probe, cache-warm scan, pending scan) | ~6% (~23 ms/s); registry copies are ~15 ms/s of it |

What this shows:
- The frame loop is 50–70% of the UI thread's busy time (~185–260 ms/s).
- A few storyboards are enough to keep the loop at 60 Hz. The animation tick fell from ~42 to ~9 ms/s,
  but each frame still walks the composition tree for ~2.7 ms. The cost of the loop depends on the size
  of the tree, not on the number of animations.
- §4c leaves these Forever storyboards running:
  - the draft dots of a STARTED (or external) session: on its header when the strip has realized the
    header, and on its side-panel card (all cards are loaded, so an off-screen card pulses too);
  - the spark crown of each realized header within 5 min of its session's last turn.
- The costs of session churn (the `LayoutUpdated` raise and the badge placement, sections 4c and 4d
  above) did not show in this window. They stay as found.
- Tab switches: 1 `[tab-switch-settle]` (905 ms) in 47 tab focuses after startup.
- Open: a frame costs ~2.7 ms here against ~1.4 ms in 0.6.8.14 (§4b), with the same fleet. The cause is
  not known. One candidate: 0.6.8.15's side-panel summary lists the whole conversation.

### 10a. The order for the rest: win × ease

| # | Fix (§8 item) | Win | Ease |
|---|---|---|---|
| 1 | Pulses and sparks on the Composition API (item 5) | ~185–260 ms/s, while any indicator animates | Medium: the two storyboards in `TabHeaderControl.cpp` and `BuildPendingDots`; the §4c gates stay |
| 2 | Header `LayoutUpdated` only while the header is tagged and loaded (item 3, second half), and a cheap change check before the badge transforms (item 4) | During session churn: up to ~110 ms/s (the 0.6.8.14 active window in §4) | Easy: Agentmaster code in `TabHeaderControl.cpp` only |
| 3 | `TermControl` `LayoutUpdated` only while loaded (item 3, first half) | During session churn: up to ~55 ms/s | Small, but upstream code on the terminal-start path |
| 4 | Lean registry copies (item 6) | ~15 ms/s on the UI thread, ~17 ms/s on the scanner, ~4 ms/s on the observer | Easy for the observer (a managed-Codex token query); medium for a copy without the prompt queue (the scanner keeps only Sent, unechoed prompts for the lost-send verdict) |
| 5 | A longer survey interval while nothing changes (item 7) | ~10–15 ms/s on the observer thread | Easy code, but a `claude` typed in a `+` tab is found later than the 2 s of today: a product decision |
| 6 | Virtualize the side panel (item 2) | After #1: only the frames that still run | Hard: an `ItemsRepeater` and an element factory in a 2,200-line file |

Notes:
- **#1, check it before you build:** switch off Windows "Animation effects" for 5 minutes. The §4c
  gates then show still dots and a still crown: each header at its next decision, each side-panel card
  at its next rebuild. Then run `threadcensus`. If the frame thread drops to ~3 wake-ups/s and the UI
  thread loses ~200 ms/s, #1 will work. If the frame thread stays at ~65/s, something else keeps the
  loop running, and #1 alone will not help.
- **#1, two traps:** start a composition animation only on an element that animates. A hand-out visual
  (`ElementCompositionPreview::GetElementVisual`) gives the element its own composition node, and each
  frame walks the nodes. Check on dev that a stopped pulse returns to the correct base opacity: set the
  visual's opacity after `StopAnimation`, and compare the still dots and crown with today's.
- **#3:** also try the start from the `Loaded` handler. Otherwise a layout pass that ran before `Loaded`
  leaves the terminal blank until the next pass. Build it alone, so that a blank-tab regression points
  at it.
- **#6:** first measure how much of the walk the side panel causes: collapse the side panel of the big
  window for one minute and sample again.
- Builds: (1) #1 + #2 + the observer part of #4; (2) #3 alone; (3) the rest of #4. #5 needs a decision.
  Do #6 only if the frame walk still shows after #1.

### 10b. #2 in the code (2026-10-04, not built yet)

All in `TabHeaderControl.cpp` / `.h`; no other file changed:
- **The hook.** The constructor no longer subscribes `HeaderRootGrid().LayoutUpdated`.
  `_UpdateBadgeLayoutHook` holds that subscription (a revoker) only while the header has tags AND is in
  the live tree. It runs from `Loaded`, `Unloaded` and `_UpdateTagBadges` (the first tag arms, the last
  one disarms). A strip with 7 tagged tabs now runs 7 header handlers per layout pass, not ~550.
- **The live tree is `IsLoaded()`, not the event** (corrected with #3, §10d). XAML can raise a STALE
  `Unloaded` after the next `Loaded` when an element is re-parented within one tick; a tab move removes
  and re-inserts its `TabViewItem`. The first version trusted the event and could drop the hook of a
  header that was still on screen. Now the method reads `IsLoaded()` and returns whether the hook is
  armed. When it is still armed after an `Unloaded`, that `Unloaded` was stale: the handler schedules a
  full placement at once, which reopens the popup that the handler's close just hid.
- **The gate.** `_PositionTagBadges(fromLayoutPass)`: the hook passes true. It then compares
  `_BadgePlacementSignature()` with `_badgePlacedSignature`, which `_PositionTagBadgesNow` records after
  each completed placement (after its popup open or close). When they match, nothing is scheduled. The
  signature holds plain property reads only: whether the title gives the x, the popup's open state, the
  open gate (IsLoaded + XamlRoot), the badge row's width, the header's height, and the `ActualOffset` of
  every element from the title (or the header grid) up to the root. Where the walk meets the first
  `TabViewItem` it adds its height; where it meets the first `ScrollViewer`, its scroll offsets and
  width. No `TransformToVisual`.
- **Unchanged.** Every other trigger (`SizeChanged` of the header or its tab, the strip's
  `ViewChanged`, `Loaded`, a tag change) skips the gate and schedules the full placement, as before.
  The placement itself is the same code.
- **The limit.** A change that moves no `ActualOffset`, size or scroll offset (an ancestor's
  `RenderTransform` alone) is not seen by a layout pass. Nothing in the tab strip does that, and the next
  explicit trigger re-places the badges.
- **Check after the build.** Tag a tab, then reorder, scroll and resize the strip, and rename the tab:
  the ribbons must follow every time. In a stacksnap profile during session churn,
  `_PositionTagBadgesNow` and `CEventManager::RemoveObject` must be gone or near zero.

### 10c. #1 in the code (2026-10-04, not built yet)

Every looping indicator now runs on the Composition API. No XAML storyboard runs forever any more, so
XAML's frame loop can stop while a pulse or a crown is on screen.

- **New:** `AgentAmbientAnimation.h` / `.cpp` (the `.cpp` added to `TerminalAppLib.vcxproj`).
  - `AgentOpacityLoop`: one XAML element's `Opacity`, run through `UIElement::StartAnimation` (Target
    "Opacity"). The loop is linear, repeats forever, and begins and ends on the element's still value.
  - `AgentDotsPulse`: the three staggered dot loops of a draft cluster.
  - `AgentEmberSparks`: the crown's three embers as composition ellipse visuals of our own, attached to
    the sparks canvas with `ElementCompositionPreview::SetElementChildVisual`.
  - The header only declares; the `.cpp` is the one translation unit that includes the 1.1 MB
    `winrt/Windows.UI.Composition.h`.
- **Changed:**
  - `TabHeaderControl.cpp` / `.h`: the two storyboards became `_pendingDotsPulse`, `_cacheWarmGlow` and
    `_cacheWarmEmbers`. The RENDERING.md §4c gates and the change gates are unchanged. The embers copy
    `AgentCacheWarmBrush`'s color through the `TabStatus` subscription (`_SyncCacheWarmEmberColor`).
  - `TabHeaderControl.xaml`: the three spark ellipses are gone; the canvas only hosts the embers.
  - `AgentManagerContent.Internal.h` `BuildPendingDots` (board and side-panel cards): the same
    `AgentDotsPulse`, started on `Loaded` and stopped on `Unloaded`. The two empty `catch (...)` blocks
    now log (Rule #18).
- **The look is the same.**
  - Dots: 1.0 → 0.3 → 1.0 per 1 s, 160 ms apart. The storyboard ran 0.3 → 1.0 → 0.3, the same wave half
    a cycle later.
  - Glow: 0.45 → 0.62 → 0.28 → 0.45 per 2.8 s, the storyboard's 0.28 ↔ 0.62 breath around the still
    0.45.
  - Embers: the same places, sizes, +1.4 → −1.6 DIP rise, 1150 ms cycle and 0 / 380 / 760 ms stagger.
- **Why these two paths** (read in the WinUI source, the lifted system XAML):
  - `Opacity` through `UIElement::StartAnimation`: `PropertySetListener::ShouldTrackFacadeProperty`
    excludes `Opacity`, so XAML gets no per-frame callback. `HasWUCOpacityAnimation` stops
    `hwwalk.cpp` from culling an element whose static `Opacity` is 0.
  - A hand-out visual (`GetElementVisual`) would be culled at `Opacity` 0. XAML also writes
    `UIElement.Opacity` into it whenever that property changes, and a direct set cancels a running
    composition animation.
  - Motion is never on a XAML element. A `Translation` animation makes XAML dirty the element on every
    frame (`NotifyVector3PropertyChanged` → `NWSetTransformDirty`), for hit-testing.
  - `UIElement::StopAnimation` applies `SetToFinalValue`, then XAML writes the stopped value back into
    the property. Because every loop ends on the still value, a stopped pulse shows exactly the still
    dots, and the write-back changes nothing.
- **Limits:**
  - The embers are composition visuals: no XAML high-contrast opacity override, and positions that
    are not rounded to pixels, at a 2.4 DIP size.
  - The compositor (DWM) still works on every frame while something animates. That cost is off our UI
    thread.
- **Check after the build:**
  - With a draft in a started tab, or a warm crown on screen, XAML's frame thread
    (`Windows.UI.Xaml.dll+0x6F59C0` in `threadcensus`) wakes ~3 times/s, not ~65.
  - In a stacksnap profile, `SubmitPrimitiveCompositionCommands` and `CTimeManager::Tick` drop to near
    zero.
  - The dots pulse in the header and in the side panel. Still dots and the still crown look as before.
  - The embers follow the tab's color, also after a tab-color change.
  - No `[exc]` line with "pending-dots pulse", "cache-warm crown" or "ember color".

### 10d. #3 in the code (2026-10-04, not built yet)

All in `TermControl.cpp` / `.h` (upstream code, marked Agentmaster), plus the #2 correction in §10b:
- **The hook.** The constructor no longer subscribes `SwapChainPanel().LayoutUpdated`, the hook that
  starts a terminal on its first non-zero layout. `_UpdateLayoutInitHook` holds it only while the
  terminal has not started, the control is in the live tree (`IsLoaded()`), the control is not closing,
  and the control is not detached. The §4c `Loaded` and `Unloaded` handlers call it, after
  `_UpdateRenderingSuspension`. A restored tab that nobody opens (~537 on the release) now has no handler
  at all; before, each one ran on every layout pass, read its size and returned.
- **The start on `Loaded`.** Whenever the hook is wanted, the method also tries the start at once. The
  layout pass that sizes the panel can run before XAML raises `Loaded` (the §4c code already notes this
  order), and a hook armed after that pass would wait for another pass that may not come.
  `_InitializeTerminal` returns false for a zero size and for a terminal that already started, so the
  attempt is free when it cannot succeed. The hook is armed BEFORE the attempt, so a start that throws
  (logged) is retried by the next layout pass.
- **The reason.** `_layoutInitReason` (Create; Reattach after `_initializeForAttach`) replaces the
  separate Reattach handler that `_initializeForAttach` used to swap in. A torn-out tab's new control
  sets Reattach before its first `Loaded`.
- **Two belts in the handler.** It drops itself on a closing or detached control. `Detach` revokes the
  `Loaded` / `Unloaded` handlers and nulls `_interactivity`, which the start uses, so a hook left armed
  across a `Detach` must never start: before, the always-on hook could do that on a never-started
  control, if a later layout pass gave it a size.
- **Unchanged.** `InitializeWithSize` (Activate Tab / Activate All) still starts a terminal off screen
  and revokes the hook. A stale `Unloaded` keeps the hook, because the method reads `IsLoaded()`.
- **The risk.** This is the path that starts every terminal. A mistake leaves a tab blank, with its
  claude never started. Build it on its own (§10a).
- **Check after the build, on dev:** a new tab; the first visit of a restored tab; Activate Tab and
  Activate All; a split pane; zoom in and out; a tab torn out into a new window and one docked into
  another window (Reattach); Deactivate, then open the tab again; a window restore with several tabs.
  Every terminal must draw and its claude must start. In a stacksnap profile during session churn,
  `TermControl` frames must be gone from the `LayoutUpdated` raise.

### 10e. Measured again, 10:27–10:33: the fixes are not built yet

No binary has #1–#3 yet: the newest layouts (Debug, Release, Release-B) are from 2026-10-03, before
the fix commits. The release (pid 222804) runs the same code as in §10. Same tools, read-only:
`threadcensus` 30 s, `stacksnap sample` 60 s, and a process-tree census (15 s).

| Item | 09:08 (§10) | 10:27 |
|---|---|---|
| UI thread | 286–374 ms/s | 263–264 ms/s |
| XAML frame thread | 65.1 wake-ups/s | 60.9 wake-ups/s |
| Frame tick, plus walk samples outside it | 70% of busy | 80% of busy (~210 ms/s) |
| Animation tick | ~9 ms/s | ~2 ms/s |
| UI work outside the frame loop | — | ~53 ms/s; registry copies are ~15 ms/s of it |
| `SessionScanner` | 31–44 ms/s | 36 ms/s: `SnapshotLive` 79% (~28 ms/s), of it prompt-queue copies ~14 ms/s; the reconcile itself ~3 ms/s |
| `ProcessObserver` | 38–41 ms/s | 38 ms/s: `SnapshotProcesses` 58% (~22 ms/s), full `Snapshot()` 12% (~5 ms/s) |
| Initialized terminals / painted | 18 / 0 in 20 s | 21 / 0 in 30 s |
| Process tree | — | 121 processes, 45% of one core (Agentmaster 38%, 18 `claude.exe` 6%); private 14.1 GB (Agentmaster 1.0 GB, `claude.exe` 11.9 GB, `pwsh` 0.7 GB) |
| Machine commit | — | 137 of 159 GB (86%) |

What this adds to §10:
- An animation tick of ~2 ms/s means one or two indicators ran. They alone kept the loop at 61 frames/s.
- After #1–#3, the UI thread should keep ~50 ms/s: the work outside the frame loop, plus the frames that
  still run for real changes.
- #4 is bigger than §10a says: ~48 ms/s in total (UI ~15, scanner ~28, observer ~5). After #1–#3 it is
  the largest item on the UI thread and on the engine threads. Its parts:
  - `_ReconcileClaudeTabsImpl` copies every live session with its prompt queue every ~2.5 s, in each
    window, and reads only `id`, `tabToken` and `workingDir`.
  - The liveness sweep calls `Get` per tab and reads only `tabToken`.
  - The cache-warm and pending-input sweeps also call `Get` per tab.
  - The scanner's `SnapshotLive` is 79% of its thread; the reconcile it feeds is ~3 ms/s.
  - The observer's full `Snapshot()` only collects the tab tokens of managed Codex sessions.

### 10f. Can XAML's loop run at 30 Hz? No

- System XAML has no frame-rate setting. Its render thread (`CompositorScheduler::RenderThreadFrame`)
  wakes the UI thread for at most one frame per display refresh. In the release it waits in
  `RefreshRateInfo::WaitForRefreshInterval` → `CDXGIOutput::WaitForVBlank` (the `stacksnap top` stack of
  the XAML frame thread). The WinUI source has the same design: `Scheduler::OnImmediateUIThreadFrame`
  (`dxaml/xcp/components/graphics/Scheduler.cpp`) and `RefreshRateInfo` (`dxaml/xcp/core/compositor/`),
  where the interval comes from DirectComposition's frame statistics, that is, from the monitor.
- A 30 Hz timer of our own would cost more than #1: each XAML frame walks the tree (~2.5 ms), so 30
  frames/s is ~75 ms/s on the UI thread. With #1, a running pulse costs the UI thread nothing.
- After #1, the pulses run in the compositor (DWM) at the display rate. An app cannot set DWM's rate
  either. To reduce DWM's work, have fewer indicators move.
- The terminals already draw only on change, with vsync (`Present1(1, 0)` in AtlasEngine), and hidden
  tabs draw nothing (RENDERING.md §4c): 21 terminals drew 0 frames in 30 s.
- A lower refresh rate for the monitor would halve the loop, but for every app on that display.

### 10g. #4 in the code (2026-10-04, the engine part tested, the UI part not built yet)

The periodic paths that copied whole session records now copy only what they read. A whole record carries
every prompt of the queue history (up to 200 per session) and every transcript text. §10e measured these
copies at ~48 ms/s.

- **New in `SessionRegistry`:**

  | Read | What it copies | Caller (before → after) | Measured cost it removes |
  |---|---|---|---|
  | `LiveTabBindings()` | id, working dir and host token of each live session (`SessionTabBinding`) | `_ReconcileClaudeTabsImpl`: `SnapshotLive` → this | ~9 ms/s, UI thread |
  | `GetTabToken(id)` | the host token alone | the liveness sweep: `Get` → this | ~2 ms/s, UI thread |
  | `GetWithoutQueue(id)` | the record without its queue | the cache-warm and inferred-color sweeps: `Get` → this | ~3 ms/s, UI thread (most of it) |
  | `ManagedCodexTabTokens()` | the host tokens of the managed Codex sessions | the observer survey: `Snapshot()` → this | ~5 ms/s, observer |
  | `SnapshotLiveForScan()` | live records with only the OPEN prompts (Pending, Held, Sent-unechoed), and lastAssistantText, recap, pendingInput and pendingPasteRefs empty | the scanner pass: `SnapshotLive` → this | most of ~28 ms/s, scanner |

- **How a lean copy is made.** `ParkedSessionMembers` moves the members the copy must not contain out of
  the stored record, the record is copied, and the guard moves them back, also when the copy throws.
  Moving a `std::vector` or `std::wstring` is noexcept and constant time. So every other member, a member
  added to `SessionInfo` later included, is copied exactly as `Get` copies it, and no field list can fall
  behind. The parking happens under the registry mutex, which every reader takes, so no thread can see the
  parked state.
- **`SnapshotLive` reserves once.** It counts the live records first. Before, each reallocation of the
  result moved every record already copied: ~21% of the scanner thread.
- **The scanner's contract** (comment at the call): it reads the queue only for open prompts
  (`ScanNeedsFreshTail`, the lost-send pre-filter, `ShouldWarnOnBoxNotVisible`), never the four emptied
  texts, and it `Get`s the fresh record before it changes anything, as before.
- **Not changed:** the Manager lens, the side panel and the pending-input sweep keep `SnapshotLive` / `Get`.
  They read the queue or the draft, and the profiles in §10 and §10e did not show them.
- **Checked:** `TestRegistryLeanReads` in the m5 harness (each read's content and order, the open-prompt
  filter, the emptied texts, the scanner predicates answer the same on the lean copy, and the stored
  record is intact after repeated reads). The three changed engine files compile cleanly with the app's
  warning settings (/W4 /WX, the five always-error warnings, /sdl /permissive-). The harness: 5,325
  checks, 3 failures, the known taskbar-badge baseline. The four `TerminalPage.AgentObserver.cpp` call
  sites are not built.
- **Check after the build:** in a stacksnap profile, `SessionRegistry::SnapshotLive` gone from the UI
  thread and from the scanner, `SessionRegistry::Snapshot` gone from the observer, and the scanner thread
  at a few ms/s while sessions sit idle. The tab reconcile, the liveness sweep (archives a dead claude's
  tab), the spark crown, the inferred tab colors and the managed Codex cards behave as before.
