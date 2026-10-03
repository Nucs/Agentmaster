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

Ranked by measured cost addressed. None is done; each needs a build.

1. **Ship §4c to the release.** It is built and running on dev. It stops off-screen header pulses,
   shows still dots for dormant tabs (headers, side panel, board), stops hidden terminals from painting,
   and releases a hidden terminal's device after 60 s. This removes the device-limit risk (§6) and most
   running storyboards (§4a).
2. **Virtualize the side panel's card list.** Use an `ItemsRepeater` with a `StackLayout`, or a
   `ListView`. Or start a card's pulse only while the card is in the scroll viewport
   (`EffectiveViewportChanged`). Today all 461 cards of the big window are live, and every card with a
   draft pulses.
3. **Subscribe to `LayoutUpdated` only where it can matter.**
   - `TermControl` (TermControl.cpp:361): subscribe on `Loaded` and revoke on `Unloaded`. A control
     outside a visual tree has no size, so it cannot initialize anyway. The §4c `Loaded` / `Unloaded`
     handlers are the place.
   - `TabHeaderControl` (TabHeaderControl.cpp:231): subscribe only while the header has tags and is
     loaded.
4. **Badge placement without transforms per pass** (TabHeaderControl.cpp:765). Gate on cheap reads first
   (tab index, strip scroll offset, `ActualOffset`), and call `TransformToVisual` only on a real change.
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
