# 20 · Robustness: freezes, crashes, leaks, hostile environments (ROB)

> Conventions and fixtures: [README.md](README.md). Spec: OBSERVER.md §13a (the reaper), CMD_PATH_LIMIT.md,
> STARTUP_GUARD.md, HOOKS.md *Hook invocation* and *Encoding*, UI_THREAD.md (§10–§14), MEMORY_LEAKS.md, RENDERING.md.
> Every flow here also runs the AMBIENT checks of E2E_TESTING.md §7.6 (no `[exc]` / `[wil]` / `[ui-stall]` line, no crash
> dump, no orphaned process, the UI thread answering). Several need a **perf / soak lane** (long runs, many tabs) or a
> **fault-injection** switch that does not exist yet — each flow says which.

**What shipped.** 0.7.7: three UI-freeze classes healed (the RDP theme storm, the dispatch wedge with its self-healing
watchdog, the hidden Manager rebuilding), the WinUI tab-drag crash class removed, orphaned console groups reaped. 0.7.9:
the startup deadlock guard (25 s watchdog), the crash on a mouse move over a just-opened tab, the 8-hour tab-switch freeze,
the large-fleet CPU work (Composition animations, in-place side panel, the transcript watcher), hooks in exec form with
timeouts, the PATH guard for Ctrl+G, UTF-8 hook payloads and a non-ASCII user name. 0.8.0: the duplicate-conversation
freeze (STA-08), the side panel's virtualized node-free cards, the UText leak in every terminal, a killed fullscreen claude
(STA-11).

---

### ROB-01 · Orphaned console sessions are reaped

> **Since** 0.7.7 · **P0** · **Needs** mock, F-S · **Spec** OBSERVER.md §13a

**Phases**
1. **Kill the host.** *Do:* find the session tab's `OpenConsole.exe` (its process tree) and `Stop-Process` it. *Expect:*
   within ~5 s (≥ 3 surveys) the tab's `pwsh` and `claude.exe` are terminated, with their MCP children on that console.
   *Check:* [LOG] `[orphan] …` then `[orphan-reap] …`; [OS] no process of that console group left; the session never appears
   in the External census.
