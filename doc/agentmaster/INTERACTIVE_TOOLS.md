# Interactive tools — AskUserQuestion with previews and notes, answered from Telegram

> **2026-10-19 · BUILT the same day; in dev since 0.0.1.31, not in production** (§10: what was built, where it differs from the design of
> §6–§7, and what was checked — the m5 suite, `cl /Zs` with the build's flags, and a LIVE end-to-end run of the shipped hook
> against Claude Code 2.1.291). The topic's side is [`MESSAGING.md`](MESSAGING.md) §21.30. Read in the Claude Code 2.1.291
> bundle and **measured on it**: a live ConPTY probe of seven scenarios (§5) and a survey of every transcript on this
> machine (§2). The scripts are in `tools/interactive-tools-probe/`. It answers the question half of
> [`MESSAGING.md`](MESSAGING.md) §7.4 (decision D9) and its probes P0-1, P0-3 and P0-22.
>
> **Scope, set by the user the same day:** "Forget all about the plan or propose goal. I meant AskUserQuestion
> with preview as it lets you to show how the choice/option will be used and lets use add note." This document
> covers `AskUserQuestion`, above all its **previews** and its **notes**. Plan approval (`ExitPlanMode`),
> `EnterPlanMode` and `ProposeGoal` are out of scope. What was read and measured on them is in this file's first
> version (commit 2c896eea1), and the probe still runs its two plan scenarios with `--with-plans`.

## 0. Summary

- **A preview question** is a single-select question in which at least one option carries a `preview`. Claude
  Code's tool description tells the model to use previews "when presenting concrete artifacts that users need to
  visually compare": ASCII mockups, code snippets, diagram variations, configuration examples (§1.1).
- **The PC shows it side by side** (§1.3):
  - the option labels on the left;
  - on the right, the preview of the **focused** option, drawn as Markdown in a monospace box;
  - under the box, a **Notes** field ("press n to add notes").

  ↑/↓ or a digit moves the focus; Enter picks the focused option.
- **A note belongs to the question, not to an option** (§1.6):
  - the PC sends it as `annotations[question].notes`;
  - a pick of a previewed option always sends that option's preview back as `annotations[question].preview`,
    with or without a note;
  - a note without a pick sends the answer `"(notes only)"`.

  The model then reads `"Q"="Center" selected preview:\n<preview> notes: <note>` (§1.7).
- **How it is used on this machine** (§2, real sessions only): 172 preview questions in 133 calls; 85 of them
  share a call with other questions. Of the 151 that were answered:
  - 95 with a pick, 15 with a pick and a note;
  - **39 with a note and no pick**: the user turned every option down and wrote what they wanted instead;
  - 2 with neither.

  So a third of the answers carry a note. Notes are short: median 55 characters, max 1,373.
- **Today's Telegram mirror** (§3) shows a question as `❓` plus the first question's option labels. It shows no
  preview and no description, and it drops the note: a notes-only answer arrives as the literal text
  "(notes only)".
- **The way in** (§4) is a **`PermissionRequest` hook**. Claude Code runs it beside the open dialog, and the first
  answer wins. Its `allow` + `updatedInput` carries the answers and the annotations.
- **Proven live** (§5), every scenario checked on the transcript:
  - the PC's own answers were recorded: a pick with a note, a note without a pick, a plain pick;
  - the hook reproduced the first two, and the model read exactly the text the PC's answers give;
  - when the PC answers first, its answer wins, and the hook's late answer is dropped.
- **The design** (§6):
  - every option's preview drawn as ONE picture by the existing Screen Image renderer (the PC shows one preview at
    a time; a phone can show all of them and zoom);
  - a question message under the picture: one button per option, **[📝 Note…]** (the note is a reply),
    **[💬 Chat about this]**;
  - the answer composed exactly as the PC composes it (§6.4), and the message edited to the outcome.

  A smaller first step needs no hook: the mirror's answer line shows the note (§6.7).
- **Decisions** Q1–Q10 (§7) were the proposal. Three of them needed a look at the user's phone first (§8).
- **Built** (§10, the user's request of 2026-10-19: "Kindly close the gap, enabling telegram picking a choice, including
  the user manual text input …"): the hook is a PowerShell helper (`<profile>\agentmaster-ask.ps1`) wired while the bot is
  on; the engine's ask pipe and question registry; the QUESTION CARD — ONE managed message per dialog with option buttons,
  the user's own answer and the note through a ForceReply prompt, several questions on one card, the previews inline and as
  a picture on demand, "Chat about this" — answering exactly as the PC answers (proven live, §10.4); and S0 (the notes in
  every answer line).

## 1. AskUserQuestion with previews and notes (Claude Code 2.1.291)

### 1.1 What the model sends, and what it is told

- **The input** (the bundled typings, `claude-code-tools/index.d.ts`):
  - `questions`: 1–4 questions, each with:
    - `question`: the text;
    - `header`: a chip, at most 12 characters recommended;
    - `options`: 2–4 options, each `{label, description, preview?}`;
    - `multiSelect`.
  - `metadata.source`, optional.
- **The preview guidance in the tool's description** (`M8o.markdown`, verbatim):

  > Use the optional `preview` field on options when presenting concrete artifacts that users need to visually compare:
  > - ASCII mockups of UI layouts or components
  > - Code snippets showing different implementations
  > - Diagram variations
  > - Configuration examples
  >
  > Preview content is rendered as markdown in a monospace box. Multi-line text with newlines is supported. When
  > any option has a preview, the UI switches to a side-by-side layout with a vertical option list on the left and
  > preview on the right. Do not use previews for simple preference questions where labels and descriptions
  > suffice. Note: previews are only supported for single-select questions (not multiSelect).

- **An HTML form exists.** `CLAUDE_CODE_QUESTION_PREVIEW_FORMAT=html` tells the model to send "a self-contained
  HTML fragment". Claude Code then refuses a preview that holds `<html>`, `<body>`, `<!DOCTYPE>`, `<script>` or
  `<style>`, or no tag at all. Agentmaster's launches do not set the variable, so its sessions use Markdown.
- **The description also says** "Users will always be able to select 'Other' to provide custom text input". In
  the preview layout that free text is the Notes field: there is no "Type something." row (§1.3).

### 1.2 Which questions show previews

- **The rule** (`P4e`): a question gets the preview layout when it is single-select and at least one option has a
  preview after the mapping below. In screen-reader mode the plain list is shown instead, and no notes are sent.
- **Each option's preview is mapped** (`G5`, with `FXe = 2000`):
  - longer than 2,000 characters → **withheld**. The box shows "(preview cannot be shown in full — compare the
    option labels and descriptions instead)";
  - nothing visible (only whitespace or invisible characters) → no preview;
  - Claude Code's own truncation marker (`… [+N graphemes]`), or a run of more than 8 stacked combining or
    zero-width marks → withheld;
  - otherwise **full**: the text, with each invisible format character replaced by U+FFFD (`zet` = `Nl`). That
    covers Unicode `Cf` and the default-ignorable code points, except ZWJ / ZWNJ, LRM / RLM, ALM, the variation
    selectors and the Mongolian selectors.
- **A multi-select question's previews are never shown.** This machine has 1 such question.

### 1.3 What the PC shows (the probe's screen, 160 × 50)

