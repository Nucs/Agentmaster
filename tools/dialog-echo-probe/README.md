# Dialog-echo probe

The kit behind [`CLI.md` §2b](../../doc/agentmaster/CLI.md): it checks, without building the app, that every
dialog shown through `AgentMaster/DialogEcho.h` is also written to the console or pipe of the command that
started the process (`AgentMaster/CallerSink.h`), for each way a caller can start a program, directly and
through the package's real console launcher (`src/cascadia/wt/shim.cpp`).

The 2026-10-23 incident: a Claude session's `agentmaster show <id>` put an "Agentmaster — profile in use"
dialog on the user's screen, and the session read nothing about it in its output.

Every dialog of the kit opens on a HIDDEN desktop (`WinSta0\AgentmasterDialogProbe`, made by the launcher)
and answers itself, so nothing ever reaches the user's screen, and no keyboard or mouse is needed.

## Files

| File | What it is |
|---|---|
| `dialoggui.cpp` | A GUI-subsystem stand-in for `Agentmaster.exe`, compiled on the REAL headers. Like the app's startup it calls `CallerSink::Capture()` first, shows a dialog through `DialogEcho.h`, then `CallerSink::RequestRelease()`. Modes: `msgbox` (the "profile in use" box, answered No), `taskdialog` (the update prompt's shape, answered Postpone + an option), `deferred` (the release requested while the box is open on another thread, as for Windows Terminal's `--help` box), `none` (no dialog), `diag` (no dialog; writes `diag.txt` beside the exe with what the caller handed over, for a failing shim check). Its exit code is the dialog's answer. |
| `launcher.cpp` | Starts a program on the hidden desktop the way a caller would: `pipe` (bash, Python, Claude Code's Bash tool: stdout + stderr = a pipe it reads to its end), `console` (an interactive cmd.exe: its standard handles are the console's, no handles passed), `conhandles` (the console's handles passed explicitly), `pspiped` (PowerShell 7 with the output piped on: stdin and stderr the console's, stdout a pipe). In the console modes it blanks its console first and then reads back the rows that hold `[agentmaster dialog]`. |
| `build.cmd` | Builds into a folder OUTSIDE the tree, `Q:\tmp\dialog-echo-probe` by default: `dialoggui.exe`, `launcher.exe`, and the REAL launcher shim as `shim\wtd.exe` with a copy of the stand-in beside it as `shim\Agentmaster.exe` (the shim forwards to the file of that name in its own folder). It compiles at `/W4 /WX /permissive-` plus the app's `/we` list, and links the stand-in with `/MANIFEST:EMBED` (see below). |
| `run.sh` | Runs every case from Git Bash and checks each one. Its exit code is the number of failed checks. |

```bash
cmd //c "K:\\source\\Agentmaster\\tools\\dialog-echo-probe\\build.cmd"   # optional argument: the output folder
bash tools/dialog-echo-probe/run.sh                                       # same folder argument, if one was given
```

⚠ Call every program by its FULL path. `shim\Agentmaster.exe` is the stand-in under the app's name, which the
shim needs; a bare `Agentmaster.exe` typed in `cmd /c` can find the INSTALLED app instead (this machine sets
`NoDefaultCurrentDirectoryInExePath=1`, so cmd skips the current folder).

## Measured 2026-10-23

`run.sh`: 11 of 11 checks passed, in three runs in a row.

1. **Pipe caller**: the message box is echoed line by line before it shows, with its choices, then
   `answered: … No`; exit 7 (IDNO).
2. **Pipe caller, task dialog**: the instruction, the content, the footer link as text + URL, each button
   (`(default)` marked), each option, then `answered: Probe update: Postpone; option: Remind me in 3 days`.
3. **Pipe caller, the release while a box is open**: the sink stays until the answer is written; the pipe
   ends after it.
4. **Pipe caller, no dialog**: nothing printed.
5. **Console caller** (interactive cmd.exe): the stand-in gets no handles and attaches to its parent's
   console for each write.
6. **Console handles passed explicitly**: refused in the GUI process, and the echo still lands in the
   console through the parent.
7. **PowerShell 7 piped**: the echo goes into stdout's pipe (stderr is the console, unusable there), and
   none into the console.
8. **The real launcher shim, pipe caller**: the shim exits at once; the stand-in it started writes into the
   caller's pipe through the handed-over handle (`AGENTMASTER_CALLER_SINK=handle:…`).
9. **The real launcher shim, console caller**: the stand-in attaches to the caller's console
   (`console:<pid>,…`).
10. **`cmd /c <shim>`**: the cmd.exe exits as soon as the shim did, before the stand-in writes. The echo
    still lands, through another process attached to the console.
11. **No probe process left running.**

What the runs taught, each now in the code or in this kit:

- **A copy of an exe has no `<name>.exe.manifest`.** The command-line linker writes the Common Controls v6
  manifest to a separate file by default; the copy in `shim\` then bound comctl32 v5.82 and died at load
  with `STATUS_ORDINAL_NOT_FOUND` (0xC0000138) before writing a byte. `build.cmd` embeds it.
- **A child copies the launcher's own standard handles.** With no handles passed, Windows gives a CONSOLE
  child copies of its parent's standard handles: under the Bash tool those are pipes, so the console modes
  set the console's handles first, as an interactive shell holds them.
- **cmd.exe passes its handles by inheritance**: copies that are not inheritable reached a program run by
  `cmd /c` as dead values (file type 0, error 6). The console modes open them inheritable, as a real
  console's are.
- **`cmd /c <shim>` lost every line** while the shim named only its parent: the cmd.exe was gone before the
  stand-in attached. The shim now names up to 8 processes of the console (`CallerSink::ConsoleCandidates`),
  and the app writes through the first one still alive.
- **The Bash tool's hidden console keeps its text across commands**, so the launcher blanks it before each
  console case; a check could otherwise pass on an earlier run's rows.

## Not covered here

- A caller with neither a console nor handles (the Run box, a shortcut): there is nothing to write to.
- The single-instance hand-off's reply (the RUNNING app writes the dialog into the sender's section): the
  m5 suite checks the section end to end (`TestDialogEcho`), but no running app is involved in this kit.
- The real `Agentmaster.exe` and the real CLI: the kit never starts the app.
