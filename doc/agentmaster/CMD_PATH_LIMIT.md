# The PATH-length guard — why Ctrl+G broke, and why it was never the side panel

> Status: **implemented + unit-tested; pending a dev build/deploy** (gated on the user's build/deploy
> permission). The pure rule and its Win32 probe live in
> [`CmdPathLimit.h`](../../src/cascadia/TerminalApp/AgentMaster/CmdPathLimit.h) (header-only, no WinRT). It runs at
> two seams: the EXE prelude (`WindowEmperor::HandleCommandlineArgs`) and every tab launch
> (`ConptyConnection::_LaunchAttachedClient`). The engine harness has `TestCmdPathLimit`, and the
> whole suite is green. Both integration blocks were also compiled verbatim with the project's flags
> and run against the incident machine's real environment (§6).

## 1. The symptom

In a Claude Code tab, pressing **Ctrl+G** (`chat:externalEditor`: "edit the prompt in `$EDITOR`")
**flashes** the screen and then shows **"Edit quit unexpectedly (exit code 1)"**. It first showed up right
after the side panel shipped (2026-09-16..18), so it was blamed on the panel. A previous investigation
spent a session on the panel's XAML hosting and could not reproduce the failure out of process.

## 2. What Claude Code actually runs

From the shipped `claude.exe` bundle (CLI 2.1.280), a terminal editor (anything not in Claude's GUI-editor
list, which `edit` is not) is launched like this:

1. `enterAlternateScreen()` switches Claude's own UI to the alternate screen.
2. `spawnSync(\`${EDITOR} "${tmpFile}"\`, { stdio: "inherit", shell: true })` runs, which on Windows is
   **`cmd.exe /d /s /c "edit "C:\...\tmp.md""`**.
3. Any non-zero exit produces `"${Name} quit unexpectedly (exit code ${status})"`.
4. `exitAlternateScreen()` switches back.

So the "flash" is Claude's alternate screen showing **cmd.exe's own error** for one frame. `edit.exe` never
started. Microsoft Edit's source agrees: `edit.exe` 1.2.1 can only exit with code 1 *before* it draws
anything (its main loop exits 0), so a code-1 exit could never have been "render, then die".

## 3. The root cause: cmd.exe and an 8191-char PATH

**cmd.exe resolves NO external command at all once its PATH is longer than 8191 characters.** Nothing
resolves, System32 included, wherever it sits in the list. Every bare name gets `'x' is not recognized
as an internal or external command`, and `cmd /c` exits **1**. Measured on the incident machine with a
controlled environment block:

| PATH length | `cmd /d /s /c hostname` |
|---|---|
| ≤ 8173 | resolves (rc 0), whether System32 is first or last |
| ≥ 8227 | "not recognized", **rc 1**, whether System32 is first or last |

The incident machine's **registry User `Path` was 13,504 chars**. It held **101 dead
`%TEMP%\docfx-hub-runtime-tests\<guid>\data\dotnet-home\.dotnet\tools` entries**, none of which existed on
disk. Together with the machine `Path` (1,291), every process started from Explorer carried a **~14,300-char
PATH**: the Store Windows Terminal, the release Agentmaster (14,295), and every tab's `pwsh` and `claude.exe`
(14,325). Reproduced from inside a release tab:

```
$ cmd.exe /d /s /c "edit \"C:\nonexistent\probe.md\""
'edit' is not recognized as an internal or external command,
operable program or batch file.
exit code: 1
```

**Where the dead entries came from:** `docfx-hub`'s `BuildManager.cs` gives every build process its own
`DOTNET_CLI_HOME=<data>\dotnet-home`, and its test suite uses a fresh temp root per test. On first run in a
new `DOTNET_CLI_HOME`, the .NET SDK's first-run experience **appends `<DOTNET_CLI_HOME>\.dotnet\tools` to the
persistent USER Path**. That meant one new entry per test run, never cleaned up.

**Why it looked like the side panel:** timing. The pollution crossed the 8191 line around the same days
the panel shipped. The earlier "works in regular Windows Terminal" observation predates it; today the Store
WT carries the same bloated PATH (its tabs rebuild their env from the same registry values). Nothing the
panel does touches a child process.

## 4. The rule (`CompactOverlongPath`)

The guard intervenes only when the PATH is at risk, and removes only what cannot matter:

1. **At or under `kCompactTriggerChars`** (8191 − 1024 = **7167**), the PATH comes back **byte-identical**.
   A healthy machine never sees a change, and the probe never runs. The 1 KiB of headroom covers what
   our children prepend on their own before they run cmd.exe: the pwsh host's `$PSHOME` (~31 chars),
   Git Bash's MSYS dirs (~130), and the engine's claude shim (~35).
