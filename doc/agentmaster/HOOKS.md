# Agentmaster — Claude Code Hooks Bridge

Session **state** (Idle / Running / Waiting-for-you / Needs-approval / Error / Done) is
derived from **Claude Code hooks**, not by parsing the terminal. Hooks are authoritative
and cheap; the TUI redraw stream is not.

## Session correlation

When the Manager spawns a Claude session it sets an environment variable on the child
`claude.exe`:

```
CCMGR_SESSION_ID=<guid>
```

and writes a Claude Code hooks configuration whose commands echo that id plus the event.
Because the hook process inherits the env, every hook payload can be tied back to the
exact `SessionInfo` / `ConptyConnection` in the `SessionRegistry`. `cwd` is included as a
secondary key (the `M` working directory).

## Transport

Hooks POST a small JSON line to a **local named pipe** the app hosts
(e.g. `\\.\pipe\agentmaster.<pid>`), read on a dedicated thread. (Fallback: append to a
per-session JSONL file the app watches.) No network, no external process.

## Events consumed

| Hook | Meaning | Effect in Agentmaster |
| --- | --- | --- |
| `SessionStart` | session began | create/confirm `SessionInfo`, state → `Idle` |
| `UserPromptSubmit` | a prompt was submitted (by user or by us) | state → `Running`; confirm our injected prompt landed (idempotency) |
| `PreToolUse`/`PostToolUse` | tool activity | keep state `Running`; activity timestamp |
| `Notification` (permission) | tool-permission prompt | state → `NeedsApproval`; run **Approval Policy** (NOT the prompt queue) |
| `Stop` | main agent finished the turn | state → `WaitingForInput`; **Autopilot.tryAdvance()** |
| `SubagentStop` | a subagent finished | informational |
| `SessionEnd` | session ended | state → `Done`; stop Autopilot |

## Payload (proposed)

```json
{ "sessionId": "<guid>", "cwd": "K:/api", "event": "Stop", "ts": 0,
  "lastMessageIsQuestion": false, "tool": null }
```

`lastMessageIsQuestion` (best-effort, from the hook input) feeds the **question-guard**:
on `Stop`, if true, the next queued prompt is **Held** rather than auto-sent.

## Scheduler trigger (see IMPLEMENTATION.md / M7)

```
onHook(sid, ev):
  s = registry[sid]
  Stop:                 s.state = WaitingForInput; tryAdvance(s)
  Notification(perm):   s.state = NeedsApproval;   applyApprovalPolicy(s)   // not the queue

tryAdvance(s):
  if s.autopilot.mode == Off:            return
  if s.state != WaitingForInput:         return
  if humanTypedWithin(s, 1500ms):        return        // pauseOnHumanInput
  item = s.queue.firstPending(); if !item: notifyPlanDone(s); return
  if !passesGuard(s, item):  item.status = Held; flagCard(s); return  // e.g. not-a-question
  if s.autopilot.mode == SemiAuto:       askConfirm(s, item); return
  sleep(s.autopilot.throttleMs)
  s.connection.WriteInput(item.text + L"\r");          // inject + submit (null-terminated)
  item.status = Sent; item.sentAtUnixMs = now; persist(s)
```
