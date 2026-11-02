# Bundled mods — Claude Code mods that ship with Agentmaster

Agentmaster carries a few Claude Code **mods** (plugins whose hooks are a TypeScript module, Claude Code 2.1.287+)
inside `TerminalApp.dll` and loads them into every Claude tab it launches, with `--plugin-dir`. They work in any
Claude Code session; Agentmaster ships them so its tabs have them without an install.

| Mod | Version | Needs | What it does in a tab |
|---|---|---|---|
| **file-links** | 0.4.1 | Claude Code 2.1.287 | Draws the file paths and file names in Claude's replies as clickable `file:///` links (OSC 8; Windows Terminal draws them), only for names it finds: on disk, among the files the session touched, in its scratchpad, in its git index. Display only. `/file-links` reports what it knows. |
| **better-verbose-bash** | 0.1.1 | Claude Code 2.1.308 | In the views that draw a shell call in full (`--verbose`, Verbose output in `/config`, ctrl+o), the reason the model gave for the call becomes the row's bulleted header, over `Bash(command)`. Display only. |

Their source is the owner's mods folder (`~/.claude/mods/<name>`, a git repository of its own, where each one's
README is the full reference); the repository keeps a vendored copy (§7).

## 1. What a launch does

Every managed Claude launch — a new tab, a resume, a fork, a restore, a Restart, a /handover successor, a preset
launch — is built by `BuildClaudeSpawn` or `BuildClaudeRestartSpec` (`ClaudeSpawn.cpp`). Both ask the **provider**
(`SetBundledModArgsProvider`, set once per process to `BundledModPluginArgs` by `TerminalPage.AgentEngine.cpp`) for
the launch's `--plugin-dir` arguments and pass them to `BuildClaudeCommandline` (`pluginArgs`), right after the
fresh / resume / fork form:

```
"<claude.exe>" --dangerously-skip-permissions --resume <id> --settings "<hooks file>"
    --plugin-dir "C:/Users/me/.agentmaster/mods/file-links/0.4.1-8d36d97c"
    --plugin-dir "C:/Users/me/.agentmaster/mods/better-verbose-bash/0.1.1-ddeb9b00" [--model …] [preset args] ["prompt"]
```

The decision is made per launch and per mod (`ChooseBundledMods`), never stored with the session, so a resume always
gets the current build's mods. In order:

1. **Off** — the cog's switch is off (`AppSettings::bundledModsOff`, §5).
2. **Claude Code unknown** — no `claude.exe`, or no version can be read from it (`ReadClaudeFileVersion`, cached by
   the file's size and write time). Never a flag a claude may not know: one from before `--plugin-dir` refuses the
   whole command line and the tab dies at start.
3. **Claude Code too old** — older than the mod's minimum (`bundled-mods.json`'s `minClaudeCode`).
4. **The user's own copy** (§3) — theirs runs, ours stands aside.
5. **No folder** — its folder could not be written into the profile (§2; logged).
6. Else it **loads**.

Each launch writes one line to `hooks.log`:

```
[mods] 5b0c3d1e file-links 0.4.1 stands aside: Claude Code loads your own file-links@claude-mods (user settings); better-verbose-bash 0.1.1 loads
```

Only the app sets the provider. The other programs that compile `ClaudeSpawn.cpp` (the CLI, the session database, the
m5 harness, the probes) leave it unset: they link no `BundledMods.cpp` and load no mod. Codex tabs, the AI search's
harness, the `/status` probe and the voice correction run their own `claude.exe` command lines and load none. A
`claude` typed by hand in a tab (through the PATH shim) gets none either: the shim adds the hooks only.

## 2. The folders