2. **Never the wrong one.** *Do:* run `claude` by hand in a plain console window (not Agentmaster's), and in a real Windows
   Terminal; kill THEIR hosts' parents' unrelated processes. *Expect:* never reaped (no `AM_SESSION` stamp).
3. **Off.** *Do:* cog → Behavior → "Shut down sessions whose terminal host has died" off; repeat phase 1. *Expect:* recognized
   as an orphan (no External row), left running.

**Edge cases**
- **E1 A graceful close** (the tab's ✕) → never logged as an orphan (the host was alive during CTRL_CLOSE).
- **E2 A host whose console host pid reads 0 (unknown)** → never treated as dead.

---

### ROB-02 · An over-long PATH does not break Ctrl+G or the shells

> **Since** 0.7.9 · **P1** · **Needs** a guest User Path padded with 120 dead entries (> 9,000 characters) · **Spec** CMD_PATH_LIMIT.md

**Phases**
1. **Start.** *Expect:* [LOG] one `[path-guard]` line (the PATH compacted: duplicates and empties first, then directories
   that provably do not exist); the registry's User Path unchanged ([OS]).
2. **Ctrl+G.** *Do:* in a session, Ctrl+G. *Expect:* the editor opens (`edit.exe`, the `EDITOR=edit` default — MSC-07); no
   "Edit quit unexpectedly (exit code 1)".
3. **A shell tab.** *Do:* a new pwsh tab: `cmd /c where cmd`. *Expect:* found.

**Edge cases**
- **E1 A healthy PATH** → untouched (no `[path-guard]` change).
- **E2 Live directories over the limit** → kept (only provably-missing ones dropped).

---

### ROB-03 · Hooks are fast and never cancelled

> **Since** 0.7.9 · **P0** · **Needs** mock; a slow `BASH_ENV` (a script that sleeps 5 s) in the guest environment · **Spec** HOOKS.md *Hook invocation*

**Phases**
1. **Exec form.** *Expect:* [LOG] `[hooks] claude <ver> -> EXEC-form`; the hooks settings file lists the forwarder as an exe
   with `args` and a `timeout` (10 s; SessionEnd 5 s).
2. **Launch is not slowed.** *Do:* launch a session. *Expect:* `[SessionStart]` in hooks.log within ~2 s of the spawn despite
   the 5 s `BASH_ENV`.
3. **SessionEnd delivered.** *Do:* `/exit` in the session. *Expect:* `[SessionEnd]` arrives; the transcript's `attachment`
   lines show `hook_success` for SessionEnd, no `hook_cancelled`.

**Edge cases**
- **E1 A claude older than 2.1.139** (the pinned-old lane) → shell form, still delivered.
- **E2 The bridge down** (the app closed while a claude runs) → the forwarder exits within its timeout; `forwarder-errors.log`
  names the drop.

---

### ROB-04 · Non-ASCII everywhere: user name, paths, prompts

> **Since** 0.7.9 · **P1** · **Needs** a guest account named `משתמש` (Hebrew) with its profile under `C:\Users\משתמש`, mock · **Spec** HOOKS.md *Encoding*

**Phases**
1. **Hooks reach the app.** *Do:* as that user, launch a session. *Expect:* `[SessionStart]` and every later hook arrive (the
   forwarder file read correctly: it carries a UTF-8 BOM).
2. **A Hebrew prompt.** *Do:* `שלום [[mock reply "ok"]]`. *Expect:* recorded ONCE as Typed with the right text (never
   `╫⌐╫£╫ò╫¥`); [MOCK] the text intact.
3. **A Hebrew folder.** *Do:* launch in `C:\e2e\תיקייה`. *Expect:* the tab, the card and the transcript lookup all work.

---

### ROB-05 · Pointer input never reaches a terminal that has not started

> **Since** 0.7.9 · **P0** · **Needs** mock, synthetic mouse moves (SendInput at ~100 Hz over the terminal area) · **Spec** CLAUDE.md *A ConPTY connection's process spawns on the control's first non-zero layout*

**Phases**
1. **Fork under a moving mouse.** *Do:* keep the mouse moving over the terminal area; fork the session 30 times (tab menu →
   Fork session ▸ Default), each time moving the mouse over the new tab at once. *Expect:* no crash; every fork's terminal
   starts. *Check:* no dump; no `[exc]` naming `TextBuffer`.
2. **Wheel and scroll bar** over a just-opened tab → ignored until it starts.

---

### ROB-06 · No freeze on the known freeze shapes

> **Since** 0.7.7 / 0.7.9 / 0.8.0 · **P1** · **Needs** perf lane (60–400 tabs), F-CORP's repeated-prompt session · **Spec** UI_THREAD.md; memory `rdp-theme-reload-freeze`, `prompt-anchor-rfind-infinite-loop-freeze`, `manager-lens-rebuild-tax`

The probe in every phase: a UI-responsiveness ping (a UIA property read on the window, or the test bridge's `ui` request,
answered on the UI thread) every 250 ms; a pass = no gap over 2 s and no `[ui-stall]` line.

**Phases**
1. **Theme flip.** *Do:* 42 tabs; switch Windows between light and dark (or broadcast `WM_SETTINGCHANGE` "ImmersiveColorSet")
   five times. *Expect:* responsive throughout; the tabs re-themed.
2. **A repeated prompt.** *Do:* select F-CORP's session whose conversation repeats a prompt's opening words 500 times, switch
   to it 20 times. *Expect:* each switch under 250 ms ([LOG] no `[tab-switch-slow]`).
3. **A notify storm with the Manager hidden.** *Do:* 300 sessions; make 30 of them change state every second for a minute while
   a terminal tab is shown. *Expect:* responsive; the Manager page shown afterwards is current.
4. **The same with the Manager shown** → rebuilds space themselves (`[manager] slow rebuild …` at most once a minute); clicks
   still answered.

**Edge cases**
- **E1 RDP connect / disconnect** (the original trigger) → the same as phase 1 (needs an RDP client against the guest).

---

### ROB-07 · Large fleets cost little at rest

> **Since** 0.7.9 / 0.8.0 · **P2** · **Needs** perf lane: 400 restored tabs (40 live claudes at rest), `tools/cpuhunt`, `tools/leakhunt/xamltree.py` · **Spec** UI_THREAD.md §10, §13, §14; SIDE_PANEL.md §2f

**Phases**
1. **At rest.** *Do:* restore the 400-tab workspace, wait 2 min, measure 5 min. *Expect:* the UI thread under 10% of a core;
   XAML's frame thread ~3 wake-ups/s when no draft pulses (`tools/threadcensus.cs`).
2. **The side panel GLOBAL.** *Expect:* realized cards only around the view (~2 viewports), the composition node count near
   ~700 not ~3,600; scrolling realizes cards smoothly; ⌖ reveals a far card.
3. **The bot's state file.** *Do:* the bot on with 400 topics, quiet for 5 min. *Expect:* `state.json` not rewritten
   ([OS] its write time unchanged; MESSAGING.md §21.35).

---

### ROB-08 · No memory growth from URL scans and Find

> **Since** 0.8.0 · **P2** · **Needs** soak lane · **Spec** MEMORY_LEAKS.md (the UText leak)

**Phases**
1. **Output with URLs.** *Do:* a shell tab printing 100,000 lines that each hold a URL; Find (Ctrl+Shift+F) 2,000 times.
   *Expect:* the process's private bytes back within 5 MB of the start after a settle (the old leak: 144 bytes per scan,
   ~14 MB for 100,000 scans).

---

### ROB-09 · A hung startup ends itself; the next launch works

> **Since** 0.7.9 · **P2** · **Needs** a fault-injection switch that makes the startup block (not built) · **Spec** STARTUP_GUARD.md

**Phases**
1. **Hang.** *Do:* start with the injected block. *Expect:* after 25 s [LOG] `[startup-hang] …`; the process gone; the marker
   file written.
2. **Next launch.** *Expect:* starts normally; a second hang in the same episode is not killed again (one-shot per episode).

**Note.** The real trigger (an NVIDIA driver DLL init deadlocking against the first known-folder lookup) cannot be reproduced
in the VM; the pre-warm is covered by the m5 `TestStartupGuard`.

---

### ROB-10 · A dispatch wedge heals itself

> **Since** 0.7.7 · **P2** · **Needs** fault injection (stall the UI dispatcher's queue) · **Spec** memory `ui-dispatch-wedge-freeze`; UI_THREAD.md

**Phases**
1. **Wedge.** *Expect:* `[ui-stall]` lines; the watchdog force-pumps the queue; clicks work again.
2. **Unhealable.** *Expect:* a clean self-restart with the workspace restored (windows, tabs, the selected tab).
