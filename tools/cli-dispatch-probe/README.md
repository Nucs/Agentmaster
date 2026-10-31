# CLI-dispatch probe

The kit behind [`CLI.md` §2a](../../doc/agentmaster/CLI.md): it checks, without building the app, that
`CliDispatch.h` makes a GUI-subsystem exe answer `<exe> <verb> …` by running the `agentmaster-cli.exe`
beside it, and that it never starts the app for a CLI call.

The 2026-10-23 incident was `agentmaster show <id>` typed on a PC with an install for all users in
`C:\Program Files\Agentmaster`. That folder is on the machine PATH, ahead of the package's alias, so the
name found `Agentmaster.exe`, the app itself, which started a second app on the user's profile ("profile in
use").

Since 2026-10-31 ([`CLI.md` §2c](../../doc/agentmaster/CLI.md)) it also checks help and the version:
`--help -h -? /? --version -v` first go to the CLI from every caller that reads text, through the GUI
stand-in AND through the REAL launcher shim (`src\cascadia\wt\shim.cpp`), and to the app only from a
caller with no console and no handles (the Run box).

## Files

| File | What it is |
|---|---|
| `fakegui.cpp` | A GUI-subsystem stand-in for `Agentmaster.exe`. It runs the real `CliDispatch::TryRunCliCall` first, as `WindowsTerminal/main.cpp` does. For any other command line it writes `app-would-start.txt` and exits 7. |
| `fakecli.cpp` | A console stand-in, built as `agentmaster-cli.exe` beside it. It prints its command line and where its stdout goes, writes a line to stderr, logs to `fakecli.log` and exits 42 (or `exit=<n>`). |
| `launcher.cpp` | Starts the stand-in (or the shim) in the ways Git Bash cannot: `console`, with no handles but sharing a console (like cmd.exe); `detached`, with no console but with a file as stdout (like pythonw); `none`, with neither (the Run box, a shortcut; only for help and the version, since a CLI verb there would open a console window). `detached` and `none` start the child with `CREATE_NO_WINDOW`, so the shim gets a console without a window. |
| `build.cmd` | Builds the three into a folder OUTSIDE the tree, `Q:\tmp\cli-dispatch-probe` by default, and the REAL launcher shim (with the repository's WIL) as `shim\wtd.exe`, beside copies of the stand-ins named as the shim expects: `shim\Agentmaster.exe` and `shim\agentmaster-cli.exe`. It compiles the headers at `/W4 /WX /permissive-` plus the app's `/we` list. |
| `run.sh` | Runs every case from Git Bash and checks each one. Its exit code is the number of failed checks. |

```bash
cmd //c "K:\\source\\Agentmaster\\tools\\cli-dispatch-probe\\build.cmd"   # optional argument: the output folder
bash tools/cli-dispatch-probe/run.sh                                       # same folder argument, if one was given
```

⚠ Call every program by its FULL path, and never name a stand-in `Agentmaster.exe` outside `shim\`. When this
kit was made, a bare `Agentmaster.exe` typed in `cmd /c` (with `NoDefaultCurrentDirectoryInExePath=1`, which
this machine sets) skipped the current folder and started the INSTALLED all-users app. The copy in `shim\` is
named so because the shim forwards to the `Agentmaster.exe` in its own folder by full path; `run.sh` never
starts it by name.

## Measured 2026-10-23

`run.sh` on this kit: 16 of 16 checks passed. The same cases had passed earlier in the scratchpad, where the
stand-in was still called `Agentmaster.exe`:

- **bash** (Claude Code's Bash tool), **Python** `subprocess.run(capture_output=True)` and **`cmd /c`**
  wait. They get the CLI's output (also through a redirect to a file, and with quoted arguments kept) and
  its exit code (42, or 3, 4 or 5 through `exit=`).
- **PowerShell** waits only when the output is piped on: `& exe … | Out-String` did, with
  `$LASTEXITCODE` 42. A plain call and `$o = & exe …` returned at once, with no output and no
  `$LASTEXITCODE`. The CLI still ran and wrote to the console.
- **No handles but a console** (the launcher's `console` mode): the CLI wrote to that console, exit 5.
- **No console but a file** (`detached`): the CLI wrote to the file with no window, exit 42.
- **`-w new`** and **`Show x`** started the "app" (exit 7). **No `agentmaster-cli.exe` beside the
  exe**: one line on stderr, exit 1, and no app.
- **Not run:** the caller with neither handles nor a console (the Run box), whose CLI opens a console
  window of its own (`CREATE_NEW_CONSOLE`). On the user's PC that window would appear on their
  screen; run it in a claude-fleet Windows desktop instead.

## Measured 2026-10-31

`run.sh`: 37 of 37 checks passed (the 16 above, unchanged, and 21 new ones):

- **Help and the version from bash** (it passes handles, so it reads text): `--help`, `-h`, `-?`, `--version`,
  `-v` and `/?` (with `MSYS_NO_PATHCONV=1`) each ran the CLI with the token unchanged, exit 42. `--archived
  sessions` did too.
- **Still the app's:** `-H` (the split-pane flag; the match is exact) and `nt --help` (exit 7).
- **The launches a shell cannot make:** `console` + `--version` and `detached` + `-h` ran the CLI (on the
  console; into the file, with no window); `none` + `--help` went to the app (exit 7), whose box is the only
  answer that caller can read. No CLI ran there, so no window opened.
- **The real shim:** from bash, `--help` and `sessions --json` ran the CLI and returned its exit code, and
  neither started the app; `-w new` was forwarded to `shim\Agentmaster.exe`, as before; `none` + `--help` was
  forwarded too; `detached` + `-v` ran the CLI into the file; `console` + `-?` ran the CLI. A console child
  keeps the standard handles it inherited, so with the launcher's own stdout a Git Bash pipe the CLI wrote
  there (`stdout=pipe`), not to the console: that check asserts the routing only.
- Every fake CLI run logged `consoleWindow=none`.