```
 ☐ Layout

Which header layout do you prefer?

  1. Left                         ┌──────────────────────────────────────────┐
❯ 2. Center                       │ +--------------+                         │
  3. Right                        │ |   [LOGO]     |                         │
                                  │ +--------------+                         │
                                  └──────────────────────────────────────────┘

                                  Notes: use rounded corners

──────────────────────────────────────────────────────────────────────────────
  Chat about this

Enter to select · ↑/↓ to navigate · n to add notes · ctrl+g to edit in <editor> · Esc to cancel
```

- **The option list** is 30 columns wide and holds the labels only. **Descriptions are not shown in this
  layout.** `❯` marks the focus; a green ✔ marks the option already picked (on a question the user came back to).
- **The box shows the focused option's preview** (`A5` → `p5` / `ohe`):
  - it is drawn by Claude Code's terminal Markdown renderer (`wIt`, the `marked` lexer): headings, bold, lists,
    quotes, and code fences with syntax highlighting;
  - its width is the terminal's columns − 34, and a longer line is hard-wrapped;
  - its height is at most the terminal's rows − 26 lines. The rest is cut, with a `─── ✂ ─── N lines hidden` line;
  - an option without a preview shows "No preview available".
- **Notes:** the field reads "press n to add notes" until a note is typed. While the user types, it is a 60-column
  input with the placeholder "Add notes on this design…".
- **"Chat about this"** is a row under the list (↓ from the last option reaches it).
- **The plain list** (a question without previews) is different: it shows the descriptions, a "Type something."
  row for free text, and no Notes field.

### 1.4 The keys, and what each does (`A5`)

| Key | Effect |
|---|---|
| ↑ / ↓, Ctrl+P / Ctrl+N | move the focus; ↓ from the last option focuses "Chat about this" |
| 1–9 | **focus** that option (it does not pick it) |
| Enter | pick the focused option; a one-question call is answered at once |
| n | open the Notes field |
| Enter in Notes | close the field and answer, by the rule below |
| Esc in Notes | close the field; the note is kept |
| Ctrl+G in Notes | edit the note in the external editor |
| Esc | cancel: a rejection that ends the turn |
| Tab / Shift+Tab, → / ← | the next / previous question (several questions) |

**Enter in the Notes field** (`zn`) answers by the first case that holds:
1. the note is not empty and the focus moved since the question opened → pick the focused option, with the note;
2. an option was picked before → answer it again, with the note;
3. the note is not empty → answer `"(notes only)"` (`_0t`).

So "2, n, the note, Enter" picks option 2 with the note (case 1), and "n, the note, Enter" sends the note alone
(case 3). Both were run in the probe (§5, C and F).

### 1.5 Several questions

