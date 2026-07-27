# System notifications — Windows toasts when a session leaves Running

> Status: **complete — the Settings cog "Notifications" tab + the Running → X toast pipe + click-to-surface
> (bring the hosting window to the FRONT and jump to the session's tab), via a per-identity TOAST COM
> ACTIVATOR (§4a) that also kills the shell's stray-window fallback.** Settings round-trip + the toast HOLD
> gates are unit-tested (engine harness green); the full chain lib-compiles green (TerminalAppLib +
> WindowEmperor). Runtime verification (a live toast + the `[notify]` trace + a click that opens NO stray
> window) pends a deploy — gated on the user's build/deploy permission, and the manifest change means this
> deploy needs a **re-register** of the loose layout.

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
“rebase onto agentmaster and fix the three conflicts in TabHeaderControl...”
```

- **Line 1** — the session's title (the ONE title value, Rule #11: Explorer name == tab title ==
  persisted `SessionInfo.title`).
- **Line 2** — `Has completed after <duration> and is <status>`: the **duration** is the session's
  observed **Running span** (how long the turn worked, compact `2h30m` / `5m12s` form — the tab tooltip's
  `TtSpan`), and the **status** is the state it landed on (`waiting for you` / `needs approval` / `idle` /
  `done` / `error` — the tooltip's `TtStateLabel` vocabulary). When the Running **entry** edge was never
  observed (a session adopted or moved into the window mid-turn), the `after <duration>` clause is
  **omitted** — an honest "no duration" over an under-reported one.
- **Line 3 — WHICH request just came back**: a quoted, one-line truncation of the prompt whose turn just
  finished. Lines 1+2 say *which session* and *what outcome*, but never what it was **doing** — and a
  title names a folder, not a task, which is the missing half when three sessions finish while you are
  elsewhere. Sourced from the registry queue's **newest `Sent` entry**
  (`LastDeliveredPrompt`, SessionModels.h) — the same record the Auto Testing's SENT summary renders, so
  it covers a prompt **typed straight into the ConPTY** as well as one the autorunner injected. **Both
  origins count**: an `Autorun` prompt is no less the user's message than a `Typed` one (they queued
  it), and filtering by origin would blank the line for exactly the autorunner-driven sessions whose
  completions you are least likely to be watching. Shortened by `PromptPreviewLine` — **first line only**,
  ≤ `kNotifyPromptPreviewChars` (**120**) chars, `...` whenever there is more than what's shown. The
  line is **omitted entirely** when there is no delivered prompt (a never-prompted launch, a managed
  **Codex** — which records no prompts — or a session adopted after its last turn); the toast then reads
  exactly as it did before, two lines. Safe even for a **held** toast fired seconds later: a new prompt
  would have put the session back in Running, which `DecideHeldToast` **drops** on.
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

- **ToastGeneric** XML with two or three `<text>` lines; every line goes in as a **DOM text node**
  (`XmlDocument::CreateTextNode`), so XML-special characters (`&`, `<`, quotes …) are escaped by the
  DOM — never hand-built markup. That matters most for **line 3**, which is arbitrary user prompt text
  and by far the likeliest source of them.
- **The template is composed, not picked from literals.** Two independent options — the silent audio
  element and whether line 3 exists at all — would otherwise need four hand-maintained strings. An
  empty `detail` emits only two `<text>` elements, so a session with no delivered prompt produces
  byte-for-byte the toast it always did. (ToastGeneric caps at 4 text elements, and a **banner**
  renders the title + ~2 body lines, so three is the most that shows without expanding it.)
- **Tag + Group replacement.** `Tag = ShortId(sessionId)` (first 8 chars — fits the legacy 16-char Tag
  cap) + `Group = "agentmaster"`: a session's **newer** toast REPLACES its older one in the Action
  Center instead of piling up. Distinct sessions keep distinct toasts.
- **Sound** — the default system notification sound; `notifySound` OFF swaps the template for one with
  `<audio silent="true"/>`.
- **On-screen time — `duration="long"` (~25 s)**, not the default `short` (~5–7 s, set by the user's
  "Show notifications for" ease-of-access setting). The toast schema exposes **only these two values** —
  there is no arbitrary duration — so this is the single lever for "keep it up longer", and long is the
  right end of it: a completion toast exists to be caught while you are looking at *another* window, and
  the short default routinely expires before you glance over. It governs the **banner** only; the toast
  lands in the Action Center either way (where Tag + Group still de-dupes it).
- **`launch` = the session id.** Set as a DOM attribute on `<toast>` (same escaping rationale as the
  text nodes). It is the activation payload — the shell hands it back verbatim as the activator's
  `invokedArgs`, so a click knows exactly which session to surface.
- **Best-effort by design.** `ToastNotificationManager::CreateToastNotifier()` (no-arg — the packaged
  app's AUMID) **throws on a build with no package identity** (unpackaged test hosts), and `Show` can
  fail when notifications are disabled system-wide. Both are swallowed; the FIRST failure logs
  `[notify] toast failed …` (`_agentToastFailLogged` — once is signal, per-fire is noise).

## 4a. Click → surface the session (and why it needed a COM activator)

**The bug this section exists for:** the first cut had no `ToastActivatorCLSID`, so the shell fell back
to a plain **AUMID activation** of the package. For a `FullTrustApplication` that means *launching
`WindowsTerminal.exe` with no arguments* — which hits the single-instance handoff, and the running
Emperor obligingly opens **a brand-new window with a default tab**. So a click produced BOTH the
in-process jump *and* a stray window ("opens both the desired tab but also a new window with new tab").
There is no way to tell that launch apart from a user typing `agentmasterdev`: the activation reason
simply isn't carried on the commandline, which is precisely the gap the CLSID fills.

**The fix — `AgentToastActivator.h` (header-only; the `PromptAnchor.h` idiom, included by exactly one
TU so it needs no `.vcxproj` entry).** Both manifests declare a **per-identity** ToastActivatorCLSID,
and the running instance registers a class object for it at engine init. The shell then
**CoCreateInstances that CLSID instead of activating the AUMID**; COM's SCM finds the already-registered
class object and calls `INotificationActivationCallback::Activate` **in the running process** — so **no
second process is ever launched and the stray window cannot exist by construction** (not "is suppressed
after the fact" — the launch never happens).

| | |
|---|---|
| **Manifest** | `<desktop:Extension Category="windows.toastNotificationActivation">` + a `<com:Class>` under a `<com:ExeServer Executable="WindowsTerminal.exe" Arguments="-ToastActivated">`. **A manifest change ⇒ the loose layout must be re-registered** (`Add-AppxPackage -Register … -ForceUpdateFromAnyVersion`). |
| **CLSIDs** | **Per identity** — release `{7608CBBC-…}`, dev `{6CB0FAE1-…}` — because a CLSID is machine-global COM state and the two installs live side by side (sharing one would let whichever registered last steal the other's clicks). Picked at runtime by `Profiles::IsDevPackage()`. Deliberately **not** more of the shared-CLSID debt PROFILES.md §5 tracks for the defterm/shellext GUIDs. Keep the header constants and the manifests in lockstep — a mismatch silently reverts to the stray-window fallback. |
| **Where it lives** | `TerminalApp.dll`, registered once (`std::once_flag`) from `_InitAgentmasterEngine`. Not `Engine.cpp` (plain C++/no-WinRT by contract) and not the EXE (which deliberately doesn't link the engine — only its header-only bits — while the jump routes through the engine's fan-out). |
| **The jump** | `Activate()` runs on an RPC/COM thread, so it hands off to `ActivateSessionInOtherWindows(id, /*source*/ L"")` — an **empty** source window id, so no window is excluded: we're not "a window asking the others", we're the shell asking the fleet. Whichever window hosts the tab selects it and foregrounds itself via `_FocusClaudeSessionTab(id, /*bringWindowToFront*/ true)` = **restore-if-minimized + `SetForegroundWindow` + the `SwitchToThisWindow` fallback** (the shell may deny a foreground hand-off; the fallback does the Alt+Tab-style switch). A session whose tab closed since the toast was shown simply finds no host — nothing happens, still no stray window. |
| **Logged** | `[nav] notify-click <id8> (toast -> foreground window + jump to tab)` at the callback (so the trail records the click even when no window hosts the session), plus `[notify] toast activator registered …` once at startup. |

Selecting the tab rides the normal tab-switch funnel, so the flash ring / unread mark / Manager lens
selection all clear and sync exactly as if the user had clicked the tab.

**The two click paths are mutually exclusive** (`ToastActivator::IsRegistered()`): with the activator
live, the toast wires **no** in-process handler — both firing would double-jump (harmless, but two
`[nav]` lines). When the activator did **not** register — unpackaged, or **a package registered before
the manifests carried the CLSID (i.e. not re-registered since this change)** — the toast keeps its
legacy in-process `ToastNotification.Activated` handler, so the jump still works exactly as it did,
stray window and all. **A stale registration is never a regression, just un-fixed.**

**Cold start** (no instance running): the SCM launches the ExeServer —
`WindowsTerminal.exe -ToastActivated -Embedding` (our `Arguments` + the `-Embedding` COM appends).
Neither token is a WT commandline, and the `-Embedding` (defterm) branch would leave the click with no
window at all, so `WindowEmperor::HandleCommandlineArgs` **strips the pair and continues as a plain
no-arg launch**: the workspace restores exactly as from the Start menu (a fresh window IS the right
answer when nothing was open), and the activation still pending in the SCM completes into the class
object the engine registers at init — jumping to the clicked session on top of the restored workspace,
if it beats the SCM's activation timeout. Either way the user gets their app back.

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
- **Click after the app exits = a normal start, then a best-effort jump.** The manifests' ExeServer
  brings Agentmaster back (workspace restore and all) and the SCM's pending activation jumps to the
  session once the engine registers the class object — *if* it beats the SCM's activation timeout on a
  slow restore. It is not a guaranteed deep-link; the user always at least gets their app back. (§4a.)
- **A manifest change ⇒ re-register.** The activator only takes effect once the package is registered
  with the CLSID-carrying manifest. Until then `ToastActivator::IsRegistered()` is false, the toast
  falls back to its in-process click handler, and the stray-window behavior persists — un-fixed, but
  never worse (§4a).
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
- ✅ **Full chain compiles**: TerminalAppLib (the cog tab + the edge tracker + the toast + the
  activator) builds green; `WindowEmperor.cpp` (the `-ToastActivated` cold start) ClCompiles green.
- ✅ **Manifests**: both well-formed; each identity's ToastActivatorCLSID round-trips to its own
  `<com:Class>` and matches the `AgentToastActivator.h` constant (release `{7608CBBC-…}` / dev
  `{6CB0FAE1-…}`).
- ⏳ **Runtime** (pends a deploy — build/deploy permission; the manifest change needs a **re-register**,
  §4a): start a turn, switch to another tab/app → on turn-complete a toast shows the title +
  `Has completed after <span> and is waiting for you`, and `[notify] <id8> running -> waiting for you
  (after <span>)` lands in hooks.log; the cog's checkboxes mute per state; the master OFF silences
  everything; `notifySound` OFF shows a silent toast; a second completion replaces the first in Action
  Center; the banner stays up the **long** ~25 s rather than the short default; **line 3** quotes the
  truncated prompt that just finished (and a never-prompted / Codex session shows a two-line toast
  instead, not an empty quote). **Click path (the fix):** startup logs `[notify] toast activator registered …`, and clicking a
  toast restores/foregrounds the hosting window + selects the tab (+ `[nav] notify-click …`) **with NO
  stray window** — the regression this section exists for. **Hold path:** a completion that leaves a
  background shell/agent running logs `[notify-hold]` → `[notify-drop]` with NO pop (the session
  re-lights Running ~20 s later — the 513d1366 repro); when the background work finally quiets, the true
  settle fires ONE toast (`[notify] … (held Ns)` when it rode a hold); a W→R→W flap inside 20 s logs
  `[notify-dedupe]` for the second entry.

### Follow-ups (non-blocking)

- ~~Toast COM activator~~ — **shipped** (§4a): it was the fix for the stray-window-on-click bug, and it
  carries the click-after-exit deep-link as far as the SCM's activation timeout allows (§6).
- **Quiet hours / focus-assist awareness** — Windows Focus Assist already suppresses toasts
  system-wide; an in-app schedule could complement it.
- **Per-state sound or priority** — e.g. Error as a high-priority toast.
- ~~An unread-delay~~ — **shipped better** as the signal-gated HOLD (§6): only sessions whose
  external-work signal is live pay any latency; a plain completion still toasts immediately.
- **Cross-install dedupe** — a second Agentmaster install tracking the SAME session (a migrated
  profile's fleet copy) toasts independently (the 513d1366 double: prod + dev ~3 s apart). The
  in-process guards can't see it; the fix class is presence-pid host classification (the CLI's
  `agentmaster-self` / `agentmaster-other`) demoting an other-install session to observe-only.
