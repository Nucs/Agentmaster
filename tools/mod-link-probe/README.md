# mod-link-probe — the Phase 0 measurements behind `doc/agentmaster/MOD_LINK.md`

Throwaway tools, kept so the measurements can be re-run on a new Claude Code build. Nothing here is
built into the app, and nothing here writes outside the folders you name.

| File | What it is |
|---|---|
| `server/amprobe-server.cs` | A .NET 10 file-based app: an HTTP/1.1 responder on 127.0.0.1 (TCP), an AF_UNIX socket and a named pipe, logging every request with its elapsed time. `/poll?ms=N` answers after N ms. It exits by itself at its deadline. |
| `mods/amprobe` | A probe mod: which transports `$.http.fetch` reaches, a 12 s long-poll inside `session.start`, a loop left running after `session.start` returns, and the payloads of `prompt.submit`, `turn.start`, `turn.complete`, `classic.Stop`, `session.end`. |
| `mods/amprobe2` | Says hello only: loaded through `CLAUDE_CODE_PLUGIN_DIRS` beside a `--plugin-dir` plugin, to see both load. |
| `mods/linklike` | What a session link would hook (session start/end, prompts, turns, the Stop payload) plus a 25 s long-poll loop. With `AMP_TCP` empty it sends nothing, which isolates the runtime's own cost. |
| `measure.ps1` | One headless Haiku turn that holds claude.exe alive in a 20 s Bash `sleep`; samples its private bytes and working set every 500 ms and prints the medians over 6–18 s. |
| `mods/libprobe` | Real npm packages vendored as ES modules (fflate, nanoid) imported by relative path, the globals a mod has, and whether `eval` / `new Function` run. It writes `<AMP_OUT>/libprobe.json`. **Populate `mods/libprobe/vendor/` first** (third-party code is not kept in the repo): copy fflate's `esm/browser.js` to `vendor/fflate.mjs`, and nanoid 3's `index.browser.js` plus `url-alphabet/index.js` to `vendor/nanoid/` (keeping that subfolder). |
| `mods/libprobe2` | A bare import (`from 'am-vendor-test'`) resolved from the plugin's own `node_modules` (a two-file stand-in package, force-added past the repo's `node_modules/` ignore). Expected: refused at load. |
| `mods/libprobe3` | A module whose source holds `eval` and `new Function` in a function never called. Expected: it loads. |
| `mods/libprobe4` | Whether `setTimeout` / `setInterval` fire, each step guarded (converting the timer functions to a string throws). |

## Running it

```bash
P=<a scratch folder>
# 1. The server. The AF_UNIX path must stay under ~100 bytes, so give it a SHORT folder.
dotnet run tools/mod-link-probe/server/amprobe-server.cs -- "$P/out" "C:/Users/<you>/AppData/Local/Temp/amp" 600 &
# 2. One probe run (port, socket and pipe come from $P/out/ready.txt). Agentmaster's own variables are
#    removed so Agentmaster never takes the child for its tab's session; --setting-sources project from a
#    folder with no settings keeps the user's hooks and installed plugins out of it.
env -u CCMGR_HOOK_PIPE -u CCMGR_SESSION_ID -u AM_SESSION -u WT_SESSION -u AGENTMASTER_PROFILE \
  AMP_TCP=http://127.0.0.1:<port> AMP_SOCK=<socket path> AMP_PIPE='<pipe>' \
  CLAUDE_CODE_PLUGIN_DIRS=<repo>/tools/mod-link-probe/mods/amprobe2 \
  ~/.local/bin/claude.exe -p "Reply with exactly: OK" --model haiku --setting-sources project \
  --no-session-persistence --plugin-dir <repo>/tools/mod-link-probe/mods/amprobe
cat "$P/out/server.log"
# 3. Memory: alternate runs with and without a plugin folder (';' separates several folders).
powershell -NoProfile -File tools/mod-link-probe/measure.ps1 -Label A1 -OutDir <dir>
powershell -NoProfile -File tools/mod-link-probe/measure.ps1 -Label B1 -OutDir <dir> -PluginDir <...>\mods\linklike -Tcp http://127.0.0.1:<port>
```

Call `claude.exe` by its path, never the `claude` on PATH: inside Agentmaster that is the shim, which adds
Agentmaster's hooks. Loading a plugin from a folder makes the engine write `.claude-plugin/types/` into it;
that folder carries its own `.gitignore`. To measure the user's own mods, copy them to a scratch folder
first (`.claude-plugin/types` excluded), so the engine writes into the copy, not into `~/.claude/mods`.

## Results (Claude Code 2.1.291, 2026-10-16)

`doc/agentmaster/MOD_LINK.md` §3 has the full table. In short: TCP and AF_UNIX work (3–6 ms); a named
pipe is refused; a 12 s fetch is held; a loop keeps running after `session.start`; both plugin-folder routes
load together; the mod runtime costs a claude.exe about +85 MB private on its first mod and nothing
measurable for one more; and an open fetch holds a headless session's end until it answers.

The library probes (F11–F14):
- Vendored libraries load by relative path, and a bare import is refused.
- A mod has no `fetch`, `WebSocket` or other network API of its own.
- Code generation throws at run time, but its mere presence in a library's source is fine.
- `setTimeout` / `setInterval` fire, although they are undocumented.

The library probes need no server: their mods write JSON files into the folder `AMP_OUT` names.