2. **Otherwise it drops empty entries and later duplicates first.** An entry is a duplicate when its key
   matches: quotes stripped, trimmed, `/`→`\`, trailing `\` dropped (a bare root `C:\` keeps its slash),
   ASCII upper-cased. The first occurrence wins. Lookup walks the list in order, so this pass is
   **resolution-identical**.
3. **Only if that is not enough, it drops directories that provably do not exist.** The Win32 probe
   `IsPathEntryLiveOrUnprobeable` says NO only for:
   - an absolute local path the file system reports as not found or invalid;
   - a drive letter with no volume behind it.

   It never probes a UNC, `\\?\`, or mapped-network entry, because an offline share stalls for seconds.
   A relative, drive-relative, or rooted-without-drive entry is kept (it depends on each child's cwd), and
   so is any probe error that doesn't prove absence (access denied, not ready, too long). It also
   suppresses the "insert a disk" critical-error dialog for the probe (`SetThreadErrorMode`).
4. **A live directory is never dropped.** If the list is genuinely too long, the result is reported as
   `stillOverCmdLimit` and logged, rather than guessing which live entry matters least.

Kept entries keep their exact spelling (quotes too) and order. The split is quote-aware: `"C:\odd;dir"` is
one entry. Pass 3 can change behavior in exactly one theoretical case: a directory created *later* in a
child's life (e.g. a first global tool install) would not be on its PATH. With cmd.exe otherwise unable to
resolve anything, that is the far smaller cost. Pass 3 is also gated behind pass 2 for exactly this reason.

**Cost:** 1.34 ms per run on the incident machine's real PATH (160 entries, 107 missing).

## 5. Where it runs

| Seam | What it covers | Log |
|---|---|---|
| **`WindowEmperor::HandleCommandlineArgs`, first statement** (`CompactProcessPathIfOverlong`) | This process's environment, before anything copies it: the portable uninstaller's `.cmd`, the single-instance **handoff** (serializes the env to the running instance, which uses it for the new window's tabs), the updater's `am-update.cmd` (bare `powershell`, broken by the same wall), the Emperor's startup **snapshot** that restored windows' `+` tabs start from, and the **live** block every managed session inherits (`reloadEnvironmentVariables` is forced OFF). | One `[path-guard]` line in `<profile>\hooks.log`, written after the profile resolves (Rule #15), and only when it acted, failed, or met a PATH it could not fix. The line names the tail of the dropped entries, i.e. the likely polluter. |
| **`ConptyConnection::_LaunchAttachedClient`**, just before `to_string()` | Every tab's **final** environment block, after the profile and Agentmaster overrides. This is the belt for a block built from another source: a profile with `reloadEnvironmentVariables` ON regenerates it from the **registry**, which is exactly where the bloat lives. | Silent: a pure repair, and TerminalConnection has no route to hooks.log. No catch; a `bad_alloc` surfaces through `Start()`'s existing failure path. |

The persistent (registry) PATH is **never written**. It belongs to the user (§7).

Example line from the incident machine:

```
[path-guard] PATH 14455 -> 2030 chars (cmd.exe resolves NO command past 8191 — e.g. Claude's Ctrl+G editor
launch): dropped 7 duplicate, 107 missing, 0 empty entries (missing e.g. C:\Users\…\Temp\docfx-hub-runtime-tests\…\.dotnet\tools | …); applied to this process
```

## 6. Verification

- **Harness (`TestCmdPathLimit`, `tests_spawn_sched.cpp`)** covers:
  - the constants;
  - untouched when healthy (probe never called);
  - the exact trigger boundary;
  - the key normalization (including "the root keeps its slash" and "non-ASCII is not folded");
  - the quote-aware split;
  - dedupe before probing, with no probe when dedupe is enough;
  - the missing pass: verbatim spelling and order, one probe per survivor, and the tail sample;
  - an all-live PATH being reported, not trimmed;
  - the Win32 probe's edges: a UNC entry kept without touching the network (< 1 s), relative entries,
    missing volumes, and files;
  - the process-env apply;
  - **the incident end to end through a real `cmd.exe`**: an overlong PATH cannot resolve `hostname`,
    and the compacted one can.
- **Scratch runs against the real environment:**
  - the ConptyConnection block, compiled verbatim with `/W4 /WX /sdl /permissive-` against the real
    til/WIL/fmt headers: live env 14,455 → 2,030 chars, registry `regenerate()` 14,796 → 1,912;
  - the Emperor blocks: compaction applied, and the `[path-guard]` line above written to a scratch profile;
  - `cmd /c where edit`: rc 1 before, rc 0 after.
- **Pending:** a live Ctrl+G in a dev-deployed tab (needs the user's build/deploy permission).

## 7. Fixing the machine itself (not Agentmaster's job, but the real cure)

The guard makes Agentmaster immune, but everything else on the machine is still broken: plain Windows
Terminal tabs, Explorer-launched scripts, and any `cmd /c`. The persistent fix:

1. **Remove the dead entries from the User `Path`.** Every `…\Temp\docfx-hub-runtime-tests\…` and
   `…\docfx-hub\.qa\…-probe-data\dotnet-home\.dotnet\tools` entry points at nothing. Then restart the
   apps that should see it: a running process keeps its old block.
2. **Stop the leak at the source:** in docfx-hub's `BuildManager.cs`, next to
   `DOTNET_CLI_HOME`, also set `DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=0` (the SDK's documented off switch for
   exactly this PATH append). `DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1` is a reasonable companion.

## 8. Related

- [`ENV_VARS.md`](ENV_VARS.md) §9: the computed `EDITOR=edit` default is the very command that went through
  cmd.exe here.
- [`PROFILES.md`](PROFILES.md): where `hooks.log` lives.
- CLAUDE.md Gotchas: *A PATH longer than 8191 chars makes cmd.exe resolve NO command.*