- **One tab per question** in a header row (each question's `header`, else `Q1`, `Q2`, …). A pick moves on to the
  next question.
- **After the last question, the review screen:**
  - "Review your answers";
  - the warning "You have not answered all questions" while some are open;
  - each answer under its question;
  - "Ready to submit your answers?" with **[Submit answers]** / **[Cancel]**.

  Unanswered questions are allowed.
- **A one-question call has no review:** a pick sends at once.

### 1.6 What the PC sends (`nJ`)

An `allow` whose `updatedInput` is the call's input plus:
- **`answers`**: question → the picked option's label; `"(notes only)"` for a note without a pick.
- **`annotations`**: per preview question, `{preview?, notes?}`:
  - `preview`: the picked option's preview, when its kind is full (§1.2). It is sent **with or without a note**;
  - `notes`: the note, trimmed, when it is not empty;
  - the entry is left out when it has neither, and `annotations` is `{}` when no question has one.
- **Images** pasted into the dialog travel as extra content blocks (`contentBlocks`), outside the input.

### 1.7 What the model reads (the result mapper, verbatim)

- **Each answered or annotated question becomes `"Q"="A"`**, or `"Q"=(no option selected)` for `(notes only)`.
  `selected preview:\n<preview>` and `notes: <note>` follow when the annotations hold them, and the pieces are
  joined with ", ".
- **Any note switches the whole text to the careful form** ("The user answered: … Read the answers carefully …").
  With no note, and only option labels as answers, it is the plain form ("Your questions have been answered: …").
- **The probe's results** (§5), each the exact tool result:

  A pick, no note (G):
  ```
  Your questions have been answered: "Which menu layout do you prefer?"="Left" selected preview:
  +--------------+
  |[LOGO]        |
  +--------------+. You can now continue with these answers in mind.
  ```
  A pick and a note (C, and the hook's D):
  ```
  The user answered: "Which header layout do you prefer?"="Center" selected preview:
  +--------------+
  |   [LOGO]     |
  +--------------+ notes: use rounded corners. Read the answers carefully — they may request clarification, changes, or that you not proceed — and follow what they actually say.
  ```
  A note, no pick (F, and the hook's E):
  ```
  The user answered: "Which logo position do you prefer?"=(no option selected) notes: neither, put the logo in the footer. Read the answers carefully — they may request clarification, changes, or that you not proceed — and follow what they actually say.
  ```
  Nothing at all: `The user did not answer the questions.`
- **"Chat about this" is a rejection** (`R4e`), and the turn goes on. The notes travel with it:
  ```
  The user doesn't want to proceed with this tool use. The tool use was rejected (eg. if it was a file edit, the new_string was NOT written to the file). To tell you how to proceed, the user said:
  The user wants to clarify these questions.
      This means they may have additional information, context or questions for you.
      Take their response into account and then reformulate the questions if appropriate.
      Start by asking them what they would like to clarify.

      Questions asked:
  - "<question>"
    Answer: <answer>            (or: (No answer provided))
    User notes: <note>          (preview questions with a note only)
  ```
- **Esc is the STOP rejection**, and the turn ends: "… STOP what you are doing and wait for the user to tell you
  how to proceed."
- **The mapper has more forms** (`response`, `followUp`, the idle auto-continue). The preview layout produces
  none of them.

### 1.8 The bounds of a remote answer

- **Claude Code's own remote cards** (`admitCardAnswer`) may add only `answers`, `annotations`, `response` and
  `followUp`. Each answer is at most 8,192 characters, and all of them together at most 32,768.
- **A hook's answer is not checked by that function.** Agentmaster applies the same limits itself (§6.4). A
  Telegram message is at most 4,096 characters, so a note typed on the phone always fits.

## 2. How previews and notes are used (the corpus, 2026-10-19 09:40)

**Method:** `tools/interactive-tools-probe/survey_interactive.py --real-only`. It reads every transcript under
`~/.claude/projects`, leaves out subagent files, and leaves out the 4 sessions that worked in a folder under
`%TEMP%` (this probe and the ConPTY driver's self-tests).

**AskUserQuestion overall:** 1,055 calls in 601 transcripts. The median wait was 2.6 min and the p90 34.8 min;
114 calls waited more than 30 min and 54 more than 2 h (of the 1,040 with a result).

**Preview questions:**
- 462 options carry a preview: median 232 characters, max 1,298. None is over 2,000, so none was withheld.
- 172 preview questions in 133 calls. **85 of them share a call with other questions.**
- 1 multi-select question has previews (never shown).

| Outcome of the 172 preview questions | Count |
|---|---|
| a pick | 95 |
| a pick + a note | 15 |
| **a note, no pick** (`(notes only)`) | **39** |
| neither an option nor a note | 2 |
| no structured answer (rejected, cut off, still open) | 21 |

- **Every pick of a previewed option (101 of 101) echoed that option's preview** in `annotations.preview`.
- **54 notes:** median 55 characters, p90 163, max 1,373. They read like conditions on the pick ("only if …"),
  changes to it ("and remove …"), or a different design instead of every option ("How about a 'page' with back
  button that covers entire window…").

**What the previews hold** (for choosing how a phone shows them):

| | Median | p90 | Max |
|---|---|---|---|
| Lines per preview | 7 | 14 | 39 |
| Widest line (terminal cells) | 52 | 70 | 113 |
| A question's previews together (characters) | 644 | 1,309 | 3,731 |
| One message with the question, every option and every preview | 1,416 | 2,296 | 4,575 (1 of 172 over 4,096) |

| Every preview line of the question fits | 32 cells | 40 | 48 | 56 | 64 | 80 |
|---|---|---|---|---|---|---|
| Share of the 172 questions | 2% | 8% | 22% | 42% | 69% | 92% |

- **Content**, by preview (of 462): box drawing or block elements 84, Markdown headings 39, Markdown tables 6,
  code fences 6, HTML-looking tags 3. The rest are plain-text mockups (`+---+`, `|`, `[ ]`).
- **So the previews are mostly monospace pictures, and most are wider than a phone's monospace line.** How wide a
  phone's line is must be measured on the user's phone (§8).

## 3. What Agentmaster did with a preview question before §10

- **State:** `NeedsApproval` comes from three belts (the `Notification` 6 s after the dialog opens, the scanner's
  `recon-block`, the heartbeat's `waiting`). The question guard keeps queued prompts Pending.
- **The Telegram mirror** (`MessagingModels.h`):
  - The `tool_use` becomes a `Question` item: `❓ <header>: <question>` for every question, then the FIRST
    question's option labels, numbered. **No descriptions, no previews, no buttons.**
  - The answer becomes an `Answer` item made from `toolUseResult.answers` alone. **The annotations are never
    read**, so a note never reaches Telegram, and a notes-only answer is posted as the text "(notes only)". That
    was 39 of the 151 preview answers on this machine.
  - A message typed in the topic is a queued prompt (D30). It waits until the dialog is gone.
- **MESSAGING.md §7.4** (the verified key plans of D9) is not built. For `AskUserQuestion` this document replaces
  it.
- ⚠ **The transcript can write a pending question's `tool_use` line late**, 10+ minutes (CLAUDE.md, the
  blocked-on-user belts; in the probe it was written at once). The hook gets the question at once (§4.2).

## 4. The way in: a PermissionRequest hook (Claude Code 2.1.291)

### 4.1 When it runs, and the other routes

- **In the main session it runs IN PARALLEL with the dialog.** The dialog is shown, the hooks start, and the
  first answer from either side wins (`zo`; `claim()`). The dialog stays fully usable while the hook waits. It
  also runs while the dialog is hidden behind a draft in the input box.
- ⚠ **For subagents and teammates the hooks run BEFORE the dialog, and the dialog waits for them**
  (`awaitAutomatedChecksBeforeDialog`). A long-waiting hook would hide a subagent's question from the PC, so the
  helper answers at once when the payload carries `agent_id` (§6.2).
- **No hooks run** for a person-only ask or a remote ("served") call.
- **The other routes:**

| Route | Verdict |
|---|---|
| **A `PermissionRequest` hook** | **recommended; measured (§5)** |
| The session link mod (MOD_LINK.md): a `tool.call` hook returns `{result}` in place of the call | could answer; nothing built; the mod runtime costs a claude ~85 MB |
| Key plans (MESSAGING.md §7.4): keystrokes into the dialog, checked on the screen | fragile here: a digit only focuses, and whether the focus moved changes what Enter in the Notes field does (§1.4); never measured |
| Claude Code channels | no: the relay skips tools that require user interaction (D16) |
| Claude Code's Remote Control | answers in the Claude app, not in Telegram |

### 4.2 What it receives (measured)

```json
{
  "session_id": "8c7d6854-…", "transcript_path": "…\\8c7d6854-….jsonl", "cwd": "…\\ws",
  "scratchpad_dir": "…\\scratchpad", "prompt_id": "b5329d2c-…", "permission_mode": "bypassPermissions",
  "hook_event_name": "PermissionRequest", "tool_name": "AskUserQuestion",
  "tool_input": { "questions": [ { "question": "Which color do you prefer?", "header": "Color",
                  "options": [ { "label": "Red", "description": "The color red" }, … ], "multiSelect": false } ] }
}
```

- **The previews arrive in `tool_input`**, each option's `preview` as the model wrote it.
- **In a subagent:** also `agent_id` and `agent_type`.
- ⚠ **There is no `tool_use_id`.** `prompt_id` names the user's turn. `session_id` is the current conversation (it
  changes on `/clear`; Agentmaster maps it like any hook).

### 4.3 What it may answer

```json
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow","updatedInput":{…}}}}
{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"deny","message":"…","interrupt":false}}}
```

- **`allow` + `updatedInput` is accepted for `AskUserQuestion`** (the set `V_r` = {ExitPlanMode,
  AskUserQuestion}, `f5n`). For a tool that requires user interaction, an `allow` WITHOUT `updatedInput` is
  ignored, and the dialog stays.
- **The rewritten input goes through the permission re-check** (`mC` with `hookUpdatedInput`). A deny or ask rule
  can still override it.
- **The PC's conversation shows who answered.** Claude Code records the decision as
  `decisionReason: {type: "hook", hookName: "PermissionRequest"}`, and the PC prints "⎿ Allowed by
  PermissionRequest hook" under the answer (the probe's screen).
- **A `deny` makes its message the tool result, verbatim** (measured: the transcript's `tool_result` content is
  exactly the message, with `is_error: true`). Without a message it reads "Permission denied by PermissionRequest
  hook". With `interrupt: true` the turn is aborted, like Esc. So a phone's "Chat about this" sends Claude Code's
  own rejection text (§1.7) as the message.

### 4.4 Timeout and lifetime

- **A command hook's `timeout` is `positive().optional()` in the schema: no upper limit.** The run applies
  `timeout × 1000` ms (`wn`); the default `zl` is 600,000 ms (10 min). The JavaScript timer caps any delay at
  2^31−1 ms (24.8 days); keep the timeout well under that (§6.2: 24 h).
- ⚠ **When the dialog is answered at the PC, Claude Code does NOT stop the hook.** Its late answer is dropped
  (`if (!L || !A()) return`). Measured: the hook process was alive 3 s after the PC's answer, and exited only when
  it answered on its own. Only the turn's abort signal (Esc; read in the code, not measured) or its own timeout
  ends it early.

### 4.5 What the transcript records

- **A hook's decision is an attachment line written just before the tool result:**
  `{"type":"attachment","attachment":{"type":"hook_permission_decision","decision":"allow"|"deny","toolUseID":"toolu_…","hookEvent":"PermissionRequest"}}`.
  It is the on-disk proof that the answer came from a hook, and it carries the `tool_use` id that the payload
  lacks. In the probe, exactly the three hook answers (A, D, E) wrote one.
- **A PC answer writes no such line.** Its `toolUseResult` carries `answers` and `annotations` (`{}` when empty).

### 4.6 Traps

- ⚠ **A `PreToolUse` / `PermissionRequest` hook entry that cannot be loaded makes Claude Code skip the
  permissions declared in the same settings file** (a fatal validation note: "nothing it sits in is applied until
  the entry is fixed or removed"). Agentmaster's `hooks-settings.json` must stay exactly valid.
- **Output exactly one JSON object, or nothing.** Plain text or two documents are ignored as non-JSON output. An
  exit code other than 0 or 2 is a hook error; 2 is a blocking error.

## 5. The probe (2026-10-19)

**How it was run** (`tools/interactive-tools-probe/run_probe.py`, 09:32):
- The `claude-wrap-ConPTY` driver ran a real `claude.exe` 2.1.291 TUI (Haiku 4.5, 160 × 50) in a throwaway
  folder, with `--dangerously-skip-permissions` like Agentmaster's launches.
- `CLAUDE_CODE_SANDBOXED=1` skipped the workspace-trust dialog without writing `~/.claude.json`.
- The `--settings` file's only hook is a `PermissionRequest` command hook (matcher `AskUserQuestion`,
  `timeout: 600`). That hook (`perm_hook.py`) logs its payload and long-polls for a reply file, as Agentmaster's
  helper would.
- For C–G the model was told to ask one single-select question with three options, Left, Center and Right, each
  with a three-line ASCII mockup as its `preview`. It did so every time.
- Every verdict was read from the transcript.

| | Scenario | Result |
|---|---|---|
| A | The hook answers a plain question (`answers` only) | ✅ `Your questions have been answered: "Which color do you prefer?"="Blue". …`; the model replied `CHOSEN=Blue` |
| B | The PC answers first (Enter on option 1) | ✅ `…"Apple"`; the hook was still alive 3 s later; its later "Pear" was dropped |
| C | **The PC:** key 2 (focus), n, the note "use rounded corners", Enter | ✅ the PC sent `answers` = Center and `annotations` = {Center's mockup as `preview`, `notes`}; the text is the careful form of §1.7 |
| D | **The hook** sends that same shape for another question | ✅ the same text form, exactly as the mapper builds it; the stored `toolUseResult` holds the hook's answers and annotations; the model replied `LAYOUT=Center` |
| E | **The hook** sends `(notes only)` + the note "none of these, make it a tab strip" | ✅ `"…"=(no option selected) notes: none of these, make it a tab strip. Read the answers carefully …`; the model offered to design a tab strip |
| F | **The PC:** n, the note, Enter (the focus not moved) | ✅ the PC sent `"(notes only)"` and `{notes}` with no preview: the form E reproduced |
| G | **The PC:** Enter on the first option, no note | ✅ the PC sent the label AND `{preview}`; the plain form with `selected preview:` |

- **The evidence** lives in the probe's folder: `C-pc-answer.json`, `F-pc-answer.json`, `G-pc-answer.json` (the
  PC's own answers) and `D-hook-answer.json`, `E-hook-answer.json` (the hook's). Each holds the call's input, the
  stored `toolUseResult` and the text. They become the composer's golden tests (§8).
- **Timing** (the transcript's times against the hook log): the hook started 0.10–0.25 s after the `tool_use`
  line, Python start included. The result was written 0.011–0.014 s after the hook answered.
- **Earlier the same day**, in two runs: A, B and the two plan scenarios (now P1 and P2, run only with
  `--with-plans`) passed both times, with the same timing.

## 6. The design (the proposal; what was built, and how it differs: §10)

### 6.1 The pieces

```
claude.exe ──PermissionRequest (exec form, matcher AskUserQuestion, timeout 86400)──▶ agentmaster ask helper
                                                                                          │ one JSON line each way
                                                                                          ▼
                                          \\.\pipe\agentmaster.<pid>.ask (duplex) ── Engine: open-question table
                                                                                          │
                                          Messaging service ◀── a preview picture + a question message with buttons
                                          a tap / a reply ──▶ the composed answer ──▶ the helper prints it ──▶ claude
```

### 6.2 The helper

- **One more hook in `hooks-settings.json`**, written by the same three writers as today (engine init, every
  spawn, the cog's Save), in exec form like the forwarder:
  ```json
  "PermissionRequest": [ { "matcher": "AskUserQuestion",
      "hooks": [ { "type": "command", "command": "<profile>\\bin\\agentmaster-ask.exe", "args": [], "timeout": 86400 } ] } ]
  ```
  - Only for a claude ≥ 2.1.139 (exec form). An older claude gets no ask hook and keeps today's show-only
    behavior.
  - The 24 h timeout is long enough for a phone answer. After it, the dialog stays with the PC.
- **A native exe, not the PowerShell forwarder** (decision Q2):
  - It waits as long as the dialog is open. A waiting Windows PowerShell 5.1 holds tens of MB and starts in
    0.3–0.7 s warm (HOOKS.md); a native helper holds a few MB.
  - It can be the CLI binary in a hook mode (`agentmaster-cli.exe hook-ask`).
  - It is copied into the profile, the way the GUI copies the session database service into
    `<profile>\sessiondb\bin`, so the path in the settings file survives a layout swap.
- **What it does:**
  1. Read the payload from stdin.
  2. Print nothing and exit 0 at once when any of these holds:
     - `agent_id` is present (a subagent's dialog waits for its hooks, §4.1);
     - the tool is not `AskUserQuestion`;
     - Agentmaster's pipe cannot be found (the forwarder's discovery: `CCMGR_HOOK_PIPE`, else `bridge.json`).
  3. Connect to `\\.\pipe\agentmaster.<pid>.ask`, and write one request line: the payload, the helper's pid and
     its parent claude's pid.
  4. Block on one reply line. Print it as the hook output when it is a decision; print nothing when it is `{}`.
     Exit 0.
- **It ends by itself** when the reply arrives, when the pipe breaks (the app quit), when its parent claude exits
  (it holds the claude's process handle; Claude Code does not end it, §4.4), or at the 24 h deadline.
- **It fails open everywhere:** any doubt means no output and exit 0, and the dialog stays with the PC.

### 6.3 The engine

- **The ask server** is a duplex pipe beside the hook pipe (`HooksBridge`; today's hook pipe is
  `PIPE_ACCESS_INBOUND`). It keeps one connection per waiting helper, with the same access as the hook pipe. It
  answers `{}` at once when no remote surface is connected (Telegram off or not connected), when the session is not
  managed, or when the request is malformed.
- **The open-question table** (`PendingQuestion`, transient, never persisted) holds per entry:
  - a nonce, the registry session (mapped from `session_id` like a hook), `prompt_id` and the open time;
  - the call's input (the questions, their options and previews);
  - per question: the pick (or the picks, multi-select), the note, an "Other" text, and the Telegram message ids
    (its picture and its question message);
  - the waiting connection;
  - a state: open / answering / closed. When closed: the outcome and its origin (phone / PC / Esc / exit /
    timeout).
- **A new state signal:** a request is an exact "a question is open now". It can set `NeedsApproval` at once,
  instead of the `Notification` 6 s later. It is a fourth belt beside today's three, never the only one.
- **Answering from the phone:**
  1. The engine composes the answer (§6.4) and validates it against the stored input: the question texts and
     labels are unchanged; an answer is at most 8,192 characters, and all of them together at most 32,768
     (§1.8).
  2. It writes the decision to the connection and logs `[ask] <sid8> answered from telegram: …`.
  3. It verifies: within ~10 s the transcript must hold a `hook_permission_decision` and the tool's result.
     Otherwise the message is edited to "not confirmed — look at the PC".
- **Answered elsewhere.** Any of these closes the entry with `{}`, and the helper exits:
  - the transcript shows the tool's result, or a `hook_permission_decision`, for the session after the open;
  - the heartbeat leaves `waiting`;
  - the turn is interrupted, the claude exits, or the tab closes.

  The phone's messages are then edited to the answer given, read from the transcript's `toolUseResult`,
  annotations included.
- **Pairing with the mirror:** the request has no `tool_use` id. The open entry pairs with the transcript's
  `tool_use` by session, tool and content; the `hook_permission_decision` line confirms the pairing afterwards.

### 6.4 The answer composer (pure)

- **`ComposeQuestionAnswer(input, choices)` → the `updatedInput`**, built as `nJ` builds it (§1.6):
  - a pick → `answers[q]` = the label. A multi-select pick → the labels joined with ", ", a label that holds ", "
    or `"` JSON-quoted (`Tct`). A typed "Other" (a question without previews) → the text;
  - a preview question → `annotations[q]` = {`preview`: the picked option's preview when `PreviewKind` says full,
    sanitized as Claude Code sanitizes it; `notes`: the note, trimmed, when not empty}. The entry is left out when
    it has neither;
  - a note without a pick → `answers[q]` = `"(notes only)"`;
  - `annotations` = `{}` when no question has an entry (the PC sends it so).
- **`PreviewKind(text)`** ports the mapping of §1.2: none / withheld / full (sanitized). The invisible-character
  test needs ICU, which `ClaudeInvisibleChars` already links.
- **`ComposeChatAboutThis(input, choices)` → the `deny` message:** the rejection prefix plus `R4e`'s text, its
  whitespace included, with each question's answer and note (§1.7).
- **Tests:** the probe's JSON files are the goldens (§8). The PC's C, F and G must come out of the composer byte
  for byte from the same picks and notes.

### 6.5 Telegram

**The previews: one picture of all of them** (recommended; Q3).
- **Drawn from the call's input, never from the PC's screen.** The PC shows one option at a time, and moving its
  focus would change what its Enter does (§1.4).
- **Each option's preview as a framed monospace block** under its number and label, stacked:
  ```
  ┌ 1 · Left ───────────────────┐
  │ +--------------+            │
  │ |[LOGO]        |            │
  │ +--------------+            │
  └─────────────────────────────┘
  ┌ 2 · Center ─────────────────┐
  │ +--------------+            │
  │ |   [LOGO]     |            │
  │ +--------------+            │
  └─────────────────────────────┘
  ┌ 3 · Right ──────────────────┐
  │ …                           │
  ```
- **Built as a `ScreenSnapshot`** (one row per line, the dark terminal's default colors) and drawn by
  `RenderScreenSnapshotPng` (ScreenImage.h; the card's 🖼 Screen Image, MESSAGING.md §21.24, ~90–185 ms for a full
  screen, on a worker thread). The widest preview here (113 cells) is ~1,800 px wide at 16 px a cell, inside the
  renderer's 2,400 px.
- **Kept as the PC words it:** a withheld preview shows Claude Code's placeholder text, and an option without a
  preview says "No preview available".
- **Sent with `sendPhoto`**, silent (the question message rings), with the caption `Previews · <header>`. A sheet
  taller than a threshold (to measure on the phone, §8) goes as an album of one picture per option.

**The question message**, one per question, sent right after its picture, with sound:
```
❓ Layout · Which header layout do you prefer?
1. Left — Logo on the left
2. Center — Logo in the middle
3. Right — Logo on the right
📝 Note: —
[ 1 · Left ] [ 2 · Center ] [ 3 · Right ]
[ 📝 Note… ] [ 💬 Chat about this ]
```
- **The descriptions are shown** (the PC's preview layout hides them; on a phone they are cheap context).
- **The option buttons:** two to a row, or one to a row when a label is long.
- **A tap on an option** answers a one-question call at once, with the note if one is set, like the PC's Enter
  (Q6). With several questions it ticks the pick (✅) and waits for Send.
- **[📝 Note…]** posts "📝 Note for “Layout”: reply to this message" with a ForceReply (placeholder "Note on this
  design…"). The reply's text becomes the question's note, the prompt is deleted, and the question message is
  edited:
  ```
  📝 Note: use rounded corners
  [ 1 · Left + note ] [ 2 · Center + note ] [ 3 · Right + note ]
  [ 📝 Send only the note ] [ ✏️ Change ] [ ✖ Remove ]
  [ 💬 Chat about this ]
  ```
  A Telegram reply to the question message sets the note the same way (Q5). Any other message in the topic stays
  a queued prompt (D30).
- **[📝 Send only the note]** answers `(notes only)` with the note: a quarter of the preview answers on this
  machine.
- **[💬 Chat about this]** sends Claude Code's own rejection (§1.7), with the picks and notes so far.
- **Several questions** (half of the preview questions): one picture and one question message per question, and
  the last message carries **[📨 Send answers (k of n)]**, the PC's review screen. Unanswered questions are
  allowed (Q7).
- **The outcome:** the question message is edited and its buttons removed:
  - `✅ Center · 📝 use rounded corners — from the phone, 14:02`;
  - `✅ Left — answered at the PC, 14:03` (read from the transcript, annotations included);
  - `📝 neither, put the logo in the footer — no option, answered at the PC`;
  - `💬 asked to clarify`, `⏹ cancelled at the PC`, `⌛ no answer (the session ended)`.
- **Buttons are action tokens** (MESSAGING.md §7.5). They bind the open entry's nonce; a stale tap answers
  "already answered: …".

**A question without previews** (every question of 922 of the 1,055 calls) takes the same flow without a
picture. The PC's plain list has "Type something." instead of Notes, so the phone offers **[✍️ Other…]** (a reply
whose text becomes the answer) instead of [📝 Note…]. Multi-select options are toggles (☐ / ☑).

### 6.6 Lifecycle

| Event | Engine | Helper | Telegram |
|---|---|---|---|
| The dialog opens | entry open; `NeedsApproval` | waits | the picture + the question message |
| A note on the phone | stored on the entry, per question | waits | the question message edited ("📝 Note: …") |
| A tap on the phone | answer composed and written; entry answering → closed on the transcript's proof | prints the decision, exits | edited to the answer |
| The PC answers first | the transcript / heartbeat shows it → `{}` | exits | edited to the PC's answer, with its note |
| Esc at the PC | the rejection in the transcript closes the entry with `{}` | ended by the turn's abort signal (read in the code, not measured), else by the `{}` | "cancelled at the PC" |
| claude exits, the tab closes | `{}` / entry dropped | exits (parent gone or pipe) | "no answer (the session ended)" |
| The app quits | the pipe breaks | exits; **the dialog stays usable at the PC** | stale buttons answer "Agentmaster is not running" (after the restart: "already closed") |
| Telegram is not connected | `{}` at once | exits within ~0.1 s | — |
| A subagent's question | never reaches the engine | `{}` at once (§4.1) | today's show-only item |
| 24 h pass | — | exits at its deadline; the dialog stays | edited to "no longer answerable from here" |

- ⚠ **A note half-typed at the PC is lost when the phone answers first.** The first answer wins; that is Claude
  Code's race, not Agentmaster's.

### 6.7 A first step that needs no hook (S0)

- **The answer line shows the note.** The mirror's `Answer` item reads `annotations` too:
  `Center · 📝 use rounded corners`, and `(notes only)` becomes `📝 neither, put the logo in the footer`.
- **The question item lists the descriptions**, and says how many options carry a preview.
- **Size:** pure `MessagingModels.h` changes with m5 tests. No hook, no new process, no answering. Today it would
  turn a quarter of this machine's preview answers from "(notes only)" into the user's own words.

### 6.8 What this changes in MESSAGING.md

- §7.4's key plans are superseded for `AskUserQuestion`. `ReadMenuSnapshot` / `MenuSnapshot.h` are not needed for
  it.
- D9's recommendation for questions becomes this hook. Plans are not designed here (the scope above).
- P0-1 is not needed for questions; P0-3 is answered by §1–§2; P0-22 is answered by §4.1: the hook needs no mod.

## 7. Decisions (the proposal, the recommendation first; what the build chose: §10.3)

| # | Decision | Recommended | Alternative |
|---|---|---|---|
| **Q1** | Answer `AskUserQuestion` from Telegram | **yes, through a `PermissionRequest` hook** (this document; it replaces D9's key plans for questions) | key plans (MESSAGING.md §7.4); leave questions to the PC |
| **Q2** | The helper | a native exe (the CLI in a hook mode), copied into the profile | the PowerShell forwarder with a duplex pipe (tens of MB while it waits) |
| **Q3** | How the previews look on the phone | **one picture of every option's preview** (a sheet, drawn by the Screen Image renderer), then the question message | a carousel: one photo message with the focused option's preview, where ◀ / ▶ replace the photo (the PC's own one-at-a-time model); text only: a monospace block per option (92% of the previews need more than 40 cells, so most would wrap; §8) |
| **Q4** | Markdown in a preview picture | the raw text, as the 84 box-drawing mockups need (Markdown markers stay visible in the 39 with headings) | styled like the PC's terminal renderer (bold headings, highlighted code), e.g. through `MarkdownBlocks` |
| **Q5** | How a note is written | [📝 Note…] (a ForceReply prompt) and a Telegram reply to the question message | only the button; any message in the topic while a question is open (that would change D30) |
| **Q6** | One question: when a tap answers | at once, with the note if one is set (the PC's Enter) | every answer needs a [Send] tap |
| **Q7** | Several questions | one picture + one question message per question, and [📨 Send answers (k of n)] on the last (the PC's review screen) | one message edited from question to question (the PC's tabs) |
| **Q8** | [💬 Chat about this] on the phone | offered, as Claude Code's own rejection text with the picks and notes so far | not offered |
| **Q9** | A subagent's question | never held: the helper answers at once, and the PC answers | answerable too; its dialog then waits for the phone before the PC sees it |
| **Q10** | S0 (§6.7) before the hook | yes: small, independent, and it fixes the lost notes now | only together with the hook |

**Not a decision:** a cue at the PC. Claude Code already prints "Allowed by PermissionRequest hook" under a
phone's answer.

## 8. Before it is built (the plan; done and still owed: §10.4 / §10.5)

- **Phone checks** (the user's phone, through the dev bot; a message is sent only with the user's go-ahead):
  - how a monospace block of 52, 70 and 113 cells shows: wrapped, or scrolled sideways;
  - the sheet picture with 3 and 4 options and a 39-line preview: readable without zoom? where the album threshold
    sits;
  - the [📝 Note…] ForceReply prompt and a plain reply to the question message;
  - the buttons with long labels (up to 112 characters on this machine).
- **Pure tests (m5):**
  - the composer against the probe's goldens (C, F, G from the PC; D, E from the hook), and the multi-select
    quoting rule, "Other", and an unanswered question in a call of several;
  - `PreviewKind`: 2,000 and 2,001 characters, whitespace only, an invisible format character (→ U+FFFD), the
    truncation marker, 9 stacked marks;
  - `ComposeChatAboutThis` against Claude Code's text, whitespace included;
  - the bounds (8,192 / 32,768) and the validation against the stored input;
  - the request parser (every field; `agent_id`; another tool) and the helper's output (one JSON object, or
    nothing);
  - the sheet builder: a 113-cell preview, wide CJK characters, a 39-line preview, a withheld one;
  - S0's answer line: a pick with a note, `(notes only)`, several questions.
- **Live, through the app and the dev bot:**
  - from the phone: a pick, a pick with a note, a note alone, several questions with [📨 Send answers], "Chat
    about this", a question without previews with "Other";
  - the PC first (the messages are edited and the helper is gone), a note half-typed at the PC;
  - Esc at the PC; the app quit with a question open (the dialog stays usable); Telegram off (no wait);
  - a subagent's question (not delayed); a claude older than 2.1.139 (no ask hook).
- **To measure:** the helper's start time and its memory while it waits.
- **Re-check on every Claude Code upgrade** that changes hooks or `AskUserQuestion`: run
  `tools/interactive-tools-probe/run_probe.py` (seven scenarios, ~1.5 min of Haiku turns). If `V_r` ever drops the
  tool, the hook's `allow` is ignored, the dialog stays with the PC, and Agentmaster falls back to show-only.

## 9. Sources

**The Claude Code 2.1.291 bundle** (`~/.local/bin/claude.exe`, a Bun single-file binary). Its plain JS chunks
start at about byte 200,000,000. Minified names are chunk-local and change with every release: search by the
quoted strings (`tools/interactive-tools-probe/grepbin.py`). Byte offsets in 2.1.291:

| What | Name | Byte offset |
|---|---|---|
| the tool's preview guidance | `M8o` | 213,909,976 |
| `"(notes only)"` | `_0t` | 222,627,667 |
| the result mapper | `mapToolResultToToolResultBlockParam` | 222,632,953 |
| the preview limit and its placeholder | `FXe` / `Xge` | 237,345,602 |
| the preview box | `ohe` / `p5` | 237,346,212 |
| the question tabs | `QS` | 237,348,733 |
| the preview layout (keys, Notes, the submit rule) | `A5` | 237,351,068 |
| the review screen ("Ready to submit your answers?") | `K5` | 237,366,631 |
| the preview kinds | `G5` | 237,369,819 |
| what the PC sends | `nJ` | 237,384,141 |
| "Chat about this" | `R4e` | 237,384,750 |
| the preview-question rule | `P4e` | 237,385,571 |
| the preview's sanitizer | `Nl` (`zet`) | 210,386,115 (210,393,687) |
| the odd-character test | `e0` / `Ym` | 210,392,840 / 210,386,771 |
| the terminal Markdown renderer | `wIt` | 220,595,646 |
| the hook's allow / deny | | 233,387,760 |
| `executePermissionRequestHooks` | `ABe` | 217,614,218 |
| the runner's `runHooks` | | 235,984,685 |
| the dialog-vs-hooks race | `zo` | 236,017,181 |
| the hook-answer set | `V_r` / `f5n` | 217,906,352 |
| the re-check | `mC` | 217,908,724 |
| the default hook timeout | `zl = 600000` | 217,474,842 |
| a command hook's timeout | `wn` | 217,530,299 |
| the hook command schema (`timeout: positive().optional()`) | | 207,214,649 |
| the 6 s permission Notification | | 233,378,382 |

**Other sources:**
- The typings Claude Code writes beside a mod: `~/.claude/mods/<mod>/.claude-plugin/types/claude-code-tools/index.d.ts`
  (the tools' inputs and outputs) and `…/claude-code/index.d.ts` (hooks).
- `tools/interactive-tools-probe/`: the probe (`run_probe.py` + `perm_hook.py`), the corpus survey
  (`survey_interactive.py`), and `grepbin.py`.
- MESSAGING.md §6.3, §7.4, §7.5, §18 (D9, D16, D30), §19, §21.2, §21.24 (Screen Image); MOD_LINK.md §4.4; HOOKS.md
  (hook invocation, the blocked-on-user belts).

## 10. What was built (2026-10-19; in dev since 0.0.1.31, not in production)

> The user's request (2026-10-19): "Kindly close the gap, enabling telegram picking a choice, including the user manual
> text input. Use telegram to the best of its ability to produce a managed message that will cover this interaction and
> including custom answer". The topic's side — what the user sees, the logs, the check after the build — is
> [`MESSAGING.md`](MESSAGING.md) §21.30.

### 10.1 The code

| Piece | Where | What it does |
|---|---|---|
| The question hook | `ClaudeSpawn.cpp` `BuildAskHookScript` → `<profile>\agentmaster-ask.ps1` (UTF-8 BOM, pure-ASCII body) | Windows PowerShell 5.1, spawned directly (exec form). Reads the payload from the raw stdin stream as UTF-8; leaves at once with no output for a subagent (`agent_id`), another tool, or no pipe (`CCMGR_HOOK_PIPE`, else `<profile>\bridge.json`). Else it sends ONE request line `{"v":1,"hookPid":…,"claudePid":0,"payload":<as claude wrote it>}` over the ASK pipe FIRST, then looks its parent claude up (Win32_Process) and waits for one reply line (an async read polled every 2 s). A decision is printed as UTF-8 bytes; "{}", a broken pipe, the parent's exit or 24 h print nothing. Every path exits 0. |
| Its wiring | `BuildHooksSettingsJson(…, execHost, askScriptPath)`, called by `MaterializeSharedHookFiles` (the shared file) and `MaterializeLaunchSettingsFile` (a preset's) | A NINTH entry, `"PermissionRequest": [{ "matcher": "AskUserQuestion", "hooks": [{ "type": "command", "command": <powershell.exe>, "args": [-NoProfile, -NonInteractive, -ExecutionPolicy, Bypass, -File, <script>], "timeout": 86400 }] }]`. Only while `AppSettings::messagingEnabled`, and only in exec form (claude ≥ 2.1.139). The shared file is rewritten at engine init, every spawn and the cog's Save, so turning the bot on reaches the claudes that START afterwards. Without it the file is the eight-event file, byte for byte. |
| The pipe | `HooksBridge.{h,cpp}`: `AskBridge`, `AskRegistry` | `\\.\pipe\agentmaster.<pid>.ask`, duplex, a listener and one worker thread per waiting helper. The request line is bounded (1 MiB, 10 s). The registry's ACCEPTOR decides at the door; a refused request hears "{}" at once. A worker waits for its reply, the helper's departure (a one-byte probe read) or Stop ("{}"). States: Open → Answered → Delivered / Released, or Gone. |
| The pure core | `AskQuestion.h` (header-only, `Agentmaster::Msg`) | The request parser and its signature (== the mirror's question item text); the answers composed as `nJ` composes them, with `Tct`'s multi-select join, `_0t`'s "(notes only)" and the picked preview's echo; the bounds; the allow / deny lines as ASCII JSON; "Chat about this" (`R4e`); the card's model, steps, callback data, text routing, texts, buttons; the previews' sheet (a `ScreenSnapshot`). |
| The preview rules | `ClaudeInvisibleChars.{h,cpp}` | `ClassifyClaudeQuestionPreview` (`G5`: None / Withheld / Full), `ClaudeQuestionPreviewEcho` (`Nl` = `zet`), `ClaudeCellWidth`. |
| The service | `Messaging.Questions.cpp` (a third TU of `MessagingService`; its block at the end of `Messaging.h`), hooks in `Messaging.cpp` and `Messaging.Inbound.cpp` | The acceptor; `_questionsTick` (new requests → cards; every card's watch, request, topic and clocks; finished cards forgotten after 30 min; the plain questions whose 8 s ran out); the card's post and edits (an edit renders the card AGAIN right before its call, so it always shows the latest state); `_onQuestionTap`; `_takeQuestionText` (the typed answer or note, the "answer or next prompt?" question). |
| S0 | `MessagingModels.h` `ClassifyTranscriptLine` | The answer item carries each question's NOTE (`toolUseResult.annotations[q].notes`): "(notes only)" reads as `📝 <note>`, a pick with a note as `<label>\n📝 <note>`. Live and in a history replay. |
| The engine | `Engine.{h,cpp}` | `asks` (the registry) is handed to the service before its first Apply; `askBridge` starts with the hook pipe, also while the bot is off (a hook then hears "{}" at once). |
| Tests | `tests/tests_ask.cpp` (`TestAskQuestion`) | §10.4. |

### 10.2 How a question travels

1. The dialog opens at the PC, and Claude Code runs the hook BESIDE it. The helper's request reaches `AskRegistry::Open`,
   which asks the acceptor (`MessagingService::_acceptsAsk`): the bot runs with topics on, and the session is a live Claude
   session of the registry. Anything else: "{}" at once, no output, and the PC answers as before.
2. The registry's listener wakes the tick. `_questionsTick` makes the card and posts it in the session's topic (Final
   priority; it replies to the turn's prompt and rings, unless the NeedsApproval edge rang the sticky card already — §21.6's
   one sound per "needs you"). A session without a topic is waited for 30 s, then released.
3. The card's WATCH reads the request's `transcript_path` (its first read looks back 512 KB) for the dialog's `tool_use` —
   the newest question item with the card's signature — and then for its tool result. The mirror pairs with the card too:
   its question item posts nothing when a card shows the dialog, else it waits `kAskPairWaitMs` (8 s) for a hook and is
   posted show-only with the reason; its answer item posts no "👤 You ↩ answer" line under a card that shows it.
4. The user answers on the card. AskQuestion.h builds the decision; `AskRegistry::Answer` queues it; the worker writes it;
   the helper prints it; Claude Code takes it and writes the tool result 0.01–0.02 s later. The card reads ⏳ Sending, then
   ✅ Answered from Telegram once the watch reads the result (after 60 s without it: "not confirmed").
5. The other endings:
   - the PC answers first: the watch reads the result → ✅ Answered at the PC (the answer, notes included), and the hook is
     RELEASED ("{}"): Claude Code does not end it (§4.4);
   - Esc / "Chat about this" at the PC: an error result, or the hook ends (Gone) → ⏹ closed at the PC;
   - the session ends (no longer live): released at once, ⏹ its session ended;
   - the topic is made again (a recreation): the open card is posted anew in the new topic; the topic gone: released
     after 30 s;
   - the bot stops: every open request is released; the PC keeps every dialog;
   - a decision refused by the registry (the PC was first by a hair): the card says so, and the watch then names the PC's
     answer.

### 10.3 The card, and where the build differs from the design (§6, §7)

The card is ONE managed message per dialog, edited in place — the user's "a managed message that will cover this
interaction". MESSAGING.md §21.30 shows it.

| Q | The proposal | What was built | Why |
|---|---|---|---|
| Q1 | a `PermissionRequest` hook | **as proposed** | — |
| Q2 | a native exe (the CLI in a hook mode), copied into the profile | **the PowerShell helper**, exec form, like the forwarder | no new binary to ship and to copy into the profile on every layout swap; the forwarder's discovery and encoding rules carry over. Its cost, measured (§10.4): 0.41–0.47 s from its start to the request (the request goes before the parent lookup, which cost ~1.7 s in front of it), and ~93 MB working set / ~72 MB private while it waits — one per OPEN question while the bot runs. A native helper stays the follow-up if that matters. |
| Q3 | one picture of every preview, then the question message | **the previews INLINE** in the card as monospace blocks while they fit the message (3,800 characters), and **[🖼 Previews as a picture]** on demand (`BuildAskPreviewSheet` → the Screen Image renderer → `sendPhoto`) | one message per dialog, and nothing posted unasked on the chat's ~1 call/s budget; a picture is one tap away |
| Q4 | the raw text in the picture | **as proposed** (the echo, TABs expanded, wrapped at 160 columns) | — |
| Q5 | [📝 Note…] (ForceReply) and a reply to the question message; other messages stay queued prompts (D30) | **[📝] / [✍️] post a ForceReply prompt**; the reply, the next message, or any reply to the card is taken; a PLAIN message while the card is open is HELD and asked about ("your answer, or the next prompt?") | a message typed while a question is open is most likely its answer — but never taken unasked |
| Q6 | one question: a tap answers at once | **as proposed** (an own answer of a one-question card too) | — |
| Q7 | one picture + one message per question, Send on the last | **ONE card for all questions**: a list of every answer so far, ◀ / ▶, [📨 Send answers (a of n)] (unanswered ones allowed) | the user's "a managed message" |
| Q8 | [💬 Chat about this] | **as proposed** (Claude Code's own rejection text, with the answers and notes so far) | — |
| Q9 | a subagent's question never held | **as proposed** (the helper leaves at once on `agent_id`; the engine refuses one too) | — |
| Q10 | S0 first | **built together**: the notes in every answer line; the question item's descriptions were not added (the card shows them) | — |

**Also different from §6.3:**
- The buttons carry a random card key (`q:<10 characters>:<verb>[:<arg>]`), not action tokens: a button of an earlier run, or
  of a card forgotten 30 min after its outcome, answers "This question is no longer open here." and loses its keyboard.
- The confirmation is the transcript's tool result, within 60 s (not the `hook_permission_decision` line within 10 s).
- There is nothing to validate but the bounds: the answer is composed from the request's own input.
- **Not built:** the request as a fourth `NeedsApproval` belt; an album for a tall sheet; the measure of how blocks of 52,
  70 and 113 cells look on the phone (§8).

### 10.4 What was checked

- **The m5 suite:** `TestAskQuestion` passes. Against the probe's goldens D, E and G, the composer builds the PC's exact
  `answers` and `annotations`. The suite's only failures are the 3 known `TestTaskbarBadge` ones.
- **`cl /Zs` with the last Debug build's recorded flags:** every changed `TerminalAppLib` TU and every TU that includes a
  changed header passed (`Messaging*.cpp`, `HooksBridge.cpp`, `ClaudeSpawn.cpp`, `Engine.cpp`,
  `ClaudeInvisibleChars.cpp`, `TerminalPage.AgentMessaging.cpp`, `MessagingSearchBridge.cpp`,
  `AgentManagerContent.Settings.cpp`, `TerminalPage.AgentEngine.cpp`, `TerminalPage.AgentSessions.cpp`,
  `TerminalPage.cpp`). The new `Messaging.Questions.cpp` passed with a negative control. The CLI's three changed engine
  TUs passed at its `/W4 /WX`.
- **LIVE, end to end** (`tools/interactive-tools-probe/askprobe.py` + `askdrive.cpp`; claude.exe 2.1.291, Haiku 4.5,
  2026-10-19 17:00). It ran the SHIPPED hook script against the real `AskBridge`, with the decision lines from
  `AskQuestion.h`. Every verdict was read from the transcript:

| | Scenario | Result |
|---|---|---|
| H1 | a preview question: option 2 + a note | ✅ the PC's own form, byte for byte; the stored `toolUseResult` holds the same answers and annotations; the model replied `LAYOUT=Center` |
| H2 | a preview question: a note alone | ✅ `"…"=(no option selected) notes: none of these, make it a tab strip. Read the answers carefully …` |
| H3 | a plain question, the hook RELEASED ("{}"), the PC's Enter | ✅ the helper was gone 2.5 s after the release; the result is the PC's `"Red"` |
| H4 | Chat about this | ✅ an error result whose text is the deny message, verbatim; the model asked "What would you like to clarify about the fruit question?" |

- **The helper measured** (`askmem.py`, three helpers held by `askdrive.exe`):

| What | Measured |
|---|---|
| From the host's start to the request at the engine | **0.41–0.47 s** with the request first. It was ~2.0 s with the parent lookup first: `Get-CimInstance Win32_Process` alone takes ~1.7 s (a bare `powershell -Command exit` ~0.27 s). |
| Memory while it waits | 92.8–94.7 MB working set, 71.4–73.4 MB private |
| End after a release | 0.04 s, exit code 0, no output |
| End after its parent's end (`askparent.py`: the helper under a `cmd /c` that is killed) | 0.5 s, no output |

The live run of the table above was repeated with the request-first helper (17:11): H1–H4 passed again.

### 10.5 Check after the build (owed)

- **The phone** (through the dev bot, a test message only with the user's go-ahead): every row of MESSAGING.md §21.30's
  "Check after the build". Also how the inline preview blocks wrap at 52, 70 and 113 cells, and the ForceReply prompt.
- **The PC side:**
  - "Allowed by PermissionRequest hook" under a phone answer;
  - a helper gone within a tick after a PC answer (no `agentmaster-ask.ps1` left in `Get-Process powershell`);
  - Esc;
  - a session closed with a question open;
  - the bot off (no helper waits);
  - a subagent's question;
  - a claude older than 2.1.139 (no question hook: the show-only message, with the reason).
- **On every Claude Code upgrade** that touches hooks or `AskUserQuestion`, re-run `run_probe.py` (the seven scenarios)
  and `askprobe.py` (the four of the shipped hook). If `V_r` ever drops the tool, the hook's `allow` is ignored, the dialog
  stays with the PC, and the card ends as "Answered at the PC".
