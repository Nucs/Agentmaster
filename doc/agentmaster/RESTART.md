# Restart session — the restart pipeline (redesign plan)

> **Status: PLAN (2026-10-05). Not implemented.** D3 (the unsent draft) is decided (the user, 2026-10-06,
> §3.13); D1, D2 and D4–D7 (§5) are open. Read against HEAD `747a35ff8` (the restart code is unchanged since
> `7cc093e01`; the draft code against `82cd65d2a`). Line numbers drift — re-check them before you edit.

## 1. Goal

**The intent (the user, 2026-10-05):** "Restart session" refreshes the loaded session. It works like
**Ctrl+C, then relaunching the same session id exactly**, in the same pane.

**The contract.** The new pipeline guarantees all of these:

| # | Rule | Meaning |
|---|---|---|
| R1 | **Same conversation** | Relaunch the conversation the pane has loaded NOW (claude's own current id). Never a new id. |
| R2 | **Stop, then start** | The new agent process starts only after the old one has exited (bounded wait, checked by process handle + start time). The resume/fresh choice is made after the stop. |
| R3 | **Same place** | The same tab and the session's OWN pane (found by `tabToken`), title, color, tags, overlay, scrollback and working directory. |
| R4 | **Same launch options** | The old process's own launch flags (whitelist, D4) come back. One-shot items (the handover's positional first prompt, a fork source after the fork has content) never come back. |
| R5 | **Clean state** | After the restart, the record describes only the new process. Data left by the old process (heartbeat, subagent files, transcript tail) never promotes the session to Running. No false completion alert. |
| R6 | **Safe** | Never touch a pane that is not the session's. Never destroy the user's own shell without consent. Never start while a delivery owns the input box. Never run two restarts at once. Never start a connection on a control that is not initialized. Every failure leaves a resumable session and a log line. |
| R7 | **Observable** | One `[nav] restart-begin` / `restart-done` pair per request, from every entry point. One `[restart]` line per phase. The user sees a refusal. |
| R8 | **Same unsent prompt** | Text in the input box comes back in the new box, each pasted block restored from its content (D3, §3.13). Never submitted. Never a literal `[Pasted text #N +M lines]` label. |

## 2. Today (`_RestartManagedSession`) and its defects

### 2.1 Entry points (unchanged by this plan)

| Entry point | Path today | Pane |
|---|---|---|
| Tab menu → **Restart session** | `Tab::_RestartActivePaneConnection` → action `RestartConnection` → `TerminalPage::_HandleRestartConnection` | active pane of the right-clicked tab |
| Command palette / key binding `restartConnection` | same action | active pane of the focused tab |
| Terminal right-click menu (only when the connection is Closed) | same action | active pane |
| **Enter** on a dead pane | `ControlCore::SendCharEvent` → `RestartTerminalRequested` → `TerminalPage::_restartPaneConnection` | that pane |
| Manager board card / Explorer-tree row / side-panel card | `_restartSessionHandler` → `_RestartClaudeSession` → `_RestartClaudeSessionLocal` or `RestartSessionInOtherWindows` | active pane of the session's tab |

All end in `_restartPaneConnection` (`TerminalPage.cpp`): the NotConnected guard, then
`_RestartManagedSession` (`TerminalPage.AgentSessions.cpp`), else the upstream shell replay.

### 2.2 What is already right

- `BuildClaudeRestartSpec` keeps `<id>` in all three forms: `--resume <id>` (transcript exists), a re-fork
  into `<id>` (a fork with no message yet), `--session-id <id>` (nothing on disk).
- `RetireHostToken` drops the dying claude's hooks and observations for 10 min (HOOKS.md *Retired hosts*).
- The record is re-stamped before the swap (`tabToken`, `live`, `pid`, `RestoredSessionState`).
- The swap has no explicit `Close()` first (no "code 259" banner, `3d481703b`).
- A never-initialized control is never reset (the NotConnected guard, `5e8eef2fc`).

### 2.3 Defects

