# CLI-dispatch probe

The kit behind [`CLI.md` §2a](../../doc/agentmaster/CLI.md): it checks, without building the app, that
`CliDispatch.h` makes a GUI-subsystem exe answer `<exe> <verb> …` by running the `agentmaster-cli.exe`
beside it, and that it never starts the app for a CLI call.

The 2026-10-23 incident was `agentmaster show <id>` typed on a PC with an install for all users in
`C:\Program Files\Agentmaster`. That folder is on the machine PATH, ahead of the package's alias, so the
name found `Agentmaster.exe`, the app itself, which started a second app on the user's profile ("profile in
use").

## Files

| File | What it is |
|---|---|
| `fakegui.cpp` | A GUI-subsystem stand-in for `Agentmaster.exe`. It runs the real `CliDispatch::TryRunCliCall` first, as `WindowsTerminal/main.cpp` does. For any other command line it writes `app-would-start.txt` and exits 7. |
| `fakecli.cpp` | A console stand-in, built as `agentmaster-cli.exe` beside it. It prints its command line and where its stdout goes, writes a line to stderr, logs to `fakecli.log` and exits 42 (or `exit=<n>`). |
| `launcher.cpp` | Starts the stand-in in the two ways Git Bash cannot: with no handles but sharing a console (like cmd.exe), and with no console but with a file as stdout (like pythonw). |
| `build.cmd` | Builds the three into a folder OUTSIDE the tree, `Q:\tmp\cli-dispatch-probe` by default. It compiles the header at `/W4 /WX /permissive-` plus the app's `/we` list. |
| `run.sh` | Runs every case from Git Bash and checks each one. Its exit code is the number of failed checks. |

```bash
cmd //c "K:\\source\\Agentmaster\\tools\\cli-dispatch-probe\\build.cmd"   # optional argument: the output folder
bash tools/cli-dispatch-probe/run.sh                                       # same folder argument, if one was given
```

⚠ Call every program by its FULL path, and never name a stand-in `Agentmaster.exe`. When this kit was made,
a bare `Agentmaster.exe` typed in `cmd /c` (with `NoDefaultCurrentDirectoryInExePath=1`, which this
machine sets) skipped the current folder and started the INSTALLED all-users app.

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
