# 18 · Notifications, the taskbar and the display (DSP)

> Conventions and fixtures: [README.md](README.md). Spec: NOTIFICATIONS.md §2–§7 (toasts), §8 (the taskbar count), §9
> (the preview frame); RENDERING.md §4c, §5; CLAUDE.md *The tab strip state dot*, *Settings cog* (TAB SIZE, RENDERING).
> Needs **F-W2** and, for toasts, **F-PKG** (the MSIX package build: an unpackaged copy shows no session toast — DSP-K1).
> Pixel oracles: a screenshot crop compared with the expected color (the flash ring, the badge disc, the frame); the
> taskbar overlay and the DWM preview through Windows APIs where they can be read, else screenshots of the flyout.

**What shipped.** 0.7.7: the completion toast's third line (the prompt that finished) and its ~25 s on screen. 0.7.9: the
red **unread count** on the taskbar button; the **frame** around the taskbar / Alt+Tab preview of a window holding an
unread tab; the selected tab of a BACKGROUND window now rings and counts; **Software rendering** (live); hidden tabs stop
drawing and give back their GPU memory; **Tab size** (width mode, min / max width, title font, height).

## Lifecycle model

```text
 a session leaves Running for Waiting / Needs approval / Idle while you are not looking at its tab
     ─► its tab's flash ring (blinking, lockstep) ─► the window's unread set ─► the app's count (union of every window)
          │                                              │                          └─► the red number on the taskbar button
          │                                              └─► the frame on that window's taskbar / Alt+Tab preview
          └─► a toast (title · "Has completed after <span> and is <state>" · the prompt), ~25 s, click → the tab, in front
 cleared by: visiting the tab · activating the window (its focused tab) · the session Running again · Move to Idle/Done · the tab closing
 Mark Unread: rings + counts + frames until the tab is VISITED (activating the window does not clear it)
```

---

### DSP-01 · A finished turn rings, counts and frames until you look

> **Since** 0.7.9 · **P0** · **Needs** mock, F-W2, pixel sampling · **Spec** NOTIFICATIONS.md §8, §9; CLAUDE.md *The tab strip state dot*

**Phases**
1. **Ring.** *Do:* in window A send `[[mock delay 4s; reply "a"]]`, then select another tab of A. *Expect:* when the turn ends,
   the session's tab dot grows a red ring that blinks (600 ms on / off); other blinking rings blink in step. *Check:* [ENG]
   the session in the window's unread set; [LOG] `[unread] 0 -> 1 (window <id8>)`.
2. **The count.** *Expect:* the taskbar button shows a red disc with `1`; its accessible text "1 unread Agentmaster session".
3. **Two windows.** *Do:* the same in window B. *Expect:* the count `2` on the (combined) button.
4. **The frame.** *Do:* hover the taskbar button; Alt+Tab. *Expect:* both previews framed in the ring's color; [LOG]
   `[taskbar-frame] window 0x… framed`.
5. **Clear.** *Do:* visit A's tab. *Expect:* A's ring gone, the count `1`, A's preview unframed at once.
6. **A background window's selected tab.** *Do:* leave B with its session's tab selected, activate another app; let a turn
   finish there. *Expect:* it rings and counts; activating window B clears it.

**Edge cases**
- **E1 → Done or → Error** → no ring, no count (by design).
- **E2 The session goes back to Running** → the ring and its count go.
- **E3 Mark Unread** (tab menu) → rings and counts; activating the window does NOT clear it; visiting the tab does.
- **E4 The ring color** (cog → TABS → Status flashing color, e.g. 50% blue) → rings recolored live in every window; the
  frame recolored (its alpha raised to ≥ 50%).
- **E5 The cog's TASKBAR switches off** → no number / no frames; back on → restored.
- **E6 Explorer restarted** → the number comes back (`TaskbarButtonCreated`).
- **E7 150 unread** → the disc reads `99+`, the accessible text the exact number.
- **E8 A post-answer blip** (an answer, then a sub-3-second run to Waiting) → no ring.
- **E9 A minimized framed window** → its preview keeps the framed picture.

---

### DSP-02 · Completion toasts

> **Since** ≤ 0.7.7 (third line + ~25 s 0.7.7) · **P1** · **Needs** mock, F-PKG · **Spec** NOTIFICATIONS.md §2–§6

**Phases**
1. **A toast.** *Do:* send `p1 [[mock delay 5s; reply "x"]]` and switch to another tab. *Expect:* when the turn ends a toast:
   line 1 the session title; line 2 `Has completed after <span> and is <state>` (the observed Running span); line 3
   `p1 …` (the prompt, cut);
   ~25 s on screen; the default sound. *Check:* [LOG] `[notify] <sid> running -> waiting (after 5s)`.