| # | Defect | Evidence | Breaks |
|---|---|---|---|
| 1 | **No wait for the old process.** `ConptyClosePseudoConsole` only signals the close; `ConptyConnection::Close()` waits for its own output thread (and cancels its read), then `Start()` runs. | `ConptyConnection.cpp` `closePseudoConsoleAsync`, `Close()`. Logged restarts: old `SessionEnd` +0.39 s / +0.66 s, new `SessionStart` +1.73 s / +4.22 s after the swap. A busy claude can need 5 s. | R2 |
| 2 | **Old-process data promotes the restarted session to Running.** The restart clears `pid` but keeps `presenceStatus` (Deactivate clears it and sets `started = false`). The scanner's external-work arm reads the stale `busy` on an unfinished turn. Fresh subagent side files (15 s) do the same (by reading). | **Proven** (the heartbeat arm). Release `scanner.log`: `[22:11:10.491] [recon-subagent] 151ac992 (presence=busy, Idle -> Running)`, 38 ms after `[22:11:10.453] [restart] claude 151ac992 (resume)`. If a later promotion lands on a pending `tool_use` tail, `recon-stop` and `recon-stop-idle` never release it. | R5 |
| 3 | **A split tab restarts the wrong pane.** The tab menu, the palette and the Manager use the ACTIVE pane. `_ManagedSessionForConnection` accepts ANY pane of a managed tab. A sibling shell becomes a second claude on the same id (two writers; the real claude's token is retired). Enter on a dead sibling shell pane does the same. | `_HandleRestartConnection` (`GetActivePane`), `_RestartClaudeSessionLocal` (`GetActiveContent`), `_ManagedSessionForConnection` (walks all panes). `/handover-here` and Deactivate resolve the session's own pane by `tabToken`. | R3, R6 |
| 4 | **An adopted `+`-tab session silently replaces the user's shell.** The main lookup has no `external` check; the archived fallback has one. The record stays `external = true` afterwards. | `_RestartManagedSession` vs its fallback (`!s.external`); comments in `706a861c4` / `3d481703b`. | R6 |
| 5 | **Launch flags are dropped.** The record holds the running process's `--model` / `--effort` / `--permission-mode` (`ParseClaudeFacts` → `ObserveClaude`); `BuildClaudeRestartSpec` takes no model. | `ProcessObserver.cpp` (`o.model = f.model`), `_RestartManagedSession` spec call. | R4 |
| 6 | **The resume/fresh choice is made before the old process stops.** A first turn that writes `<id>.jsonl` during the overlap makes the fresh form (`--session-id <id>`) collide ("session id already in use"). | `_RestartManagedSession` order. | R1, R2 |
| 7 | **False attention cue (by reading).** The Running → Idle normalize starts the red flash ring on an unfocused tab (always from the Manager page, where no tab has focus). Deactivate pre-seeds the flash / toast trackers; Restart does not. The toast itself is mostly held, then dropped (the stale `busy` holds it; the re-promotion cancels it). | Deactivate's tracker pre-seed in `_DeactivateManagedTab`. | R5 |
| 8 | **A revived (swept) session gets only its injector back.** The submitter, the Enter presser and the overlay return through the ~2.5 s reconcile tick, and never when `showTabOverlay` is off (the reconcile skips a bound tab when overlays are off). | `_ReconcileClaudeTabsImpl` (`boundHere && overlayOk`). | R5 |
| 9 | **Start-early hazard for a waiting design.** `_WakeDeactivatedTab` (in the tab-selection funnel) and `_ActivateDormantSession` (Activate / Activate All) start an initialized control's parked connection. A restart that waits would be started early by a tab click unless they respect it. | `TerminalPage.AgentObserver.cpp` `_WakeDeactivatedTab`, `_ActivateDormantSession`. | R2 |
| 10 | **Logging and feedback gaps.** Only the Manager path writes `[nav]`; all 25 `[restart]` lines in both logs have none. `RestartSessionInOtherWindows` has no per-sink try/catch (`RaiseCommandActionInWindows` has one). A refused or unhosted restart shows nothing. | Both hooks.log files; `Engine.cpp`. | R7 |
| 11 | **An agent pane that cannot be resolved falls back to the upstream replay** of its original launch command (`--session-id <id>` inside the pwsh `-EncodedCommand`). | `_restartPaneConnection` fallthrough. | R1 |
| 12 | **A fork with no message re-forks from the parent's CURRENT end**, so turns the parent added since the fork come along. | `BuildClaudeRestartSpec` (`forkParentId`). | R1 (edge) |
| 13 | **The unsent draft is erased.** The restart clears `pendingInput`, `pendingInputUnixMs`, `pendingPasteRefs` and the durable store (`_PersistSessionDraft(id, {})`); the code calls it "the ONE eager clear". The re-fill pump drops an entry whose memory was cleared, so nothing is typed back. The content behind a `[Pasted text #N]` placeholder lived only in the old claude's memory and the paste journal, and the restart uses neither. (PENDING_INPUT.md §5 and §8c name `_RestartTabIntoFreshSession` as that clear; the code is in `_RestartManagedSession`.) | `_RestartManagedSession` (the record update, then `_PersistSessionDraft`); `_PumpDraftRestoresImpl` ("memory cleared while waiting"). | R8 |

**Live record.** The release log has 12 `[restart]` lines, the dev log 13; the newest of each predates
`RetireHostToken` (`7cc093e01`). The current code has never run a restart live.

## 3. Design

### 3.1 One request, one pipeline

Every entry point builds a `RestartRequest` (new) and calls ONE coroutine, `_RestartSession` (new; a
`fire_and_forget` terminate-net over an `IAsyncAction` Impl, the `_SweepClaudeLiveness` idiom):

```
RestartRequest { source (tab-menu | action | dead-pane-enter | manager | manager-remote),
                 sessionId (when known), pane (when the entry point names one) }
```

| Phase | Thread | Work |
|---|---|---|
| A. Resolve | UI | Find the session and ITS pane (§3.2), check the identity (§3.3). |
| B. Preflight | UI | Refuse with feedback (§3.9). Route a never-started session to Activate (D2). |
| C. Capture | UI | Old root + agent process handles and start times; the agent's command line → carried flags (§3.4). The unsent draft and its pasted content; ask when part of it cannot be restored (§3.13). |
| D. Detach | UI | In-flight marker, tracker pre-seed, `RetireHostToken`, the record reset (§3.6, which keeps the draft), the draft re-fill armed (§3.13), swap in the PARKED connection (not started; the swap closes the old one silently), then `HardResetWithoutErase` (after the swap, so no late output of the old connection lands after the reset), erase the dead input box (§3.13), injector to the parked connection. |
| E. Wait | background | Wait for the old agent + root to exit; escalate once (§3.4). |
| F. Re-validate | UI | Page, control, connection, record and marker still as left in D. Else stop here (§3.8). |
| G. Decide + start | UI | Final command (§3.5); re-swap only if it changed; set the fence (§3.6); `StartDormantConnection()`; `SetStarted(true)` (the control's one-shot `Initialized` event has already fired, so `_TrackSessionStartedOn` cannot do it); bindings; the draft re-fill runs (§3.13); clear the marker; log. |

`_restartPaneConnection` stays the upstream event target: it builds the request and, for a pane that is not
an agent pane, keeps the upstream shell replay unchanged.

### 3.2 Targeting rules

| Entry point | Target pane | Not a session pane |
|---|---|---|
| Tab menu | The session's OWN pane in that tab (by `tabToken`), whatever pane has focus | No session in the tab → upstream restart of the active pane |
| Palette / key binding / terminal menu | The active pane, only if it IS the session's pane | Upstream shell restart of that pane |
| Enter on a dead pane | That pane, only if it IS the session's pane (or the archived record whose `tabToken` is that pane's) | Upstream shell restart |
| Manager / side panel | The session's OWN pane in the hosting window | Not hosted anywhere → feedback (§3.12) |

- **Pane resolver** (new, pure core + thin UI walk): match each terminal pane's connection `WT_SESSION` to the
  record's `tabToken`. No match and exactly ONE terminal pane → that pane. No match and several panes →
  **refuse** (ambiguous — never guess, Rule #9).
- **Agent-pane marker** (new): `TerminalPaneContent` remembers that it hosts a managed agent launch (set at
  the same places as `SuppressAutoClose()`). An agent pane that resolves to no record is REFUSED, never
  replayed upstream (defect 11).
- **Codex prerequisite:** `_LaunchCodexSession` stamps `tabToken` from the connection at launch, like Claude
  (today it is filled on the first prompt, so a never-prompted codex is untargetable by token).

### 3.3 Identity: which conversation is loaded now

1. The id bound to the tab (`_claudeTabs`) is the default.
2. If the pane's agent is alive, read claude's own presence row (`~/.claude/sessions/<pid>.json`, pid-keyed,
   checked by `PresenceRowMatchesProcess`). If it names another id, the binding lags a `/clear` or `/resume`:
   re-home first through `_BindClaudeSessionToTab`, then restart THAT id. Log `[restart] binding lagged`.
   If the registry has no record for that id yet, refuse ("the tab is switching conversations — try again").
3. No live agent (claude exited to the pwsh prompt, or the pane is dead) → the bound id, else the archived
   record whose `tabToken` is the pane's `WT_SESSION` (today's fallback, most recent first, `!external`).

### 3.4 Stop and wait

**Capture (phase C, before anything changes):**
- Root: `DuplicateHandle` of `ConptyConnection::RootProcessHandle()` with `SYNCHRONIZE` (the connection
  closes its own handle in `Close()`, so the duplicate must exist first).
- Agent: one `SnapshotProcesses()`, `FindDescendantByImage(root, L"claude.exe" | L"codex.exe")`,
  `OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE)`, plus
  `ProcessStartUnixMs` as its identity. None found → claude already exited; wait for the root only.
- Flags: `ReadProcessCommandLine(agent)` → the carried flags (§3.5).

**Wait (phase E):** `WaitForMultipleObjects` on agent + root, deadline **8 s**. Windows ends a console process
5 s after `CTRL_CLOSE_EVENT`, and measured idle exits are ~1–2.5 s, so 8 s is margin, not a normal wait.

**Escalate (D7):** an agent still alive at the deadline is terminated (`STATUS_CONTROL_C_EXIT`) only when its
start time still matches the capture (no PID reuse) — the orphan reaper's identity rule. Wait 2 s more.
Still alive → do not start (§3.9). The root (pwsh) is never terminated by us: the closed console ends it, and
a root still alive at the deadline does not block the start (it writes no transcript; it is logged).

### 3.5 Decide after the stop

The command is chosen in phase G, from the FINAL disk state:

| On disk after the stop | Command |
|---|---|
| `<id>.jsonl` exists | `claude --resume <id>` |
| no own transcript, the fork source exists | `claude --resume <parent> --fork-session --session-id <id>` |
| neither | `claude --session-id <id>` |

- **Carried flags (D4):** a whitelist parsed from the OLD command line (`ExtractCmdlineArg`): `--model`,
  `--effort`; `--permission-mode` only when the relaunch does not add `--dangerously-skip-permissions`.
  Never `--resume`, `--continue`, `--session-id`, `--fork-session`, `--settings`, `-p` / `--print`, or the
  positional first prompt. Dropped flags are logged. `BuildClaudeRestartSpec` gains a flags parameter
  (`BuildClaudeCommandline` already appends `--model` last).
- **Launch directory:** the record's `workingDir` (today's mechanics owner). Phase 0 checks whether
  `claude --resume <id>` finds a transcript filed under ANOTHER project folder; if it does not, phase G
  checks the transcript's folder against `EncodeCwdToProjectDir(dir)` and, on a mismatch, launches in the
  `cwd` recorded in the transcript (logged).
