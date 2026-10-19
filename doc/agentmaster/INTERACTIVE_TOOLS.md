# Interactive tools — Claude Code's questions, plans and goals, answered from Telegram

> **2026-10-19 · DESIGN, nothing built.** Read in the Claude Code 2.1.291 bundle and **measured on it**: a live
> ConPTY probe (§6) and a survey of every transcript on this machine (§2). It answers the open half of
> [`MESSAGING.md`](MESSAGING.md) §7.4 (decision D9) and its probes P0-1, P0-3 and P0-22. The probe and the
> survey scripts are in `tools/interactive-tools-probe/`.

## 0. Summary

- **The gap.** Four Claude Code tools stop and wait for the user:
  - `AskUserQuestion`: questions with options;
  - `ExitPlanMode`: approve a plan;
  - `EnterPlanMode`: switch to plan mode;
  - `ProposeGoal`: propose a session goal.

  Agentmaster's Telegram bot shows a question as a numbered list and a plan as text. It cannot answer
  either; the user must go to the PC (MESSAGING.md §21.2). **There is no `ProposePlan` tool** in 2.1.291:
  the plan tool is `ExitPlanMode`.
- **How much it matters** (§2):
  - 1,055 questions in 600 sessions. The median wait was 2.6 min; 114 waited more than 30 min and 54 more
    than 2 h.
  - 132 plan approvals; 11 of them waited more than 30 min. 109 of 126 plans are longer than one Telegram
    message.
- **The way in** (§4, §5): a **`PermissionRequest` hook**. Claude Code runs it when the question or plan
  dialog opens, at the same time as the dialog, and the first answer wins. For exactly these two tools it
  accepts the hook's answer:
  - `allow` + `updatedInput` carries the answers of a question, or approves a plan (optionally with a mode
    change);
  - `deny` + a message is "keep planning" feedback.

  The schema puts no upper limit on a hook's `timeout`. So Agentmaster's hook can hold the dialog for the
  phone for hours, and the PC dialog stays usable the whole time.
- **Proven live** (§6), four scenarios, each checked on the transcript:
  - the hook answers a question;
  - the PC answers first, and the hook's late answer is ignored;
  - the hook approves a plan, and the mode from before plan mode comes back;
  - the hook rejects a plan with feedback, then approves the revision with a mode change.

  ⚠ When the PC answers first, Claude Code does **not** kill the waiting hook. Agentmaster must release it
  (§7.5).
- **The design** (§7):
  - a small helper is the hook;
  - a duplex pipe to the engine;
  - a table of open dialogs in the engine;
  - Telegram messages with buttons, edited to the outcome.

  `ProposeGoal` needs no hook: its approval is `/goal <condition>`, which a topic can send as a prompt.
  `EnterPlanMode` never asks in Agentmaster's bypass launches.
- **Decisions** I1–I8 (§8) are open.

## 1. The tools that wait for the user (Claude Code 2.1.291)

| Tool | Waits how | The user's answer reaches the model as | Can a hook answer it? |
|---|---|---|---|
| `AskUserQuestion` | a permission dialog in every mode, bypass included (`checkPermissions` always asks; `requiresUserInteraction`) | the tool result (§1.1) | **yes**: `allow` + `updatedInput.answers` (measured) |
| `ExitPlanMode` | a permission dialog "Exit plan mode?" (plan mode only) | the tool result; on approval the mode changes (§1.3) | **yes**: `allow` + `updatedInput: {}`, optionally `setMode`; `deny` = feedback (measured) |
| `EnterPlanMode` | no dialog of its own; the mode's rules decide. Under bypass it never asks | "Entered plan mode. …" | only as a normal permission prompt (default mode) |
| `ProposeGoal` | **no permission ask.** A dialog under the input box; the turn goes on | approval queues `/goal <condition>` | **no**: no hook sees it (§1.4) |
| Any other tool (permission prompt) | a permission dialog when the mode does not allow the call | allowed or rejected | yes, but out of scope here (§7.6) |

### 1.1 AskUserQuestion

**The model passes** (the bundled typings, `claude-code-tools/index.d.ts`):
- `questions`: 1–4 questions, each with:
  - `question`: the text;
  - `header`: a chip of at most 12 characters (recommended);
  - `options`: 2–4 options, each `{label, description, preview?}`;
  - `multiSelect`.
- `metadata.source`, optional.

The output schema also knows an extended form: a `title`, and kinds `choice` / `text` / `number`. It is
behind a feature gate that reads off here.

