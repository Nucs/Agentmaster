# prompt-cut

Cut for Claude Code's prompt box. A key bound to `command:prompt-cut` puts the
whole draft on the clipboard and empties the box. It works at once, also while
Claude works. Pastes are expanded: a `[Pasted text #3 +21 lines]` placeholder
goes to the clipboard as the text it stands for, not as the placeholder.

A mod (Claude Code 2.1.310+, function hooks). Version lives only in
`.claude-plugin/plugin.json`.

## Install

From the `claude-mods` marketplace (`~/.claude/mods`):

```
claude plugin install prompt-cut@claude-mods --scope user
```

Then bind a key in `~/.claude/keybindings.json`. A plugin cannot ship a key
binding:

```json
{
  "$schema": "https://www.schemastore.org/claude-code-keybindings.json",
  "$docs": "https://code.claude.com/docs/en/keybindings",
  "bindings": [
    {
      "context": "Chat",
      "bindings": {
        "ctrl+x ctrl+x": "command:prompt-cut"
      }
    }
  ]
}
```

Claude Code reads that file again when it changes. The command itself exists
only in sessions started (or resumed) after the install. In a session without
it, the keys print `● Unknown command: /prompt-cut` and keep the draft. That
notice is a display-only `system` row, and nothing reaches the model (checked
on 2.1.310).

### Why ctrl+x twice, not ctrl+x

Claude Code uses ctrl+x as the first key of 11 two-key shortcuts
(`ctrl+x enter` queues, `ctrl+x tab` focuses the band, `ctrl+x ctrl+e` opens
the editor, …). Its key resolver lets a chord prefix win over a single-key
binding (`R7e` in 2.1.310's bundle): while any `ctrl+x …` chord is bound, a
single ctrl+x only waits for its second key. A plain `"ctrl+x"` binding works
only when every `ctrl+x …` chord is unbound (`null`) in every context, Pane,
Task, DiffPanel and FeedbackSurvey included. `ctrl+x ctrl+x` keeps all of them.

## What a cut does

1. It waits up to 3 s for clipboard reads still running (a cut right after a
   paste).
2. It reads the box (`$.prompt.read`). An empty box, blanks or images alone:
   nothing happens.
3. It expands each placeholder:
   - `[Pasted text #N …]` and `[✦ Team setup guide #N …]`: the paste's text;
   - `[...Truncated text #N +M lines...]`: the middle Claude Code cut out of a
     draft over 10,000 characters;
   - `[Image #N]`: taken out of the copied text and left in the box (no text
     clipboard carries an image);
   - a placeholder that was only typed, or pasted inside a short text, is
     plain text to Claude Code and is copied as written.
4. It copies the text (`$.ui.copy`, as `/copy` writes: the machine's clipboard
   tool and OSC 52). Only if the copy took does it empty the box
   (`$.prompt.fill`).
5. It shows a toast: `Cut 1,234 characters to the clipboard, 1 paste expanded.`

If a paste's text is unknown, step 3 cannot expand it, and the key arms Enter
instead (below).

The command answers with no text, so a cut leaves no row in the transcript and
nothing reaches the conversation the model reads. A key-run command is never
added to the prompt history. `/prompt-cut` is hidden from the typeahead: typed,
it has nothing to cut, because sending it empties the box first.

Claude Code's own undo (ctrl+_) brings a cut draft back, as of its last undo
snapshot: on 2.1.310 the last characters typed before the cut can be missing.

## How it knows a paste's text

Claude Code never gives a mod a paste's text. `$.prompt.read` returns the
draft with the placeholder in it, and the pasted contents stay inside the
engine. So the mod keeps its own record per placeholder id, in `$.state`
(which outlives a reload of the mod) for the session's newest 200:

- **A paste** reaches the box as a `prompt.change` with origin `engine` (seen
  live on 2.1.310, though the API docs place pastes at `prompt.edit`; an edit
  of that form is handled too). The mod reads the clipboard right then. It
  normalizes the text as the engine did (line breaks `\n`, tabs four spaces,
  escape sequences out, the `!` of a shell-mode paste into an empty box
  dropped). It keeps the text when its line count fits the placeholder's
  `+M lines`. A one-line placeholder (no count) also needs a text over 800
  characters. Otherwise the placeholder is recorded unread.
- **A truncation** is rebuilt from the draft before it: the last box text
  seen, or the text another plugin's fill was to write (prompt-queue's
  `[edit next]` on a long prompt).