- **Parked vs final connection:** phase D builds the parked connection from a PREDICTION of this table, with
  a pre-generated `WT_SESSION` guid **G** (the 9th `CreateSettings` argument; `_BuildAgentConnection` gains a
  guid parameter). If phase G's decision differs, it builds the final connection with the SAME guid G and
  swaps again (closing a never-started connection is a no-op), so `tabToken` never changes twice.
- **Codex:** `codex resume <codexSessionId>` when the rollout exists after the stop, else `codex`.

### 3.6 The reset set (phase D, one registry `Update`)

| Field | Action | Why |
|---|---|---|
| `tabToken` | = G | The liveness sweep finds the pane by it. |
| `live` | true | Revives an archived (swept) record. |
| `started` | **false** until phase G starts the connection | Dormant hold: no scanner promotion, no auto-send, no Enter-retry press while nothing runs. |
| `pid` | 0 | The observer fills it from the new process. |
| `presenceStatus`, `presenceWaitingFor` | **clear** | They described the old process (defect 2). |
| `compactingSinceUnixMs` | clear | A killed compaction is over. |
| `turns` | reset | The old turn and its type-ahead are void. |
| `state` | `RestoredSessionState(state)` | Rule #16. |
| `lastMessageWasQuestion` | `RestoredQuestionFlag` | Rule #16. |
| ~~`pendingConfirmPromptId`~~ | — | The field was removed with the SemiAuto mode on 2026-10-09 (DELIVERY.md §17.4). Nothing to reset. |
| `pendingInput`, `pendingInputUnixMs`, `pendingPasteRefs` | **keep** (D3) | The draft is typed back after the start (§3.13). The durable store keeps it too (no `_PersistSessionDraft(id, {})`). |
| paste journal (registry side map, not a field) | keep | It resolves the draft's placeholders now, and the new box's placeholders later. |
| `pendingBoxState`, `pendingBoxStateUnixMs` | `NoBox`, now | The old input box is gone. `DecideAdvance` holds ("input box not visible") until the scan reads the NEW box. While the draft re-fill is armed, the scan does not record the box state, so the hold lasts until the draft is back (§3.13). |
| `forkEchoConsumed` | false when the prediction is a re-fork | The re-forked process echoes the parent id once. |
| `restartFenceUnixMs` (new, transient) | = the instant phase G starts the connection | The scanner counts side-file activity and transcript turn events only AFTER it (§3.7). |
| `restartPendingSinceUnixMs` (new, transient) | = now; cleared in phase G | The cross-window in-flight marker (§3.8). |
| `model`, `effort`, `permissionMode` | keep | The observer refreshes them from the new process. |
| `external` | false after a consented conversion (D1) | The ConPTY is ours from then on. |