**What the user sees** (the probe's screen):

```
 ☐ Color

Which color do you prefer?

❯ 1. Red
     The color red
  2. Blue
     The color blue
  3. Type something.
────────────────────────────────────────────────
  4. Chat about this

Enter to select · ↑/↓ to navigate · Esc to cancel
```

- **"Type something."** is the free-text "Other". **"Chat about this"** is offered for single-select only.
- A multi-select question shows toggles, and an option with a `preview` opens a side-by-side preview pane
  (PENDING_INPUT.md met that pane).
- A dialog opened while the user has a draft in the box stays hidden. The box line says "Claude has a
  question for you — it shows once you send or clear what you're typing."

**The answer** (`call()` and `mapToolResultToToolResultBlockParam`) is the updated input plus:
- `answers`: question text → answer. An answer is the option label. A multi-select answer is the labels
  joined with ", ", and a label that holds ", " or `"` is JSON-quoted (`Tct`). A typed "Other" answer is
  the text itself.
- `annotations`: question text → `{preview?, notes?}`. The PC dialog adds `annotations: {}` even when it is
  empty.
- `response`: free text instead of answers.
- `followUp: true`: the user asks for more questions first.
- `afkTimeoutMs`: set only by the idle auto-continue.

The sentinel `"(notes only)"` means notes without a choice.

**The texts the model reads** (verbatim, 2.1.291):

| Case | Tool result |
|---|---|
| every answer is an option label (or a well-formed list) | `Your questions have been answered: "Q"="A", …. You can now continue with these answers in mind.` |
| an "Other" text among them | `The user answered: "Q"="A". Read the answers carefully — they may request clarification, changes, or that you not proceed — and follow what they actually say.` |
| a preview / notes | each pair gets `selected preview:\n<preview>` and `notes: <notes>` |
| `response` | `The user responded: <text>` |
| `followUp` | `The user asked for more questions before you proceed. So far they answered: …. Call AskUserQuestion again with follow-up questions …` |
| nothing answered | `The user did not answer the questions.` |
| the idle auto-continue | `No response after Ns — the user may be away from keyboard. Proceed using your best judgment …` |
| "Chat about this" | a **rejection**: `The user doesn't want to proceed with this tool use. … To tell you how to proceed, the user said:\nThe user wants to clarify these questions. … Start by asking them what they would like to clarify. Questions asked: - "<q>" Answer: … / (No answer provided)` |
| Esc | a **rejection that ends the turn**: `… STOP what you are doing and wait for the user to tell you how to proceed.` |

**The idle auto-continue:**
- Off unless the setting `askUserQuestionTimeout` is `60s` / `5m` / `10m` (the default is `never`) or
  `CLAUDE_AFK_TIMEOUT_MS` is set.
- Claude Code switches it off while a Remote Control bridge races the dialog.

**A remote card's answer** (Claude Code's own remote surfaces, `admitCardAnswer`):
- It may add only `answers`, `annotations`, `response` and `followUp`.
- It may not change anything shown, and it may not set `afkTimeoutMs`.
- Each answer is at most 8,192 characters, and all of them together at most 32,768.
- A hook's answer is **not** checked by this function; it passes the permission re-check of §5.3 instead.
  Agentmaster applies the same limits itself (§7.3).

### 1.2 EnterPlanMode

