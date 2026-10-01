# Rendering — the per-process GPU device limit and the "Software rendering" setting

Setting: `AppSettings::softwareRendering` (Settings cog → **Behavior → RENDERING → "Software
rendering"**, default **OFF** = the GPU). Engine hook: `ControlCore::SetSoftwareRenderingOverride` /
`ReapplySoftwareRendering` (`src/cascadia/TerminalControl/ControlCore.{h,cpp}`), projected on
`TermControl`. App side: `TerminalPage::_ApplySoftwareRenderingSetting`
(`TerminalPage.AgentEngine.cpp`). Benchmark: `tools/warpbench.cpp`. Harness: the `softwareRendering`
round-trip in `tests_persistence.cpp`.

## 1. The problem — one GPU device per terminal, and a limit near 62

- **Every terminal that has been shown owns its own Direct3D 11 device** (AtlasEngine creates one per
  `TermControl`), and keeps it until its tab is CLOSED. Hidden tabs keep theirs. Deactivated tabs should
  give theirs back: "Deactivate ▸" now closes the tab's control the way a pane close does and parks a
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
| RAM per shown terminal | VRAM | ~19 MB system commit (3 × 6.2 MB frame buffers) |

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
- **RAM:** 65 shown terminals ≈ 1.2 GB of system memory, moved from VRAM to RAM.
- **Caveat:** when other jobs saturate the CPU (2026-09-27: pexport workers + a Docker benchmark),
  software-drawn tabs repaint late. The GPU path is immune to that, but not to §1.

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
4. **Renderer state.** NtQuerySystemInformation(5) gives each thread's state and wait reason, and
   ThreadLastSystemCall (class 21) gives its blocked syscall. For a `WaitOnAddress` waiter the first
   argument is the waited address (verified). In a 16-aligned `Renderer`, `_enable` (wil `slim_event`,
   a `LONG`) is at +0x20 and `_redraw` at +0x24: an address ≡ 4 (mod 16) is idle-awaiting-a-frame,
   ≡ 0 is painting disabled.
5. **Did anything fall back?** `(Get-Process -Id <pid>).Modules` containing `d3d10warp.dll` means some
   terminal went to WARP, by this setting or by WT's failure fallback.

Don't point a stack-sampling profiler at the release to "look": it suspends threads. See the
livesample warning in `tools/livesample.cpp`.

## 6. Follow-ups (not done)

- One shared D3D11 device per process (or per window) instead of one per terminal: the real fix, an
  AtlasEngine change.
- Release a hidden terminal's device and swap chain after N minutes off-screen, and recreate on show.
- Detect the storm (device creation slower than ~1 s, or event-153-shaped failures) and drop new
  terminals to WARP automatically.
