# System notifications — Windows toasts when a session leaves Running

> Status: **complete — the Settings cog "Notifications" tab + the Running → X toast pipe + click-to-surface
> (bring the hosting window to the FRONT and jump to the session's tab).** Settings round-trip unit-tested
> (engine harness green); the full chain lib-compiles green (TerminalAppLib). Runtime verification (a live
> toast + the `[notify]` trace + the click-to-surface) pends a deploy — gated on the user's build/deploy
> permission.

## 1. What this is & why

Agentmaster's whole point is running **N sessions while you look at one of them**. The Triage Board, the
tab status dot, and the flash ring all signal "a session needs you" — but only **inside** the app. When
the app is minimized, behind your editor, or on another monitor's stack, a finished agent is invisible
until you happen to switch back.

The notification system closes that gap with a **Windows toast** whenever a managed session's status
changes **from Running to anything else** (the default rule — configurable per target state):

```
<session title>
Has completed after 2h30m and is waiting for you
```

- **Line 1** — the session's title (the ONE title value, Rule #11: Explorer name == tab title ==
  persisted `SessionInfo.title`).
- **Line 2** — `Has completed after <duration> and is <status>`: the **duration** is the session's
  observed **Running span** (how long the turn worked, compact `2h30m` / `5m12s` form — the tab tooltip's
  `TtSpan`), and the **status** is the state it landed on (`waiting for you` / `needs approval` / `idle` /
  `done` / `error` — the tooltip's `TtStateLabel` vocabulary). When the Running **entry** edge was never
  observed (a session adopted or moved into the window mid-turn), the `after <duration>` clause is
  **omitted** — an honest "no duration" over an under-reported one.
- **Clicking the toast surfaces the session**: the hosting window is brought to the **front**
  (restore-if-minimized + foreground) and the session's **tab is selected** — see §4.
- Both managed agents ride it: **Claude** (all six states) and **Codex** at its 3-state floor
  (Running / Waiting / Idle — OBSERVER.md §11f).

This is a **UI-layer consumer of the existing state engine** — no new state source, no engine changes:
the toast fires off the same registry-observer push that recolors the tab status dot, so whatever the
push/pull machinery decided (hooks, transcript-tail reconcile, recon-error, presence-idle release, …)
is exactly what notifies.

## 2. The settings (the cog's "Notifications" tab)

A new top tab in the Settings cog (between **Behavior** and **Tabs & Overlay**; the card widened
560 → 660 so seven tab buttons fit one row). All keys live in `AppSettings` (`settings.json`) and are
read **at fire time** from the page's `_appSettings` — which the cog Save + the cross-window settings
broadcast already keep current — so a change applies **live**, no restart, in every window.

| key | control | default | meaning |
|---|---|---|---|
| `notificationsEnabled` | "Show Windows notifications" toggle | **ON** | the master switch; while OFF the dependents grey out (but keep their stored values) |
| `notifyOnWaiting` | "Waiting for you" checkbox | **ON** | notify on Running → WaitingForInput |
| `notifyOnNeedsApproval` | "Needs approval" checkbox | **ON** | notify on Running → NeedsApproval |
| `notifyOnIdle` | "Idle" checkbox | **ON** | notify on Running → Idle |
| `notifyOnDone` | "Done" checkbox | **ON** | notify on Running → Done (claude exited cleanly) |
| `notifyOnError` | "Error" checkbox | **ON** | notify on Running → Error (the recon-error API-failure capture) |
| `notifySuppressFocused` | "Skip when the tab is focused" toggle | **ON** | skip the toast when the session's tab is the FOCUSED tab of the ACTIVE window (you watched it finish); a focused tab in a **background** window still notifies |
| `notifySound` | "Play the notification sound" toggle | **ON** | OFF emits `<audio silent="true"/>` — a visual-only toast |

- **The transition is always FROM Running** — the five checkboxes pick the *target* states. All checked
  == the requested default rule, "Running to anything else"; unchecking one mutes just that transition.
  An at-rest → at-rest move (e.g. the Waiting → Idle decay, or a manual triage move) never notifies.
- **Missing-key ⇒ ON** (`Persistence.cpp` — every switch defaults true), so a pre-feature
  `settings.json` gets the default rule with zero migration.
- The checkboxes are **read back even while disabled** (master OFF) — like `_setInferGitRoot`, they
  still hold the user's stored preference, and dropping them on Save would silently reset a mute.
- The suppress-focused rule is the flash ring's *"the current tab is always considered visited"*
  applied to toasts (`_activated && tab == _GetFocusedTab()`).

## 3. Architecture (the firing pipe)

```
SessionRegistry::_notify (any session change; bridge/scanner/UI thread)
  └─ per-window dot observer (TerminalPage.AgentEngine.cpp) → dispatcher hop →
     TerminalPage::_UpdateTabAgentDot(id, state, live, dormant)          (UI thread)
       ├─ !live → erase the toast track  ◄── BEFORE the host gate (§5 — the archive seams
       │                                      drop _claudeTabs before this queued hop lands)
       ├─ host gate: _claudeTabs.find(id) — only the ONE hosting window proceeds
       ├─ _SetTabAgentDot / _EvaluateAgentFlash            (the dot + flash ring, unchanged)
       ├─ _EvaluateAgentNotification(id, tab, state, live) ◄── the toast edge tracker
       │    ├─ enter Running on a SEEN edge → stamp _agentNotifyRunningSinceMs[id] = now
       │    ├─ leave Running → consume the stamp (the duration), then the gates:
       │    │     master → per-target-state switch → suppress-focused
       │    ├─ title: registry (Rule #11) → tab text → "Agent session"
       │    └─ _ShowAgentSessionToast(id, title, body, silent)   + "[notify] …" in hooks.log
       └─ _UpdateTabAgentToolTip
```

- **Its own edge tracker.** `_agentNotifyLastState` + `_agentNotifyRunningSinceMs` are DELIBERATELY
  separate from the flash ring's `_agentFlashLastState`, even though both watch the same push: the flash
  holds/erases its map on its own rules (visited-clears, manual Mark Unread, target-state semantics),
  and coupling the two would let a change in either feature silently break the other's edge detection.
  Two small per-window maps are the cheap price of independence.
- **Exactly one toast per transition, N windows notwithstanding.** Every window's observer sees every
  fleet event, but `_UpdateTabAgentDot` proceeds only for a session in **this** window's `_claudeTabs` —
  and a live tab is hosted by exactly one window.
- **Duration = the observed Running span.** The stamp is written only on a **SEEN** entry edge
  (`prev != Running → Running` with a tracked prev). A session first observed **already Running** —
  adopted mid-turn, or its tab moved into this window mid-turn — gets **no stamp**, so its completion
  toast reads `Has completed and is <status>` instead of reporting a span measured from first sight.
  Leaving Running consumes the stamp whether or not a toast is ultimately shown (the span belonged to
  the turn that just ended; a re-entry restamps).
- **Fires for every state producer.** Real hooks (`Stop`, `Notification`), the scanner's synthesized
  reconciles (`recon-stop`, `recon-block`, `recon-error`, presence-idle release, …), and the managed-
  Codex rollout-tail reconcile all mutate the registry through the same notify — the toast keys on the
  resulting *state edge*, not on who produced it.

## 4. The toast itself (`_ShowAgentSessionToast`)

- **ToastGeneric** XML with two `<text>` lines; the title/body go in as **DOM text nodes**
  (`XmlDocument::CreateTextNode`), so XML-special characters in a session title (`&`, `<`, quotes …)
  are escaped by the DOM — never hand-built markup.
- **Tag + Group replacement.** `Tag = ShortId(sessionId)` (first 8 chars — fits the legacy 16-char Tag
  cap) + `Group = "agentmaster"`: a session's **newer** toast REPLACES its older one in the Action
  Center instead of piling up. Distinct sessions keep distinct toasts.
- **Sound** — the default system notification sound; `notifySound` OFF swaps the template for one with
  `<audio silent="true"/>`.
- **Click → surface the session.** The in-process `ToastNotification.Activated` event (the platform
  raises it on a non-UI thread → marshaled to the window's dispatcher) runs the **foregrounding**
  jump — deliberately NOT `_ActivateClaudeSession`, whose local path skips the foreground
  (`bringWindowToFront=false` is right for an in-window click, which is already foreground). A toast
  click arrives from the **shell** with the app possibly minimized or behind other apps, so:
  - **local** (this window still hosts the tab): `_FocusClaudeSessionTab(id, /*bringWindowToFront*/ true)`
    — select the tab + **restore-if-minimized + `SetForegroundWindow` + the `SwitchToThisWindow`
    fallback** (the shell may deny a foreground hand-off to a background process; the fallback performs
    the Alt+Tab-style switch).
  - **moved** (the tab migrated to another window since the toast was shown):
    `ActivateSessionInOtherWindows(id, _windowId)` — the engine's activate fan-out; the receiving
    window's sink runs the SAME `_FocusClaudeSessionTab(id, true)` recipe, so both ends foreground.
  - The jump is logged as `[nav] notify-click <id8> (toast -> foreground window + jump to tab)` — the
    shell-side entry in the user-navigation audit trail.
  - Selecting the tab rides the normal tab-switch funnel, so the flash ring / unread mark / Manager
    lens selection all clear/sync exactly as if the user clicked the tab.
- **Best-effort by design.** `ToastNotificationManager::CreateToastNotifier()` (no-arg — the packaged
  app's AUMID) **throws on a build with no package identity** (unpackaged test hosts), and `Show` can
  fail when notifications are disabled system-wide. Both are swallowed; the FIRST failure logs
  `[notify] toast failed …` (`_agentToastFailLogged` — once is signal, per-fire is noise).

## 5. Stale-track hardening (where the cleanup runs, and why)

The toast tracker must **forget** a session when it closes, or a state recorded in a previous life
leaks: close a session while **Running**, later **resume** it, and its first live update (Idle — the
`RestoredSessionState` normalization) would read as a Running → Idle edge and phantom-fire a
"completed" toast.

The subtlety is **event ordering**: the archive seams write `live=false` to the registry and then erase
`_claudeTabs` **synchronously on the UI thread**, while the observer's reaction is a **queued**
dispatcher hop — so by the time the hop lands, the host-gated path already misses. Three cleanups make
the track leak-proof:

1. **`_UpdateTabAgentDot`, TOP, before the host gate** — every `live=false` update erases the track,
   host or not (covers tab close / batch close / liveness sweep / tab-swap re-home / teardown).
2. **`_EvaluateAgentNotification`'s own `!live` branch** — the belt-and-braces mirror of
   `_EvaluateAgentFlash`'s rule (idempotent with 1).
3. **The two move-out seams** (`TerminalPage.AgentSessions.cpp`) — a tab dragged to ANOTHER window
   keeps `live=true` (no `live=false` event fires), so the seams that drop the per-window binding also
   drop the track; the destination window re-tracks from first sight (and, mid-turn, deliberately
   without a duration stamp — §3).

## 6. Known behaviors & limitations (v1)

- **The teammate/background-work flap is ABSORBED by a signal-gated HOLD (was: toasts twice).** A lead
  session with a live shell / background agent / teammate ends its turn with a real `Stop` (→ Waiting)
  and is re-promoted to Running by the outlived-turn `recon-subagent` — for a shell-only session up to
  ~20–24 s later (the promotion's presence arm needs the transcript quiet past
  `kScanExternalWorkGraceMs`; proven live on `513d1366`: "waiting for you" toasted at the Stop edge,
  then presence=shell re-lit Running 20 s later). A shown toast can't be recalled, so the edge now
  **HOLDS** a Running → **Idle/Waiting** toast while the session's external-work signal is live
  (`PresenceIsWorking` on the registry's heartbeat copy, or side files fresh within
  `kScanSubagentFreshMs` — the same two raw inputs the promotion reads, evaluated on demand by
  `AgentExternalWorkSignal`): the liveness tick sweeps the hold (`_SweepAgentPendingToasts` → the pure
  `DecideHeldToast`, SessionScanner.h) — **dropped** when the session re-lights Running
  (`[notify-hold]` → `[notify-drop]`, no pop; the push edge's Running re-entry is the primary drop,
  the sweep the belt), **fired with the CURRENT state** when the signal clears (a real completion's
  post-Stop `busy` linger costs ~one 2.5 s sweep tick of toast latency), a hard needs-you state lands
  mid-hold, or the `kNotifyExternalHoldCapMs` (30 s) backstop elapses — the cog switches +
  focused-skip re-apply at fire time (a tab visited during the hold suppresses). **NeedsApproval /
  Error / Done never hold** — external work can't answer a question, undo a failure, or revive an
  exited claude. A **per-session double-toast guard** (`kNotifyDuplicateToastMs`, 20 s;
  `[notify-dedupe]`) additionally caps a W→R→W flap at one SHOWN toast per window, immediate and
  deferred paths alike. The flash ring still fires on the transient edge (deliberate — it self-clears
  on the re-promotion; a toast doesn't).
- **Click after the app exits = plain launch.** No toast **COM activator** (`ToastActivatorCLSID`) is
  registered in the manifests, so clicking a leftover toast once the app is gone just launches
  Agentmaster (the single-instance handoff opens its normal startup window) — it does not deep-link to
  the session. Registering an activator (+ launch args carrying the session id → a startup jump) is the
  documented follow-up if click-after-exit matters.
- **Suppress-focused is (active window && focused tab).** A focused tab in a background window still
  notifies — deliberate: you are not looking at it.
- **Uniform wording.** `Has completed … and is error` reads slightly off for Error — the template is
  kept uniform by design (the requested shape); the state label carries the meaning.
- **Not dev-gated.** Unlike the Tests Autorunner, notifications are a user feature — release installs
  get the tab and the toasts.

## 7. Verification

- ✅ **Settings round-trip** (engine harness `tests_persistence.cpp`): all 8 switches stored OFF
  round-trip; the empty/garbage defaults block asserts all 8 default **ON** (the "Running to anything
  else" rule). Harness green (1787 checks, 0 failures).
- ✅ **Toast HOLD gates** (engine harness `tests_transcript.cpp`, beside the outlive checks):
  `ShouldHoldCompletionToast` / `DecideHeldToast` / `ShouldSuppressDuplicateToast` matrices (15
  checks — hold only Idle/Waiting; Drop on re-lit Running / archived; Fire on signal-clear / hard
  needs-you / cap; the 20 s dedupe boundary) + the compile-time cap-vs-promotion-latency
  `static_assert`. Harness green (1907 checks, 0 failures).
- ✅ **Full chain compiles**: TerminalAppLib (the cog tab + the edge tracker + the toast + the click
  handler) builds green.
- ⏳ **Runtime** (pends a deploy — build/deploy permission): start a turn, switch to another tab/app →
  on turn-complete a toast shows the title + `Has completed after <span> and is waiting for you`, and
  `[notify] <id8> running -> waiting for you (after <span>)` lands in hooks.log; **clicking it restores/
  foregrounds the window and selects the tab** (+ `[nav] notify-click …`); the cog's checkboxes mute per
  state; the master OFF silences everything; `notifySound` OFF shows a silent toast; a second completion
  replaces the first in Action Center. **Hold path:** a completion that leaves a background shell/agent
  running logs `[notify-hold]` → `[notify-drop]` with NO pop (the session re-lights Running ~20 s
  later — the 513d1366 repro); when the background work finally quiets, the true settle fires ONE toast
  (`[notify] … (held Ns)` when it rode a hold); a W→R→W flap inside 20 s logs `[notify-dedupe]` for the
  second entry.

### Follow-ups (non-blocking)

- **Toast COM activator** — deep-link a click into a fresh app launch (click-after-exit, §6).
- **Quiet hours / focus-assist awareness** — Windows Focus Assist already suppresses toasts
  system-wide; an in-app schedule could complement it.
- **Per-state sound or priority** — e.g. Error as a high-priority toast.
- ~~An unread-delay~~ — **shipped better** as the signal-gated HOLD (§6): only sessions whose
  external-work signal is live pay any latency; a plain completion still toasts immediately.
- **Cross-install dedupe** — a second Agentmaster install tracking the SAME session (a migrated
  profile's fleet copy) toasts independently (the 513d1366 double: prod + dev ~3 s apart). The
  in-process guards can't see it; the fix class is presence-pid host classification (the CLI's
  `agentmaster-self` / `agentmaster-other`) demoting an other-install session to observe-only.