- **No input, and no `checkPermissions` of its own.** The permission mode decides.
  - In `bypassPermissions` (Agentmaster's default launch) it runs at once. The corpus has 88 calls, every
    one answered in under 1 s.
  - In `default` mode it is a normal permission dialog titled "Enter plan mode?".
- **Main session only:** "Only the main session can enter plan mode."
- **The user can also switch to plan mode with Shift+Tab**, with no tool call.
- **Result:** `Entered plan mode. You should now focus on exploring the codebase and designing an
  implementation approach.` It is followed by the six-step plan-mode list and "DO NOT write or edit any
  files yet."

### 1.3 ExitPlanMode

- **The model passes** `{allowedPrompts?}` (deprecated, unused). Before the permission check, Claude Code adds
  `plan` and `planFilePath` from the plan file (`~/.claude/plans/<three-word-slug>.md`); the hook's
  `tool_input` carried both (§6).
- **Validation:** outside plan mode the call is refused with "You are not in plan mode. …".
- **`checkPermissions`:** asks "Exit plan mode?". In a teammate the call is allowed, and the plan goes to the
  team lead's inbox instead.
- **The dialog**, in a session launched with `--dangerously-skip-permissions` (the probe's screen):

```
Claude has written up a plan and is ready to execute. Would you like to proceed?

❯ 1. Yes, and switch to BYPASS PERMISSIONS (no further prompts) for this session
  2. Yes, manually approve edits
  3. Tell Claude what to change
     shift+tab to approve with this feedback

ctrl+g to edit in <editor> · ~\.claude\plans\glistening-noodling-lagoon.md
```

- **The option builder** (`u9e`) and what each option returns (`g9e`):

| Option | Shown when | Returns |
|---|---|---|
| Yes, clear context (N% used) and bypass permissions / use auto mode / auto-accept edits | only with the setting `showClearContextOnPlanAccept` (default **false**) | a **rejection** of the call, and Claude Code then starts a fresh conversation in the same process with the first message `Implement the following plan:\n\n<plan>\n\nIf you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: <path>` (+ `User feedback on this plan: …`), in the chosen mode — the plan-restart link Agentmaster's `TranscriptTurns` already follows |
| Yes, and switch to BYPASS PERMISSIONS … / Yes, auto-accept edits / resume auto mode | one of them, by what the session allows | `allow`, `updatedInput: {}` (or `{plan}` when the user edited the plan with Ctrl+G), and a `setMode` update for the session |
| Yes, manually approve edits | always | the same with `setMode: default` |
| No, refine with Ultraplan in a cloud session | when Ultraplan is available | a rejection with a canned hand-off text |
| No, keep planning (the box "Tell Claude what to change") | always | with text: a rejection with the text as feedback; the turn goes on. Without text: nothing |

- **`call()` itself restores the mode** from before plan mode (`prePlanMode`, default `default`) when the
  session is still in plan mode. A `plan` in the input is written to the plan file and marks the plan
  "edited by user".
- **The texts the model reads:**
  - approved: `User has approved your plan. You can now start coding. Start with updating your todo list if
    applicable\n\nYour plan has been saved to: <file>\nYou can refer back to it if needed during
    implementation.…\n\n## Approved Plan:\n<plan>`. The heading reads `## Approved Plan (edited by user):`
    after an edit.
  - approved without a plan: `User has approved exiting plan mode. You can now proceed.`
  - rejected with feedback: `The user doesn't want to proceed with this tool use. The tool use was rejected
    (eg. if it was a file edit, the new_string was NOT written to the file). To tell you how to proceed, the
    user said:\n<feedback>`. The model keeps planning.
  - Esc: the STOP text of §1.1; the turn ends.

### 1.4 ProposeGoal

- **The model passes** `{condition (at most 500 characters), ask_user?}`.
- **Where it runs:** only in interactive local sessions, and only behind a feature gate. It is refused:
  - in plan mode ("propose the goal after the plan is approved");
  - in agents;
  - when the user switched off model-proposed goals.
- **It is not a permission ask.** The tool returns at once, and a dialog opens **under** the input box
  (`requestDialog`, `place: "under"`) while the turn goes on.
- **On approval** Claude Code queues `/goal <condition>` (origin `auto-continuation`). With `ask_user: false`
  (when the settings allow it) it queues the goal directly.
- **No hook sees it.** The corpus has 0 calls.

### 1.5 Permission prompts, and the Notification Agentmaster uses today

- **Any other tool call** that the mode does not allow opens a permission dialog. Under `bypassPermissions`
  that is only the safety checks that always ask.
- **Six seconds after any ask** (`kUt = 6000` ms), Claude Code fires `Notification` with
  `{message: "Claude needs your permission to use <tool>", notificationType: "permission_prompt"}`, for
  `AskUserQuestion` and `ExitPlanMode` too. `CLAUDE_CODE_DISABLE_PERMISSION_PROMPT_NOTIFY_HOOKS` turns it
  off. This is what Agentmaster turns into `NeedsApproval`.
- **A "person-only" ask** (some safety checks, a few internal confirmations) runs no hooks at all.

## 2. How often, how long (the corpus survey, 2026-10-19)

**Method:** every transcript under `~/.claude/projects`, subagent files left out (1,081 files hold one of the
four tools before the probe; `tools/interactive-tools-probe/survey_interactive.py`). Each `tool_use` was paired with its
`tool_result`, and the wait is the time between them. The numbers include the first probe session of §6 (2
questions, 3 plan approvals, 2 `EnterPlanMode` calls); real use before it was 1,053 questions, 129 plan
approvals and 88 `EnterPlanMode` calls, the newest plan call on 2026-10-10.

| | AskUserQuestion | ExitPlanMode | EnterPlanMode | ProposeGoal |
|---|---|---|---|---|
| Calls / transcripts | 1,055 / 600 | 132 / 87 | 90 / 82 | 0 |
| Wait: median · p75 · p90 · max | 2.6 min · 7.4 min · 34.8 min · 350.8 h | 1.7 min · 5.0 min · 23.1 min · 22.6 h | 0 s (all within 1 s) | — |
| Waited > 5 min / > 30 min / > 2 h | 341 / 114 / 54 (of 1,040) | 31 / 11 / 5 (of 126) | 0 | — |

**AskUserQuestion outcomes:**
- 832 answered; 127 answered with notes or a preview.
- 36 "Chat about this"; 31 Esc.
- 3 auto-continued while idle.
- 11 errors: 5 calls cut off by the session's end, 4 input-validation errors, and 2 whose result is the bare
  text "Answer questions?".
- 39 answers were `(notes only)`.

**AskUserQuestion shapes:**
- Questions per call: 1 in 716, 2 in 190, 3 in 73, 4 in 73 (and 2 malformed calls).
- Options per question: 2 in 394, 3 in 840, 4 in 372 (and 1 with 6).
- 115 multi-select questions.
- 458 options carry a preview (median 229 characters, max 1,298).
- Question text: median 109 characters, max 755. Header at most 21 characters; label at most 112.

**ExitPlanMode outcomes:**
- 22 approved in place (19 with a plan, 3 without; 2 of them the probe's).
- **66 approved with "clear context"**: the old default. Each is recorded as a rejection, plus a fresh
  conversation that names it in "read the full transcript at". 66 of the 71 such rejections have that
  successor.
- 33 "keep planning" with feedback (1 of them the probe's); 5 rejected (Esc).

**Plan length** (every call whose input holds a plan, 126): median 9,607 characters, p90 22,660, max 91,116.
**109 of the 126 are longer than one Telegram message (4,096 characters).**

## 3. What Agentmaster does today

- **State.** `NeedsApproval` comes from three independent belts (HOOKS.md; CLAUDE.md, "blocked-on-user"):
  - the `Notification(permission_prompt)` 6 s after the dialog;
  - the scanner's `recon-block` on an unanswered `AskUserQuestion` in the transcript;
  - claude's heartbeat `waiting`.

  It leaves `NeedsApproval` on the answer. The question guard keeps queued prompts Pending.
- **Telegram** (MessagingModels.h):
  - The transcript's `tool_use` becomes a `Question` item (the headers and question texts, the FIRST
    question's option labels, `multiSelect`) or a `Plan` item (`input.plan`).
  - They are shown as `❓ <question>` with numbered options, and `📋 Plan` with the text (cut at 3,000
    characters in HTML, 20,000 in a rich message).
  - The user's answer appears as a `↩ answer` line, and the card is re-sent with sound on `NeedsApproval`.
  - **No buttons.** A message typed in the topic is a queued prompt (D30); it waits until the dialog is gone
    and then goes as a new turn.
- ⚠ **The transcript can write a pending question's `tool_use` line late**, 10+ minutes (CLAUDE.md, the
  blocked-on-user belts; in the probe it was written at once). The mirror then shows the question late, or after
  it was answered. The hook gets the question at once (§5.2).
- **MESSAGING.md §7.4** (D9, recommended "verified key plans": keystrokes into the menu, checked by re-reading
  the screen) **is not built**, and its probes P0-1, P0-3 and P0-22 were never run. This document answers them
  (§8).

## 4. The ways in

| Route | Answers questions | Approves plans | Cost and risk | Verdict |
|---|---|---|---|---|
| **A. A `PermissionRequest` hook** | yes: `allow` + `updatedInput.answers` | yes: `allow` + `{}` (± `setMode`); `deny` + message = feedback | one waiting helper process per open dialog; a reply channel into the engine; Claude Code's documented hook | **recommended; measured (§6)** |
| B. The session link mod (MOD_LINK.md) | a `tool.call` hook can return `{result}` in place of the call | **no:** a `{result}` skips `call()`, so the session STAYS in plan mode (the mode is restored in `call()`); it would need the mod's `classic.PermissionRequest`, i.e. route A again | nothing built; ~85 MB per claude for the mod runtime | not needed for this |
| C. Key plans (MESSAGING.md §7.4) | keystrokes into the menu, verified on the screen | the same | fragile: the menu's layout, a dialog hidden behind a draft, focus; never measured (P0-1) | superseded for these two tools |
| D. Claude Code channels | **no:** the relay skips tools that require user interaction | no | a development flag and a start-up modal | excluded (D16) |
| E. Claude Code's Remote Control | yes, in the Claude app | yes | the user's claude.ai account and app, not Telegram | beside it, not instead of it |

## 5. The PermissionRequest hook in Claude Code 2.1.291

### 5.1 When it runs

- **In the main session it runs IN PARALLEL with the dialog.** The dialog is shown, the hooks start, and the
  first answer from either side wins (`zo`; `claim()`). The dialog stays fully usable while the hook waits.
  Other racers are the Remote Control relay, a channel relay and a server-approval watch.
- ⚠ **For subagents and teammates the hooks run BEFORE the dialog, and the dialog waits for them**
  (`awaitAutomatedChecksBeforeDialog`). A long-waiting hook would hide a subagent's dialog from the PC. The
  helper therefore answers at once for a request that carries `agent_id` (§7.2).
- **No hooks run** for a person-only ask or a remote ("served") call.

### 5.2 What it receives (measured)

```json
{
  "session_id": "8c7d6854-…", "transcript_path": "…\\8c7d6854-….jsonl", "cwd": "…\\ws",
  "scratchpad_dir": "…\\scratchpad", "prompt_id": "b5329d2c-…", "permission_mode": "bypassPermissions",
  "hook_event_name": "PermissionRequest", "tool_name": "AskUserQuestion",
  "tool_input": { "questions": [ { "question": "Which color do you prefer?", "header": "Color",
                  "options": [ { "label": "Red", "description": "The color red" }, … ], "multiSelect": false } ] }
}
```

- **In a subagent:** also `agent_id` and `agent_type`.
- **For an MCP tool:** `mcp_server`.
- **`permission_suggestions`** is absent for these two tools.
- **For `ExitPlanMode`**, `tool_input` is `{plan, planFilePath}`, injected from the plan file, and
  `permission_mode` is `plan`.
- ⚠ **There is no `tool_use_id`.** `prompt_id` names the user turn: both `ExitPlanMode` calls of one turn
  carried the same id. `session_id` is the current conversation (it changes on `/clear`; Agentmaster maps it
  like any hook).

### 5.3 What it may answer

```json
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow","updatedInput":{…},"updatedPermissions":[…]}}}
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"deny","message":"…","interrupt":false}}}
```

- **Accepted for exactly two tools.** For a tool that requires user interaction, an `allow` WITHOUT
  `updatedInput` is ignored, and the dialog stays. An `allow` WITH `updatedInput` is accepted only for the
  set `V_r = {ExitPlanMode, AskUserQuestion}` (`f5n`).
- **The rewritten input goes through the permission re-check** (`mC` with `hookUpdatedInput`). A deny or ask
  rule can still override it (logged "PermissionRequest hook allowed … with updatedInput, but … rule
  overrides").
- **`updatedPermissions` are applied the way the dialog's are** (`persistPermissions`). The dialog's own
  approve rows are `[{type: "setMode", mode, destination: "session"}]` (measured with `default`).
- **A `deny` gives the model the message verbatim.** The tool result reads `Error: <message>`. With
  `interrupt: true` the turn is aborted (like Esc).
- **For a plan, approve with `updatedInput: {}`.** Echoing `tool_input` back would carry `plan` and mark the
  plan "edited by user". The dialog itself sends `{}` unless the plan was edited (§1.3).

### 5.4 Timeout and lifetime

- **A command hook's `timeout` is `positive().optional()` in the schema: no upper limit.** The run applies
  `timeout × 1000` ms (`wn = e.timeout ? e.timeout*1000 : zl`), and the default `zl` is 600,000 ms (10 min).
  The JavaScript timer caps any delay at 2^31−1 ms (24.8 days); keep the timeout well under that (§7.2: 24 h).
- ⚠ **When the dialog is answered at the PC, Claude Code does NOT stop the hook.** Its late answer is dropped
  (`if (!L || !A()) return`). Measured: the hook process was alive 3 s after the PC answer, and exited only
  when it answered on its own. The hook can end early only through the turn's abort signal (Esc; read in the
  code, not measured) or its own timeout.

### 5.5 What the transcript records

- **A hook's decision is written as an attachment line just before the tool result:**
  `{"type":"attachment","attachment":{"type":"hook_permission_decision","decision":"allow"|"deny","toolUseID":"toolu_…","hookEvent":"PermissionRequest"}}`
  This is the on-disk proof that the answer came from a hook, and it carries the `tool_use` id that the hook
  payload lacks.
- **A PC answer writes no such line**, and its `toolUseResult` carries `annotations: {}`.
- **Plan mode leaves `plan_mode` / `plan_mode_exit` attachments** with the plan file's path.

### 5.6 Traps

- ⚠ **A `PreToolUse` / `PermissionRequest` hook entry that cannot be loaded makes Claude Code skip the
  permissions declared in the same settings file** (a fatal validation note: "nothing it sits in is applied
  until the entry is fixed or removed"). Agentmaster's `hooks-settings.json` must stay exactly valid.
- **Output exactly one JSON object, or nothing.** Plain text or two documents are ignored as non-JSON output.
  An exit code other than 0 or 2 is a hook error; 2 is a blocking error.
- **A `setMode` to `bypassPermissions` needs a session that may bypass.** For the Remote Control bridge Claude
  Code falls back to `default` otherwise; the hook path was not tested with it. The recommended buttons (§7.4)
  never ask for it: "Approve" restores the pre-plan mode, which is bypass for an Agentmaster launch.

## 6. The probe (2026-10-19)

**How it was run** (`tools/interactive-tools-probe/run_probe.py`):
- The `claude-wrap-ConPTY` driver ran a real `claude.exe` 2.1.291 TUI (Haiku 4.5, 160×50) in a throwaway
  folder.
- `CLAUDE_CODE_SANDBOXED=1` skipped the workspace-trust dialog without writing `~/.claude.json`.
- The launch carried `--dangerously-skip-permissions` (like Agentmaster's) and a `--settings` file whose only
  hook is a `PermissionRequest` command hook (matcher `AskUserQuestion|ExitPlanMode`, `timeout: 600`).
- That hook (`perm_hook.py`) logs its payload and long-polls for a reply file, like Agentmaster's helper
  would.
- Every verdict was read from the transcript.

| | Scenario | Result |
|---|---|---|
| A | The hook answers a question (`allow`, `updatedInput` = the input + `answers: {"Which color do you prefer?": "Blue"}`) | ✅ `Your questions have been answered: "Which color do you prefer?"="Blue". …`; the model replied `CHOSEN=Blue` |
| B | The PC answers first (Enter on option 1) | ✅ `…"Apple"`; the hook was still alive 3 s later; its later "Pear" was ignored; the model said `FRUIT=Apple` |
| C | The hook approves the plan (`allow`, `updatedInput: {}`, no `updatedPermissions`) | ✅ `User has approved your plan. …`; the footer showed "bypass permissions on" again (the pre-plan mode); `toolUseResult` had no `planWasEdited` |
| D | The hook rejects the plan with Claude Code's own "keep planning" text + "Change step 1 to print bye.", then approves the revision with `setMode: default` | ✅ the model rewrote the plan ("Step 1: print bye") and asked again; after the approval the footer showed "manual mode on" |

**Timing** (the transcript's times against the hook log):
- The hook process started 0.11–0.28 s after the transcript's `tool_use` line, including the Python start.
- The tool result was written 0.01–0.02 s after the hook answered.
- In B the PC's answer came 3.7 s before the hook's late one, which was dropped.

**Reproduced:** the committed script ran again at 08:11 the same day, in a fresh folder and session, with the
same four results.

## 7. The design (proposal, nothing built)

### 7.1 The pieces

```
claude.exe ──PermissionRequest (exec form, matcher AskUserQuestion|ExitPlanMode, timeout 86400)──▶ agentmaster ask helper
                                                                                                       │ one JSON line each way
                                                                                                       ▼
                                                     \\.\pipe\agentmaster.<pid>.ask (duplex) ── Engine: open-dialog table
                                                                                                       │
                                                       Messaging service ◀── a question / plan message with buttons
                                                       a tap / a reply ──▶ the decision ──▶ the helper prints it ──▶ claude
```

### 7.2 The helper

- **One more hook in `hooks-settings.json`**, written by the same three writers as today (engine init, every
  spawn, the cog's Save) and in exec form like the forwarder:
  ```json
  "PermissionRequest": [ { "matcher": "AskUserQuestion|ExitPlanMode",
      "hooks": [ { "type": "command", "command": "<profile>\\bin\\agentmaster-ask.exe", "args": [], "timeout": 86400 } ] } ]
  ```
  - Only for a claude ≥ 2.1.139 (exec form). An older claude gets no ask hook and keeps today's show-only
    behavior.
  - The 24 h timeout is long enough for a phone answer; after it the dialog stays with the PC.
- **A native exe, not the PowerShell forwarder** (decision I2):
  - It waits for as long as the dialog is open. A waiting Windows PowerShell 5.1 holds tens of MB and starts in
    0.3–0.7 s warm (HOOKS.md); a native helper holds a few MB.
  - It can be the CLI binary in a hook mode (`agentmaster-cli.exe hook-ask`).
  - It is copied into the profile, the way the GUI copies the session database service into
    `<profile>\sessiondb\bin`, so that the path in the settings file survives a layout swap.
- **What it does:**
  1. Read the payload from stdin.
  2. Print nothing and exit 0 at once when any of these holds:
     - `agent_id` is present (a subagent's dialog waits for its hooks, §5.1);
     - the tool is not one of the two;
     - Agentmaster's pipe cannot be found (the forwarder's discovery: `CCMGR_HOOK_PIPE`, else `bridge.json`).
  3. Connect to `\\.\pipe\agentmaster.<pid>.ask`. Write one request line: the payload plus the helper's pid and
     its parent claude's pid.
  4. Block on one reply line. Print it as the hook output when it is a decision; print nothing when it is
     `{}`. Exit 0.
- **It ends by itself** when:
  - the reply arrives;
  - the pipe breaks (the app quit);
  - its parent claude exits (it holds the claude's process handle; Claude Code does not end it, §5.4);
  - the 24 h deadline passes.
- **It fails open everywhere:** any doubt means no output and exit 0, and the dialog stays with the PC.

### 7.3 The engine

- **The ask server** is a duplex pipe beside the hook pipe (`HooksBridge`; today's hook pipe is
  `PIPE_ACCESS_INBOUND`). It keeps one connection per waiting helper, with the same access as the hook pipe.
  It answers `{}` at once when:
  - no remote surface is connected (Telegram off or not connected);
  - the session is not managed;
  - the request is malformed.
- **The open-dialog table** (`PendingInteraction`, transient, never persisted) holds per entry:
  - a nonce;
  - the registry session (mapped from `session_id` like a hook);
  - the tool, its input (the questions, or the plan + its file), `prompt_id` and the open time;
  - the waiting connection;
  - a state: open / answering / closed;
  - when closed, the outcome and its origin: phone / PC / Esc / exit / timeout.
- **A new state signal:** a request is an exact "a dialog is open now". It can set `NeedsApproval` at once,
  instead of the `Notification` 6 s later, and carry the question to the board and the badge. It is a fourth
  belt beside today's three, never the only one.
- **Answering from the phone:**
  1. `AnswerInteraction(nonce, choice)` checks the choice against the stored input:
     - the question texts are the same;
     - every label exists;
     - an answer is at most 8,192 characters, and all of them together at most 32,768 (Claude Code's own
       bound for remote answers, §1.1).
  2. It builds the decision (§7.4) and writes it to the connection.
  3. It logs `[ask] <sid8> answered from telegram: …`.
  4. It verifies: within ~10 s the transcript must hold a `hook_permission_decision` for the session and the
     tool's result. Otherwise the message is edited to "not confirmed — look at the PC".
- **Answered elsewhere.** Any of these closes the entry with `{}`; the helper exits:
  - the transcript shows the tool's result, or a `hook_permission_decision` line, for the session after the
    open;
  - the heartbeat leaves `waiting`;
  - the turn is interrupted;
  - the claude exits;
  - the tab closes.

  The phone message is then edited to the answer given, read from the transcript's `toolUseResult`.
- **Pairing with the mirror:** the request has no `tool_use` id. The open entry pairs with the transcript's
  `tool_use` by session, tool and content. The `hook_permission_decision` line confirms the pairing
  afterwards.

### 7.4 Telegram

- **A question message**, sent at once with sound. It replaces today's `❓` item for this `tool_use`.

  ```
  ❓ Color · Which color do you prefer?
   1. Red — The color red
   2. Blue — The color blue
  [ Red ] [ Blue ]
  [ ✍️ Other… ] [ 💬 Reply instead… ]
  ```

  - One row of option buttons per question. For a single single-select question, a tap sends at once.
  - Multi-select options are toggles (☐ / ☑).
  - With several questions, each pick ticks in place, and **[✅ Send answers]** sends them all. Claude Code
    accepts an unanswered question.
  - **✍️ Other…** asks for a reply (ForceReply). The reply's text becomes that question's answer, which gives
    "The user answered: …".
  - **💬 Reply instead…** sends the reply's text as `response` ("The user responded: …").
  - A preview is shown under its option as an expandable quote.
- **A plan message**, also with sound:

  ```
  📋 Plan ready for approval
  <the first ~3,000 characters, as Markdown>
  📎 plan.md (the whole plan as a document — 109 of 126 plans need it)
  [ ✅ Approve ] [ ✅ Approve · ask before edits ]
  [ ✏️ Keep planning… ] [ ⏹ Stop ]
  ```

  | Button | Decision |
  |---|---|
  | **Approve** | `allow`, `updatedInput: {}`, no `updatedPermissions`: back to the mode from before plan mode (case C) |
  | **Approve · ask before edits** | the same with `setMode: default` (case D) |
  | **Keep planning…** | asks for a reply; `deny` with Claude Code's own text, `The user doesn't want to proceed with this tool use. … To tell you how to proceed, the user said:\n` + the reply. The model reads what the PC's "Tell Claude what to change" gives it (case D) |
  | **Stop** | `deny` + `interrupt: true` with the STOP text (like Esc) |

- **The outcome:** the message is edited and its buttons removed:
  - `✅ Blue — from the phone, 14:02`;
  - `✅ Apple — answered at the PC, 14:03`;
  - `⏹ stopped at the PC`;
  - `⌛ no answer (the session ended)`.

  The card stops ringing with the state.
- **Buttons are action tokens** (MESSAGING.md §7.5). They bind the open entry's nonce; a stale tap (already
  closed) answers "already answered: …".

### 7.5 Lifecycle

| Event | Engine | Helper | Telegram |
|---|---|---|---|
| The dialog opens | entry open; `NeedsApproval` | waits | question / plan message |
| A tap or reply on the phone | decision written; entry answering → closed on the transcript's proof | prints the decision, exits | edited to the answer |
| The PC answers first | the transcript / heartbeat shows it → `{}` | exits | edited to the PC's answer |
| Esc at the PC | the rejection in the transcript closes the entry with `{}` | ended by the turn's abort signal (read in the code, not measured), else by the `{}` | "stopped at the PC" |
| claude exits, the tab closes | `{}` / entry dropped | exits (parent gone or pipe) | "no answer (the session ended)" |
| The app quits | the pipe breaks | exits; **the dialog stays usable at the PC** | stale buttons answer "Agentmaster is not running" (after the restart: "already closed") |
| Telegram is not connected | `{}` at once | exits within ~0.1 s | — |
| A subagent's question | never reaches the engine | `{}` at once (§5.1) | today's show-only item |
| 24 h pass | — | exits at its deadline; the dialog stays | edited to "no longer answerable from here" |

### 7.6 The other tools

- **ProposeGoal:** a topic message with **[🎯 Set this goal]**, which queues `/goal <condition>` as a remote
  prompt (D30), the very command the approval queues. No hook is involved, and the dialog at the PC stays
  until it is answered or dismissed there. The corpus has 0 calls, so this is low priority.
- **EnterPlanMode:** nothing to do under bypass. In `default` mode it is a normal permission prompt.
- **Generic permission prompts** (sessions not launched with skip-permissions): the same hook with matcher `.*`
  can answer Allow once / Always allow (from `permission_suggestions`, as `updatedPermissions`) / Deny. That
  is the `permission` message type of MESSAGING.md §6.3. Later, off by default (decision I6).

### 7.7 What this changes in MESSAGING.md

- §7.4's key plans are superseded for `AskUserQuestion` and `ExitPlanMode`. `ReadMenuSnapshot` / `MenuSnapshot.h`
  are not needed for them.
- D9's recommendation becomes this hook.
- P0-1 is not needed for these two tools. P0-3 is answered by §1–§2. P0-22 is answered by §4 B: a mod's
  `{result}` cannot approve a plan.

## 8. Decisions (open; the recommendation first)

| # | Decision | Recommended | Alternative |
|---|---|---|---|
| **I1** | Answer questions and plans from Telegram | **Yes, through a `PermissionRequest` hook** (this document; replaces D9's key plans) | key plans (MESSAGING.md §7.4); leave them to the PC |
| **I2** | The helper | a native exe (the CLI in a hook mode), copied into the profile | the PowerShell forwarder with a duplex pipe (tens of MB while it waits) |
| **I3** | The plan buttons | Approve (the pre-plan mode) · Approve · ask before edits · Keep planning… · Stop | mirror the PC's rows exactly (bypass / auto-accept / manual) |
| **I4** | A text message in the topic while a question or plan is open | a REPLY to the question / plan message answers it ("Other" / feedback); any other message stays a queued prompt (D30) | every message answers the open dialog |
| **I5** | "Approve in a fresh session" (Claude Code's clear-context option, the most used on this machine until it became opt-in) | not offered (Claude Code hides it by default now) | offered, carried out by Agentmaster: deny + interrupt, then `/clear` and the "Implement the following plan:" first message |
| **I6** | Generic permission prompts from the phone | later, off by default, only for sessions not in bypass | never |
| **I7** | A subagent's question | never held (the helper answers at once) | answerable too; its dialog then waits for the phone before the PC sees it |
| **I8** | A cue at the PC | none: the dialog is unchanged and the first answer wins | "📱 answerable from Telegram" on the tab badge |

## 9. Before it is built

- **Pure tests (m5):**
  - the answer composer: single, multi-select with the quoting rule, "Other", `response`, `(notes only)`;
  - the plan decisions;
  - the validation against the stored input;
  - the request parser (every field; `agent_id`; an unknown tool);
  - the helper's output (exactly one JSON object, or nothing);
  - the open-dialog table's transitions.
- **The pipe:** a duplex round trip; a broken pipe; two helpers at once.
- **Live, through the app and the dev bot** (the four probe scenarios, then the rest):
  - from the phone: a single answer, a multi-select answer, an "Other" answer, a reply instead;
  - the PC first: the message is edited and the helper is gone;
  - a plan approved: the mode is back;
  - keep planning, then approve the revision;
  - Esc at the PC;
  - the app quit with a dialog open (the dialog stays usable);
  - Telegram off (no wait);
  - a subagent's question (not delayed);
  - a claude older than 2.1.139 (no ask hook).
- **To measure:** the helper's start time and its memory while it waits.
- **Re-check on every Claude Code upgrade** that changes the hooks or these tools: run
  `tools/interactive-tools-probe/run_probe.py` (four scenarios, a few Haiku turns). If `V_r` ever drops a
  tool, the hook's `allow` is ignored, the dialog stays with the PC, and Agentmaster should fall back to
  show-only.

## 10. Sources

**The Claude Code 2.1.291 bundle** (`~/.local/bin/claude.exe`, a Bun single-file binary). Its plain JS chunks
start at about byte 200,000,000. Minified names are chunk-local and change with every release: search by the
quoted strings (`tools/interactive-tools-probe/grepbin.py`). Byte offsets in 2.1.291:

| Function | Name | Byte offset |
|---|---|---|
| `executePermissionRequestHooks` | `ABe` | 217,614,218 |
| the runner's `runHooks` | | 235,984,685 |
| the dialog-vs-hooks race | `zo` | 236,017,181 |
| the hook-answer set | `V_r` / `f5n` | 217,906,352 |
| the re-check | `mC` | 217,908,724 |
| the default hook timeout | `zl = 600000` | 217,474,842 |
| a command hook's timeout | `wn` | 217,530,299 |
| the hook command schema (`timeout: positive().optional()`) | | 207,214,649 |
| `ExitPlanMode` | | 222,337,232 |
| its dialog's options | `u9e` / `g9e` | 237,417,237 |
| the clear-context restart | | 237,424,220 |
| `EnterPlanMode` | | 222,585,949 |
| `ProposeGoal` | | 239,912,042 |
| the `AskUserQuestion` chunk | | 222,615,275–222,635,325 |
| "Chat about this" | `R4e` | 237,384,830 |
| the rejection texts | | 218,877,401 |
| `cancelAndAbort` | | 235,984,349 |
| the 6 s permission Notification | | 233,378,382 |
| the hook payload's base fields | `yd` | 217,515,842 |

**Other sources:**
- The typings Claude Code writes beside a mod: `~/.claude/mods/<mod>/.claude-plugin/types/claude-code-tools/index.d.ts`
  (the tools' inputs and outputs) and `…/claude-code/index.d.ts` (hooks).
- `tools/interactive-tools-probe/`: the probe (`run_probe.py` + `perm_hook.py`) and the corpus survey
  (`survey_interactive.py`).
- MESSAGING.md §6.3, §7.4, §7.5, §18 (D9, D16, D30), §19, §21.2; MOD_LINK.md §4.4; HOOKS.md (hook invocation,
  the blocked-on-user belts).
