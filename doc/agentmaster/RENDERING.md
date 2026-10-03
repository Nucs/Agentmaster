# Rendering — the per-process GPU device limit and the "Software rendering" setting

Setting: `AppSettings::softwareRendering` (Settings cog → **Behavior → RENDERING → "Software
rendering"**, default **OFF** = the GPU). Engine hook: `ControlCore::SetSoftwareRenderingOverride` /
`ReapplySoftwareRendering` (`src/cascadia/TerminalControl/ControlCore.{h,cpp}`), projected on
`TermControl`. App side: `TerminalPage::_ApplySoftwareRenderingSetting`
(`TerminalPage.AgentEngine.cpp`). Benchmarks: `tools/warpbench.cpp` (CPU per frame, GPU vs WARP),
`tools/devidle.cpp` (what an idle GPU device costs), `tools/threadcensus.cs` (a read-only census of a
running instance — safe on production), `tools/renderwait.cs` (read-only: which `Renderer` flag each
render thread waits on, the flags read from memory, and when the wait began). Harness: the
`softwareRendering` round-trip in `tests_persistence.cpp`.

Hidden tabs (§4c): `AppSettings::pauseHiddenTerminals` / `releaseHiddenTerminalDevices` /
`releaseHiddenTerminalDevicesAfterSeconds` (Behavior → RENDERING), `TermControl::ConfigureHiddenRenderingPolicy`
+ `_UpdateRenderingSuspension`, `ControlCore::SetRenderingSuspended`, `Renderer::SetPaintingSuspended`,
`IRenderEngine::ReleaseDeviceResources` / `AtlasEngine::ReleaseDeviceResources`; app side
`TerminalPage::_ApplyHiddenRenderingSetting`. Harness: the hidden-terminal round-trip + the
`HiddenTerminalReleaseDelayMs` checks in `tests_persistence.cpp`.

## 1. The problem — one GPU device per terminal, and a limit near 62

- **Every terminal that has been shown owns its own Direct3D 11 device** (AtlasEngine creates one per
  `TermControl`), and kept it until its tab was CLOSED. Hidden tabs kept theirs. **§4c changes that:** a
  terminal hidden for 60 s (a setting) now gives its device back and rebuilds it when shown, so the count
  stays near "on screen + recently viewed" (built and deployed to the dev instance 2026-10-03, not in a
  release yet). Deactivated tabs
  should give theirs back too: "Deactivate ▸" closes the tab's control the way a pane close does and parks a
  never-initialized one in its place, which is the state a window-restored tab is in. (Expected from that
  code path; not yet measured with §4's thread count.) Only dormant tabs (restored, or deactivated, and
  not entered since) have none.
- **NVIDIA's user-mode driver starts 20 `nvwgf2umx.dll` worker threads per device,** despite
  `D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS`. That is how a process's device count
  can be read from outside (§4).
- **Past ~62 devices in one process the driver breaks.** Every new device, and every repaint of some
  existing ones, triggers a channel hang-recovery. The System log records `nvlddmkm` **event 153**,
  "Error occurred on GPUID: 100", about 3 s each, clustered at each new tab or tab focus. The threshold
  reproduced exactly twice:
  - **2026-09-25 20:16.** The storm began with 62 devices held.
  - **2026-10-01 18:55:27.** The release had no NVIDIA events since at least 09-29. The 1,240 NVIDIA
    threads created before that moment are exactly 62 devices, and the first event 153 came with the
    63rd.
  - Earlier storms on 09-23 and 09-25 06:35 started at 55–65 initialized terminals.
  - Every storm ended only when the app restarted.
- **It is per process.** The dev instance (18–19 devices) created devices mid-storm without an error.
  GPU memory is not the cause: the card had 5.4 of 16 GB in use. The exact limit (a 64-context or
  subcontext cap?) is unverified.

## 2. What the user sees — a tab that stays black

10-01, tab "sysd catalog" (`8ff8b3e2`), opened at 18:59:29 four minutes into the storm:

- **Its claude was fine.** The console buffer showed the welcome header and a ready input box, and the
  heartbeat read `idle`.
- **The tab showed only its black background**, "like claude loading", for over an hour.
- **The first event 153 came 36 ms after the tab's renderer thread started** (18:59:29.700). They
  repeated every ~3 s for a minute, and again at each focus (20:14:59, 20:15:15, 20:18:30).
- **The renderer never noticed.** `Renderer::PaintFrame` only gives up after 6 consecutive failed
  frames; `ControlCore::_rendererEnteredErrorState` then switches that terminal to WARP + Direct2D and
  retries, and only a second failure shows the "Resume" notice. Neither happened: `d3d10warp.dll` was
  never loaded in the process. Every render thread, this one included, sat in the normal idle wait on
  `Renderer::_redraw`. A wait on `_enable` would mean painting is disabled; §4 shows how to tell the
  two apart. So the frames were "presented" but never reached the screen, and nothing in WT ever
  rebuilds that surface.
- **Workarounds before this setting existed:** restart the app (restored tabs come back dormant, with
  no device), or close enough viewed tabs to get under the limit and reopen the black one.
  "Deactivate ▸ Other Tabs" should now do the same without closing anything: each parked tab's control
  is closed (and with it, expectedly, its device — not yet measured), and the tab resumes its
  conversation when it is next entered.

## 3. The setting — draw every terminal on the CPU

- **Software rendering = WARP**, Direct3D's CPU rasterizer. On a software adapter AtlasEngine picks its
  Direct2D backend. No GPU device is created per terminal, so the limit cannot be reached. XAML and
  DWM composition still use the GPU (one device).
- **Live, every window, no restart.**
  - The cog's Save calls `TerminalPage::_ApplySoftwareRenderingSetting` in the saving window, and
    `_ApplyBroadcastSettings` calls it in every other window.
  - It sets the process-wide override (one Control DLL per process), then calls
    `TermControl::ReapplySoftwareRendering()` on every pane of every tab of that window.
  - A changed value makes AtlasEngine drop its adapter, device and swap chain and build new ones on
    the next frame. The new swap chain is re-attached to the SwapChainPanel, so **a terminal stuck
    black after the storm gets a fresh surface.**
- **New terminals.** `ControlCore::Initialize` and `UpdateSettings` read
  `_softwareRenderingEffective()` = the profile's `rendering.software` **OR** the override.
  `_InitAgentmasterEngine` sets the override before the window creates or restores any tab, so a
  CPU-rendering fleet never builds a per-tab GPU device at all.
- **OR, never "override wins".** With the cog switch off, Terminal's own `rendering.software` (its
  settings UI → Rendering) still applies.