`EnsureBundledModsMaterialized` writes each mod once per process, at the first launch that loads one, into
`<profile>\mods\<name>\<version>-<hash8>\` (`BundledModFolderLeaf`; the hash is the SHA-256 of the mod's embedded
files, so a changed file gives a new folder):

- **What goes in:** the mod's runtime files only — `.claude-plugin/plugin.json` and its `hooks/` (no tests, no README),
  byte for byte as embedded (UTF-8, LF line ends).
- **A new folder** is written whole under `.tmp-<pid>-<tick>` beside it and renamed into place, so the final name never
  holds half a mod (a crash, a second Agentmaster process on the same profile). A rename that loses the race uses the
  winner's folder once it checks out.
- **An existing folder** is checked file by file; a file that differs or is missing is written again (`[mods] … rewrote
  N … file(s)`). A file that cannot be written leaves the mod unloaded for this run.
- **Claude Code writes into a folder it loads** — `.claude-plugin\types\` (the API's `.d.ts` files, with a `.gitignore`)
  and a `tsconfig.json` — and watches it. Those files are not ours and are never checked or removed.
- **Older versions' folders stay** (a folder never changes under a running claude that loaded it). Nothing removes
  them yet: each one is ~1 MB with Claude Code's types.

## 3. The user's own copy

A mod the user's Claude Code already loads is left to Claude Code. Measured on 2.1.310 (§6), a `--plugin-dir` plugin
**replaces** an installed plugin of the same name (`Plugin "file-links" from --plugin-dir overrides installed
version`): loading ours would silently take the place of the user's copy, often a newer one or a working copy they
edit. So a launch reads what the claude will read (`ReadUsersPlugins`):

- **`enabledPlugins`** of each settings scope — user (`<CLAUDE_CONFIG_DIR | ~/.claude>\settings.json`), project
  (`<working folder>\.claude\settings.json`), local (`…\.claude\settings.local.json`) and managed
  (`%ProgramFiles%\ClaudeCode\managed-settings.json`) — merged key by key, a higher scope winning
  (`EffectiveEnabledPluginKey`). A key `<name>@<marketplace>` (or a bare `<name>`) whose effective value is `true`
  stands ours aside, whatever the marketplace (`file-links@claude-mods`, `file-links@icskills`). Names compare without
  case.
- **`CLAUDE_CODE_PLUGIN_DIRS`** as the tab's claude ends up with it: this process's environment, the launch's own
  variables on top (the cog's global env, the folder's per-directory env), then each settings scope's `env` block. A
  listed folder whose `.claude-plugin\plugin.json` names the mod stands ours aside.

Not read: the managed settings' registry key, and whether an enabled key's plugin is actually installed (an enabled key
for a missing plugin still stands ours aside). The "project" is the session's working folder, not its git root. The
files are read through a cache keyed by each file's size and write time, so a launch burst parses each once.

## 4. Seat order

The plugin listed first in `enabledPlugins` sits above the ones after it in Claude Code's hook chain, and a folder
plugin (`--plugin-dir`, `CLAUDE_CODE_PLUGIN_DIRS`) sits above every installed plugin (the mods' READMEs, "seat
order"). Both mods are written for chat-view (an installed mod that redraws the transcript in its own views):

- **better-verbose-bash** puts its header only over Claude Code's own row (the engine reference `{ type: "engine" }`
  that `next(e)` resolves to). From 0.1.1, a row that a plugin beneath drew its own way keeps that drawing: a row
  chat-view's Compact or Minimal view hides stays hidden, and in its Full and Max views the row shows chat-view's dim
  `# …` description line and no header. 0.1.0, in this seat, drew a header alone where a row was hidden and laid
  chat-view's line over the header. Verified live with chat-view installed (§6).
