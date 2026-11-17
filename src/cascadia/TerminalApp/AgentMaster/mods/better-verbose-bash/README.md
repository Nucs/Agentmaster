# better-verbose-bash

A Claude Code mod. Wherever Claude Code draws a shell call's own `Bash(command)` row, in any view, the reason that
the model gave for the call (its `description`) becomes the row's bulleted header. The command and its output
follow under it.

Display only. The transcript, and the text that the model reads back, do not change.

## Install

```bash
claude plugin marketplace add https://dev.azure.com/sharegain/ShareGainProject/_git/ICSkills
claude plugin install better-verbose-bash@icskills
```

Then run `/reload-plugins` in open sessions, or start a new one. It needs Claude Code 2.1.308 or later (see
[Requirements](#requirements)).

With [chat-view](../chat-view/README.md) too, either order in the `enabledPlugins` of your
`~/.claude/settings.json` works (see [With chat-view](#with-chat-view)).

## What it changes

Claude Code draws a shell call like this, and the model's description shows only in the permission dialog:

```
● Bash(git status)
  ⎿  On branch master
     nothing to commit, working tree clean
```

With the mod, wherever Claude Code draws that row:

```
● Show working tree status
  Bash(git status)
  ⎿  On branch master
     nothing to commit, working tree clean
```

- The header's bullet has Claude Code's colors: dim while the call runs or waits its turn, green when it
  succeeded, red when it failed or was interrupted. On macOS it is `⏺`, as Claude Code draws it there.
- A long description wraps under itself. A description with line breaks draws as one line, and one longer than
  300 characters ends in `…`.
- From the second line on, the row is Claude Code's own: the full command, its wrapped lines, "Waiting for
  permission…", the progress lines of a running call, and the output.

## When it applies

The mod changes a row only when all of these are true:

| Condition | Why |
|---|---|
| The tool is `Bash` or `PowerShell` | Only these calls carry a shell command and a description of it. |
| Claude Code draws the call's own row | The row is the test, not the view: the mod does not ask whether the view is verbose. Every `Bash(command)` row gets the header: with `--verbose` or **Verbose output** in `/config`, in the ctrl+o transcript, in a call group that is unfolded, and in a compact view that draws a call on its own row. A call that the compact view folds into a count line (`Ran 3 shell commands`) has no row, so nothing changes there. |
| The call has a description with text in it | A call with no description keeps its own row and its own bullet. |
| The surface is the terminal | Desktop, VS Code and the phones draw their rows in their own way. |
| Claude Code says how it draws the row | Claude Code 2.1.308 and later send `isExpanded` (true or false) with each row. On older builds, which do not, the rows stay as Claude Code draws them (see [Requirements](#requirements)). |
| The row beneath is Claude Code's own | A plugin seated beneath this mod may draw the row its own way: hide it, or add lines of its own. The header's layout fits only Claude Code's row, so such a row keeps that plugin's drawing. |

A call group that another plugin unfolds (chat-view's Full and Max views do) draws a row for each call, so those
rows get the header too.

In the compact view, Claude Code 2.1.308 folds every shell call into a count line, a lone call too (`Ran 1 shell
command`). While such a group runs, its live line shows `Running <command>` (or Claude Code's own wording of the
description, such as `Printing a test line`) over `⎿  $ <command>`. That line belongs to the group, not to a call's
row, so the mod does not change it.

## With chat-view

The plugin listed first in `enabledPlugins` sits above the ones after it, and a plugin loaded from a folder
(`--plugin-dir`, `CLAUDE_CODE_PLUGIN_DIRS`, or the copy Agentmaster bundles) sits above every installed plugin.
Both seats work:

| Seat | Compact and Minimal (tool calls hidden) | Default (Claude Code's own rows) | Full and Max (a dim `# …` description line above each shell call) |
|---|---|---|---|
| chat-view above this mod | A hidden row never reaches this mod. | chat-view hands the row on; this mod draws the header. | This mod draws the header, and chat-view adds its line above it, so the description shows twice. |
| This mod above chat-view | chat-view hands the row on as an empty Box; this mod passes it on, so the row stays hidden. | chat-view hands Claude Code's own row on; this mod draws the header. | chat-view hands the row on with its line; this mod passes it on, so the row shows chat-view's line and no header. |

Before 0.1.1, the second seat drew a header alone where chat-view hid a row, and laid chat-view's line over the
header.

## Requirements

Claude Code 2.1.308 or later. The mod checks that the `ToolUse` row carries the `isExpanded` prop, true or false
(it does not read the value). Claude Code 2.1.308 and later send it, and the header's layout was checked against
their rows. Claude Code 2.1.292 and earlier do not send it. There, the mod changes nothing and reports no error
(checked live on 2.1.292 for 0.1.0; a test checks it for 0.2.0).

## How it works

One `ui.render` hook, on the terminal's `ToolUse` rows of `Bash` and `PowerShell` calls, in any view. Other rows
do not reach the hook. Claude Code raises a `ToolUse` row only where it draws a call's own `Bash(command)` line (in
2.1.308: a standalone row, and a row of an unfolded call group), and none for a call folded into a count line, so
the row itself tells the mod that the command is on the screen. A row without `isExpanded` (a build older than
2.1.308) is handed on unchanged. The hook asks for the row beneath it (`next(e)`). When that is not Claude Code's
own row (the engine reference `{ type: 'engine', ref }`) but a tree that a plugin beneath built, the hook returns
that tree unchanged. Over Claude Code's own row, it draws:

1. One blank line above the row, as every transcript row has.
2. The header: the bullet in a 2-column box, then the description in bold.
3. Claude Code's own row, pulled up one line over its own top margin, so that its first line, `Bash(command)`,
   is directly under the header.
4. A box with two blanks, placed on the row's bullet and painted after the row. It hides the engine's bullet,
   so that the row keeps one bullet.

Claude Code does not let a mod clip or resize its row: it refuses a tree that puts the row under a box with
`overflow`, `position`, `display`, a size or an offset. A box beside the row may have these props, so the mod
paints over the bullet instead of clipping it.

The layout expects the blank top line that Claude Code gives every tool row. A row without that line (Claude Code
draws one only after a speaker label, in agent-team views) would draw over the header's last line.

## Tests

```bash
claude plugin test plugins/better-verbose-bash
```

21 tests, all passing on Claude Code 2.1.308. The kit draws no row of its own: a test's hook stands beneath the
mod, answering with the engine reference `{ type: 'engine', ref: 0 }` where Claude Code's row belongs, and with a
Box or a Text where another plugin's drawing belongs. The test kit checks the tree that the hook returns, not what
the terminal paints. So the look was also checked in a real terminal:

- 0.2.0 (2.1.308, 120 columns, the fullscreen layout and the classic one, a fake Messages API that plays fixed
  calls): with Verbose output on, every described call has its header (green when it succeeded, red when it failed,
  dim while it waits for permission), a call with no description keeps its own row, and a long description wraps
  under itself. With it off, Claude Code folds the calls into count lines, and ctrl+o shows every row with its
  header.
- 0.1.0 (2.1.308, 120 and 60 columns): `--verbose`, the compact view, and ctrl+o.

## Versions

- 0.2.0: the header goes over every shell row that Claude Code draws, in any view. The mod no longer asks whether
  the view is verbose (`isExpanded: true`): the row itself shows that the command is on the screen. A build that
  does not say how it draws the row (2.1.292 and earlier) still keeps its rows.
- 0.1.1: the header goes only over Claude Code's own row. A row that a plugin seated beneath drew its own way
  keeps that drawing, so the mod works in any seat (see [With chat-view](#with-chat-view)).
- 0.1.0: the first version.
