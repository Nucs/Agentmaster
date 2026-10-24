# Logging and observability

Every trace goes through `AppendStateLog(fileLeaf, line)` (`AgentMaster/ClaudeSpawn.cpp`; thread-safe,
best-effort) into one of three files in the active profile. Each record is prefixed at that chokepoint
with a local-time `[HH:MM:SS.mmm]` stamp (only at a line boundary, so a partial line is never split).

## The files

- **`hooks.log`:** the hook event stream (`[SessionStart]`, `[UserPromptSubmit]`, `[Stop]`, …), the
  engine's mechanism tags, and the `[nav]` user-intent trail. The EXE-side writers — the updater's
  `[update]`, the portable installer's `[install]`, the startup notes `[path-guard]`, `[taskbar-badge]`,
  `[taskbar-frame]`, `[startup-hang]` — append here too.
- **`autorunner.log`:** the Prompt Queue Runner's scheduler — `[send]` (accepted for delivery, not yet
  delivered), `[send-deferred]`, `[advance-skip]` (why the queue did not move; change-deduped per
  session), `[plan-done]`, `[stop-on-error]`, `[pause-all] on|off`, `[enter-retry] <id> press k/3`,
  `[enter-retry-giveup]`.
- **`scanner.log`:** the PULL reconciler — every `[recon-*]` synth (`recon-run`, `-stop`, `-stop-idle`,
  `-stop-hold`, `-block`, `-block-presence`, `-resume`, `-resume-presence`, `-subagent`, `-error`,
  `-error-release`, `-cutoff`, `-dormant`), `[decay-waiting]` and the transcript watcher's `[watch]`.
- **Both hooks.log and autorunner.log:** every verdict that pauses a runner or changes its mode —
  `[lost-send]`, `[merge-detected]`, `[send-reclaim]`, the terminal `[send-verify] … FAILED` and the
  box-not-visible `[send-verify] … HELD`, `[interrupt-hold]` / `[interrupt-resume]`.
- `forwarder-errors.log`: the hook forwarder's own drops (no session id, no pipe, a dead pipe's connect
  timeout) — lines that never reached the bridge.

## hooks.log's three layers

1. **The hook event stream:** the push state machine's input, one line per hook.
2. **Mechanism tags**, each carrying the ids involved: `[fork]`, `[resume]`, `[restore-fresh]`,
   `[restore->refork]`, `[rehome]`, `[spawn]`, `[launch-fail]`, `[launch-blocked]`, `[archive]`,
   `[teardown-archive]`, `[codex-*]`, `[adopt-*]`, `[pending]`, `[notify]`, `[update]`, `[trust]`,
   `[transcript]`, `[cmd]`, `[cmd-fire]`, `[cmd-expire]`, `[persist-fail]`, `[observer]`, `[activity]`,
   `[orphan]`, `[orphan-reap]`, `[close-successor]`, `[unread]`, `[taskbar-badge]`, `[tags]`, `[harness]`,
   `[ai-search]`, `[sessdb]`, `[msg]`, `[render]`, `[tab-size]`, `[strip]`, `[deactivate]`,
   `[retired-host]`, `[restore-wake]`, `[api-error-rewind]`, `[interrupt-rewind]`, `[fork-stop]`;
   - **delivery forensics:** `[ups]` (one per UserPromptSubmit: echo consumed / recorded as Typed / noise
     / EMPTY), `[pull-echo]`, `[gate]`, `[draft-swap]`, `[draft-fork]`, `[draft-restore]`,
     `[send-verify]`, the verified presser's `[enter-retry]`, `[delivered]` (bytes that reached the
     ConPTY, against autorunner.log's `[send]`), `[advance-dropped]` (a turn completed in a process with
     no advance handler: never the app);
   - **the window-restore story**, one trace per `windowId`: `[window-claim]` / `[window-fresh]` →
     `[rehome-begin]` (every tab ref by session id + the focus target) → per tab `[rehome] window <id>
     resume|skip <sid>` (a skip carries its reason) → `[rehome] window <id> resumed=N shells=M skipped=K`;
     `[window-save]` is the change-gated saved tab set and `[reopen]` the "Reopen Windows (N)" dispatch.