- **file-links** should sit beneath chat-view, which matches transcript rows by their text: above it, a chat-view view
  that folds earlier exchanges (Minimal, or a Custom view with that part) keeps an earlier reply that holds a link
  unfolded. The Default view is unaffected. On a machine where file-links is installed too (the owner's), ours stands
  aside (§3) and theirs keeps its seat.

## 5. The setting

The cog's **Claude** tab, **MODS IN CLAUDE TABS**: one switch per bundled mod, on by default, and a status line saying
what the next launch does with each (a preview from the switches' positions: the user and managed scopes, the detected
`claude.exe`; nothing is written). `AppSettings::bundledModsOff` holds the names switched off; empty — the default, and
every `settings.json` from before the feature — loads them all. A name this build does not ship is kept on Save, so a
newer build's mod turned off stays off. A Save applies to the next launch, resume or restart; an open tab keeps the
mods it started with.

## 6. Measured (Claude Code 2.1.310, 2026-11-02)

ConPTY probes through the `claude-wrap-ConPTY` driver, from a scratch folder, with the user's real configuration
(chat-view, prompt-queue, file-links, design-ask, bash-loader installed), the two folders written as §2 writes them:

- `claude.exe --dangerously-skip-permissions --plugin-dir <file-links> --plugin-dir <better-verbose-bash> --debug`
  reached the input box with **no dialog**. The debug log: `--plugin-dir … is one plugin: .claude-plugin at its top
  marks it`, `Loaded inline plugin from path`, `hooks module file-links@inline loaded (worker, …, tier user)`,
  `plugin.register: … judged by core alone: admitted`, `plugin-dir watch: watching file-links, better-verbose-bash`,
  and `Plugin "file-links" from --plugin-dir overrides installed version`.
- `/file-links` answered from the bundled copy.
- One Haiku turn with `--verbose` and a random `WT_SESSION`: the screen showed `● Print a greeting` over
  `Bash(echo hi)` with one bullet (better-verbose-bash above the installed chat-view), and the reply carried the OSC 8
  link `file:///Q:/tmp/am-bm-probe/work/note.txt` (file-links).
- The probe's own env drops `WT_SESSION` / `AGENTMASTER_PROFILE` (or sets a random `WT_SESSION`) so it never binds to a
  real tab, and sets `CLAUDE_CODE_SANDBOXED=1` so the scratch folder raises no trust dialog.

## 7. Updating a mod

The vendored copy is `src/cascadia/TerminalApp/AgentMaster/mods/<name>/` (the mod's files: plugin.json, hooks, tests,
README), with the manifest `mods/bundled-mods.json` (name, `minClaudeCode`, the cog's `summary`, in `--plugin-dir`
order). The engine embeds it through the generated `AgentMaster/BundledModsData.h` (only `BundledMods.cpp` includes it):

```
python tools/bundled-mods/bundled_mods.py sync    # copy each listed mod from ~/.claude/mods (or --from <folder>)
python tools/bundled-mods/bundled_mods.py gen     # write BundledModsData.h (C++ literals, the hash, the versions)
python tools/bundled-mods/bundled_mods.py check   # exit 1 when the header is stale
```

- Test a mod itself with its own kit: `claude plugin test <mod folder>` and `claude plugin validate --strict <mod folder>`.
- `TestBundledMods` (the m5 harness, `tests_bundled_mods.cpp`) recomputes each embedded mod's hash, so a header that no
  longer matches its recorded hash fails there.
- To ship another mod: add it to the manifest with its minimum Claude Code and summary, `sync`, `gen`, and give it a
  catalog line here. The cog builds its switch from the catalog.

## 8. Files

| Where | What |
|---|---|
| `AgentMaster/BundledMods.{h,cpp}` | The catalog, the embedded files, the pure rules (versions, `enabledPlugins`, `CLAUDE_CODE_PLUGIN_DIRS`, the arguments), the folders, the per-launch choice, the provider. |
| `AgentMaster/BundledModsData.h` | GENERATED: every runtime file of each mod as C++ literals, with its name, version, minimum, summary and hash. |
| `AgentMaster/mods/` | The vendored mods and `bundled-mods.json`. |
| `tools/bundled-mods/bundled_mods.py` | `sync`, `gen`, `check`. |
| `ClaudeSpawn.{h,cpp}` | `BuildClaudeCommandline`'s `pluginArgs`, the provider seam (`BundledModArgsProvider`, `SetBundledModArgsProvider`), its call in both builders. |
| `TerminalPage.AgentEngine.cpp` | Sets the provider once per process. |
| `AgentManagerContent.Settings.cpp` | The Claude tab's switches and status line. |
| `tests/tests_bundled_mods.cpp` | `TestBundledMods`. |