- **A recalled prompt** (the up arrow, ctrl+r) is looked up in
  `history.jsonl` when it is cut. The recall renumbers placeholders (the
  entry's `#3` comes back as `#4`), so the entry is found by its text with any
  ids and paired back in order. Its pastes come from the entry
  (`pastedContents`, inline or `paste-cache/<hash>.txt`). An id the entry
  lacks was plain text in that prompt.
- **Typed text** that spells a placeholder, or one inside a longer paste or a
  plugin's fill, is recorded as plain text.

The clipboard is read only right after a paste that became a placeholder,
kept in the session's memory and never written to disk.

## When a paste's text is unknown: Enter cuts

Sometimes the mod never read a paste's text: it was pasted before the mod
loaded (an install or `/reload-plugins` with a draft in the box), the
clipboard changed before the read, Agentmaster or a script typed the paste,
or the paste cache lost it. Claude Code still has the text, and it expands
every paste on its own send (`prompt.submit`, the place prompt-queue's
`[queue]` button reads pastes from too). So the key arms Enter instead of
cutting:

1. The toast says `Press Enter to cut: Claude Code expands [Pasted text #1
   +37 lines] only when you send, so prompt-cut takes that Enter and sends
   nothing. Typing cancels.` The hint row under the box shows
   `[Enter cuts ✕]`. For 8 s after a paste, Claude Code's own "paste again to
   expand" hint holds that row, and the marker shows once it goes.
2. Enter: the mod copies the submitted text (every paste expanded, images out)
   and drops the send. Claude Code shows `Prompt dropped by a hook: cut 1,234
   characters to the clipboard, pastes expanded (prompt-cut)`. That notice is a
   display-only system row with the count alone. Nothing reaches the model or
   the prompt history.
3. Claude Code puts the dropped draft back in the box. On 2.1.310 it comes
   back expanded. The mod empties the box again and leaves the images.

The draft is first written over itself, which puts the box in prompt mode. A
shell-mode (`!`) draft's Enter would otherwise run its command, because such a
draft never reaches `prompt.submit` (read in 2.1.310's code; the live bench
checks that the shell command does not run). Typing, the key again, the ✕, or
60 s takes the arm back, and then Enter sends again.

A slash-command draft cannot be armed: Enter would run the command. There the
first press keeps the box and names the placeholder. A second press within
5 s cuts it as written.

## Options

`/config`, search `prompt-cut`:

- **Clipboard reader** (`clipboardCommand`): the command that prints the
  clipboard's text, run without a shell, its words split at spaces (no
  quoting). Empty (the default) uses Windows PowerShell's `Get-Clipboard` on
  Windows and in WSL (it prints base64, so no console code page can change a
  character), `pbpaste` on macOS, and `wl-paste`, `xclip` or `xsel` on Linux.
  The history search uses Windows PowerShell, or Perl elsewhere.

## Files

- `hooks/register.ts`: the hooks: `session.start` (registers `/prompt-cut`,
  `immediate`), `command.describe` (hides it), `command.run` (the cut),
  `prompt.edit`, `prompt.change`, `prompt.fill` (the records).
- `hooks/placeholders.ts`: pure helpers with no `$`: the placeholder grammar
  (the engine's `Fp`), paste normalization, the fit check, expansion,
  truncation rebuild, the history line pattern, base64 and UTF-8 codecs.
- `types/index.d.ts`: the `$.state` contract (`paste`, `pasteIds`).
- `tests/register.test.ts`: 35 tests (`claude plugin test .`), engine
  truncation mirrored from 2.1.310's `K5e`/`Ato`.
- `dev/drive_tui.py`: the live bench, 38 checks in real Claude Code in a
  hidden ConPTY with the fake API of the claude-code-debug skill. It never
  touches your clipboard: the mod reads a fake one through its
  `clipboardCommand` option, and `SSH_CONNECTION` makes Claude Code copy
  through OSC 52 alone, which the bench reads back from the output.

```
PYLIBS=<dir with pywinpty, pyte> python -I dev/drive_tui.py <run folder> --plugin-dir <scratch copy>
```

## Limits

- The text of a paste the clipboard no longer holds when the mod reads it
  (within about half a second) is unknown. The same goes for a paste that
  never came from the clipboard (Agentmaster or a script typing a bracketed
  paste) and for a draft Claude Code put back from an earlier process. Such a
  draft takes two keys: the key arms Enter, and Enter cuts.
- A one-line paste (no `+M lines`) is checked by length alone (over 800
  characters). Another long one-line clipboard text read in that half second
  would pass.
- A shell-mode draft (`!`) is copied without its `!`, and the box goes back
  to prompt mode. A mod cannot read the box's mode.
- The armed Enter's drop shows Claude Code's own wording, `Prompt dropped by
  a hook: …`, which a mod cannot change.
- Copies go through Claude Code's own clipboard path. Over SSH that is OSC 52
  alone, which some terminals drop.