- **Turning it back off** puts every shown terminal back on the GPU at once. With more than ~62 of them
  that walks straight back into the storm, so turn it off after a restart, not mid-session on a big
  fleet.
- **Logged** in hooks.log as `[render] software rendering ON|OFF (cog|broadcast) window <id8>:
  re-pointed N of M terminal(s)`, and at engine init when ON. The cog Save's `[nav] settings-save`
  line carries `softwareRendering=0|1`.

## 4. Cost — is it realistic for many tabs? Yes

Measured 2026-10-01 with `tools/warpbench.cpp` at the release's real terminal size: 178×51 cells =
1,602×969 px (Cascadia Mono 12 pt, 96 DPI, 1920×1032 windows). i9-13900K (32 threads), RTX 4060 Ti.
The machine was 45–60% busy with other work during the runs.

| per full frame | GPU | software (WARP) |
|---|---|---|
| quiet screen (15% filled, 2 colors/row) | 0.10 ms CPU | 1.0–2.8 ms CPU |
| busy Claude screen (60%, 4 colors/row) | 0.31–0.39 ms CPU | 1.9–5.9 ms CPU |
| very dense (90%, 8 colors/row) | — | 5.2 ms CPU |
| RAM per shown terminal | VRAM | ~19–22 MB system commit (3 × 6.2 MB frame buffers + the device) |
| threads | 20 driver threads per terminal | one shared pool: +11 for 1 terminal, +24 for 10, +26 for 30 |

- **Only repainting tabs pay.** The Direct2D backend redraws the whole screen each frame
  (`BackendD2D::Render`: full background fill + every row's glyph runs), so the cost is frames × screen
  size. A terminal with no output paints nothing.
- **The release's real load on 10-01:** 66 initialized terminals, three of them repainting (the running
  sessions). Each woke 16–27 times/s, about 8–13 frames/s, and the whole fleet's render threads used
  12.5 ms of CPU per second on the GPU path.