3. **`[nav]`, the user-navigation audit trail** (`LogNav(msg)` writes `[nav] <msg>`; `ShortId(id)` is the
   8-character id). `grep '\[nav\]' hooks.log` reconstructs the user's journey.

## The [nav] trail

- **Begin/end pairing:** every action that launches or tears down a process logs a BEGIN before the work
  and an END after it, so a begin without its end pinpoints a crash or hang mid-action. The END of a
  creating action carries the new managed id, resolved from the created tab (`_ClaudeSessionForTab`), so
  a resume or fork that fell back to fresh reports the new id. A blocked launch (no native `claude.exe`)
  still logs its `…-done (blocked …)`.
- **Paired funnels:** `fork-click` ↔ `fork-done new=… from=…` (the Sessions page's fork) ·
  `fork-managed-begin` ↔ `fork-managed-done` (a live session's fork) · `resume-click` ↔ `resume-done` (or
  `resume -> jump` when already open) · `open-new {claude,codex}` ↔ `… done` · `adopt-{external,codex}` ↔
  `… done` · `restart-begin` ↔ `restart-done` · `handover-begin` ↔ `handover-done new=… from=…` (and the
  `handover-here` / `handover-standby` twins) · `close-begin` ↔ `close-done` · `sessions-page open-begin` ↔
  `shown` · `reopen-windows-begin` ↔ `…-done` · `close-all begin tabs=N managed=M` ↔ `close-all done` ·
  `deactivate-begin` ↔ `deactivate-done` · `tab-drag-begin` ↔ `tab-drag-end`.
- **Other covered actions** (user-driven, never per tick; ~52 sites):
  - **Sessions page:** `search` (the query + active scopes + fast count, then `search-done content=N`),
    `select` (row click and Up/Down, tagged `via=name/dir | content | browse`), `favorite on/off`, `hide` /
    `unhide`, `sessions-page close (back)`, `sessions ai …`.
  - **Cross-cutting:** `activate` (local or the cross-window fan-out), `rename` (the Explorer and the
    tab-strip sync, the latter only on a real change).
  - **Prompt Queue:** `queue`, `queue-pop`, `send-now` (delivered or rolled back), `autorunner -> Off|Full`,
    `pause-all`, `template-save`, `template-apply` (one session, or every session in a folder with the
    count), `compose pull-draft`.
  - **Navigation:** `tab-focus` (gated on `Initialized`), `tab-swap` (a tab's conversation changed in
    place: `/clear`, `/resume`), `manager select-external`, `manager-page …`, `home-button jump-back ->
    <sid8>` / `home-button click ignored …`, `jump-to-prompt`, `notify-click`, `tab-move`,
    `tab-send-to-window`, `session-menu <verb> <scope> <sid8>`.
  - **Lifecycle and windows:** `mark-unread`, `move-out`, `manager bring-to-front`, `new-window`,
    `pane-split` (gated on `Initialized`), `duplicate-tab`, `tab-color <sid8> -> <#hex|reset> (individual)`
    / `tab-color dir=<dir> -> …`, `tag add` / `tag remove` / `tag delete` / `tag recolor`.
  - **Close and quit decisions:** `close-cancelled <sid8>`, `close-confirm <sid8> (favorite & close |
    unfavorite & close)`, `close-all cancelled …`, `window-close cancelled|confirmed|-> close ALL windows
    (quit requested)`, `quit cancelled|confirmed (closing all windows)`, `deactivate-idle-ask` /
    `deactivate-idle-answer`.
  - **Clipboard and shell:** `copy <field>` (the one `CopySessionField` chokepoint behind every copy menu;
    each Transcript level adds a `[transcript] <sid8> copy <level>: chars=N rendered in Xms` line or the
    reason nothing was copied; Current Prompt adds a `[pending] … copy current prompt: live|remembered
    chars=N` line or `nothing`), `copy auto-testing-prompt <sid8> (auto|typed)`, `open-path`.
  - **Transcript view:** `transcript-show <sid8> [codex] (from <menu>)`, `transcript-level`,
    `transcript-copy`, `transcript-refresh`, `transcript-close <sid8> (<reason>)`, beside the
    `[transcript] <sid8> show <Level>: chars=N blocks=M rendered in Xms` line.
  - **Settings:** `settings-save` (skipPerms / model / autorunner / claudeExe), `settings-reset tab=…`,
    `profile-change <from> -> <to>`, `check-for-updates`, `update-prerelease -> on/off`,
    `reset-hidden-sessions`.
- **The fork chain reads:** `[nav] sessions fork-click row=…` → `[sessions-page->fork] source=…` →
  `[fork] <new> (forked from <source>)` → `[nav] sessions fork-done new=<new> from=<source>`.
- **Deliberately not logged:** prompt-queue micro-edits (move / delete a Pending row), opening the cog,
  the summary panel's toggles, the transcript view's folds / Expand all / line-wrap toggle, path-picker
  navigation, and pure view filters (sort column, scope, time window, row-filter facets; the active
  scopes ride the `search` line).
- **Census gating:** the observer's `[observer] census` re-logs only when Agentmaster's own fleet changes
  (the set or the identifying facts of its claudes) or every 5 min, never for external churn.

## Silent failures surfaced

- A launch that built no tab logs `[launch-fail]` (the Claude and Codex null-pane returns); a failed state
  write logs `[persist-fail]` (the `WriteAllUtf8` chokepoint behind `sessions.json`, the window records,
  templates, `dir-colors.json`, …).

## Exception forensics (Rule #18)

- A `catch (...)` runs after the unwind, when the throw-site stack is gone, so a process-wide vectored
  exception handler (`InstallThrowStackCapture`, `ClaudeSpawn.{h,cpp}`) captures each MSVC C++ throw
  (`0xE06D7363`) at raise time: ≤ 64 frames and a tick into a per-thread 4-entry ring (lock-free,
  allocation-free, capture-only, always `EXCEPTION_CONTINUE_SEARCH`). A coroutine's stored exception
  re-raised at `co_await` captures again while the older entries keep the original throw.
- **Two reporters, one chokepoint** (`LogSwallowedExceptionCore`):
  `Agentmaster::LogSwallowedException(L"<context>")` for the plain-C++ engine (which also compiles in the
  test harness and the CLI) and `AgentLogCaughtException(context)` (`AgentCatchLog.h`) for the
  TerminalApp layer, which also classifies `winrt::hresult_error` (hr + message),
  `wil::ResultException` and `std::system_error` by rethrowing (RTTI is off repo-wide). Both are
  `noexcept` and throttled ~2 s per context (failing open, with a `[suppressed N earlier repeat(s)]`
  count).
- `AgentCatchLog.h` installs `AgentWilFailureLogger`, the `wil::SetResultLoggingCallback` sink for
  `TerminalApp.dll` (wil state is per module), so every `CATCH_LOG` / `LOG_*` / fail-fast there logs
  `[wil] <kind> hr=… at <file>:<line> … ret=Module+RVA`.
- `InstallAgentExceptionTrace()` installs both once per process at engine init
  (`[exc] exception forensics installed …`).
- Frames log as `Module.dll+0xRVA` (ASLR-stable): a hooks.log line can be symbolized later against that
  build's PDBs, e.g. `./.claude/skills/debug-dumps/scripts/diasym.exe <TerminalApp.pdb> 0x22B0F9` (the PDB
  must match the build that wrote the log).
- Shape: `[exc] _ObserverProbe: swallowed winrt::hresult_error hr=0x8001010E msg="…" tid=0x1586C (no
  crash)`, then `[exc]   throw#0 (age 2ms, 31 frames): TerminalApp.dll+0x22B0F9 …`.
- ⚠ The catches on the logging path itself stay bare (`LogSwallowedException` → `LogSwallowedExceptionCore`
  → `ExcLogThrottleAllow` / `CaptureRecentThrowStacksText` → `FormatAddressModuleRva` → `AppendStateLog`
  → `AgentmasterStateDir`): logging from them would recurse, and `AppendStateLog` holds a non-recursive
  mutex, so it would deadlock first. Each is marked `⚠ DELIBERATELY BARE` under one
  `LOGGING-PATH RECURSION RULE` block in `ClaudeSpawn.cpp`; the classifier's terminal `catch (...)`, which
  records `"unknown exception"`, is not one of them.
