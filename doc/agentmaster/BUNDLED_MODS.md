# Bundled mods — Claude Code mods that ship with Agentmaster

Agentmaster carries a few Claude Code **mods** (plugins whose hooks are a TypeScript module, Claude Code 2.1.287+)
inside `TerminalApp.dll` and loads them into every Claude tab it launches, with `--plugin-dir`. They work in any
Claude Code session; Agentmaster ships them so its tabs have them without an install.

| Mod | Version | Needs | What it does in a tab |
|---|---|---|---|
| **file-links** | 0.5.1 | Claude Code 2.1.292 | Draws the file paths and file names in Claude's replies as clickable `file:///` links (OSC 8; Windows Terminal draws them), only for names it finds: on disk, among the files the session touched, in its scratchpad, in its git index, at the end of a folder the session knows (`<session id>/scratchpad`), a folder name written with its slash (`projects/`). A path with a folder part draws two links: the folder part opens the folder, the name opens the file. In the fullscreen terminal a plain click on a file link opens it once, through the mod (Windows: PowerShell's `Invoke-Item`; a program or script is shown in its folder, never run). Display only. `/file-links` reports what it knows. |
| **better-verbose-bash** | 0.2.0 | Claude Code 2.1.308 | Wherever Claude Code draws a shell call's own `Bash(command)` row, in any view (verbose, ctrl+o, or a compact view's row), the reason the model gave for the call becomes the row's bulleted header. A call folded into a count line has no such row. Display only. |
| **agentmaster-link** | 0.1.0 | Claude Code 2.1.322 | The tab's end of the **session link** ([`MOD_LINK.md`](MOD_LINK.md) §14): it polls Agentmaster's loopback server (`AM_LINK`, `AM_LINK_TOKEN`, set per launch) and runs what Agentmaster asks of the session's own claude — so far Telegram's `/btw` side question, through Claude Code's `$.model.fork` (MESSAGING.md §21.56). Draws nothing; hooks only `session.start`; does nothing in a claude Agentmaster did not launch. |
| **prompt-cut** | 0.2.0 | Claude Code 2.1.310 | Cut for the prompt box: the key bound to `command:prompt-cut` (ctrl+x ctrl+x in the user's own `keybindings.json`, §9) puts the whole draft on the clipboard and empties the box, also while Claude works. A `[Pasted text #N +M lines]` placeholder goes to the clipboard as the text it stands for. When a paste's text is unknown, the key arms Enter instead (`[Enter cuts ✕]` on the hint row): the armed Enter is copied, pastes expanded, and dropped, never sent. Images stay in the box; no transcript row. Without the key it does nothing. |

**file-links**, **better-verbose-bash** and **prompt-cut** come from the owner's mods folder (`~/.claude/mods/<name>`,
a git repository of its own, where each one's README is the full reference); the repository keeps a vendored copy
(§7). **agentmaster-link** is Agentmaster's own: its source IS the repository's copy (`"source": "repo"` in the
manifest, which `sync` leaves alone), with its README beside it.

## 1. What a launch does

Every managed Claude launch — a new tab, a resume, a fork, a restore, a Restart, a /handover successor, a preset
launch — is built by `BuildClaudeSpawn` or `BuildClaudeRestartSpec` (`ClaudeSpawn.cpp`). Both ask the **provider**
(`SetBundledModArgsProvider`, set once per process to `BundledModPluginArgs` by `TerminalPage.AgentEngine.cpp`) for
the launch's `--plugin-dir` arguments and pass them to `BuildClaudeCommandline` (`pluginArgs`), right after the
fresh / resume / fork form:

```
"<claude.exe>" --dangerously-skip-permissions --resume <id> --settings "<hooks file>"
    --plugin-dir "C:/Users/me/.agentmaster/mods/file-links/0.5.1-bc85bf53"
    --plugin-dir "C:/Users/me/.agentmaster/mods/better-verbose-bash/0.2.0-18bafdb3"
    --plugin-dir "C:/Users/me/.agentmaster/mods/agentmaster-link/0.1.0-8c83784b"
    --plugin-dir "C:/Users/me/.agentmaster/mods/prompt-cut/0.2.0-b84a6dac" [--model …] [preset args] ["prompt"]
```

The same builders add the session link's two variables to the launch's environment (`AppendSessionLinkEnv`, through
the link's own provider, `SetSessionLinkEnvProvider`): `AM_LINK` and a token minted for that launch, `AM_LINK_TOKEN`.
Every launch gets them while the link's server runs, whether agentmaster-link loads or not; without the mod nothing
reads them (MOD_LINK.md §14).

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
[mods] 5b0c3d1e file-links 0.5.1 stands aside: Claude Code loads your own file-links@claude-mods (user settings); better-verbose-bash 0.2.0 loads; agentmaster-link 0.1.0 loads; prompt-cut 0.2.0 stands aside: Claude Code loads your own prompt-cut@claude-mods (user settings)
```

Only the app sets the provider. The other programs that compile `ClaudeSpawn.cpp` (the CLI, the session database, the
m5 harness, the probes) leave it unset: they link no `BundledMods.cpp` and load no mod. Codex tabs, the AI search's
harness, the `/status` probe and the voice correction run their own `claude.exe` command lines and load none. A
`claude` typed by hand in a tab (through the PATH shim) gets none either: the shim adds the hooks only.

## 2. The folders

`EnsureBundledModsMaterialized` writes each mod once per process, at the first launch that loads one, into
`<profile>\mods\<name>\<version>-<hash8>\` (`BundledModFolderLeaf`; the hash is the SHA-256 of the mod's embedded
files, so a changed file gives a new folder):

- **What goes in:** the mod's runtime files only — `.claude-plugin/plugin.json`, its `hooks/` (no tests, no README) and
  the contract its plugin.json names under `types` (prompt-cut's `types/index.d.ts`, which declares its `$.state`
  families: Claude Code's loader refuses a mod whose named contract is missing, §6), byte for byte as embedded (UTF-8,
  LF line ends).
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
  chat-view's line over the header. Verified live with chat-view installed (§6). 0.2.0 no longer asks whether the view
  is verbose: every shell `ToolUse` row Claude Code draws gets the header, and a build that does not say how it draws
  the row (2.1.292 and older send no `isExpanded`) keeps its rows as they were.
- **file-links** should sit beneath chat-view, which matches transcript rows by their text: above it, a chat-view view
  that folds earlier exchanges (Minimal, or a Custom view with that part) keeps an earlier reply that holds a link
  unfolded. The Default view is unaffected. On a machine where file-links is installed too (the owner's), ours stands
  aside (§3) and theirs keeps its seat. 0.5.1 was checked in this seat: above better-verbose-bash and chong, in the main
  view, the ctrl+o view and back, its drawing matched Claude Code's own row for row (the mod's commit, 2.1.311 bench).
- **prompt-cut** comes last and hooks no row the others draw. On the prompt's hint row (`ui.render` on `PromptHint`,
  only while Enter is armed) it follows the protocol prompt-queue, chat-view and next-prompt share: the outermost draws
  the line's text once and hands the ones beneath an empty hint, then adds their parts and its `[Enter cuts ✕]`, so
  any seat works. Its `prompt.submit` hook acts only on an armed Enter (the person pressed the key, then Enter); an
  unarmed Enter passes untouched, so a send through Agentmaster's delivery is never taken.

## 5. The setting

The cog's **Claude** tab, **MODS IN CLAUDE TABS**: one switch per bundled mod, on by default, and a status line saying
what the next launch does with each (a preview from the switches' positions: the user and managed scopes, the detected
`claude.exe`; nothing is written). `AppSettings::bundledModsOff` holds the names switched off; empty — the default, and
every `settings.json` from before the feature — loads them all. A name this build does not ship is kept on Save, so a
newer build's mod turned off stays off. A Save applies to the next launch, resume or restart; an open tab keeps the
mods it started with. With **agentmaster-link** off (or not loaded: a Claude Code older than 2.1.322, the user's own
copy), a tab still gets its link variables, nothing answers them, and Telegram's `/btw` is answered by a headless fork
instead of the session's own claude (MESSAGING.md §21.56).

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

**Claude Code 2.1.322, 2026-11-17** (file-links 0.5.1, better-verbose-bash 0.2.0 and prompt-cut 0.2.0 joining):

- `claude plugin validate --strict` on prompt-cut's `plugin.json` and `hooks/` alone, the set `gen` embedded until
  then: `types: Path not found: ./types/index.d.ts. The runtime loader will report this as a load failure.` With the
  file: passed (`declares state: prompt-cut.armed, prompt-cut.paste, prompt-cut.pasteIds`). So `gen` embeds the contract
  a plugin.json names under `types` (§2).
- Each of the four mods' embedded files, written out as the header holds them (what §2 writes into the profile):
  `validate --strict` passed for all four (the plugin directory's README notes aside, which do not change the result).
- Each vendored mod's own kit on a copy of its folder (`claude plugin test`): file-links 90 pass, better-verbose-bash
  21 pass, prompt-cut 35 pass, none failing (the counts their own commits report). Not run: a live tab of either
  instance, and the m5 harness (no build was made).

## 7. Updating a mod

The vendored copy is `src/cascadia/TerminalApp/AgentMaster/mods/<name>/` (the mod's files: plugin.json, hooks, tests,
README), with the manifest `mods/bundled-mods.json` (name, `minClaudeCode`, the cog's `summary`, in `--plugin-dir`
order). The engine embeds it through the generated `AgentMaster/BundledModsData.h` (only `BundledMods.cpp` includes it):

```
python tools/bundled-mods/bundled_mods.py sync    # copy each listed mod from ~/.claude/mods (or --from <folder>)
python tools/bundled-mods/bundled_mods.py gen     # write BundledModsData.h (C++ literals, the hash, the versions)
python tools/bundled-mods/bundled_mods.py check   # exit 1 when the header is stale
```

- **agentmaster-link** is edited where it is (`mods/agentmaster-link/`), then `gen`: `sync` skips a mod whose manifest
  entry says `"source": "repo"`.
- Test a mod itself with its own kit: `claude plugin test <mod folder>` and `claude plugin validate --strict <mod folder>`
  (agentmaster-link: 11 tests, and `--strict` passes).
- `TestBundledMods` (the m5 harness, `tests_bundled_mods.cpp`) recomputes each embedded mod's hash, so a header that no
  longer matches its recorded hash fails there. It also pins the catalog's count, order and two minimums, and checks
  that a contract a plugin.json names under `types` is embedded.
- `gen` embeds a mod's `plugin.json`, its `hooks/` (no tests) and the contract its plugin.json names under `types`; a
  named contract that is missing, or outside the mod's folder, stops `gen` (the loader would refuse the mod).
- To ship another mod: add it LAST in the manifest (a folder plugin listed earlier sits above the ones after it, §4)
  with its minimum Claude Code and summary (ASCII, no double quote or backslash: `gen` writes it into a C++ literal as it
  stands), `sync`, `gen`, give it a catalog line here, and move `TestBundledMods`' count and order along. The cog builds
  its switch from the catalog.
- Before committing a new or updated mod, prove its embedded set loads: write each mod's files as the header holds them
  into a scratch folder (`bundled_mods.load_mods(bundled_mods.load_manifest())`, each `ModFile` under its mod's folder),
  then `claude plugin validate --strict` each one; run `claude plugin test` on a COPY of the vendored folder (Claude Code
  writes `.claude-plugin/types/` into a folder it loads, and a `tsconfig.json` when the mod has none; no ignore rule
  here covers either).

## 8. Files

| Where | What |
|---|---|
| `AgentMaster/BundledMods.{h,cpp}` | The catalog, the embedded files, the pure rules (versions, `enabledPlugins`, `CLAUDE_CODE_PLUGIN_DIRS`, the arguments), the folders, the per-launch choice, the provider. |
| `AgentMaster/BundledModsData.h` | GENERATED: every runtime file of each mod as C++ literals, with its name, version, minimum, summary and hash. |
| `AgentMaster/mods/` | The vendored mods, agentmaster-link's own source, and `bundled-mods.json`. |
| `tools/bundled-mods/bundled_mods.py` | `sync` (skips `"source": "repo"`), `gen`, `check`. |
| `ClaudeSpawn.{h,cpp}` | `BuildClaudeCommandline`'s `pluginArgs`, the provider seam (`BundledModArgsProvider`, `SetBundledModArgsProvider`), its call in both builders; the session link's env seam (`SessionLinkEnvProvider`, `SetSessionLinkEnvProvider`, `AppendSessionLinkEnv`). |
| `AgentMaster/SessionLink.{h,cpp}` | The server agentmaster-link polls (MOD_LINK.md §14). |
| `TerminalPage.AgentEngine.cpp` | Sets the providers once per process; starts the link's server. |
| `AgentManagerContent.Settings.cpp` | The Claude tab's switches and status line. |
| `tests/tests_bundled_mods.cpp` | `TestBundledMods`. |

## 9. prompt-cut's key binding

prompt-cut answers a command, `/prompt-cut` (hidden from the typeahead), and does its work only when a KEY runs it. A
mod cannot ship a key binding: Claude Code 2.1.322's plugin API has no way to add one, and a `Button`'s `action` takes
only Claude Code's own action names (`app:…`), never `command:<name>`. The key lives in the user's own
`<CLAUDE_CONFIG_DIR | ~/.claude>\keybindings.json`:

```json
{
  "bindings": [
    { "context": "Chat", "bindings": { "ctrl+x ctrl+x": "command:prompt-cut" } }
  ]
}
```

- **Why ctrl+x ctrl+x:** a single `ctrl+x` never fires while any of Claude Code's `ctrl+x` chords is bound
  (`ctrl+x enter`, `ctrl+x tab`, `ctrl+x ctrl+e`, …): the chord prefix wins (the mod's README).
- **Agentmaster writes nothing there** (the owner's choice): no new write into Claude Code's files (*Runtime state dir*
  in CLAUDE.md lists them). A user without the line gets a mod that loads and does nothing; the cog's summary line names
  the binding.
- **With the line but without the mod** (a claude Agentmaster did not launch, a claude older than 2.1.310, or the mod
  off in the cog), the key prints `Unknown command: /prompt-cut`: a display-only row, the draft kept, nothing sent.
- **On the owner's PCs** the line is committed in their `~/.claude`. Where the owner's own `prompt-cut@claude-mods` is
  on, ours stands aside (§3).
- **Agentmaster's own typing** (the Prompt Queue Runner's fills, the draft swap, a restored or carried draft): Claude
  Code turns a long fill into a placeholder, and prompt-cut reads the clipboard for it as for any paste. The clipboard
  holds something else, so the placeholder is recorded unread and the key arms Enter for that draft (the mod's README
  names Agentmaster among the sources of such pastes). ⚠ The mod keeps the clipboard's text when its line count FITS
  the placeholder (`fits`, `hooks/register.ts`): a fill whose line count happens to equal the clipboard's would be cut
  with the clipboard's text in its place. A fill changes the draft, which takes an armed Enter back, so a queued
  prompt's own Enter is never taken. By the mod's code, not seen live in an Agentmaster tab.
