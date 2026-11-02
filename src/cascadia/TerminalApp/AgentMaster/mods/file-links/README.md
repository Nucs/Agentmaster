# file-links

A Claude Code mod. It draws the file paths and file names in Claude's replies as clickable `file:///` links, on Windows and macOS. Where a `CLAUDE.md` rule asks replies to write every file as a `file:///` link, the model can write the plain path instead, and the mod draws the link.

Display only. The mod changes the text that the terminal draws. The stored reply, the transcript, and the text that the model reads back do not change.

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
| Relative path | `hooks/register.ts`, `./x/y`, `mods/chat-view/` | The working folder, the project root, then the session's scratchpad, then the files the session touched, then the git index |
| File name | `register.ts`, `README.md`, `.gitignore` | The newest file of that name that the session touched, then the working folder, the project root, the session's scratchpad, then the git index |

- A span can be a code span (`` `x.md` ``) or bare text (`x.md`). A code span keeps its backticks inside the link, so it still draws as code.
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
- Your own prompts and the tool rows (`● Read(path)`): the mod draws only Claude's replies.

## How a link looks

The engine draws a Markdown link as an OSC 8 hyperlink in blue when the terminal supports hyperlinks. A click opens the file the way the terminal (or Claude Code's fullscreen view) opens a `file:` URI.

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
- **Scratchpad:** the session's scratchpad (the folder Claude Code gives Claude for temporary files) is found by Claude Code's own layout, as 2.1.292 builds it. No plugin API gives that path:
  - the temp root: `CLAUDE_CODE_TMPDIR`, else the system temp folder (`os.tmpdir()`: `TEMP`, `TMP` on Windows; `TMPDIR`, `TMP`, `TEMP`, `/tmp` on macOS); then `claude` below it, at its real path (`/var` resolves to `/private/var` on macOS);
  - the project folder: the session's original working folder with every character outside `[a-zA-Z0-9]` as `-` (longer than 200 characters: cut to 200, then `-` and a base-36 hash);
  - then `<session id>/scratchpad`. Claude Code names it after the ORIGINAL working folder, which no API gives, so the mod tries the folder at `session.start`, the working folder and the project root, and takes the first scratchpad that exists. A new session id (after `/clear`) gets its own lookup.
  - Footgun: this copies one build's layout. If a later Claude Code moves the scratchpad, `/file-links` says "scratchpad: none found" and its files link only by full path, as before 0.4.0.
- **Cache:** the text drawn for each reply text is kept (600 texts at most), so a redraw (a scroll) costs one lookup.

## Seat order with chat-view

chat-view matches transcript rows by their text. A plugin seated beneath `file-links` at `AssistantMessage` sees the linked text, not the stored text. So `file-links` must sit beneath chat-view:

- An installed `file-links` that `enabledPlugins` lists after `chat-view` sits beneath it. Seen live: the engine's chain is `chat-view@…, file-links@…`.
- A folder plugin (`--plugin-dir`, `CLAUDE_CODE_PLUGIN_DIRS`, or a hot-reloaded mods folder) sits above every installed plugin. Then, in a chat-view view that folds earlier exchanges (Minimal, or a Custom view with that part), an earlier reply that holds a link stays unfolded. chat-view keeps a text it cannot match open. The Default view does not match texts, so there it does no harm.

## Command

`/file-links` shows what the mod knows: the folders, the platform, whether the terminal draws hyperlinks (and the variable that decided), the session's scratchpad (or "none found"), the index (file count, repository, age), the touched files, and the cache sizes.

## Tests

- `claude plugin validate plugins/file-links`
- `claude plugin test plugins/file-links`: 63 tests, with Windows and macOS sessions, on the 2.1.287, 2.1.292 and 2.1.296 test kits. On a machine where `claude` on `PATH` is a wrapper, run the real `claude` executable.
- Type check: `tsc -p plugins/file-links`, after an interactive session or a `--plugin-dir` run laid `.claude-plugin/types/`.
- Live proof (Windows, 2.1.292): `claude --plugin-dir <mod folder>` in a pseudo-terminal, against a fake Messages API that streams a reply full of paths. The raw output held 10 OSC 8 hyperlinks: each path form in the table above, a name that two folders hold, the reply's own `file:///` link, and an `https:` URL that the engine links by itself. With `WT_SESSION` removed it held 0, and the engine drew the reply's own link as `CLAUDE.md:27 (file:///...)`, the clutter the mod avoids. Installed after chat-view, the chain read `chat-view@…, file-links@…`. macOS is covered by the kit's POSIX sessions, not by a live Mac run.
- Live scratchpad proof (Windows, 2.1.292): `claude --session-id <uuid>` with `CLAUDE_CODE_TMPDIR` set, the scratchpad folder created first where the layout puts it. A reply's bare `notes.txt` and `run1/summary.json` linked into it, a missing name stayed plain, and `/file-links` named that folder. The layout functions also rebuild a real session's scratchpad path exactly, from its `TEMP`, its working folder and its id.
- To try an edit in a running session: load the `plugin-authoring` skill, put the mod in the session's mods folder (`~/.claude/dev-mods/<session id>/`), and answer "Enable for this session" to the engine's question. The session then reloads the mod when its files change.

## Versions

- 0.4.1: review fixes (ICSkills PR 43212): a scratchpad that a shell command makes mid-session is found after the turn, and replies drawn before link then; the temp folder must be a folder; its real path is kept once resolved.
- 0.4.0: names and relative paths also link to files in the session's scratchpad, found by Claude Code 2.1.292's folder layout; `/file-links` names the scratchpad.
- 0.3.2: review fixes (ICSkills PR 43139): a turn that ends while an index build runs is no longer lost; the touched files and the path answers are capped; a malformed tool path is skipped.
- 0.3.1: documentation only (this README for a shared repo).
- 0.3.0: macOS support (POSIX paths, `HOME`, no `git` call outside a repository), the terminal hyperlink rule, one shared first read of the folders.
- 0.2.0: when two or more folders hold the file, the shortest path wins (0.1.0 linked nothing on a tie). Tracked files deleted from the disk are not in the index.
- 0.1.0: the first version.

## Files

- `hooks/links.ts`: the pure part. It finds the spans (`scan`), reads a span as a path (`pathOf`), writes the links (`linkify`), spells paths and URIs (`fileUri`, `normalize`, `absoluteOf`, `bestOf`, `parentOf`), tells the platform (`platformOf`), and decides whether the terminal draws hyperlinks (`hyperlinksOf`).
- `hooks/register.ts`: the hooks and the file lookup.
- `tests/links.test.ts`, `tests/register.test.ts`: the tests.