- **Projection on WARP:**
  - A running claude adds roughly 1–8% of one core (8–13 frames/s × 1–6 ms).
  - 5 running tabs come to at most ~40% of one core, ~1% of this 32-thread machine.
  - A tab streaming fast output (~60 frames/s) costs ~6–36% of a core while it streams.
- **RAM:** 65 shown terminals ≈ 1.2–1.4 GB of system memory, moved from VRAM to RAM. Measured directly
  for 30: +646 MB.
- **Caveat:** when other jobs saturate the CPU (2026-09-27: pexport workers + a Docker benchmark),
  software-drawn tabs repaint late. The GPU path is immune to that, but not to §1.

### 4a. Sizing a big fleet — e.g. 100+ tabs, 30 of them activated

Three different counts matter, and only two of them cost anything:

- **Dormant tabs cost nothing in either mode.** A restored, never-entered tab (or a Deactivated one)
  has no text buffer, no renderer and no device. So 70+ dormant tabs add no rendering cost; they still
  add the app's per-tab UI bookkeeping, which is the same in both modes.
- **Activated terminals set the fixed cost.** Activated means initialized: started in place, entered,
  or viewed since the last restart.
  - On the GPU, 30 activated = 30 devices + XAML's 1. That is under the ~62 limit, but every OTHER tab
    you view adds one more until it is closed (or deactivated). That leaves ~30 views of headroom in a
    100+ tab window before §1's storm.
  - On the GPU, the 30 devices also mean 600 NVIDIA threads plus VRAM.
  - On WARP there is no limit. The 30 terminals cost ~650 MB of RAM (measured) and ~26 threads in
    total.
- **Working terminals set the CPU cost.** Working means repainting right now: a claude mid-turn, or
  output streaming. Idle claudes repaint nothing; 63 of 66 terminals were silent in the 10-01 sample.
  At ~8–13 frames/s each:

| working at once | GPU | software (WARP) |
|---|---|---|
| 5 | ~0.02 core | 0.05–0.4 core |
| 15 | ~0.06 core | 0.15–1.3 cores |
| all 30 | ~0.1 core | 0.3–2.5 cores (1–8% of a 32-thread machine) |

  Add 0.06–0.36 core while one tab streams at ~60 frames/s.

**Rule of thumb:**
- If the process routinely holds more than ~50 activated terminals, software rendering is the safe
  mode: it trades ≤ ~2.5 cores in the all-busy worst case, plus ~20 MB per terminal, for never hitting
  the storm.
- Below that, the GPU is cheaper, as long as finished tabs get deactivated or closed. Restarting the app
  also resets the count, because tabs come back dormant.
- §6's per-window device would remove the trade-off altogether. §4c's release-when-hidden (built and
  deployed to the dev instance 2026-10-03) already removes most of it: only terminals on screen or viewed in the
  last 60 s hold a device, so the ~62 limit is out of reach in normal use without WARP's CPU cost.

### 4b. One tab on screen, many in the strip — where the cost goes

This section is the measurement BEFORE §4c: it explains why §4c exists. After §4c, hidden terminals
stop drawing and give back their device; the numbers below are the baseline to compare against.

Measured 2026-10-03 on the release: 4 windows, 537 tabs, 30 running claudes. Tools: the Windows GPU
performance counters, `tools/threadcensus.cs` on the live process (read-only; it never suspends a
thread), and `tools/devidle.cpp` in a separate process. RTX 4060 Ti, driver 32.0.16.1656, 60 Hz.

**How a frame reaches the screen.**
- Each initialized terminal owns one `Renderer` thread ("Rendering Output Thread"), one AtlasEngine,
  one D3D11 device and one swap chain. The swap chain has 3 BGRA buffers at the panel's pixel size,
  `FLIP_SEQUENTIAL`, frame latency 1, and is bound to the control's `SwapChainPanel`.
- The thread sleeps until output, a selection, a scroll or a timer asks for a frame. The timers are
  the cursor blink (only on the focused terminal) and blinking text (1 s).