Fields not listed keep their value (the queue and its statuses, the autorunner mode, titles, tags, colors,
lineage, `errorMessage`). The reset is a pure function `ApplyRestartReset(SessionInfo&, guid, now, options)`
(new, SessionModels.h) so the harness can pin every row.

### 3.7 Scanner and scheduler holds

- **While parked** (`started = false`): `ScanSessionIsDormant` holds recon-run, recon-resume,
  recon-resume-presence and recon-subagent; at-rest synths still run. No automatic send can reach the
  parked connection: the scheduler's idle-start trigger needs `started || external`, `DecideAdvance` holds on
  the `NoBox` box state and on the restarted settle clock, and `DecideEnterRetry` never presses into a
  not-started session. (`DecideAdvance` has no `started` check of its own, so the box state and the settle
  clock are what hold a timed re-advance.)
- **After the start:** the presence is empty until the NEW host reports (the old token is retired). A new
  pure gate fences the other two inputs at `restartFenceUnixMs`: the subagent arm needs
  `subagentActivityMs > fence`, and recon-run counts only turn events whose `lineTsMs > fence`. Phase E waited
  for the old process to exit before the fence was set, so every write it made is older than the fence and can
  never promote; new work after the start counts normally.
- **Settle clock:** the `started` flips restart the 40 s settle hold (DELIVERY.md §16), so no queued prompt is
  typed into a claude that just booted.