2. **Replace.** *Do:* a second turn of the same session. *Expect:* the newer toast replaces the older in the notification
   center (one per session).
3. **Click.** *Do:* click the toast while another app is in front. *Expect:* the hosting window comes to the front, restored
   if minimized, with the session's tab selected; no second process, no new window. *Check:* [LOG] `[nav] notify-click`.
4. **Settings.** *Do:* turn off "Waiting for you"; finish a turn. *Expect:* no toast; the ring still rings.

**Edge cases**
- **E1 The focused tab of the active window** → no toast (default "skip when focused").
- **E2 A background agent / shell still running when the turn ends** → the toast HELD while the work runs: dropped if the
  session goes Running again (`[notify-hold]` → `[notify-drop]`), shown with the current state when the work quiets.
- **E3 W → R → W within 20 s** → one toast (`[notify-dedupe]`).
- **E4 Sound off** → a silent toast.
- **E5 The app closed, then a toast clicked** → Agentmaster starts (workspace restored) and jumps to the session if the
  activation is still pending (best effort).
- **E6 A prompt with `<`, `&` and quotes** → shown literally.

---

### DSP-03 · Software rendering, live

> **Since** 0.7.9 · **P1** · **Needs** three windows of terminals · **Spec** RENDERING.md §4–§5

**Phases**
1. **Turn on.** *Do:* cog → Behavior → RENDERING → Software rendering; Save. *Expect:* every terminal of every window keeps
   drawing (now on the CPU); [LOG] a `[render]` line; [OS] the process loads `d3d10warp.dll`, and its
   NVIDIA (or vendor) driver thread count does not grow when new tabs are opened.
2. **Turn off.** *Expect:* every terminal back on the GPU, nothing black.
3. **A new tab while on.** *Expect:* drawn on the CPU from its first frame.

**Edge cases**
- **E1 Terminal's own `rendering.software`** → OR'ed: still software.
- **E2 A guest without a GPU** (the test VM is WARP already) → the switch must not break anything; the GPU-limit symptom
  itself is reproduced only on a real NVIDIA PC (out of the VM suite; RENDERING.md §5 has the read-only diagnosis).

---

### DSP-04 · Hidden tabs stop drawing and release the GPU

> **Since** 0.7.9 · **P1** · **Needs** a running session in a background tab, `tools/renderwait.cs` / `tools/threadcensus.cs`
> (read-only) · **Spec** RENDERING.md §4c

**Phases**
1. **Hidden.** *Do:* a session printing continuously (`[[mock stream 5 100; say "<2,000 chars>"; reply "x"]]`) in a tab you leave.
   *Expect:* its render thread parks (waits on `_suspended`); no frames while hidden.
2. **Release.** *Do:* wait 60 s. *Expect:* its device released (the wait start of the parked thread ≈ the deadline);
   [LOG] none per tab (only `[render] hidden tabs …` at setup).
3. **Back.** *Do:* select the tab. *Expect:* a brief blank (~0.1 s) then the up-to-date screen; no black tab.
4. **Minimized window** → its terminals stop drawing too.

**Edge cases**
- **E1 The delay set to 0** → released at once; blank = 60.
- **E2 Both switches off** → hidden tabs keep their device (the old behavior).

---

### DSP-05 · Tab size

> **Since** 0.7.9 · **P2** · **Needs** F-W2, 40 tabs · **Spec** CLAUDE.md *Settings cog* (TAB SIZE)

**Phases**
1. **Equal width** with min 80 / max 160 → every tab the same width between the two; a long title ends in "…" inside its tab.
2. **Fit to title** → a long title widens its tab up to the max.
3. **Compact** → only the max applies.
4. **Font 16 Bold, min height 48** → applied live in every window (the broadcast).
5. **Windows Terminal setting** (default) → Terminal's own `tabWidthMode` again.
6. **Rename in Equal width** → the selected tab held at the max width while its box is open.

**Edge cases**
- **E1 Min above max** → the max raised to the min.
- **E2 Reset tab size to defaults** → the form only (Save applies).

---

## Known defects (expected failures until fixed)

### DSP-K1 · A portable (unpackaged) copy shows no session toast, and its clicks would do nothing
> **P1** · memory `toast-notification-gaps` (2026-09-27, verified by probe)

`_ShowAgentSessionToast` calls the no-arg `CreateToastNotifier()`, which needs package identity, so every toast of a
portable / per-user install throws (logged once `[notify] toast failed`). And `ToastActivator::IsRegistered()` returns
true from an unpackaged process, so the in-process click fallback is never wired. **Repro:** the per-user install (INS-01),
finish a turn in a background tab → no toast. **Expected:** a toast under the shortcut's AUMID whose click surfaces the tab.
