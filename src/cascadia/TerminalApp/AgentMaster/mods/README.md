# The bundled mods (vendored)

The Claude Code mods Agentmaster ships inside `TerminalApp.dll` and loads into every Claude tab it launches
(`--plugin-dir`). The design, the launch rules and the measurements: [`doc/agentmaster/BUNDLED_MODS.md`](../../../../../doc/agentmaster/BUNDLED_MODS.md).

- `bundled-mods.json` lists them, in `--plugin-dir` order, with the oldest Claude Code each one runs on and the line the
  cog shows for it.
- Each folder is a copy of the owner's mod (`~/.claude/mods/<name>`, their source of truth and a git repository of its
  own), taken by `python tools/bundled-mods/bundled_mods.py sync`. Edit the source, then `sync`; an edit made only here
  is lost at the next `sync`.
- The engine does not read these files at run time: `python tools/bundled-mods/bundled_mods.py gen` turns each mod's
  runtime files (`.claude-plugin/plugin.json` and `hooks/`, tests left out) into `../BundledModsData.h`, which is what
  ships. Run `gen` after every `sync` (or `check` to see whether the header is stale); the m5 harness's `TestBundledMods`
  fails when the header's bytes no longer match their recorded hash.
- Each mod's own README is its full reference; test a mod here with `claude plugin test <folder>`.
