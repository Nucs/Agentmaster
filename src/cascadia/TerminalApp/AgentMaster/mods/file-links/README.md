# file-links

A Claude Code mod. It draws the file paths and file names in Claude's replies as clickable `file:///` links, on Windows and macOS. Where a `CLAUDE.md` rule asks replies to write every file as a `file:///` link, the model can write the plain path instead, and the mod draws the link.

Display only. The mod changes the text that the terminal draws. The stored reply, the transcript, and the text that the model reads back do not change. In the fullscreen terminal, a plain click on a file link opens it once, through the mod (see [How a click opens a link](#how-a-click-opens-a-link)).

## Install

```bash
claude plugin marketplace add https://dev.azure.com/sharegain/ShareGainProject/_git/ICSkills
claude plugin install file-links@icskills
```

Then run `/reload-plugins` in open sessions, or start a new one. Type `/file-links` to see what the mod knows.

With [chat-view](../chat-view/README.md) too, keep `chat-view@icskills` before `file-links@icskills` in the `enabledPlugins` of your `~/.claude/settings.json` (install chat-view first: a new install is added at the end). chat-view matches the transcript's rows by their text, so it must see the text before file-links adds the links (see [Seat order](#seat-order-with-chat-view)).

## What it links

The mod reads the Markdown of each reply block (the `AssistantMessage` render site). It finds the spans that can name a file:

| Written as | Example | Found by |
| --- | --- | --- |
| Full Windows path (Windows) | `C:\Users\me\.claude\CLAUDE.md`, `C:/x/y.md` | The path itself, if it exists |
| Git Bash path (Windows) | `/c/Users/me/x.md` | The same path as `C:/Users/me/x.md` |
| Full POSIX path (macOS) | `/Users/me/.claude/CLAUDE.md`, `/etc/hosts` | The path itself, if it exists |
| Home path (both) | `~/.claude/settings.json` | The path under the home folder: `USERPROFILE` on Windows, `HOME` on macOS |
| Relative path | `hooks/register.ts`, `./x/y`, `mods/chat-view/` | The working folder, the project root, then the session's scratchpad, then the files the session touched, then the git index, then the end of a known folder (below) |
| Folder name with its slash | `projects/`, `scratchpad\` | The working folder, the project root, the session's scratchpad, then the end of a known folder |
| End of a known folder | `<session id>/scratchpad`, `/<session id>/scratchpad/run1/x.json`, `.claude/mods/x.md` | Its first segments are the last segments of the scratchpad, the working folder, the project root or the home folder (in that order); the rest lies under that folder |
| File name | `register.ts`, `README.md`, `.gitignore` | The newest file of that name that the session touched, then the working folder, the project root, the session's scratchpad, then the git index |

- A span can be a code span (`` `x.md` ``) or bare text (`x.md`). A code span keeps its backticks inside the link, so it still draws as code.
- A path with a folder part draws as two links side by side (0.5.0): the folder part, separator included, opens the folder; the name opens the file. `C:\x\a.md` draws `C:\x\` and `a.md`; `.\a.md` draws `.\` (the folder the file was found in) and `a.md`. A bare name, and a path written as a folder (`mods/chat-view/`), stay one link. A code span becomes two code spans.
- A bare folder name without its slash (`projects`) stays plain: it is too often a word.
- A path written as a folder (`hooks/`) never links to a FILE of that name: the touched files and the git index list files, so they are skipped for it.
- A line suffix (`:12`, `:12:5`, `#L12`) stays in the link text. The URI does not carry it, because a file URI cannot hold a line.
- Wrapping punctuation and emphasis stay outside the link: `(see **x.md**),` links only `x.md`.
- Letter case does not matter on Windows: `claude.md` links to `CLAUDE.md`.
- A bare label is escaped, so the link shows the name as written. Example: plain Markdown drops the backslash in `\.claude`, but the link keeps it.

### When two or more folders hold the file

The rows above are tried in order, and the first row that finds a file wins. So a collision is settled like this:

1. A file that the session read or wrote wins: the newest one. Example: after an edit of `plugins/prompt-queue/hooks/hooks.json`, a reply's `hooks.json` means that file, not a shorter `hooks.json` in another folder.
2. A file directly in the working folder, the project root or the session's scratchpad wins next, in that order: a project file wins over a scratchpad file with the same name.
3. Then, in the git index, the shortest path wins: the fewest characters. A tie in length goes to the fewest folders, then to A-to-Z order. Example: a name that 29 skill folders hold links to the shortest of the 29 paths.

### Not linked

- Text in fenced code blocks.
- Links, images, reference links and autolinks that the reply already has. A link the model writes stays as written.
- URLs, flags (`--x`), globs (`*.ts`), versions (`2.1.292`), commands in code spans (`git status`).
- A name with no extension (`Makefile`). With a folder part (`./Makefile`), it links if the file exists.
- A name inside a longer word (`CLAUDE.md's`).
- A span that names no file the mod can find. A file outside the places above links only by its full path.
- Relative paths with spaces, and bare (not code span) paths with spaces.
- Your own prompts and the tool rows (`● Read(path)`, `Bash(command)`, a Bash call's output and its `Updated <path> (+1 -1)` lines): the mod draws only Claude's replies. A mod cannot add a link there: Claude Code 2.1.311 refuses a rewrite of a tool row's `input` or `output` that holds an escape sequence ("a control character (an escape sequence the terminal would honour ...)"), and a Bash result draws its output as text, not Markdown. Only a mod that drew those rows whole could link them.

## How a link looks

The engine draws a Markdown link as an OSC 8 hyperlink in blue when the terminal supports hyperlinks.

## How a click opens a link

- **Plain click, fullscreen terminal** (`CLAUDE_CODE_NO_FLICKER=1`, Claude Code 2.1.292 and later, on by default): the mod draws each reply that holds a `file:` link of this machine as a `Markdown` element that answers its file links (`onLinkPress`). A plain click on such a link (the mod's own, or a `file:///` link the reply wrote) opens it once, through the mod:
  - a file opens in the app the system opens it with: on Windows PowerShell's `Invoke-Item -LiteralPath`, on macOS `open <path>`, on Linux `xdg-open <path>`;
  - a folder (a folder link, or a link to a folder) opens in the file manager;
  - a program, script, installer, shortcut, disk image or app bundle (`.exe`, `.ps1`, `.bat`, `.lnk`, `.msi`, `.py`, `.app`, `.command`, `.pkg`, `.dmg` and kin) is shown selected in its folder (`explorer /select,"<path>"` through PowerShell's `Start-Process` on Windows, `open -R <path>` on macOS), never started by a click;
  - a link to a path that is not there shows a toast: `file-links: <path> is not there`; an opener that fails shows its first error line.
  - An `https:` link, and a `file:` link to another machine (`file://server/share/x`), keep Claude Code's own behaviour.
  - On Windows the path reaches PowerShell in an environment variable (`FILE_LINKS_TARGET`), never in the command text, so no file name can change the command. Not `explorer <path>`: explorer splits its argument at commas, and a click on a folder named `a&b,c` opened Documents (0.5.0, seen live). Checked live on folders named with a space, `&` and a comma, `#` and `%`, parentheses, Hebrew, `~`, a quote and brackets, a curly quote, `;` and `^`: each opened its own folder, and a script in each was shown selected. `rundll32 url.dll,FileProtocolHandler` failed on the Hebrew name. Windows PowerShell starts in about 0.4 s; where it cannot start (an AppLocker rule), the click shows why.
- **Ctrl-click or alt-click:** the terminal's and Claude Code's, not the mod's. In Windows Terminal and Agentmaster (and any host built on the Windows Terminal control) both open the link: the terminal opens it on the press (`ShellExecute`, the file's own app), and Claude Code on the release (`explorer /select,<path>`, a window on its folder), so one ctrl-click opens two windows. No mod can stop that; use a plain click. The fix belongs in the terminal host: do not open a link while the app has mouse tracking on, or do not send the app the release of a press the host took for a link.
- **The main screen** (`CLAUDE_CODE_NO_FLICKER=0`) reports no clicks to Claude Code: the reply stays Claude Code's own row, the links added, and a ctrl-click is the terminal's alone.
- **The row keeps Claude Code's look:** Claude Code still draws the reply's row, around a one-row stub (one zero-width space), with its own top margin and bullet; the mod's `Markdown`, lifted one row (`marginTop: -1`), covers the stub's row from where the text starts. The engine's margin depends on where the row stands (one blank row in the main transcript, none under the ctrl+o transcript's time and model line, none after an agent team's speaker label), and no hook is told; this way the mod never has to know it. 0.5.0 drew the row itself and added a blank row under every time and model line of the ctrl+o view. Checked live on 2.1.311: a long reply (headings, lists, a table, a quote, a code block, links) at 120 and 200 columns, and a turn of replies between Read and Bash calls with chong and better-verbose-bash seated beneath, matched Claude Code's own drawing row by row in the main view, the ctrl+o view and back. A summary row (` · summary`), a reply with no `file:` link of this machine, and a reply holding a character a `Markdown` refuses (a carriage return) stay Claude Code's own rows, links added. Where a plugin seated beneath draws the stub its own way (no engine row to lay the text over), the mod draws the whole row as 2.1.311 does: one blank row, the bullet (`●`, `⏺` on macOS) in the theme's text color, the text beside it. Footgun: a plugin seated beneath file-links at `AssistantMessage` sees the stub, not the reply's text. The `plainClickOpens` option (`/config`) turns the whole path off.
- **No hover tooltip:** a `Markdown` element is a leaf, and a hover applies to a whole keyed `Box`, so no mod can show a tooltip for one link inside a reply. A Windows Terminal host shows its own tooltip with the link's URI.

Without hyperlink support, the engine draws a link as `text (file:///...)`. A long URL after every name is clutter, so in such a terminal the mod leaves the reply as written. The desktop app and VS Code draw anchors, so there the mod always links. A phone gets no links: it cannot open the computer's files. The mod decides as Claude Code 2.1.292 does (`hyperlinksOf`, from the same environment variables):

| Hyperlinks on | Hyperlinks off |
| --- | --- |
| Windows Terminal and hosts that set `WT_SESSION` | Windows without `WT_SESSION` (the plain console; VS Code's terminal on Windows) |
| macOS: iTerm2, Ghostty, WezTerm, kitty, Alacritty, Warp, Hyper, VS Code ≥ 1.72, JetBrains, tmux ≥ 3.4 | macOS: Apple Terminal, tmux < 3.4, any other terminal |

`FORCE_HYPERLINK=1` turns hyperlinks on for Claude Code and the mod together, and `FORCE_HYPERLINK=0` turns them off. `/file-links` says which variable decided.

## Platforms

The plugin API names no OS, so the mod reads it from the session's folders: a drive (`C:\`) means Windows, a leading `/` means macOS (or Linux).

- **Windows:** `C:\x`, `C:/x` and the Git Bash form `/c/x` are full paths. The home folder is `USERPROFILE`. Paths compare without case, as NTFS does.
- **macOS:** `/x` is a full path, and `/c/x` is the folder `c` under the root. A drive path (`C:\x`) is no path there. The home folder is `HOME`. Names compare without case in the index and the touched files. On a case-sensitive volume, a name with other case still links to the one real file the index holds.
- **Both:** `~/x`, relative paths, bare names and the `file:///` URIs (`file:///C:/x` on Windows, `file:///Users/x` on macOS).
- **git:** the mod looks for a `.git` folder (or file) at or above the project root before it runs `git`. On a Mac without the command-line developer tools, any `git` call opens the system's install dialog, so no repository means no `git` call.

## How it finds files

- **Touched files:** a `tool.call` hook on `Read`, `Edit`, `Write` and `NotebookEdit` notes each file path, newest last. At `session.start` the mod also reads the touched files of a resumed session from `$.session.messages()`. The hook hands each call on unchanged; its `.catch` hands the call on if the note fails.
- **Git index:** `git ls-files -z --cached --others --exclude-standard` in the repository of the project root (found by its `.git`), at `session.start` and after a turn ends (a 15 s poll). Ignored files are not in it, and neither are tracked files deleted from the disk (`git ls-files -z --deleted` takes them out, since `--cached` still lists them). When a new index can place a span that an earlier draw left plain, the mod redraws the replies once.
- **Disk:** `$.fs.exists` for full paths and for paths under the three folders (working folder, project root, scratchpad). A "not there" answer is dropped when a file may have appeared (a `Write`, a new index).
- **End of a known folder** (0.5.0): a relative path that nothing above placed, and a `/x` path that is not there (on Windows never a path of this machine, unless Git Bash's `/c/x`), are matched against the scratchpad, the working folder, the project root and the home folder: when the path's first segments are a folder's last ones (without case), the rest is looked for under that folder, the longest overlap first.
- **Scratchpad:** the session's scratchpad (the folder Claude Code gives Claude for temporary files) is found by Claude Code's own layout, as 2.1.292 builds it. No plugin API gives that path:
  - the temp root: `CLAUDE_CODE_TMPDIR`, else the system temp folder (`os.tmpdir()`: `TEMP`, `TMP` on Windows; `TMPDIR`, `TMP`, `TEMP`, `/tmp` on macOS); then `claude` below it, at its real path (`/var` resolves to `/private/var` on macOS);
  - the project folder: the session's original working folder with every character outside `[a-zA-Z0-9]` as `-` (longer than 200 characters: cut to 200, then `-` and a base-36 hash);
  - then `<session id>/scratchpad`. Claude Code names it after the ORIGINAL working folder, which no API gives, so the mod tries the folder at `session.start`, the working folder and the project root, and takes the first scratchpad that exists. A new session id (after `/clear`) gets its own lookup.
  - Footgun: this copies one build's layout. If a later Claude Code moves the scratchpad, `/file-links` says "scratchpad: none found" and its files link only by full path, as before 0.4.0.
- **Cache:** the text drawn for each reply text is kept (600 texts at most), so a redraw (a scroll) costs one lookup.

## Seat order with chat-view

chat-view matches transcript rows by their text. A plugin seated beneath `file-links` at `AssistantMessage` sees the linked text, not the stored text, and where the mod draws a reply itself (a plain-click reply in the fullscreen terminal) no plugin beneath it draws that reply at all. So `file-links` must sit beneath chat-view:

- An installed `file-links` that `enabledPlugins` lists after `chat-view` sits beneath it. Seen live: the engine's chain is `chat-view@…, file-links@…`.
- A folder plugin (`--plugin-dir`, `CLAUDE_CODE_PLUGIN_DIRS`, or a hot-reloaded mods folder) sits above every installed plugin. Then, in a chat-view view that folds earlier exchanges (Minimal, or a Custom view with that part), an earlier reply that holds a link stays unfolded. chat-view keeps a text it cannot match open. The Default view does not match texts, so there it does no harm.

## Command and option

`/file-links` shows what the mod knows: the folders, the platform, whether the terminal draws hyperlinks (and the variable that decided), the session's scratchpad (or "none found"), the index (file count, repository, age), the touched files, and the cache sizes.

`plainClickOpens` (in `/config`, on by default): whether, in the fullscreen terminal, the mod draws replies with file links itself so that a plain click opens a link (see [How a click opens a link](#how-a-click-opens-a-link)). Off, replies are Claude Code's own rows with the links added.

## Tests

- `claude plugin validate plugins/file-links`
- `claude plugin test plugins/file-links`: 90 tests, with Windows and macOS sessions, on the 2.1.292, 2.1.308 and 2.1.311 test kits. On a machine where `claude` on `PATH` is a wrapper, run the real `claude` executable. Run against 0.4.1, the tests of 0.5.0's changes fail (14 of 34 in register.test.ts; links.test.ts does not load); run against 0.5.0, the tests of 0.5.1's changes fail (7 in register.test.ts; links.test.ts does not load). So they test the changes.
- Live proof (Windows, 2.1.311, fullscreen, 0.5.0): against a fake Messages API, a reply's `notes.md`, `sub/data.json`, `projects/`, `.\notes.md` and `C:/Windows/win.ini` drew as 9 OSC 8 links with the targets above (folder parts ending in `/`); a plain click on the reply's own link to a missing file showed `file-links: C:/nowhere/ghost.md is not there`; a plain click on `projects/` opened one Explorer window on that folder.
- Live QA (Windows, 2.1.311, fullscreen, 0.5.1), each drawing compared row by row with Claude Code's own (the same scenario with no file-links): a long reply at 120 and 200 columns; a turn of text-then-tool messages (Read, Bash) and a streamed last reply, with better-verbose-bash and chong (your options) seated beneath, in the main view, the ctrl+o view and back: no row differs. Plain clicks on folders named with a space, `&` and a comma, `#` and `%`, parentheses, Hebrew and `~` each opened that folder, and a click on a script showed it selected in its folder; a ctrl-click was Claude Code's own (`explorer /select`, its folder with the item selected) and ran nothing of the mod's. Every Explorer window a run opened was found and closed afterwards. A ctrl-click in a real Windows Terminal host was not driven (the bench's terminal is a pseudo-console).
- Type check: `tsc -p plugins/file-links`, after an interactive session or a `--plugin-dir` run laid `.claude-plugin/types/`.
- Live proof (Windows, 2.1.292): `claude --plugin-dir <mod folder>` in a pseudo-terminal, against a fake Messages API that streams a reply full of paths. The raw output held 10 OSC 8 hyperlinks: each path form in the table above, a name that two folders hold, the reply's own `file:///` link, and an `https:` URL that the engine links by itself. With `WT_SESSION` removed it held 0, and the engine drew the reply's own link as `CLAUDE.md:27 (file:///...)`, the clutter the mod avoids. Installed after chat-view, the chain read `chat-view@…, file-links@…`. macOS is covered by the kit's POSIX sessions, not by a live Mac run.
- Live scratchpad proof (Windows, 2.1.292): `claude --session-id <uuid>` with `CLAUDE_CODE_TMPDIR` set, the scratchpad folder created first where the layout puts it. A reply's bare `notes.txt` and `run1/summary.json` linked into it, a missing name stayed plain, and `/file-links` named that folder. The layout functions also rebuild a real session's scratchpad path exactly, from its `TEMP`, its working folder and its id.
- To try an edit in a running session: load the `plugin-authoring` skill, put the mod in the session's mods folder (`~/.claude/dev-mods/<session id>/`), and answer "Enable for this session" to the engine's question. The session then reloads the mod when its files change.

## Versions

- 0.5.1: QA fixes. Claude Code draws the reply's own row around a stub and the mod's text covers it, so the row keeps the engine's margin in every view (0.5.0 added a blank row under each time and model line of the ctrl+o view). On Windows a click opens through PowerShell, the path in an environment variable (`explorer <path>` opened Documents for a folder named with a comma). A failed opener says why; a `file:` link to another machine and a reply holding a carriage return keep Claude Code's own behaviour.
- 0.5.0: a path with a folder part draws two links (folder and file); a plain click in the fullscreen terminal opens a file link once, through the mod (a program or script is shown in its folder, never run), with the `plainClickOpens` option; a folder name written with its slash (`projects/`) links; the end of a known folder (`<session id>/scratchpad`, with or without a leading slash) links into it; a path written as a folder never links to a file of that name.
- 0.4.1: review fixes (ICSkills PR 43212): a scratchpad that a shell command makes mid-session is found after the turn, and replies drawn before link then; the temp folder must be a folder; its real path is kept once resolved.
- 0.4.0: names and relative paths also link to files in the session's scratchpad, found by Claude Code 2.1.292's folder layout; `/file-links` names the scratchpad.
- 0.3.2: review fixes (ICSkills PR 43139): a turn that ends while an index build runs is no longer lost; the touched files and the path answers are capped; a malformed tool path is skipped.
- 0.3.1: documentation only (this README for a shared repo).
- 0.3.0: macOS support (POSIX paths, `HOME`, no `git` call outside a repository), the terminal hyperlink rule, one shared first read of the folders.
- 0.2.0: when two or more folders hold the file, the shortest path wins (0.1.0 linked nothing on a tie). Tracked files deleted from the disk are not in the index.
- 0.1.0: the first version.

## Files

- `hooks/links.ts`: the pure part. It finds the spans (`scan`), reads a span as a path (`pathOf`, `isFolderPath`), writes the links (`linkify`, `linkPartsOf`), spells paths and URIs (`fileUri`, `folderUri`, `pathOfFileUri`, `normalize`, `absoluteOf`, `bestOf`, `parentOf`), matches the end of a known folder (`anchoredCandidatesOf`), lists a text's file links and the ones a click is answered for (`fileHrefsOf`, `pressableHrefsOf`), tells what a click must not run (`runsWhenOpened`), tells the platform (`platformOf`), and decides whether the terminal draws hyperlinks (`hyperlinksOf`).
- `hooks/register.ts`: the hooks, the file lookup, the reply a plain click opens links in (`overlaidReplyOf` over the engine's row, `ownReplyRowOf` when there is none), and the opener (`openPressed`, `openerOf`).
- `tests/links.test.ts`, `tests/register.test.ts`: the tests.