- A frame re-shapes only the changed rows (DirectWrite). The D3D backend then draws one background
  quad plus one quad per visible glyph, normally in one instanced draw call. It presents with a dirty
  rectangle (`Present1`, sync interval 1).
- A glyph is rasterized once, into the glyph atlas texture. The atlas starts small (128×128 px at the
  release's font size), doubles when full, and stops at 2× the swap chain's area.
- XAML draws the tab strip, the overlays and the side panel on the one UI thread. DWM composes each
  window: the XAML visuals plus the swap chain of the terminal on screen.

**Nothing paused a hidden terminal (before §4c).**
- A tab switch only changes which content is the child of `_tabContent` (`TabManagement.cpp`).
  `Renderer`, `AtlasEngine` and `TermControl` never check visibility. A minimized window
  (`ControlCore::WindowVisibilityChanged`) only tells ConPTY.
- So a hidden terminal whose claude works (spinner, elapsed timer, streamed text) keeps painting
  frames that DWM never shows. Nothing throttles them: a swap chain that nothing shows presented
  59 frames/s at 60 requested (`devidle`). A bare clear + `Present1` costs ~0.5 ms of CPU per frame.
- An idle claude paints nothing. A hidden terminal is unfocused, so its cursor does not blink.

| Tab state | Render thread | GPU device + VRAM | CPU and GPU time |
|---|---|---|---|
| Dormant (restored or deactivated, not entered since) | none | none | none |
| Initialized, claude idle, shown or hidden | asleep | 1 device, ~20–27 MB (the swap chain alone is 18.6 MB at 1,602×969 px) | the driver's polling only (below) |
| Initialized, claude working, **hidden** | ~3–11 frames/s | same | frames that no one sees |
| On screen, claude working | ~3–11 frames/s | same | its frames + DWM composition |

**The driver polls every device, all the time.**
- 2 of the 20 NVIDIA threads of each device wake ~65–70 times/s, painting or not. A painting device
  adds about 1 wake per frame.
- `devidle` shows the same with no swap chain and no XAML in the process, after 2 s, 150 s and 15 min
  of idleness, and under the exe names `WindowsTerminal.exe` and `Agentmaster.exe`.
- The release: 680 NVIDIA threads (34 devices = 33 terminals + XAML's own), 2,270 wakes/s in total,
  12–16 ms of CPU per second.
- The idle Store Windows Terminal's 4 devices did not poll (0.9 wakes/s in total). The cause is
  unknown: the same driver build, normal priority, no EcoQoS.

**Claude tabs never show WT's progress ring.** Claude Code 2.1.x sends no OSC 9;4 when `WT_SESSION`
is set (`LBe()` in its bundle), and every Agentmaster tab sets it.

**Header animations keep XAML ticking.**
- The tab header has two Forever storyboards: the unsent-draft dots and the cache-warm spark crown.
  They start and stop on their flags only (`TabHeaderControl::_UpdatePendingAnimation` /
  `_UpdateCacheWarmAnimation`). They do not follow `Loaded` / `Unloaded`, as the board and side-panel
  dots do (`BuildPendingDots`).
- A dormant tab with a remembered draft turns its dots on before its claude starts (the
  `NotConnected` branch of `_ScanPendingInputImpl`, PENDING_INPUT.md §5). The release had 50 such tabs
  among its 537. One of them sat two tabs from a selected tab, so its pulse ran all the time.
- XAML's frame thread (`Windows.UI.Xaml.dll+0x6F59C0`) woke 62–68 times/s in both Agentmaster
  instances, and 0 times/s in the idle Store Windows Terminal. While any animation runs, XAML and DWM
  work on every display frame (60 Hz here).
- A header outside the strip's realized range is not in the visual tree, so it draws nothing. Whether
  its running storyboard still costs UI-thread work is not verified.

**The release's numbers (10 s samples).**

| Measure | Value |
|---|---|
| GPU 3D engine, Agentmaster | 0.95% average, 1.41% peak |
| GPU 3D engine, DWM (every app, every monitor) | 3.3% |
| GPU 3D engine, dev instance (12 devices, 1 working tab) | 0.07% |
| Dedicated video memory | release 915 MB (≈27 MB per device); dev 340 MB |
| Render threads that painted | 3–7 of 33–34; 6 sessions Running, 1 of them on screen |
| CPU of all render threads | 9–22 ms/s |
| NVIDIA threads | 12–16 ms/s CPU, 2,270 wakes/s |
| XAML frame thread | 5–8 ms/s CPU, 62–68 wakes/s |
| UI thread | 185–362 ms/s CPU, 450–580 wakes/s (attributed later without ETW, in UI_THREAD.md: XAML's 60 Hz frame loop that the pulses keep running, plus per-tab layout fan-out) |

Per-process GPU memory counters overlap. DWM's "Dedicated Usage" (9.9 GB) includes other processes'
shared surfaces and was larger than the 7.2 GB in use on the whole card. Do not add them up.

**So, with 100 tabs and one on screen:**
- GPU time = the frames of the terminals that work (shown or not) + XAML's and DWM's composition.
  With nothing working and nothing animating, it is close to 0.
- Each initialized terminal holds ~20–27 MB of video memory, a render thread, 20 NVIDIA threads (2 of
  them polling ~68 times/s) and one of the ~62 device slots (§1), until its tab closes or is
  deactivated.
- Dormant tabs cost no rendering at all.

### 4c. Hidden tabs: stop drawing, give the device back

In source since 2026-10-03. **Built and deployed to the dev instance on 2026-10-03, not in a release
yet** (two measurements below "Expected effect"). It acts on §4b's two biggest wastes: hidden terminals
that draw, and devices kept by every tab ever viewed.

**Settings** (Settings cog → Behavior → RENDERING; live in every window on Save):

| Setting | Default | Meaning |
|---|---|---|
| "Stop drawing tabs you can't see" (`pauseHiddenTerminals`) | ON | A terminal that is off screen draws no frames. |
| "Give back the GPU memory of a hidden tab" (`releaseHiddenTerminalDevices`) | ON | After the delay below, the hidden terminal drops its device, swap chain and glyph cache. Needs the first switch. |
| "… after the tab has been hidden this many seconds" (`releaseHiddenTerminalDevicesAfterSeconds`) | 60 | 0 = at once; a blank box = 60; at most 86400. |

- The engine init publishes the policy before any tab exists. The cog's Save and the cross-window
  broadcast publish it again and re-apply it to every terminal, so an already-hidden terminal adopts a new
  delay at once.
- hooks.log: `[render] hidden tabs (settings.json|cog|broadcast) …`. The `[nav] settings-save` line
  carries `pauseHidden=` and `releaseHidden=`.

**How it works.**
- **On screen** means: the `TermControl` is in its window's visual tree (`IsLoaded()`, re-read on
  `Loaded` / `Unloaded`) AND its window is not minimized (`WindowVisibilityChanged`). A tab switch, a
  zoom, the Agent Manager page and a minimize all change it.
- `TermControl::_UpdateRenderingSuspension` pushes the state on each change, and also right before
  painting is first enabled. So a terminal initialized off screen ("Activate Tab") never paints.
- `ControlCore::SetRenderingSuspended` combines it with the process-wide policy
  (`TermControl::ConfigureHiddenRenderingPolicy`) and calls `Renderer::SetPaintingSuspended`.
- While suspended, the render thread parks in `Renderer::_waitWhileSuspended`. Invalidations still
  collect in the engine; only the frames stop. After the delay, the render thread calls each engine's
  `IRenderEngine::ReleaseDeviceResources()` once.
- `AtlasEngine::ReleaseDeviceResources` drops the swap chain, the backend (shaders, buffers, glyph
  atlas, Direct2D target), the device, the context and the adapter. The next `Present()` rebuilds them
  through the device-removed path and hands the `SwapChainPanel` a new surface. `devidle` measured that
  a released device's 20 NVIDIA threads, and their polling, end within 3 s.
- Shown again: the core queues one full repaint (`TriggerRedrawAll`). On a released terminal, that
  frame first rebuilds the device.
- `TriggerTeardown` clears the flag before it wakes the thread, so a close or a tear-out never waits on
  a parked thread.

**The animations.**
- The tab header's draft dots and spark crown run only while the header is in the live tree, and only
  with Windows' "Animation effects" on (`AgentAnimationsEnabled`, `SPI_GETCLIENTAREAANIMATION`).
- A dormant tab's REMEMBERED draft shows still dots. The header reads `AgentStatusHalfVisible` (the
  dormant half-hollow dot) as "this tab has not started", so the pulse begins when its claude starts.
  The board and side-panel cards follow the same rule (`BuildPendingDots(…, animated)`).
- A still spark crown is the steady glow, without embers.
- A change gate (`_pendingDotsRunning` / `_cacheWarmRunning`) keeps re-asserts from restarting a
  running storyboard.

**Expected effect** (projected from §4b; measure it after the build):
- Frames: only on-screen terminals paint. On the release that removes 5 of the 6 painting terminals.
- Devices: from "every tab viewed since start" (34 on the release) to "on screen + viewed in the last
  60 s". Each device dropped saves ~27 MB of video memory, 20 driver threads and ~68 wake-ups/s.
- The XAML frame tick stops whenever no visible header animates.
- To check: `dotnet run tools/threadcensus.cs -- <pid>`. Painting render threads should all be on
  screen. NVIDIA threads ÷ 20 should be about the number of windows + recently viewed tabs + 1 (XAML).
  `Windows.UI.Xaml.dll+0x6F59C0` should wake about 0 times/s when no visible header pulses.
  `dotnet run tools/renderwait.cs -- <pid>` shows each render thread's state: `_suspended (parked)` for
  an off-screen terminal, `_redraw (idle, shown)` for a drawable one.

**First measurement** (dev build, 2026-10-03, 3–4 min after the start: one window, 27 restored tabs, 3
of them shown since the start, two 10–15 s census windows):
- Frames: 3 initialized terminals, 1 painting (the tab on screen).
- Devices: 40 NVIDIA threads, so 2 devices: XAML's (created at the process start) and the tab on
  screen. The 2 hidden tabs had given their devices back (both were hidden for more than 60 s).
  Without §4c they keep theirs: 4 devices.
- XAML's frame thread (`Windows.UI.Xaml.dll+0x6F59C0`): 62 wake-ups/s while the on-screen tab's
  unsent-draft dots pulsed (as designed), 3.2/s after the draft was sent.
- Not measured yet: the release's workload (many claudes in many windows), the blank time when you go
  back to a tab hidden longer than the delay, and the minimized-window path.

**Second check** (dev build, 2026-10-03 20:30–20:41, ~1 h after the start; read-only, with
`tools/threadcensus.cs` and `tools/renderwait.cs`; one window, 29 tabs, 6 of them shown since the start,
the user switching tabs normally):
- Frames: 6 initialized terminals, 0 render-thread wake-ups in two 15 s windows. One hidden tab's
  claude was working all the time: its ConPTY output thread woke ~9 times/s, its render thread never.
- Wait state: the 5 hidden terminals' render threads wait on `Renderer::_suspended` (+0x26). Their
  memory reads `_suspended` = 1, `_suspendedReleaseDelayMs` = 60000 and `_redraw` = 1 (the frame that
  was due, drawn when the tab is shown again). The tab on screen waits on `_redraw` (+0x24), with
  `_suspended` = 0. The window was not minimized.
- Release timing: each parked thread entered its no-timeout wait 27–49 ms after "hidden + 60 s" (the
  hide times are the `[nav] tab-focus` lines in hooks.log). An 8 s hide restarted the count, as
  designed.
- Devices: 40 NVIDIA threads, so 2 devices: XAML's and the tab on screen. That tab's device dates from
  the tab's creation (19:32:11): it was never hidden for more than 35 s. The two devices' driver threads
  woke 136–141 times/s (2 × ~68). Without §4c the 6 shown terminals keep 6 devices: 7 with XAML's,
  ~476 wake-ups/s.
- Video memory (`\GPU Process Memory(pid_<pid>*)\Dedicated Usage`): 57 MB. For scale, the production
  instance (old code, hundreds of tabs) held 1,607 MB at the same time.
- XAML's frame thread: 2.9–3.9 wake-ups/s. Two dormant tabs in the window keep a remembered draft;
  before §4c their dots pulsed and kept this thread at ~60 wake-ups/s.
- hooks.log since the start: no `[exc]`, `[wil]` or `[ui-stall]` line, and no `[tab-switch-slow]` when
  the user went back to tabs whose devices had been released.
- Still not measured: the blank time on a switch back, the minimized-window path, the release's
  workload.

**Trade-offs and limits.**
- Going back to a tab hidden longer than the delay shows a blank terminal for about 0.1 s, while the
  device, swap chain and glyph cache are rebuilt.
- A tab hidden for less than the delay shows its last frame from before it was hidden for one frame
  (~16 ms), then the current screen.
- XAML's own device and DWM are not changed by §4c. The UI thread's cost (18–36% of a core in §4b) is
  attributed in UI_THREAD.md: §4c removes only its part from off-screen and dormant pulses. A pulse
  that stays on screen, the side panel's non-virtualized card list and the per-tab `LayoutUpdated`
  handlers remain.
- A control created while its window is minimized counts as visible until the next minimize/restore
  report (TerminalPage reports only a visible window to a new control).

## 5. Diagnosing a black tab (read-only, nothing suspended)

1. **Is claude up?** `am_inspect.py screen <ref>` reads the ConPTY's console buffer. A ready input box
   there plus a black tab means the problem is rendering, not the session.
2. **Driver events.** `Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='nvlddmkm'}`,
   with times compared to hooks.log `[nav] open-new` / `tab-focus`. hooks.log stamps carry no date, so
   a large tail mixes days.
3. **Device count.** Read each thread's Win32 start address (NtQueryInformationThread class 9) and
   creation time (GetThreadTimes), and group them into 150 ms bursts:
   - `RENDER + CONN + NV×20` in one burst is a healthy tab init.
   - `RENDER + CONN` alone, with `NV×19…NV×1` arriving seconds later, is the storm.
   - NVIDIA threads ÷ 20 ≈ devices.
   - `dotnet run tools/threadcensus.cs -- <pid>` prints this grouping, each device's burst next to its
     render thread, and which render threads painted (§4b).
4. **Renderer state.** NtQuerySystemInformation(5) gives each thread's state and wait reason, and
   ThreadLastSystemCall (class 21) gives its blocked syscall. For a `WaitOnAddress` waiter the first
   argument is the waited address (verified). In a 16-aligned `Renderer`, `_enable` (wil `slim_event`,
   a `LONG`) is at +0x20, `_redraw` at +0x24 and `_suspended` at +0x26 (§4c): an address ≡ 4 (mod 16) is
   idle-awaiting-a-frame, ≡ 0 is painting disabled, ≡ 6 is parked off screen.
   - `dotnet run tools/renderwait.cs -- <pid>` prints this per render thread, the flags read at the
     `Renderer`'s address, and when each wait began (SYSTEM_THREAD_INFORMATION.WaitTime). A black tab
     whose thread waits on `_redraw` believes it presents fine (the 2026-10-01 storm); one on `_enable`
     took the renderer's failure path.
5. **Did anything fall back?** `(Get-Process -Id <pid>).Modules` containing `d3d10warp.dll` means some
   terminal went to WARP, by this setting or by WT's failure fallback.

Don't point a stack-sampling profiler at the release to "look": it suspends threads. See the
livesample warning in `tools/livesample.cpp`.

## 6. Follow-ups (not done)

- One shared D3D11 device per process (or per window) instead of one per terminal: the real fix, an
  AtlasEngine change. The driver's per-device polling (§4b) is one more reason for it. §4c's release
  removes most of the pressure meanwhile.
- Detect the storm (device creation slower than ~1 s, or event-153-shaped failures) and drop new
  terminals to WARP automatically.
- Measure §4c on the release workload (the dev-build measurements and the census checks are in §4c
  "Expected effect"), then tune the default delay. Also still open: the blank time on a switch back to a
  released tab, and the minimized-window path.
- Attribute the UI thread's 18–36% of a core (§4b) with an elevated ETW (WPR) CPU trace.
- Done in §4c: paint nothing for an off-screen terminal; release a hidden terminal's device after a
  delay and rebuild it on show; run the tab-header storyboards only in the live tree, still dots for a
  dormant tab's remembered draft.