- **Regression test:** replay the 151ac992 sequence on a real scanner. Before the fix: Idle + the old `busy`
  + a `tool_use` tail → `[recon-subagent]` (reproduces the log). After `ApplyRestartReset`: no heartbeat, a
  pre-fence side file, a pre-fence assistant line → no promotion; a post-fence side file → promotion.

### 3.8 Concurrency and lifetime

- **One restart per session, process-wide:** `restartPendingSinceUnixMs` on the record (any window reads it).
  A second request is refused (`already restarting`). A 30 s belt expires a leaked marker.
- **No early start:** `_WakeDeactivatedTab`, `_ActivateDormantSession` and Activate All skip a session whose
  marker is set (defect 9).
- **Delivery:** refuse while `DeliveryGateHeld` or `_draftSwapsInFlight` holds the session (Deactivate's rule).
- **Teardown:** the coroutine holds `get_strong()`, and every exit path ends on the UI thread (the last-ref rule
  in Gotchas). A tab or window that closes during the wait: phase F stops; the parked connection closes with
  the tab; the close seams archive the record as usual.
- **Tab moved to another window during the wait:** if the control still holds the parked connection, phase G
  starts it anyway (the connection is ours); the destination window binds it through the observer.
- **Restarted again later:** each restart moves the fence and retires the previous token; the guid is new.

### 3.9 Failure handling

| Failure | When | Result |
|---|---|---|
| No native `claude.exe` | B | Refuse; nothing changed. |
| Ambiguous pane / agent pane without a record | A | Refuse; nothing changed. |
| Adopted session, no consent (D1) | B | Refuse or ask. |
| Delivery or draft swap in flight | B | Refuse ("try again in a moment"). |
| Already restarting | B | Refuse. |
| Part of the unsent draft cannot be restored, old agent alive | C | Ask (§3.13): "Restart anyway" (the draft comes back without that part) or "Cancel" (nothing changed). |
| Spec / connection build throws | C–D | Caught (`AgentLogCaughtException`); if D did not swap, nothing changed; else parked. |
| Old agent does not exit, even after the escalation | E | Stay parked (dormant tab, resumable with Activate or Restart); notice + log. |
| Page, control or record changed during the wait | F | Do not start; log. |
| `StartDormantConnection()` returns false | G | Parked; log; Activate retries. |
| The draft re-fill gives up (box never readable, the user typed first, a mismatch) | after G | The box is left as it is; the memory is left to the scan's revalidation; `[draft-restart]` names the reason. |

### 3.10 Codex

Same pipeline: image `codex.exe`, the rollout check after the stop, no injector, no hooks (the marker and the
fence still apply; `_ReconcileManagedCodex` re-fills `codexSessionId` and the state from the new process).

### 3.11 Logging

```
[nav] restart-begin <sid8> via=<source> pane=<own|active|dead> state=<state>
[restart] <sid8> refused: <reason>                       (then [nav] restart-done … (refused: <reason>))
[restart] <sid8> draft: none | chars=<n> placeholders=<k> (journal <a>, cache <b>) [left out with consent: <list>]
[restart] <sid8> stopping: agent pid=<n> root pid=<m> carry=<flags>|none
[restart] <sid8> dead input box erased (rows <r1>-<r2>) | no box on screen
[restart] <sid8> old agent exited after <ms> ms (graceful|terminated)
[restart] <sid8> plan: resume | refork from <src8> | fresh (same id) model=<m> dir=<dir>
[restart] <sid8> started wt=<G> (replaced <old wt>)
[nav] restart-done <sid8> (<form>, stop <ms> ms)
[draft-restart] <sid8> armed | typed <n> piece(s) | RESTORED + VERIFIED | <give-up reason>
```

Plus the existing `[retired-host]` lines. `RestartSessionInOtherWindows` gets a per-sink try/catch with
`LogSwallowedException` (Rule #18).

### 3.12 User feedback

- While parked: the tab dot's dormant ring and the board card's dormant style (free, from `started = false`).
- Refusals: the page's notice path (`_ShowControlNoticeDialog`) or a non-modal hint — decided in P3.
- The Manager menu: "Restart session" disabled or replaced by "Activate" for a session that never started
  (D2); a Manager restart that no window takes logs `restart-done … (no host)` and shows a hint.
- The D3 question (§3.13): a buttons-only dialog in the hosting window (a buttons-only `ContentDialog` is
  safe under XAML Islands — it takes no typing), tagged `agentmaster-dark`.

### 3.13 The unsent draft (D3 — decided)

**The decision (the user, 2026-10-06):** "When user has text in the prompt textbox of claude code ui and we
decide to restart session, restore the text with "[Pasted text #1 +24 lines]" expanded."

**What the user sees.** Before the restart the box holds, for example, `fix this: [Pasted text #1 +24 lines]`.
During the restart the tab's "3 dots" keep showing. After it, the new claude's box holds the same thing: the
typed words as text, and a new `[Pasted text #1 +24 lines]` that Claude minted for the real 24 lines, so Enter
sends them. Nothing is submitted.

- "Expanded" means: the content behind the placeholder comes back. The label is never typed back; a typed
  label looks the same on screen but sends only the label text (PENDING_INPUT.md §9).
- Claude collapses every paste of more than 800 characters or more than 2 line breaks into a placeholder,
  so the restored box shows the placeholder again, not the 24 lines.

**How.** The fork carry's method (PENDING_INPUT.md §11), applied to the session's own id:

| Step | Phase | Work |
|---|---|---|
| 1. Capture | C | Read the box of the session's own pane now, through the deep probe (`_ReadInputBoxProbeForSession` with `kSendVerifyProbeRows`); else the record's memory (`PickCurrentPromptText`: the live read wins). Copy the session's paste journal (`SessionRegistry::PasteJournal`). |
| 2. Resolve | C | `ResolvePendingDraftCarry`, STRICT policy: the journal first (every clipboard paste into this session in this app run), Claude's paste-cache second. Off the UI thread when the draft holds a placeholder; a draft without one needs no resolve. |
| 3. Ask | C | Only when step 2 cannot resolve every placeholder (an image or audio attachment, a drag-dropped text, a paste the journal's size caps removed) and the old agent is still alive, because its memory holds the only copy. The question names each part: "Restart anyway — the prompt comes back without `[Image #1]`" or "Cancel" (nothing changes). "Restart anyway" resolves again with the new DROP policy: each unresolvable placeholder is left out, never typed as a label. |
| 4. Keep | D | Keep the draft trio, the durable store and the journal (today's eager clear goes, defect 13). Arm the re-fill entry in phase D, BEFORE the swap. `_ScanPendingInput` then holds the session's clear debounce from the first tick, so neither the parked pane nor the new claude's empty box can erase the memory or prune the journal. The dots show from the memory. |
| 5. Erase the dead box | D | After `HardResetWithoutErase`, erase the dead claude's input box (from its top rule to the end of the buffer) and put the cursor on that row. See "Why the dead box must go" below. |
| 6. Fill | after G | The pump's new lane `DraftFillOrigin::Restart` (below): the session started → a 1.5 s settle → the hands-off check → the box reads Empty → the piecewise delivery → `VerifyDraftFillPastes`. |

A dead pane (the agent already exited) is not asked: the content is already gone. Its draft is typed back
only when every placeholder resolves (STRICT); otherwise the memory stays display-only (the dots, and Copy
Current Prompt copies the rendered text). With `restoreDraftOnResume` OFF the restart clears the draft as
today, and asks nothing.

**The new pump lane `DraftFillOrigin::Restart`.** It uses the Fork lane's piecewise delivery
(`BuildDraftFillPastes` → `_DeliverDraftFillPastes`: the read-only lock, the delivery gate
`kDeliveryGateFillTag`, each piece sent only after the box shows the previous one) and its verify rules. It
differs from the Fork lane in six points:

- No seed and no re-journal. The memory is already the session's, and the blocks are already in its journal
  (`NotePastedText` de-dupes only the newest entry, so a re-note would duplicate them).
- A non-empty `pendingInput` on the record is expected. (The Fork lane drops its copy when the record holds a
  draft.) Only the live box decides whether the user typed first.
- The setting is `restoreDraftOnResume` (default ON; its cog switch is on the Prompt Queue Runner
  tab, which is in every build since 2026-10-09 and was dev-or-debug only before).
- The resolve policy is STRICT, with steps 2–3. The fork's LIVE policy types an unresolved mid-line
  placeholder as text, which would lose a drag-dropped paste silently.
- The hands-off baseline (`armedUnixMs`) is the first pump tick that sees the session started, not the arm
  instant. A line the old process wrote while it stopped (an interrupt marker) is then older, so it never
  reads as "someone drove the session" (`RestoredDraftSessionTakenOver`). The same rule covers a restart that
  ended parked and is started later by Activate.
- Log lines: `[draft-restart] <sid8> …`.

**Why the dead box must go (Claude's inline renderer).** `HardResetWithoutErase` keeps the main buffer. Under
Claude's inline renderer, the dead claude's last frame stays on screen, with its input box and the draft in
it. Every box reader takes the bottom-most box (`ControlCore::ReadPendingInputDraft` scans up from the last
non-blank row). Without step 5:

1. The re-fill's first read finds the dead box and drops the restore ("the box already holds text — the
   live draft wins").
2. The new claude draws its empty box at the inherited cursor, under or over the dead one.
3. The scan reads the new empty box, clears the memory and prunes the journal. The draft is gone.

With the box erased, no reader finds a box until the new claude draws one. The fullscreen renderer
(`CLAUDE_CODE_NO_FLICKER=1`, this machine) is not affected: the reset leaves the alternate screen, which held
the box. Step 5 is a new `TermControl` / `ControlCore` call. It finds the box rows with the same detector
(`DetectPendingInput`), erases them under the write lock, and moves the cursor there; the new connection
inherits that cursor position (`inheritCursor`). P0 confirms the dead frame and checks whether a resumed
inline claude clears the screen itself.

**Outcomes (the pump's existing rules):**

| Case | Result |
|---|---|
| The box verifies | `[draft-restart] … RESTORED + VERIFIED`. The scan then records the new box as the memory. |
| The user types into the new box first | Their text wins; the copy is dropped. |
| A prompt runs after the start (the user, Send-now) | Hands off; the memory is left to the scan. |
| The box shows text that does not match | Left as it is (never typed onto). |
| Claude ate the first paste (an empty box, nothing seen) | One re-type from piece 1. |
| The box is never readable within 10 min of the start | Given up; logged. |
| The restart ends parked (§3.9) | The entry stays armed; when Activate starts the claude, the fill follows. |

While the entry is armed the scan does not record the box state, so phase D's `NoBox` keeps `DecideAdvance`
holding: no queued prompt is sent before the draft is back. The fill itself holds the delivery gate.

**Outside D3.** A message the user already entered while claude was working is in claude's own queue, not in
the box. It ends with the old process (the transcript keeps it as a `queue-operation` enqueue line). Bringing
it back belongs with D5.

## 4. What does NOT change

- **Deactivate** and **/handover-here** keep their code (they may adopt the new helpers later, only with
  identical behavior).
- The upstream shell restart for a pane that is not an agent pane.
- `RetireHostToken`, the `[stale-end]` guard, the Rule #16 mapping, `BuildClaudeRestartSpec`'s three forms.
- Deactivate's draft re-fill keeps the single-paste §10 lane (`DraftFillOrigin::Resume`). The piecewise fill
  for §10 stays a PENDING_INPUT.md §11 follow-up.

## 5. Decisions needed

| # | Question | Options | Recommended |
|---|---|---|---|
| D1 | An adopted `+`-tab session (claude typed into the user's own shell) | (a) refuse with a message; (b) confirm, then convert to a managed session (replaces the shell; `external = false`, `SuppressAutoClose`); (c) today's silent conversion | **(b)** |
| D2 | Restart on a session that never started | (a) start it in place (= Activate); (b) refuse with a message | **(a)** |
| D3 | The unsent draft in the box | **DECIDED (the user, 2026-10-06):** keep it and type it back after the start, each `[Pasted text #N +M lines]` restored from its content — §3.13. (Rejected: clear it, today's behavior.) | — |
| D4 | Launch options | (a) carry `--model` + `--effort` (+ `--permission-mode` rule, §3.5); (b) settings only (today); (c) (a) + a "Restart ▸ on model…" submenu | **(a)**, (c) later |
| D5 | A prompt that was Sent but not yet submitted when the old process stopped | (a) after the stop, check the transcript: found → mark echoed; not found → `Failed` "lost to restart", autorunner NOT paused; (b) leave it to the lost-send verdict (`Failed` + autorunner paused) | **(a)** |
| D6 | Confirm before restarting a Running session (it ends the turn) | (a) no confirm (like Ctrl+C); (b) always; (c) a cog setting | **(a)** |
| D7 | The old agent does not exit in 8 s | (a) terminate it (start-time checked), then start; (b) never terminate, stay parked | **(a)**, (b) as the last resort |

## 6. Implementation phases

| Phase | Work | Build? |
|---|---|---|
| **P0 — Check the assumptions** | (1) exit time of an idle vs a busy claude after `CTRL_CLOSE`; (2) does a stopped claude write transcript lines while it closes; (3) does `claude --resume <id>` find a transcript filed under another project folder; (4) does claude.exe accept a resume-at-message option (defect 12); (5) the D1 dialog text; (6) Claude's inline renderer: does the dead frame, with its input box, stay after `HardResetWithoutErase`, and does a resumed inline claude clear the screen at start (§3.13 step 5). Read-only or throwaway (the claude-wrap-ConPTY driver, a scratch directory). | No |
| **P1 — Engine, pure, harness-tested** | `ApplyRestartReset`; the carried-flags parser; `BuildClaudeRestartSpec` flags parameter; the pane-resolver core; the scanner fence gate; the record's in-flight marker + its belt; the Codex eager `tabToken`; the per-sink try/catch in `RestartSessionInOtherWindows`; the DROP resolve policy (`ResolveDraftCarry`'s bool becomes a policy enum: refuse / keep a mid-line quote / drop, plus the list of left-out parts); the dead-box row finder (pure, over the `DetectPendingInput` rows). | Harness only |
| **P2 — The pipeline (TerminalPage)** | `RestartRequest` + `_RestartSession` (phases A–G); entry points rewired (tab menu → own pane, Manager → own pane, Enter / palette → pane-precise); `_BuildAgentConnection` guid parameter; the agent-pane marker; marker checks in Wake / Activate / Activate All; tracker pre-seed; the revival full bind; logging; the draft capture, keep and arm; the dead-box erase (`ControlCore` / `TermControl`); the `DraftFillOrigin::Restart` lane. | Yes — with your permission |
| **P3 — The decisions** | D1, D2, D4–D7 as chosen; the D3 question; Manager menu state; feedback UI. | Yes |
| **P4 — Docs + verification + release** | This file → final; CLAUDE.md, PERSISTENCE.md rule 6, HOOKS.md *Retired hosts*, DELIVERY.md (restart vs delivery); PENDING_INPUT.md §5 and §8c (the "one eager clear" goes; they name `_RestartTabIntoFreshSession`, but the clear is in `_RestartManagedSession`), §10, §11; CLAUDE.md's pending-input bullets; the `restoreDraftOnResume` cog tooltip ("reopens" → "reopens or restarts"); the live matrix (§7); dev deploy, then release. | Yes |

Every new or changed member is fully documented (summary that states the consequence, every parameter,
return value and exception; WHY comments on every non-obvious block). Rollback = revert the P2 commit; no
runtime switch (two restart paths would double the test matrix).

**Files (expected):** `TerminalPage.AgentSessions.cpp` (the pipeline; `_RestartManagedSession` retired),
`TerminalPage.cpp` (`_restartPaneConnection` builds the request), `TerminalPage.h`,
`TerminalPage.AgentObserver.cpp` (Wake / Activate guards, revival bind), `TerminalPage.AgentEngine.cpp`
(handlers), `AppActionHandlers.cpp` (source tag), `TerminalPaneContent.{h,cpp}` (agent-pane marker),
`AgentManagerContent.Tree.cpp` (menu state), `AgentMaster/ClaudeSpawn.{h,cpp}`,
`AgentMaster/SessionModels.h`, `AgentMaster/SessionRegistry.{h,cpp}`, `AgentMaster/SessionScanner.{h,cpp}`,
`AgentMaster/Engine.cpp`, `AgentMaster/tests/*`; for D3 also `AgentMaster/DraftCarry.h` (the DROP policy),
`AgentMaster/PendingInput.h` (the dead-box rows), `TerminalControl/ControlCore.{h,cpp,idl}` and
`TerminalControl/TermControl.{h,cpp,idl}` (the dead-box erase), `AgentManagerContent.Settings.cpp` (the
tooltip).

## 7. Verification

**Harness (P1):** every reset row; the flags parser (`--model x`, `--model=x`, quoted, positional prompt
ignored, identity flags ignored); the three command forms with flags; the pane resolver (own pane, single-pane
fallback, ambiguous → refuse); the fence gate; the 151ac992 replay; the marker and its belt; the retired-token
tests unchanged; the DROP policy (an image, an unjournaled paste, a mid-line quote, the left-out list); the
dead-box rows (an inline frame holding a draft, a fullscreen frame, a screen with no box).

**Live matrix (P4, dev instance first):**

| # | Scenario | Pass when |
|---|---|---|
| 1 | Idle session | Same id, `--resume`; old agent gone before the new one starts (`[restart]` timings); no Running blip; no flash. |
| 2 | Running, mid tool call | Ends Idle; no `[recon-subagent]`; no flash or toast; the killed turn is not resumed. |
| 3 | Running a subagent | No promotion from the old side files. |
| 4 | Never prompted | `--session-id <id>` (same id). |
| 5 | Fork with no message | Re-fork into the same id. |
| 6 | Right after `/clear` | Restarts the NEW id. |
| 7 | Started on a picked model | Same `--model` (D4). |
| 8 | Split tab, shell pane focused | Tab menu restarts the claude pane; the shell is untouched. Enter on a dead shell pane restarts only the shell. |
| 9 | Adopted `+` tab | D1 behavior. |
| 10 | Never-started tab | D2 behavior. |
| 11 | Manager, session in another window | The hosting window restarts it; the `[nav]` pair in both windows. |
| 12 | During a delivery / draft swap | Refused with feedback. |
| 13 | Double restart | Second refused. |
| 14 | Close the tab during the wait | No start, no crash, record archived. |
| 15 | Codex (idle, never prompted) | Same rollout / fresh. |
| 16 | Unsent typed draft (no paste) | The same text is back in the new box, not submitted; `[draft-restart] … RESTORED + VERIFIED`. |
| 17 | Draft with a 24-line paste (`fix this: [Pasted text #1 +24 lines]`) | The new box reads `fix this: [Pasted text #1 +24 lines]`; Enter sends all 24 lines (the transcript's user line holds them, not the label). |
| 18 | Draft with an image (`[Image #1] what is this?`) | The question. "Restart anyway": the box reads `what is this?`. "Cancel": nothing changes. |
| 19 | Inline renderer (`CLAUDE_CODE_NO_FLICKER` unset) with a draft | `[restart] … dead input box erased`; the draft is back in the new box. |
| 20 | The user types into the new box before the fill | Their text stays; nothing is typed onto it. |
| 21 | Hooks | `[retired-host] dropped SessionEnd`; no `[liveness] dead -> archived`. |
