# E2E crash guard: one guard around every test, and a suite that provokes crashes, corruption, leaks and sudden shutdowns

> **Status (2026-10-23): PLAN ONLY.** Nothing here is built, installed or run. The code it names (`e2e/guard/`,
> `e2e/faults/`, `e2e/suites/reliability/`, `e2e/host/watchdog.py`, `e2e/host/triage.py`) does not exist.
>
> **How each fact was established:**
> - *measured*: measured on the workstation for this plan (Appendix D).
> - *read*: read from the code at `3773f7149`, plus the end-to-end driver as it stood in the working tree that
>   afternoon.
> - *G0 checks*: still to be confirmed in a test desktop (§11).
>
> Another agent implements the feature scenarios (`e2e/suites/` and the [flow catalog](flows/README.md)). This plan
> specifies the code that wraps every one of them, and the special suite. It builds on [E2E_TESTING.md](E2E_TESTING.md):
> it details that plan's §7.6 (the ambient checks) and §7.7 (the evidence), and extends its §8 (the runner).
>
> **A second plan for the same request** was written the same afternoon by another session, independently:
> [E2E_RELIABILITY.md](E2E_RELIABILITY.md) (fafece429). The owner picks one, or merges them.
>
> **The request, in the user's words (2026-10-23):** "Our automated testing needs to be handling memory corruption,
> memory leaks and crashes as part of special scenarios suite that runs. Crashes and memory corruption and sudden shut
> downs must be handled by every single test (single code running 'around' all) so we get clear signal on such a case."

**The plan in brief.**
- **The Guard: one wrapper around every test.** It is a pytest plugin that is always on. It starts every process of
  the test inside a Windows **job object**, so it knows each process and its exit code even after the app has died. It
  watches:
  - Windows Error Reporting;
  - the app's own logs;
  - the UI's liveness, from outside the app;
  - resources;
  - the profile's files.
- **It stops a test the moment the app crashes, hangs or exits unasked**, and reports that as the failure: the
  process, the exit code, the step the test was on, and a dump. The timeout that would have followed is listed under it
  as a consequence.
- **Two outer rings cover what pytest cannot survive:**
  - the **Sentinel**, in the guest: pytest crashed or hung → record, end it, resume the run;
  - the **Watchdog**, on the host: the guest froze or died → look at it through QEMU, collect, reset, resume.
- **A special reliability suite provokes each failure on purpose.** First it proves that every detector works on the
  build under test. Then it checks the product:
  - sudden kills, crashes, a Windows shutdown and power loss at chosen moments, each followed by a durability check;
  - damaged files on disk;
  - hostile input to the app's parsers under page heap and Application Verifier (and AddressSanitizer, if that build
    flavor is approved);
  - leak cycles with a slope test and heap-census attribution;
  - a hostile machine.

## Contents

1. [What "a clear signal" means](#1-what-a-clear-signal-means)
2. [What exists today, and the gaps](#2-what-exists-today-and-the-gaps)
3. [Facts the design rests on](#3-facts-the-design-rests-on)
4. [Architecture: three rings](#4-architecture-three-rings)
5. [Ring 1 — the Guard, around every test](#5-ring-1--the-guard-around-every-test)
6. [Ring 2 — the Sentinel, in the guest](#6-ring-2--the-sentinel-in-the-guest)
7. [Ring 3 — the Watchdog, on the host](#7-ring-3--the-watchdog-on-the-host)
8. [The special scenarios suite](#8-the-special-scenarios-suite)
9. [The leak method](#9-the-leak-method)
10. [The corruption lanes](#10-the-corruption-lanes)
11. [Phases and acceptance](#11-phases-and-acceptance)
12. [Risks and limits](#12-risks-and-limits)
13. [Decisions for the owner](#13-decisions-for-the-owner)
- [Appendix A — exit codes and statuses](#appendix-a--exit-codes-and-statuses)
- [Appendix B — files, API and markers (proposed)](#appendix-b--files-api-and-markers-proposed)
- [Appendix C — a red test, as it would be reported](#appendix-c--a-red-test-as-it-would-be-reported)
- [Appendix D — the probes behind this plan](#appendix-d--the-probes-behind-this-plan)

---

## 1. What "a clear signal" means

The first lines of a red test must answer five questions:

1. **What ended or stopped that nobody asked to end?** A crash, a fail-fast, a quit, a hang, a dialog nobody
   answers, a leftover process, the test runner, the machine.
2. **Which process,** by role:
   - the app;
   - a ConPTY host;
   - a session's `claude.exe`;
   - a helper: a hook forwarder, the session database service, the AI search's harness;
   - a tool the session ran;
   - the runner;
   - the guest.
3. **When,** relative to the test's own steps: "1.8 s after `tab_menu('t1').choose('Fork session')`".
4. **What kind,** in one vocabulary shared by the guest, the host and the reports (§5.6).
5. **Where the evidence is:** the dump, the screen at that moment, the app's last log lines, the process ledger, the
   steps.

Two rules follow from that:

- **A crash is never reported as a timeout.**
  - **Today:** a test whose app dies usually fails first on whatever it was waiting for ("timed out after 10.0 s
    waiting for an element …"), and the death shows only in teardown.
  - **Under the Guard:** the first line names the death, and the test's own timeout is listed under it.
- **No author has to remember anything.** The Guard is on for every test. A scenario declares only the incidents it
  causes on purpose (§5.3).

What runs where:

| Concern | Every test (the Guard, §5) | The special suite (§8) |
|---|---|---|
| Crash | detected, classified, the test stopped at once, the dump kept | provoked: canaries, a real product crash, crash-instead-of-kill variants |
| Memory corruption | the crash codes and dump signatures that mean corruption (§5.6); the profile's files checked after every unexpected end | instrumented lanes (page heap, Application Verifier, AddressSanitizer), lifecycle stress, hostile input to the parsers |
| Sudden shutdown | any end nobody asked for: of any process, of the runner, of the machine | kills, crashes, a Windows shutdown and power loss at chosen moments, each followed by the durability check |
| Hang | UI liveness measured from outside, the app's own `[ui-stall]` lines, modal dialogs | hang canaries; the self-restart path |
| Leak | resource budgets as runaway tripwires | leak cycles: repetition, a slope test, heap-census attribution |

What the Guard cannot promise:

- Memory corruption that does not crash is invisible outside the instrumented lanes. Even there, page heap and
  Application Verifier see heap and handle misuse, not every logic error.
- A leak is a slope over repetitions. One test's growth is mostly caches and new tabs, so per test there are only
  budgets.
- Only the paths that the flows exercise are covered.
- The test VM has no GPU, so the driver-level classes of [RENDERING.md](RENDERING.md) (device storms, black tabs) are out
  of reach.

---

## 2. What exists today, and the gaps

**What the suite does now** (*read*: `e2e/suites/conftest.py`, `e2e/amdriver/app.py`, `e2e/amdriver/evidence.py`,
`e2e/guest/*`, `e2e/host/e2e_run.py`):

- **Before each test,** the `am` fixture kills whatever the previous test left: an `Agentmaster.exe`, a `claude.exe`, or
  an `OpenConsole.exe` of the suite (`_end_strays`). Then it starts the app.
- **After each test,** it writes evidence when any of these happened:
  - the test failed;
  - the app died without the test ending it (`app.alive()` and `ended_by_test`);
  - a new file appeared in `C:\e2e\dumps`.

  The evidence is the screen, a UI Automation dump, the terminal text, the profile's logs, the mock log and the
  process tree.
- **Then it ends the app:** it kills the GUI and gives the ConPTY hosts 30 s to drain. Processes still alive after that
  are written to `leftovers.txt` and killed.
- **It fails the test** for a death or a new dump.
- **`provision.ps1`** sets up WER LocalDumps (full dumps, 5 kept per exe) for `Agentmaster.exe`, `OpenConsole.exe` and
  `claude.exe`, into `C:\e2e\dumps`.

| # | Gap | Consequence |
|---|---|---|
| G1 | The checks run only after the test body. | A crash surfaces first as the test's own timeout. The cause comes second, in teardown. |
| G2 | `ended_by_test` is set before `close()` / `kill()` start, and no exit code is ever checked. | A crash during Close Window or during the quit is forgiven. That is a known crash class: a coroutine dropping a page's last reference on a pool thread killed the whole process when ONE window closed (CLAUDE.md, Gotchas). |
| G3 | `App.children()` needs the GUI alive, and `kill()` takes its list of children before it kills. | After a GUI crash, its orphaned children are invisible. A child started during teardown escapes the drain count. |
| G4 | Dumps are compared by file name; WER keeps 5 per exe and deletes the oldest; `run_suite.py` never packs `C:\e2e\dumps`. | A dump can be rotated away. Every dump stays in the guest and is lost at the next desktop reset. |
| G5 | No per-test time limit (pytest-timeout is not installed). UI Automation runs through `CUIAutomation`, which cannot set timeouts. | A UI Automation call into a hung app waits UIA's default 20 s (*measured*, §3.5), and a page-object method makes dozens of calls. A stuck test holds the run until the host's 120-minute deadline. |
| G6 | Evidence runs UI Automation in the test's own thread, with no deadline. | On a hung app, the evidence blocks too. |
| G7 | The app's logs are never scanned. | `[exc]`, `[wil]`, `[ui-stall]`, `[persist-fail]` and `[startup-hang]` lines pass unseen (E2E_TESTING.md §7.6 planned the scan). |
| G8 | No resource numbers: no memory, handles, GDI/USER objects or threads. | A runaway leak shows only when the guest runs out of memory. |
| G9 | Strays are killed silently at the next test's start. Leftovers only go to a text file. | A process that should have ended is never a failure. The 11 stored run zips hold 0 leftovers (*measured*), so making leftovers a failure costs nothing today. |
| G10 | Nothing outside pytest watches pytest. The mock runs inside it. The host polls `status.json` and collects only the final zip. | A crashed or hung runner, or a dead guest, loses the whole run's results. |
| G11 | The published zips carry no PDBs (*measured*: the 0.8.0 x64 zip has 67 entries, none a `.pdb`), and the release workflow publishes no symbols. | A dump of the tested build names `TerminalApp.dll+0x…`, never a source line. |
| G12 | The guest's error reporting is partial. There are no LocalDumps for `pwsh.exe`, `powershell.exe`, `agentmaster-cli.exe`, `agentmaster-sessiondb.exe` or `python.exe`. `DontShowUI` is not set. Nothing checks that no JIT debugger is registered. The app inherits the runner's error mode. | A helper's crash leaves no dump. A registered debugger would hold a crashed process on screen. An inherited `SEM_NOGPFAULTERRORBOX` might keep WER from running at all (GSF-01 rules it out). |

---

## 3. Facts the design rests on

### 3.1 How the app ends itself (*read*)

| Path | Exit code | Code |
|---|---|---|
| A quit, the last window closed, the single-instance hand-off | `0`, through `TerminateProcess` | `WindowEmperor.cpp` |
| The user closed the splash during startup | `0` | `Splash.h` |
| The startup watchdog: 25 s over the pre-warm, `App{}` and `ReloadSettings`; once per episode; never with a debugger attached | `1460` (`ERROR_TIMEOUT`) | `StartupGuard.h` |
| The UI-stall restart (a dispatch wedge): a hidden PowerShell waits for the process to end, then starts the exe again | `0x51A11` | `TerminalPage.AgentEngine.cpp` |
| An unhandled exception, a fail-fast | the status: `0xC0000005`, `0xC0000409`, `0xC000027B`, `0xC0000374`, … | — (the app has no `Application.UnhandledException` handler) |

- **No destructor and no `Stop()` runs at a quit.** The app ends with `TerminateProcess`, and the engine is leaked by
  design. Code that must run at the end runs at the last window's close.
- **`RegisterApplicationRestart` is called only for `WM_QUERYENDSESSION`**, with `RESTART_NO_CRASH | RESTART_NO_HANG`.
  So Windows never restarts the app after a crash or a hang, but may after a reboot for updates. `WM_ENDSESSION` runs
  `_finalizeSessionPersistence`.
- **Exit code `0` means "a quit".** That is correct only when the test asked for one.

### 3.2 The app's own watchdogs (*read*: `Engine.cpp`, `StartupGuard.h`)

**`[ui-stall]`:**
- **Beats.** Each window's UI lane beats from its observer probe, about every 2 s.
- **Ladder.** 20 s of silence is declared a stall. From 45 s a rescue runs (a sent-message probe plus a dispatcher
  drain), then again every 30 s. After 180 s, with at least 3 pump-alive rescues and every window stalled, the app
  restarts itself (`0x51A11`).
- **The LIMP guard.** Three drain-assisted recoveries within 10 minutes also restart the app.
- **Never restarted.** A rescue whose probe times out means a blocked UI thread, not the wedge, so it never restarts
  the app.
- **Vetoes.** The setting `uiStallAutoRestart = false` and an attached debugger veto the restart.

**The startup watchdog** never kills a debuggee either.

**What this means for the Guard:**
- It must never attach a debugger to the app; both watchdogs would stand down.
- A test meets a wedge minutes before the app reacts to it, so the Guard needs its own, faster liveness checks
  (§5.4, M4).

### 3.3 Processes (*read*)

- **Killing the GUI closes the ConPTY pipes.** Each `OpenConsole.exe` then ends its shell and claude with `CTRL_CLOSE`
  and exits.
- **Killing an `OpenConsole.exe` orphans its tab's processes** ([OBSERVER.md](OBSERVER.md) §13a). The Guard never does.
- **`0xC000013A` is normal during a drain.** A console process that ends through `CTRL_CLOSE` or Ctrl+C commonly exits
  `0xC000013A` (`STATUS_CONTROL_C_EXIT`). The app's orphan reaper ends a dead console group with the same code.
- **The session database service stays in the job.** `agentmaster-sessiondb.exe` is started with
  `CREATE_BREAKAWAY_FROM_JOB`, and again without it when the job forbids breakaway (`SessionDbClient.cpp`). So it stays
  inside a job that allows no breakaway.
- **The app opts into the segment heap** (`WindowsTerminal.manifest`).

### 3.4 Job objects on this Windows (*measured*, Appendix D)

- **The "abnormal exit" message covers the classic exception codes only.**
  - It fires for `0xC0000005`, `0xC00000FD` and `0x80000003`, and also for `0xC000013A`, which is an ordinary console
    ending.
  - It does NOT fire for `0xC0000409` (fail-fast), `0xC000027B` (stowed exception), `0xC0000374` (heap corruption),
    `0xC000041D` or `0xE06D7363`.
  - So the Guard classifies every exit code itself. "Abnormal" is not a crash signal.
- **A notification-only job memory limit refuses nothing.** It posts a message when the limit is exceeded, so it can
  serve as a runaway tripwire without changing the app's behavior.
- **The job records peak commit:** that of its biggest process, and that of the whole job.
- **Spawning into a job works from Python.** A child started suspended through `subprocess.Popen`, assigned to the job
  and then resumed is in the job from its first instruction, and so is everything it starts.

### 3.5 UI Automation (*measured*)

- **Defaults:** 2,000 ms to connect (`ConnectionTimeout`) and 20,000 ms per call (`TransactionTimeout`).
- **Changing them** is possible only through `IUIAutomation2`. The `CUIAutomation8` class provides it; `CUIAutomation`,
  which the driver uses today, does not (`QueryInterface` fails).

### 3.6 Windows Error Reporting (*read*: the incidents of 2026-07 → 10)

- **LocalDumps is keyed by exe name.** The 2026-09-29 heap-corruption crash left no dump because the key still named
  the old exe ([STARTUP_GUARD.md](STARTUP_GUARD.md) §6). `DumpCount` rotates dumps per exe.
- **An event 1000 and a dump do not prove that the process died.** A MUX tab-drag access violation was reported, then
  swallowed, and the same process ran on, wedged (the debug-dumps skill).
- **A fail-fast can leave no event.** One inside the Debug CLI's `exit()` logged no event 1000 ([CLI.md](CLI.md) §11).
- **Error modes are inherited.** A process inherits its parent's error mode unless it is started with
  `CREATE_DEFAULT_ERROR_MODE`.
- **A registered JIT debugger** (`AeDebug`) would hold a crashed process until someone clicks.

### 3.7 Builds and the history the suite can lean on

- **PDBs.** The published zips carry no PDBs (*measured*). The local layouts in `Q:\agentmaster-layouts\<slot>` do
  (CLAUDE.md, Building FAST #1).
- **A real leak with a published before and after** (*read*: `git tag --contains`):
  - `v0.7.10-prerelease` lacks fa3245561, the UText fix ([MEMORY_LEAKS.md](MEMORY_LEAKS.md) §3);
  - `v0.8.0` has it.

  So the URL-scan leak cycle must flag the first and pass the second.
- **No published build has the fork + mouse-move crash.** Its cause (071031fdf) and its fixes (a790584c9, b59c23418)
  all landed before `v0.7.9-prerelease`.
- **A real crash in every build to date:** the upstream context-menu crash, microsoft/terminal#20570 (*read*; not yet
  reproduced in the guest).
  - Steps: the terminal's context menu from the Menu key → Split pane → Esc → the "…" button twice → `0xC000027B`
    (memory `release-crash-2026-10-09-context-menu-toggle`).
  - The nested `CommandBarFlyout`s it needs are still in `TerminalPage.cpp`.

---

## 4. Architecture: three rings

```text
 host PC (the user's)                       guest: a claude-fleet Windows 11 VM
 ┌────────────────────────────┐            ┌──────────────────────────────────────────────────────────────┐
 │ Ring 3: the WATCHDOG       │  Z: share  │ Ring 2: the SENTINEL (Python, outside pytest, one per run)    │
 │  e2e_run.py + watchdog.py  │◄───────────┤  heartbeat · run journal · supervises pytest · resume         │
 │  heartbeat staleness       │ heartbeat  │  guest settings check · incremental export to Z:              │
 │  QEMU monitor: status,     │ journal    │  ┌────────────────────────────────────────────────────────┐  │
 │   screendump, stop/cont,   │ per-test   │  │ pytest — the suite                                      │  │
 │   reset, powerdown         │ zips       │  │  Ring 1: the GUARD — one wrapper around EVERY test       │  │
 │  docker state, desktop_logs│            │  │   preflight · job object · monitors · expectations       │  │
 │  reset + resume            │            │  │   verdict · bounded evidence                             │  │
 │  triage: symbols, buckets  │            │  │   ┌────────────── the test's job object ─────────────┐   │  │
 └────────────────────────────┘            │  │   │ Agentmaster.exe ─ OpenConsole.exe ─ pwsh ─        │   │  │
                                           │  │   │  claude.exe ─ hook forwarders ─ tools ─ helpers   │   │  │
                                           │  │   └───────────────────────────────────────────────────┘   │  │
                                           │  └────────────────────────────────────────────────────────┘  │
                                           └──────────────────────────────────────────────────────────────┘
```

Each ring covers what the ring inside it cannot survive:
- the Guard cannot survive pytest's death, or a hang of the test's own thread;
- the Sentinel cannot survive the guest's death;
- the Watchdog sees the guest from outside.

**Principles:**

1. **One choke point for process lifetimes.** Every process a test starts is started by the Guard, inside the test's
   job. Every end the test asks for is announced to the Guard first. This is the "single code running around all".
2. **Watch from outside.** No debugger is ever attached to the app (it would silence both of the app's watchdogs,
   §3.2). No thread is suspended to observe it. No code is injected outside the fault suite.
3. **Classify at the moment of detection,** with a screenshot taken then, not at teardown.
4. **Two sources per verdict where possible:** the exit code and WER; the Guard's ping and the app's `[ui-stall]`. The
   confidence is printed.
5. **Nothing silent.** Whatever the Guard kills, moves or forgives is written into the report.
6. **Expectations are narrow and single-use.** Anything not expected is an incident.
7. **Evidence never blocks the run.** Every collector has a deadline and can be abandoned. UI Automation is used only
   while the app answers.
8. **The Guard checks itself.** Canaries prove each detector on the machine and build under test. A monitor that
   cannot run makes the test BLIND, never green.
9. **Zero burden on scenario authors.**

---

## 5. Ring 1 — the Guard, around every test

### 5.1 One wrapper around the whole test

The plugin is `e2e/guard/plugin.py`, loaded by `pytest.ini` (`addopts = … -p guard.plugin`). It wraps pytest's per-test
protocol, so the setup, the body and the teardown of every fixture all run inside one `TestGuard`:

```python
@pytest.hookimpl(wrapper=True, tryfirst=True)
def pytest_runtest_protocol(item, nextitem):
    with TestGuard.for_item(item) as guard:   # preflight, arm ... drain, verdict, evidence, export
        return (yield)                        # pytest runs setup, call and teardown inside
```

Three more hooks put the verdict into pytest's outcome at the right phase:
- a `pytest_runtest_call` wrapper, after the body: the call-check;
- a `pytest_runtest_teardown` wrapper, after every fixture's finalizer: the drain and the final check;
- `pytest_runtest_makereport`: the verdict text first in the report, the details as JUnit properties.

An autouse fixture, `guard`, hands the `TestGuard` to the test and to the driver. Autouse fixtures are set up before the
other fixtures of their scope, so it is set up before `am` and torn down after it.

```text
preflight → arm → [setup: fixtures; am → guard.spawn(app)] → [call: the test body] → call-check
          → [teardown: fixtures; am → guard.end(app)] → drain → final-check → verdict → evidence → export
            ▲ the monitors run from arm to final-check; a FATAL incident stops the phase in progress (§5.5)
```

| Phase | Does | An incident found here is reported as |
|---|---|---|
| preflight | the clean-machine check (§5.11); the Guard's self-check | a STRAY, charged to the previous test; BLIND when the Guard cannot arm |
| arm | creates the job; starts the monitors; takes the baselines | BLIND |
| setup | the fixtures; the app starts inside the job | a setup ERROR, the verdict first |
| call | the test body | a FAILURE: the verdict first, the test's own exception chained under it |
| call-check | the incidents so far; a liveness probe when the test failed without an incident | a FAILURE |
| teardown | the fixture finalizers; the app's end, announced | a teardown ERROR, the verdict first |
| drain | waits for the job to empty (the ConPTY hosts end their clients) and for WER to finish | a teardown ERROR (ORPHAN, a crash during the quit) |
| final-check | late dumps and events, the logs to their end, the profile's integrity, the budgets | a teardown ERROR |
| verdict, evidence, export | `guard.json`, the JUnit properties, the test's folder zipped to Z: | — |

### 5.2 Ownership: the test's job object

**The job.** It is created at arm, unnamed, with these settings:
- **`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.** The job handle lives in pytest, so if pytest dies, Windows ends the whole
  tree. No process survives a runner crash.
- **No breakaway.** Nothing escapes: the session database service's breakaway attempt fails, and its own fallback
  starts it inside (§3.3).
- **A notification-only `JobMemoryLimit`** as a runaway tripwire: 6 GB in an 8 GB guest; nothing is refused (§3.4).

**Spawning: `guard.spawn(argv, env, cwd, role)`.**
1. Start the process through `subprocess.Popen` with `CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP |
   CREATE_DEFAULT_ERROR_MODE`. The last flag keeps the runner's error mode from reaching the app.
2. Send stderr to `guard\app-stderr.log`. A file never blocks its writer, and it collects the dialog echo of
   [CLI.md](CLI.md) §2b once that is built.
3. `AssignProcessToJobObject`.
4. Resume the main thread (*measured* to work).

Every process the app ever starts is then in the job:
- the ConPTY hosts, the shells and the claudes;
- the hook forwarders and the tools the sessions run;
- the session database service;
- the AI search's harness `claude.exe`;
- the UI-stall restart's relauncher, and the app it starts again. *G0 checks:* that `Start-Process` from inside the job
  keeps the new process in the job. For an unpackaged build it should; AUMID activation of a packaged one would not.

**The ledger.** The job's completion port posts `NEW_PROCESS`, `EXIT_PROCESS`, `ABNORMAL_EXIT_PROCESS`,
`NOTIFICATION_LIMIT` and `ACTIVE_PROCESS_ZERO`, each with the process id.
- **On `NEW_PROCESS`,** the Guard at once opens a `SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION` handle and records
  the image, the command line, the parent, the start time and the role.
- **On an exit,** it reads the exit code from that handle. A process that ended before its handle opened is recorded
  with "exit code unknown"; its abnormal flag is kept only as a hint.
- **The Guard classifies every exit code itself** (Appendix A), because "abnormal" is not a crash signal (§3.4).
- **`children()` and `claude_processes()` read this ledger.** Unlike today's `App.children()`, it still knows a dead
  GUI's children.

**Roles:**

| Role | How it is recognized |
|---|---|
| `app-gui` | `Agentmaster.exe` (or the `WindowsTerminal.exe` shim, which only forwards) |
| `conpty-host` | `OpenConsole.exe` |
| `session-shell` | `pwsh.exe` / `powershell.exe` under a host, `-EncodedCommand` (the managed launch) |
| `agent` | `claude.exe` under a session shell (also `codex.exe`) |
| `harness` | `claude.exe` with `--no-session-persistence` (the `AM_HARNESS` child, [SDK_HARNESS.md](SDK_HARNESS.md)) |
| `forwarder` | PowerShell running the hook forwarder (`agentmaster-hook.ps1`, or the exec-form forwarder) |
| `ask-helper` | PowerShell running `agentmaster-ask.ps1` |
| `sessiondb` | `agentmaster-sessiondb.exe` |
| `cli` | `agentmaster-cli.exe` |
| `relauncher` | PowerShell `-Command "Wait-Process … ; Start-Process …"` (the UI-stall restart) |
| `tool` | anything else under a claude: `bash.exe`, `git.exe`, `node.exe` MCP servers |
| `explorer`, `werfault`, `unknown` | as named |

**Generations.** Every `app-gui` process is a generation, with:
- its start reason: test start, test restart, self-restart, hand-off;
- its end: expected or not, the exit code, the time.

Today `App.proc` is replaced at each restart; the Guard keeps every generation.

**The end: `guard.end(proc, how)`.**
- `how="kill"` terminates the GUI with exit code `0x00E2E0D1`, which is recognizable in every record (Appendix A).
- `how="close"` / `"quit-all"` registers the expectation (§5.3) and lets the driver click.
- **Then the drain:** wait for the GUI to exit; for WER, when a dump is being written; and for the job's other
  processes, 30 s by default plus the WER time.
- **What is left** is recorded with its ancestry, then ended with `TerminateJobObject(job, 0x00E2E0D4)`.

### 5.3 Expectations: the only way an incident is forgiven

```python
with guard.expect("crash", role="agent", code=0xC0000005, within_s=10):   # a scenario that crashes a claude
    faults.raise_in(session.claude_pids()[0], code=0xC0000005)
guard.allow_log(r"\[persist-fail\]", reason="HMC-02 fills the disk on purpose")
```

**Rules:**
- **Matched once.** An expectation matches one incident.
- **An expectation not met is a failure.** If its window passes without its incident: "the expected crash of agent did
  not happen".
- **Only an exact match is forgiven.** An incident that differs in code, process or time is unexpected.
- **The driver registers its own expectations,** so scenario authors don't:

| Driver call | What it registers |
|---|---|
| `App.start()` | a new generation; the dialogs the start answers ("Reopen your N previous Agentmaster windows?") |
| `App.close(how="window")` | the dialog "Close this window?"; for the last window: exit `0` within 60 s after `[nav] window-close confirmed` |
| `App.close(how="all")` | both dialogs; exit `0` within 60 s after `[nav] quit confirmed (closing all windows)` |
| `App.kill()` | the exit with the Guard's kill code |
| `App.restart()` | the above, plus a new generation |
| `tabs.close(title, answer)` | the close dialog; no exit |

- **A failed close is classified, never forgiven.**
  - Any exit code other than `0` while a quit is expected (a `0xC0000409` during the close: the pool-thread
    last-reference class) → **CRASH during the quit**.
  - Still alive at the deadline → **QUIT-HANG** (a HANG).
- **`known_defect` never hides an incident.** A known defect that IS a crash names its exact signature:
  `@pytest.mark.known_defect("CG-K1", incident="CRASH app-gui 0xC000027B")`.

### 5.4 The monitors

| # | Monitor | Source | Cadence | It reports | Its own health |
|---|---|---|---|---|---|
| M1 | Process ledger | the test's job (completion port) and one handle per process | event-driven | every start and end, with exit code, role and parent; the job's memory tripwire | the port thread alive; a `NEW_PROCESS` seen for the app |
| M2 | Windows Error Reporting | `C:\e2e\dumps` (LocalDumps); the `Report.wer` files in `ReportArchive` / `ReportQueue`; the Application log (events 1000, 1001, 1002); a `WerFault.exe -p <pid>` in the ledger | 1 s (the folder), 5 s (the event log) | a crash's dump and report; "WER is writing a dump now" | the folder writable; the LocalDumps keys present (preflight) |
| M3 | The app's logs | the test profile's `hooks.log`, `autorunner.log`, `scanner.log`, `forwarder-errors.log` | 0.5 s tail | `[exc]`, `[wil]`, `[ui-stall]`, `[startup-hang]`, `[persist-fail]`, `[launch-fail]`, `[orphan]`, `[orphan-reap]`, `[tab-switch-slow]`; a forwarder drop; the journey's last `[nav]` line | the log grows after the app starts (`[engine]` lines) |
| M4 | UI liveness | per main window: `SendMessageTimeoutW(WM_NULL)`, `IsHungAppWindow`, `GetGUIThreadInfo`; a UI Automation ping from a worker process; the app's own `[ui-stall]` lines; top-level dialogs | 1 s (pump), 2 s (UIA) | a dead message pump; an unanswering XAML with the pump alive (the wedge's shape); a modal dialog | the worker answers for a live window |
| M5 | Resources | per job process: private bytes, working set, handles, GDI and USER objects, threads; the job's peaks; the guest's commit and `C:` free space | 1 s (the app), 5 s (the rest) | budget overruns, a runaway, low commit, low disk | the samples arrive |
| M6 | The runner | the Guard's own sampling loop; `faulthandler`; a watch on the test's thread | 0.25 s | a machine stall (the Guard's own loop paused > 5 s); a stuck test thread | — |
| M7 | The mock | its server thread, its error counters, its request log | at each check | the mock dead; a request from a claude of an ended generation; unknown kinds or endpoints (E2E_TESTING.md §7.6) | the thread alive |
| M8 | Profile integrity | the test profile's JSON files; the session records' continuity | after every unexpected end, and at final-check | a file that no longer parses; a session record lost; a temp file left behind | — |
| M9 | The steps journal | every driver action: start, click, keys, menu choice, dialog answer, wait begin and end | per action | the last steps before an incident | — |
| M10 | The app's stderr | `app-stderr.log` (the dialog echo, CLI.md §2b, NOT BUILT) | tail | the text of a startup dialog | — |

**M1 — the process ledger:** §5.2.

**M2 — Windows Error Reporting.**
- **Dumps are moved out at once.** The folder is polled. A dump is moved into the test's folder as soon as WER has
  finished it: no `WerFault.exe` for its pid, and the size stable for 2 s. Rotation can then never delete it, and every
  dump belongs to exactly one test.
- **The exception is read in the guest.** A small Python reader of the minidump's exception and module streams gives:
  - the code;
  - the fail-fast subcode (`ExceptionInformation[0]` of a `0xC0000409`);
  - the faulting address, with its module and offset;
  - the thread.
- **Symbols come later, on the host** (§7.3).
- **`Report.wer` is the second source.** It gives the code and the module when LocalDumps wrote nothing.
- **An event is matched to a process** by the pid it names.

**M3 — the app's logs.**
- **One file per test.** The test's profile is fresh, so each log is read from its start. A restart inside a test
  continues the same files.
- **`[exc]` signatures.** A swallowed exception's signature is its context, its type and HRESULT, and its first
  `Module+RVA` frame (the format of [`AgentCatchLog.h`](../../src/cascadia/TerminalApp/AgentCatchLog.h)).
- **What each line becomes:**

| Line | Incident |
|---|---|
| `[exc] … swallowed …` | SWALLOWED, unless allowlisted |
| `[wil] failfast …` | the cause line of the CRASH that follows |
| `[wil] exception …` | SWALLOWED; the allowlist that GSF-15 builds absorbs upstream code's expected ones |
| `[wil] log` / `return` | a NOTE, until calibration shows how often upstream code logs them |
| `[ui-stall] … has NOT pumped` | HANG (the app's own judgment, CONFIRMED with M4) |
| `[ui-stall] … RECOVERED` | recorded |
| `[ui-stall] UNRECOVERABLE … RESTARTING` | announces the SELF-RESTART |
| `[startup-hang]` | STARTUP |
| `[persist-fail]` | DATA |
| `[launch-fail]` | a product failure (MAJOR) |
| `[orphan]` / `[orphan-reap]` outside a scenario that expects them | ORPHAN |
| `[tab-switch-slow]`, `[manager] slow rebuild` | NOTEs (performance) |

**M4 — UI liveness.**
- **The pump ping:** every second, `SendMessageTimeoutW(WM_NULL)` with `SMTO_ABORTIFHUNG`, 1,000 ms.
- **The XAML ping:** a worker process (`python -m guard.uiaping`) reads the window's UI Automation name and its first
  child, every 2 s, with a 3-second call timeout. It is killed and replaced when it does not answer within 5 s, so the
  Guard itself never blocks on UI Automation.
- **The app's own `[ui-stall]` declaration** (at 20 s) is the third source. With the test bridge (E2E_TESTING.md §6.2),
  each window's heartbeat age would be a fourth: a proposed `uiBeatAgeMs`.
- **The pump ping alone misses the wedge.** In the dispatch wedge the message pump stays alive and `IsHungAppWindow`
  stays false (memory `ui-dispatch-wedge-freeze`).
- **Dialogs**, checked every second:
  - top-level windows of class `#32770` belonging to the job's processes;
  - the app's ContentDialogs (through the driver's reader);
  - `WerFault.exe` windows;
  - hard-error boxes owned by `csrss.exe` (a loader error).

  The driver registers the dialogs it triggers.

**M5 — resources.**
- **What is sampled:** `PROCESS_QUERY_LIMITED_INFORMATION` reads only: the `memwatch.py` counters of `tools/leakhunt`,
  plus GDI and USER objects through `GetGuiResources`.
- **Where it goes:** a `resources.csv` per test, and the peaks in `guard.json`.
- **System headroom:** under 512 MB of commit headroom during a test → MACHINE-PRESSURE. The process that holds the
  most commit is named.

**M6 — the runner.**
- **A machine stall is never an app hang.** A gap of more than 5 s in the Guard's own 0.25-s loop means the machine
  stopped, not the app. The window is marked MACHINE-STALL, and no HANG is decided across it. The app's ladder counts
  observed spans for the same reason (§3.2).
- **`faulthandler`** writes the Python stacks of a runner that dies.

**M8 — profile integrity.**
- **Every JSON file in the profile must parse:** `sessions.json`, `open-windows.json`, `windows\*.json`,
  `settings.json`, `session-store\*.json`, `tags.json`, `tag-colors.json`, `dir-colors.json`, `recent-dirs.json`,
  `templates.json`, `model-list.json`, `launch-presets.json`, `hooks-settings.json`.
- **No session record may be lost.** At each step boundary the Guard notes the session ids in `sessions.json` (cheap
  at test sizes). After an unexpected end and the next start, an id that is missing is DATA.

### 5.5 From detection to a stopped test

| Severity | Incidents | Effect |
|---|---|---|
| FATAL | the app's generation ended unasked (CRASH, CORRUPTION, SUDDEN-EXIT, SELF-RESTART, STARTUP); a confirmed HANG; MACHINE | the test is stopped now |
| MAJOR | a child's crash (agent, host, helper); SWALLOWED; DIALOG; ORPHAN; DATA; RUNAWAY | the test runs on and fails at the next check |
| NOTE | soft budget overruns; slow tab switches; expected incidents | recorded |

**The stopping ladder** for a FATAL incident:

1. **Cooperative (0 s).** `wait_until`, `hold_true` and the driver's read loops check `guard.tripped` on every
   iteration, and raise `GuardAbort`. That is a subclass of the driver's `Abort`, which `wait_until` already re-raises
   at once.
2. **Asynchronous (2 s).** If the test's thread has not reached a check, the Guard raises `GuardAbort` in it through
   `PyThreadState_SetAsyncExc`. It lands at the thread's next Python instruction.
3. **Unblock (15 s).** A thread blocked in a native call does not run Python: a UI Automation call into a hung app, a
   `SendInput`, a pipe read. The Guard first takes the hang evidence (§5.7), then kills the app (expected, code
   `0x00E2E0D3`). A call into a dead process returns at once, and step 2's exception lands.
4. **Hard (the test's hard budget).** The Sentinel ends pytest (§6). The test is RUNNER-LOST, and the run resumes.

**A failure with no incident is re-examined.** When a test fails and the Guard has seen no incident, the call-check
runs a 3-second probe:
- Is the app alive? Does its pump answer? Does the XAML ping answer?
- Is a `WerFault.exe` running for its pid? Is there a new dump?
- Is there a `[ui-stall]` line from the last 30 s?

When one of them answers, its verdict leads the report ("HANG …, then the test's wait timed out"). A failure the probe
cannot explain stays the test's own: a behavior defect.

### 5.6 Classification

| Kind | Meaning | Default |
|---|---|---|
| CRASH | a process ended with an exception status or a fail-fast, or WER reported it | fail |
| CORRUPTION | a CRASH whose code or dump says memory was corrupted (the rules below); an instrumentation stop (page heap, Application Verifier, AddressSanitizer) | fail |
| SUDDEN-EXIT | the app's generation ended with a non-exception code nobody asked for: `0` with no quit in the journey, `1`, a hand-off to another instance | fail |
| SELF-RESTART | the app ended with `0x51A11` and started again (a dispatch wedge) | fail, unless expected (decision CG-D3) |
| STARTUP | the app ended with `1460` (the startup watchdog), exited before its first window, or its first window never came | fail |
| HANG | the UI did not answer: the pump dead ≥ 10 s, or the XAML ping dead ≥ 30 s with the pump alive, or an app `[ui-stall]`. Also QUIT-HANG (still alive 60 s after an expected quit) and DRAIN-HANG (a ConPTY host that does not end its clients) | fail |
| DIALOG | a modal dialog nobody expected: the app's, a hard-error box, `Debug Assertion Failed!`, WER's | fail |
| ORPHAN | a process alive after the drain (with its role and ancestry); a stray found at the next preflight; an unexpected `[orphan-reap]` | fail for our roles, warn for `tool` (CG-D4) |
| SWALLOWED | an `[exc]` or `[wil] exception` line that is not on the allowlist | fail for a new signature (CG-D1) |
| DATA | a profile file that no longer parses, a lost session record, a `[persist-fail]` | fail |
| RESOURCE | RUNAWAY: a hard budget or the job's tripwire exceeded. LEAK-SUSPECT: a soft budget exceeded | fail (RUNAWAY), warn (LEAK-SUSPECT) |
| RUNNER | pytest crashed or hung, the mock died, the UI Automation client broke | reported apart (E2E_TESTING.md §8.3) |
| MACHINE | the guest paused, rebooted, ran out of commit or disk, or died | reported apart |
| BLIND | a monitor the test needs could not run | not green: an error |

**The primary verdict** is the first that applies, in this order:

> MACHINE > RUNNER > CORRUPTION > CRASH > STARTUP > SELF-RESTART > SUDDEN-EXIT > HANG > DATA > DIALOG > ORPHAN >
> SWALLOWED > RESOURCE > the test's own failure > BLIND > pass

The report lists every incident, the primary first. BLIND leads only when nothing else failed: a real crash stays the
headline even when another monitor was blind.

**Blame, by role:**

| Role | Blamed on |
|---|---|
| `app-gui`, `forwarder`, `ask-helper`, `sessiondb`, `cli`, `relauncher` | Agentmaster |
| `conpty-host` | Agentmaster: `OpenConsole` is upstream Windows Terminal code in this tree |
| `agent`, `harness` | Claude Code, or the mock's reply. It is an Agentmaster defect only when the app killed it or fed it something wrong |
| `tool` | the session's work (a warning) |
| the runner's `python.exe`, the mock | the harness |
| `werfault`, `explorer`, `unknown` | recorded |

**Corruption rules.** They are read from the exception record; the guest's reader takes the subcode from the dump.

| Evidence | Verdict |
|---|---|
| `0xC0000374`: the heap manager found corruption | CORRUPTION |
| `0xC0000409` with subcode 2 (stack cookie), 3 (corrupt list entry), 10 (CFG: an indirect call to a non-target, often through a freed or overwritten vtable), 50 (heap metadata corruption) or 57 (shadow stack: a bad return address) | CORRUPTION |
| `0xC0000409` with subcode 7 (fatal app exit: `std::terminate` / `abort`, e.g. an exception escaping a `fire_and_forget`) | CRASH |
| `0xC0000005` executing a non-image address (DEP); or touching an address that holds a freed-memory fill pattern, or that is non-canonical | CORRUPTION (a suspected use-after-free) |
| `0xC0000005` near address 0 | CRASH (a null dereference) |
| `0xC000027B` (a stowed WinRT/XAML error), `0xC000041D` (an exception in a user callback), `0xC00000FD` (stack overflow) | CRASH; the host decodes the stowed errors (§7.3) |
| `0x80000003` in an instrumented lane | CORRUPTION: a verifier stop, named by its log |
| a CRASH with no dump | CRASH, "exit code only" |

**Confidence:**
- **CONFIRMED** when two independent sources agree: the job's exit code and a WER dump or report; the pump ping and an
  app `[ui-stall]`.
- **PROBABLE** with one.

### 5.7 Evidence

| Kind | Taken at detection | Taken after | Never |
|---|---|---|---|
| CRASH, CORRUPTION | the screen; the app's last 200 log lines; the ledger | the WER dump (moved, at most 1 per process), `Report.wer`, the event's text, the in-guest exception summary | UI Automation of a dead process |
| HANG | the screen; `GetGUIThreadInfo`; each thread's CPU over 2 s (spinning or blocked); a PSS-snapshot dump (`tools/leakhunt/snapdump.exe`: the app pauses 80–230 ms) | the app's `[ui-stall]` lines | UI Automation; a debugger; any other pause of the app (the PSS clone's is the only one) |
| DIALOG | the screen; the dialog's title and text, read as window text (not through UI Automation) | — | — |
| ORPHAN | each leftover's command line, ancestry and start time | — | — |
| DATA | the damaged file, copied before anything can rewrite it; its last good copy | — | — |
| RESOURCE | `resources.csv`; for a runaway, a snapshot dump of that process | — | — |
| Every failure | `steps.jsonl`; `timeline.txt` (every monitor's events, merged in time order); the profile's logs and records; the mock log; `app-stderr.log` | — | — |

**Rules for collectors:**
- **Each has a deadline** and runs in a thread, or in a process when it can block.
- **A failing collector does not stop the others.** It writes its error and the rest go on (today's `evidence.py` rule,
  kept).
- **UI Automation evidence** (the tree dump, the terminal text) is taken only while the XAML ping answers.

**Storage:**
- **Where:** `C:\e2e\out\tests\<test>\guard\` holds `guard.json`, `timeline.txt`, `steps.jsonl`, `ledger.jsonl`,
  `resources.csv`, `logs\`, `dumps\`, `screens\` and `triage.txt`. The folder is zipped at export (§6.7).
- **Dump kinds:** full dumps for `app-gui`, `conpty-host` and `agent`; minidumps for the rest (§6.4).
- **Caps:** at most 3 full dumps per test and 20 per run; the rest are kept as their exception summaries.
- **No secrets:** dumps from the guest hold mock data only. No real credential enters a test machine (E2E_TESTING.md
  §5.3). They still stay on `Q:` and are never committed.

### 5.8 Reports

- **`guard.json`, for every test** (a few KB; decision CG-D14). It holds:
  - the verdict and the incidents, with times, roles, codes and evidence paths;
  - the generations;
  - each monitor's health;
  - the resource peaks and the budgets;
  - each expectation and how it was met;
  - the last steps.

  Green runs then calibrate the budgets (§5.9).
- **The pytest report:** the verdict's lines come first in the failure text; the test's own exception follows.
- **JUnit `<properties>`:** `guard.kind`, `guard.process`, `guard.exit`, `guard.at`, `guard.after_step`, `guard.dump`,
  `guard.bucket`.
- **A "RELIABILITY" block in pytest's terminal summary:** counts by kind, then one line per incident.
  `reliability.json` sits beside the JUnit file.
- **Crash buckets on the host** (§7.3).

Appendix C shows a full example.

### 5.9 Policy, budgets and default numbers

**`e2e/guard/policy.toml`** holds two things:
- **An action per kind and lane:** fail, warn or record.
- **The allowlist.** Each entry has a signature (a regex), a reason, a defect id and an expiry date. An expired entry
  is reported in the run summary. The format:

```toml
# The format only: this is not a real entry.
[[allow]]
kind = "SWALLOWED"
signature = 'SomeContext: swallowed winrt::hresult_error hr=0x80070490'
reason = "why this exception is harmless here"
defect = "TCM-K9"
expires = "2026-12-31"
```

**`e2e/guard/budgets.toml`** holds, per lane and per role, soft and hard limits for:
- private bytes, handles, GDI and USER objects, and threads;
- the job's commit.

A test overrides them with `@pytest.mark.guard(...)`. The budgets are **calibrated, not guessed** (decision CG-D11):
1. Run the suite green 5 times.
2. Take each test's maximum per counter.
3. Set soft = maximum × 1.5 and hard = maximum × 4, never below a floor.
4. Recalibrate when a test changes.

The calibration run is GSF-15.

**Default numbers,** before calibration:

| What | Default | Why |
|---|---|---|
| Pump ping | every 1 s, 1 s timeout; HANG after 10 s dead | the app's own declaration comes at 20 s |
| XAML ping | every 2 s, 3 s per call; the worker replaced after 5 s; HANG after 30 s dead with the pump alive | lets the app's `[ui-stall]` line (20 s) land first, for a CONFIRMED verdict |
| Stopping ladder | cooperative at 0 s, asynchronous at 2 s, unblock at 15 s, hard at the test's hard budget | §5.5 |
| WER wait | up to 120 s per crash | a full dump of a large app |
| Drain | 30 s plus the WER wait | today's 30 s |
| Machine stall | the Guard's own loop paused > 5 s | §5.4 M6 |
| Test time | soft 300 s, hard = soft + 180 s, × the lane factor (fast 1, realtime 2, heap 4, asan 3) | run7: 40 tests in 24 min |
| Heartbeat | every 10 s; the host acts after 120 s without one | §7 |
| Machine floors at preflight | 2 GB of commit headroom, 5 GB free on `C:` | §5.11 |
| Job memory tripwire | 6 GB (in an 8 GB guest) | §5.2 |

### 5.10 How it plugs into today's code

| File (today) | Change | Why |
|---|---|---|
| `pytest.ini` | `-p guard.plugin`; the markers `guard`, `rel_smoke`, `rel_shutdown`, `rel_leak`, `rel_heap`, `rel_asan`, `rel_machine` | the wrapper is always on |
| `suites/conftest.py` | `_end_strays()` becomes the Guard's preflight; the dump comparison and the `pytest.fail` paths become the Guard's verdict; `am` keeps its shape | one owner of crash detection |
| `amdriver/app.py` | `start()` spawns through `guard.spawn()`; `kill()`, `close()` and `restart()` announce their ends; `children()` and `claude_processes()` read the ledger; `ended_by_test` retires | the choke point |
| `amdriver/waits.py` | `wait_until` and `hold_true` check `guard.tripped` on every iteration | the cooperative stop |
| `amdriver/uia.py` | `CUIAutomation8` with `IUIAutomation2` timeouts (connect 2 s, call 5 s); a `UIA_E_TIMEOUT` raises a typed error that the Guard counts | no 20-second blocks |
| `amdriver/evidence.py` | its collectors become the Guard's: bounded, with UI Automation only while the app answers | evidence never blocks |
| the driver's actions | a `@step` decorator writes the steps journal | "1.8 s after step 14" |
| `guest/provision.ps1` | §6.4 | dumps for every process; no JIT debugger |
| `guest/run_suite.py` | becomes the Sentinel (§6) | survives pytest |
| `host/e2e_run.py` | gains the Watchdog (§7) | survives the guest |

Scenario files do not change. A scenario that causes an incident on purpose wraps it in `guard.expect(...)`.

### 5.11 A clean machine before every test

The preflight checks:

1. **Nothing of the previous test runs.** Its job closed, which ends its tree. No `Agentmaster.exe`,
   `OpenConsole.exe`, `claude.exe`, `pwsh.exe` or `powershell.exe` with a `C:\e2e` command line runs.
2. **No `WerFault.exe`, and `C:\e2e\dumps` is empty.** A dump there now is a late crash; it joins the previous test's
   verdict.
3. **No window the previous test left:** an Explorer folder window from "Open In Explorer", a dialog.
4. **At least 2 GB of commit headroom and 5 GB free on `C:`.**
5. **The guest did not reboot** since the last test (its boot time).
6. **The mock** has no request in flight, and its gates are open.
7. **The Guard's own setup:** the LocalDumps keys, `DontShowUI`, no registered `AeDebug` debugger, the dump folder
   writable, the Sentinel's heartbeat fresh.

**What it does with a finding:**
- Anything left over is a **STRAY**. It is charged to the previous test in the run journal, which amends that test's
  verdict in `reliability.json`, and then it is cleaned up.
- A failed self-check makes the test **BLIND**.

---

## 6. Ring 2 — the Sentinel, in the guest

### 6.1 What it is

`run_suite.py` grows into the Sentinel: a long-lived Python process, started detached by the host as today, that starts
and supervises pytest. It runs with `-X faulthandler`.

### 6.2 Heartbeat and journal

The host creates `Z:\e2e\out\<run>\` and its `tests\` before the run. Python in the guest cannot create folders on Z:
(e2e/README.md), but it can write files into existing ones.

- **`heartbeat.json`, every 10 s.** It holds the time, a sequence number, the current test and its phase, pytest's
  pid, and the guest's commit, disk and CPU. It is flushed on each write: Z: is the host's folder over SMB, and a
  heartbeat must reach the host even if the guest dies a second later.
- **`journal.jsonl`.** One line at each test's start and end, with the verdict summary. This is the source of truth
  for resuming a run.
- **`current-test.json`, in `C:\e2e\out`.** The plugin keeps the test in flight and the app's pids there, for the
  Sentinel to read.

### 6.3 Supervising pytest

- **pytest exits.** A normal code ends the run. A crash code (a `python.exe` crash from comtypes or ctypes) makes the
  test in flight RUNNER, with `faulthandler`'s output and python's WER dump.
- **The test in flight passes its hard budget.** The Sentinel:
  1. snapshot-dumps the app (its pid is in `current-test.json`) and takes a screenshot;
  2. ends pytest's process tree. The test's job closes with pytest's handles (kill-on-close), so the app tree ends too;
  3. records the test as RUNNER-LOST.
- **Resume.** pytest runs again with `--guard-resume <journal>`. The plugin deselects the finished tests and the lost
  one, and at the end the JUnit files of the parts are merged. After `max_resumes` (default 5) a run that keeps losing
  its runner ends.
- **The off-the-shelf alternative** (decision CG-D6): pytest-xdist with one worker plus pytest-timeout give the same
  recovery from crashes and hangs, without evidence taken before the kill.

### 6.4 Guest settings (provisioning sets them, the Sentinel checks them at start)

- **LocalDumps, for every process.**
  - **The default,** values on the `LocalDumps` key itself: `DumpType 1` (a minidump), `DumpCount 50`. Any crashing
    process then leaves at least a minidump.
  - **Full dumps** (`DumpType 2`) for `Agentmaster.exe`, `OpenConsole.exe`, `claude.exe`, `agentmaster-cli.exe`,
    `agentmaster-sessiondb.exe`, `WindowsTerminal.exe` and `python.exe`.
  - The Guard moves the dumps out at once, so rotation never matters.
- **No crash UI and no JIT debugger:**
  - `HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\DontShowUI = 1`;
  - no `AeDebug` `Debugger` with `Auto = 1`.
- **Defender:** exclusions for `C:\e2e` and the venv (decision CG-D15). The fault toolkit's remote threads look like an
  attack.
- **No surprise reboots or relaunches:**
  - Windows Update paused for the run;
  - "restart apps after sign-in" off, so that a reboot never relaunches the app behind the suite's back (§3.1).

### 6.5 The mock out of process (decision CG-D7)

- **The Sentinel runs it:** `python -m mockllm --port 8787 --log …`, started and restarted by the Sentinel.
- **The tests reset it** through its control API, as they already do (`mock.reset()`).
- **A dead mock** is a RUNNER incident of the test in flight, never the app's.

### 6.6 The machine-wide ledger (optional)

**What for.** Processes outside any test's job:
- `WerFault.exe`;
- Explorer;
- a packaged relaunch (AUMID activation starts it outside the job);
- services.

**How.** WMI's `Win32_ProcessStartTrace` and `Win32_ProcessStopTrace`, with `ExitStatus`. G0 measures whether WMI keeps
up with 500 short processes a minute. If not, a small ETW consumer of `Microsoft-Windows-Kernel-Process` replaces it.

### 6.7 Incremental export

- **After each test,** its folder is zipped to `Z:\e2e\out\<run>\tests\<seq>-<test>.zip`. A run that dies keeps
  everything it finished.
- **`status.json`** keeps today's fields (the host reads them) and adds `heartbeat`, `current_test`, `resumes` and the
  reliability counts.

---

## 7. Ring 3 — the Watchdog, on the host

### 7.1 Liveness

`e2e_run.py` reads the heartbeat every 10 s. After 120 s with no fresh heartbeat it runs these probes, each with its
own timeout:

- **The fleet:** a `read_file` of `C:/e2e/out/current-test.json` through `desktop_call`. Is the guest's agent alive?
- **QEMU's monitor,** through `docker exec`, as [`tools/install-qa/qm.py`](../../tools/install-qa/qm.py) already does:
  - `info status`: running, paused or shut down;
  - two `screendump`s 60 s apart (`qm.py shot`): the screen of a guest that answers nothing else, which may show a stop
    screen, a frozen desktop or a dialog.

  The container is `claude-desktop-<name>`, as `qm.py` assumes for `win-1`. *G0 checks* that this holds for the E2E
  desktops.
- **`docker inspect`** of that container: running, exited or restarting.
- **`desktop_logs`:** the VM console and the gateway.

### 7.2 Verdicts and actions

| Seen | Verdict |
|---|---|
| the heartbeat comes back after a gap, with the same boot time | MACHINE-STALL (the guest was paused) |
| a new boot time | MACHINE-REBOOT |
| QEMU paused or shut down | MACHINE-STOPPED |
| the screen changes, but the agent and the Sentinel are silent | GUEST-AGENT-DOWN |
| two identical screens, and nothing answers | GUEST-FROZEN |
| the container exited | MACHINE-DEAD |
| a gap > 60 s in the Watchdog's own loop | HOST-SLEPT: the guest paused with the host, and nothing is blamed |

**What the Watchdog then does:**
1. Writes `machine-incident.json`, with the screens, into the run's folder.
2. Collects what Z: holds: the heartbeat, the journal, the per-test zips.
3. Either ends the run, or, with `--resume-on-machine-loss`, resets the desktop, provisions it, and runs the tests the
   journal has not finished. The test in flight is MACHINE-LOST.

**Scenarios that span a guest reboot** (SDN-12, SDN-13) are split into a before part and an after part, joined by a file
on Z:. The Watchdog's resume runs the after part first.

### 7.3 Triage on the host

`e2e/host/triage.py` runs after collection.

**Per dump:**
1. **`dumpexc`:** the exception record and the registers.
2. **`dumpwalk2` / `dumpourscan` + `diasym`,** against the tested build's PDBs: our frames with their source lines. It
   refuses a PDB whose RSDS GUID or age differs; matching the PDB first is the debug-dumps skill's first rule.
3. **`dumpstowed2`** for a `0xC000027B`, with Microsoft's symbol server. That is fine offline against a dump; a symbol
   server is never pointed at a live process.
4. **A `triage.md`** per incident.

**Crash buckets:** `Q:\agentmaster-e2e\crash-buckets.json`.
- **The key:** the image, the code, the subcode, and the first of our frames (`Module+RVA`, which becomes a function
  name once the symbols are in).
- **Per bucket:** first and last seen, count, runs, and the defect id when known.
- **The run summary** says NEW, or names the bucket.

### 7.4 The host's rules stay

- The fleet token lives only in a variable and is never printed.
- No desktop token and no noVNC link goes into an artifact.
- The desktop is deleted after the run. `--keep-on-failure` (decision CG-D17) keeps it for a live look.

---

## 8. The special scenarios suite

### 8.1 Shape

- **Where:** `e2e/suites/reliability/`, one marker per lane.
- **Scenario ids:** GSF (the Guard's self-tests), SDN (sudden shutdown), CST (damaged state), MCR (memory corruption),
  LKS (leaks), HMC (hostile machine). Once built, they become area `24-reliability.md` of the flow catalog.
- **Every scenario runs under the Guard** and declares its own incident with `guard.expect`. Anything else it causes is
  a failure.

| Lane | Runs | Build | What | Time |
|---|---|---|---|---|
| rel-smoke | at the start of EVERY run, before the feature suites | the tested build | the GSF canaries that need no fault DLL | ~5 min |
| rel-shutdown | nightly | the tested build | SDN and CST | ~40 min |
| rel-leak | nightly (short N), weekly (long N) | the tested build, plus `v0.7.10-prerelease` as the positive control of LKS-12 | LKS | 1–3 h |
| rel-heap | weekly | the tested build under page heap / Application Verifier | the P0 feature suite, MCR-01–07, CST again | 2–4 h |
| rel-asan | on demand (each needs a build) | an AddressSanitizer build | the P0 suite and MCR | 2–4 h |
| rel-machine | weekly | the tested build | HMC, GSF-12, SDN-12, SDN-13 | ~1 h |

A canary that fails at the start of a run marks the whole run UNGUARDED in its summary (decision CG-D16).

### 8.2 The fault toolkit (`e2e/faults/`, NOT BUILT)

It is test-only and is used only in the disposable guest.

| Fault | How, from outside the app | Needs | Proves |
|---|---|---|---|
| An exception crash in the app | `VirtualAllocEx` + `WriteProcessMemory` an `EXCEPTION_RECORD`; `CreateRemoteThread` at `ntdll!RtlRaiseException` with its address | Python ctypes, no build | the chain from exit code to WER dump to event to verdict; how fast the test stops |
| A crash that leaves no WER trace | `TerminateProcess(app, 0xC0000409)` | ctypes | classification from the exit code alone (the CLI's fail-fast in `exit()` left no event) |
| A child's crash | the first row, aimed at a `claude.exe`, an `OpenConsole.exe`, a shell, or `agentmaster-sessiondb.exe` | ctypes | the role and the blame; the app's reaction; the orphan reaper |
| A hung UI thread | `SuspendThread` on the app's UI thread (the owner of its main window), resumed later | ctypes | HANG, its evidence, recovery (a deliberate fault, not an observation) |
| The app's memory growing | `VirtualAllocEx(MEM_COMMIT)` into the app, 1 MB at a time | ctypes | the private-bytes budget, the job's tripwire, the slope test |
| The app's handles growing | `DuplicateHandle` of an event into the app, N times | ctypes | the handle budget and slope; the handle census |
| UI-thread faults: a fail-fast with a chosen subcode, a stowed exception (`RoFailFastWithErrorContext`), a Win32 message box, GDI growth | a test DLL loaded onto the UI thread through `SetWindowsHookEx(WH_GETMESSAGE, <the UI thread's id>)`, triggered by a registered message | a small C++ DLL and its injector: a tool build (the owner's go) | the UI-thread crash shapes; DIALOG; the GDI budget |
| A kill at a moment | `TerminateProcess(GUI, 0x00E2E0D2)` after a log line or a delay | ctypes | SDN |
| Machine faults | QEMU's monitor (`stop` / `cont`, `system_reset`); `shutdown /r` in the guest; a commit hog; a disk filler | host and guest scripts | HMC, SDN-12, SDN-13, GSF-12 |
| Runner faults | `kernel32.Sleep(INFINITE)` in a test; `ctypes.string_at(0)`; stopping the mock | none | the Sentinel's resume |

### 8.3 In-app fault switches (NOT BUILT: app changes, active only under `AGENTMASTER_E2E=1`)

These follow the pattern of E2E_TESTING.md §6.1. Only these four things cannot be done from outside:

| Variable | Effect | For |
|---|---|---|
| `AGENTMASTER_E2E_FAULT=startup-block` | the startup prelude blocks inside the startup watchdog's window | ROB-09; a STARTUP canary |
| `AGENTMASTER_E2E_FAULT=wedge` | the UI heartbeat stops while the rescue reports the pump alive, so the ladder runs to a self-restart (a simulation: the real wedge is CoreMessaging's) | ROB-10, SDN-08 |
| `AGENTMASTER_E2E_FAULT=swallow:<context>` | one guarded path throws, and the exception is swallowed through the `[exc]` reporter | GSF-07, end to end |
| `AGENTMASTER_E2E_UISTALL_FAST=1` | the ladder's 20 / 45 / 30 / 180 s become 4 / 8 / 6 / 20 s | ROB-10 in a minute instead of four |

Two additions to the test bridge (E2E_TESTING.md §6.2) would also help:
- **`uiBeatAgeMs` per window,** the heartbeat the app's watchdog reads: the most direct HANG source.
- **A `counters` request:** live `Tab`s, `TermControl`s, tab overlays, tooltip-host entries, popups, registry
  observers, engine sinks and coroutines in flight. Leak attribution without dumps.

### 8.4 GSF — the Guard's self-tests

The rel-smoke lane runs the canaries that need no fault DLL. Each canary proves one detector on this build and this
guest.

**GSF-01 · An app crash is caught at once, with its dump**
- *Do:* an app with one session; at a quiet moment, raise an access violation in a new thread of the app (the
  toolkit's remote raise).
- *Expect:*
  - the test stops within 5 s of the exit;
  - the verdict is CRASH, CONFIRMED, naming `Agentmaster.exe`, `0xC0000005` and the injected address;
  - the WER dump is moved into the test's folder and parsed, and event 1000 is matched;
  - the ConPTY hosts drain, and the profile's files parse.
- *Proves:* the whole chain on this build and guest, including that the runner's error mode does not stop WER (G12).

**GSF-02 · A crash that leaves no WER trace is still a crash**
- *Do:* `TerminateProcess(app, 0xC0000409)`.
- *Expect:* CRASH, "exit code only", PROBABLE.
- *Proves:* the exit-code classifier alone.

**GSF-03 · A fail-fast and a stowed exception on the UI thread** (needs the fault DLL)
- *Do:* `__fastfail(FAST_FAIL_INVALID_ARG)` on the UI thread; then, in another run, `RoFailFastWithErrorContext(E_FAIL)`.
- *Expect:* CRASH `0xC0000409` subcode 5; then CRASH `0xC000027B`, with the stowed record decoded on the host.

**GSF-04 · A hung UI thread is a HANG, with its evidence taken before the kill**
- *Do:* suspend the UI thread for 120 s (the toolkit), or block it on an event (the DLL).
- *Expect:*
  - HANG at about 10 s (the pump dead);
  - the snapshot dump and the threads' CPU, showing a blocked thread, not a spinning one;
  - the app's own `[ui-stall]` line at about 20 s, in the evidence;
  - then the unblock kill, never a 120-second wait.
- *Variant:* resume at 25 s. The app logs `RECOVERED`, and the verdict still records the HANG: a hang that heals is
  still a defect.
- ⚠ A thread suspended from outside may hold a lock that other threads need (the log's mutex, a heap lock). The app's
  own `[ui-stall]` line can then fail to appear, so the canary checks the Guard's verdict, and the app's line is only a
  bonus. The DLL's event-wait form has no such side effect.

**GSF-05 · A child's crash is the child's**
- *Do:* crash a session's `claude.exe`; then its `OpenConsole.exe`; then `agentmaster-sessiondb.exe`, when it is on.
- *Expect:*
  - CRASH with the right role and blame each time;
  - the app alive and answering;
  - for the host, the orphan reaper's `[orphan]` and `[orphan-reap]` within about 5 s (ROB-01).

**GSF-06 · A process that outlives the drain is an ORPHAN**
- *Do:* from a session's shell tab, start a hidden process that is not on the tab's console (`Start-Process
  -WindowStyle Hidden …`); then end the app.
- *Expect:* an ORPHAN after the drain, named with its role (`tool`) and ancestry, then ended by the job; a warning, per
  the policy.
- *The same with a copy named `claude.exe`:* a failure.

**GSF-07 · A swallowed exception fails the test; an allowlisted one does not**
- *Do:*
  1. append a synthetic `[exc]` block to the test profile's `hooks.log` (the tailer's own input; no app change);
  2. append one that matches an allowlist entry;
  3. later, with the in-app switch, cause a real one.
- *Expect:*
  1. SWALLOWED;
  2. nothing;
  3. SWALLOWED, with the real throw stack.

**GSF-08 · Growth trips the budgets**
- *Do:* commit 1 MB a second into the app up to its hard budget; duplicate 5,000 event handles into it.
- *Expect:*
  - LEAK-SUSPECT at the soft budget;
  - RUNAWAY at the hard one, and at the job's notification;
  - the handle census names `Event`.

**GSF-09 · A dialog nobody answers is a DIALOG**
- *Do:* open "Close this window?" and leave it; then, with the DLL, a message box with unknown text.
- *Expect:* DIALOG, with the title and the text: at the end of the test for the first; after its grace period for the
  second.

**GSF-10 · The runner dies or hangs, and the run goes on**
- *Do:* a test that blocks in `Sleep(INFINITE)`; a test that crashes Python; a test that stops the mock.
- *Expect:*
  - RUNNER-LOST, RUNNER-CRASH and RUNNER (mock), each charged to its own test;
  - the app tree ended by the job;
  - the next test runs and passes.

**GSF-11 · A blind monitor makes the test BLIND, never green**
- *Do:* make `C:\e2e\dumps` read-only and crash the app; separately, start the app outside the job.
- *Expect:* for the first, CRASH (the exit code still tells) plus BLIND (WER); for the second, BLIND (job), and the
  test does not pass.

**GSF-12 · A paused machine is not an app hang** (rel-machine, the host ring)
- *Do:* QEMU `stop` for 4 minutes, then `cont`.
- *Expect:*
  - the Watchdog reports MACHINE-STALL, with screens;
  - in the guest, the Guard marks the gap MACHINE-STALL and decides no HANG;
  - the app does not restart itself (its ladder counts observed spans).

**GSF-13 · Expectations are exact and must be met**
- *Do:* expect a crash with `0xC0000409` and inject `0xC0000005`; then expect a crash and inject nothing.
- *Expect:* both fail: "an unexpected code", then "the expected crash did not happen".

**GSF-14 · A stray from the previous test is charged to it**
- *Do:* a test leaves a `claude.exe` running outside its job.
- *Expect:* the next preflight reports a STRAY and kills it; the previous test's verdict in `reliability.json` is
  amended.

**GSF-15 · No false alarms** (the acceptance and calibration run)
- *Do:* the whole feature suite, three times, on the tested build.
- *Expect:* zero unexpected incidents, or each one allowlisted with a defect id; the budgets calibrated from these
  runs.

**GSF-16 · A real product crash, end to end**
- *Do:* the upstream context-menu crash on the tested build (§3.7): the Menu key in a terminal → Split pane → Esc → the
  "…" button twice.
- *Expect:*
  - CRASH `0xC000027B`;
  - the stowed error decoded on the host down to `AppBar::SetFocusOnAppBar` and `E_INVALIDARG`;
  - one crash bucket across runs.
- Kept as a known defect (`CG-K1`) until upstream's fix (PR #20612: `MenuFlyout` for the nested menus) is taken.
- *G0 checks* that it reproduces in the guest.

### 8.5 SDN — sudden shutdown and recovery

These run in the rel-shutdown lane. They test durability, CLAUDE.md Rule #16.

**The durability check** is one helper (`suites/reliability/durability.py`) that ends every SDN scenario. After the
restart:

1. **Files:** every profile JSON file parses. A `<file>.tmp.<thread id>` left by an interrupted write is recorded,
   never treated as the file itself: `WriteAllUtf8` (`Persistence.cpp`) writes a temp, flushes it, then renames it
   over the file, and its own comment calls a leftover temp harmless.
2. **Windows:**
   - `open-windows.json` held the full set (a hard end skips nothing);
   - the reopen question was asked when more than one window was open;
   - each window has its geometry and its Manager lens.
3. **Tabs** come back in order with their titles (Claude tabs first, then shells: PERSISTENCE.md §13.5), and the
   selected tab is selected again.
4. **Resume:** every session that had a transcript resumes (`--resume` on its command line, from the ledger), and a
   never-messaged fork re-forks into the same id.
5. **Queues:** `Pending` stays pending, and nothing `Sent` is sent again. The mock's request log must not hold any
   prompt twice (Rule #4).
6. **Everything else the user set** is as before: drafts, tags, favorites, titles, tab colors.
7. **States,** per Rule #16:
   - Waiting for you and Needs approval are kept;
   - a turn that was cut off reads Error (HOOKS.md *Cut-off turns*), and its runner is paused.
8. **Sessions:** none duplicated, none lost.

**SDN-01 · Killed at rest, the workspace comes back whole**
- *Do:* 2 windows and 4 sessions: one renamed, one tagged, one starred, one with a queued prompt and a draft. Kill the
  GUI and start again.
- *Expect:* the durability check. (This is F-RS's crash variant, `am.restart(graceful=False)`.)

**SDN-02 · Killed in the middle of a turn**
- *Do:* a turn held by the mock (`hold`); kill; start; open the tab.
- *Expect:*
  - the session reads Error (a cut-off turn), and its runner is paused;
  - with `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` the resumed claude continues the turn, and the mock sees the
    continuation;
  - no prompt is sent twice.

**SDN-03 · A change survives a kill at any moment of its save**
- *Do:* rename the tab; kill after 0, 50, 150, 300, 600, 1,000 and 2,000 ms; start again. Repeat with a tag, a star, a
  queued prompt, a reorder and a recolor, one per run.
- *Expect:* the change is fully present or fully absent, and every file parses.
- *Records:* the longest delay at which a change was still lost. That is the measured durability window, to compare
  with the session autosaver's 300 ms quiet time and 1 s cap (`Engine.cpp`) and with the window records' debounced
  save.

**SDN-04 · Killed during the startup restore**
- *Do:* a profile with 3 windows and 40 tabs; kill 1, 3 and 6 s into the restore; start again.
- *Expect:* every window still offered; no record overwritten by a half-built window (the anti-clobber guard); the
  durability check.

**SDN-05 · Killed during a window's close**
- *Do:* Close Window; kill 0–300 ms after the confirm (a sweep).
- *Expect:* the closed window's record keeps its Claude tabs as Claude tabs. TAB-K1's class is a late autosave that
  overwrites the close flush.

**SDN-06 · Killed while a session is being created**
- *Do:* kill during a fork, during New Session Here, and during a `/handover` (after the mock has written the briefing,
  before the successor exists).
- *Expect:* the new session exists once or not at all; the source is untouched; a briefing is never delivered twice.

**SDN-07 · A crash instead of a kill**
- *Do:* SDN-01 to SDN-03, with an injected exception instead of `TerminateProcess`.
- *Expect:* the same results, plus a WER dump. The two paths differ only by WER, because the app has no crash handler.

**SDN-08 · The self-restart brings the workspace back** (needs the wedge switch)
- *Do:* simulate the wedge, with the fast ladder.
- *Expect:*
  - `[ui-stall] UNRECOVERABLE … RESTARTING`, then exit `0x51A11`;
  - a new generation in the job: the relauncher stayed in it;
  - the durability check;
  - the Guard reports a SELF-RESTART, expected here.
- This is ROB-10.

**SDN-09 · A session's claude dies**
- *Do:* kill, then crash, a claude at rest, in the middle of a turn, inside an AskUserQuestion, and during a tool call.
- *Expect:*
  - the tab drops to the PowerShell prompt, with the console guard's note;
  - the state does not stay Running. A known open defect is that a killed claude never reaches Done (memory
    `killed-agent-terminal-modes-console-guard`), so that part is an expected failure;
  - Restart session resumes it;
  - no app incident.

**SDN-10 · A session's ConPTY host dies** (ROB-01 under the Guard)
- *Expect:* the reaper ends the console group in about 5 s; the tab shows the failed connection; no ORPHAN after the
  drain.

**SDN-11 · A helper dies**
- *Do:* end the session database service (when it is on), the AI search's harness claude, and a hook forwarder in
  flight.
- *Expect:* the app reconnects to or restarts what it needs; a dropped hook is logged in `forwarder-errors.log`; no app
  incident.

**SDN-12 · Windows restarts with the app open** (rel-machine)
- *Do:* `shutdown /r /t 0` in the guest.
- *Expect:* the app receives `WM_QUERYENDSESSION` and `WM_ENDSESSION` and finalizes its persistence. After the
  auto-logon, the after part starts the app and runs the durability check.
- *Variant:* with "restart apps after sign-in" on, the app comes back by itself (`RegisterApplicationRestart`), on the
  right profile.
- *G0 checks* that a guest reboot keeps the desktop: QEMU does not exit, and `C:` stays intact.

**SDN-13 · Power loss during a save storm** (rel-machine)
- *Do:* in the guest, rename and tag every 200 ms for 60 s, while the host resets the VM (`system_reset`) at a random
  moment. After the boot, start the app. Ten runs.
- *Expect:*
  - every profile file parses, or the app refuses it without losing the others (CST);
  - no file is left full of zeros;
  - the restore is at most a few seconds stale.
- *Records:* the worst loss.

**SDN-14 · Killed with several windows open**
- *Do:* 3 windows; kill.
- *Expect:* "Reopen your 3 previous Agentmaster windows?". Yes reopens all three. No reopens one and keeps the others
  recoverable (Reopen Windows (N)).

**SDN-15 · A prompt in delivery is never sent twice**
- *Do:* kill at moments around a Prompt Queue Runner delivery: between `[send]`, `[delivered]` and the echo.
- *Expect:* after the restart the prompt is either Sent once (the mock saw it once) or Pending (never sent), never sent
  twice (Rule #4). A prompt that was Sent but never delivered is not replayed.

### 8.6 CST — damaged state on disk

These run in rel-shutdown, and again in rel-heap: our C++ parses these files, so they are memory-corruption surfaces
too.

**Kinds of damage:**
- truncated mid-token;
- 0 bytes;
- a tail of NUL bytes (the power-loss shape);
- invalid UTF-8;
- valid JSON of the wrong shape: an array for an object, a string for a number, missing keys, unknown keys;
- 10,000 levels of nesting;
- 50 MB;
- locked by another process (a sharing violation);
- read-only.

**Files:**
- **The workspace:** `sessions.json`; `windows\<id>.json` and `open-windows.json`; `settings.json`, and Terminal's own
  settings under `terminal\`.
- **The session store and its companions:** `session-store\<sid>.json`, `tags.json`, `tag-colors.json`,
  `dir-colors.json`, `recent-dirs.json`, `templates.json`, `model-list.json`, `launch-presets.json`.
- **Generated files:** `hooks-settings.json`, `agentmaster-hook.ps1`, `bridge.json`, the PATH shim.
- **Caches:** `sessions-index\<sid>.json`.
- **Claude Code's files, in the test's Claude config:**
  - a transcript: a torn last line, a 20 MB line, invalid UTF-8, a NUL tail;
  - the paste cache;
  - a presence row (`sessions\<pid>.json`).

**Expect, for every combination:**
- no crash and no hang (the Guard);
- one log line naming the file;
- the damaged file kept: set aside, or left as it is. A generated file is rebuilt instead;
- the rest of the workspace works;
- never a silent rewrite that loses the user's data.

Today's behavior is unknown for most combinations. The first run records it, and each data loss found becomes a known
defect. Some losses are already likely (*read*): the comment in `WriteAllUtf8` (`Persistence.cpp`) says the loaders
fail open, so a corrupt `sessions.json` loads as an EMPTY fleet and a corrupt `windows\<id>.json` as an empty record.
The atomic writer keeps the app's own writes from producing such a file, but nothing protects a file damaged some
other way, and the next save would replace it.

### 8.7 MCR — memory corruption

These run in the rel-heap and rel-asan lanes.

**MCR-01 · The P0 suite under light page heap**
- *What:* `Agentmaster.exe` and `OpenConsole.exe`; time factor 2.
- *Expect:* no stop.

**MCR-02 · Full page heap, targeted**
- *What:* by a random share (10 %, a different seed each night) and by size bands (16–512 B; 512 B–64 KB).
- *Effect:* every allocation in the band gets a guard page, so a use-after-free or an overrun faults at the
  instruction itself.

**MCR-03 · Application Verifier**
- *What:* the layers Heaps, Handles, Locks, Memory, TLS and Threadpool, plus SRWLock.
- *Effect:* an invalid handle, a lock released by another thread or a TLS misuse stops at the misuse.

**MCR-04 · Lifecycle stress: the historical use-after-free classes.** Each is a loop, run in the heap lanes:
- **(a)** fork 30 times with the mouse moving over the new tab (ROB-05; the TextBuffer null-dereference class);
- **(b)** open and close 50 session tabs fast;
- **(c)** close a window 0–300 ms after a tab switch (the coroutine's-last-reference class);
- **(d)** drag, tear out and dock a tab while it is closing;
- **(e)** `/clear` and `/resume` re-homes in a loop (the map-key use-after-free);
- **(f)** Deactivate / Activate storms;
- **(g)** menus and tooltips opened and closed in storms (the tooltip classes);
- **(h)** theme flips during tab churn;
- **(i)** the Sessions page opened and closed while its search runs.

**MCR-05 · Hostile model output**
- *Do:* the mock replies with pathological Markdown and text:
  - 10,000 nesting levels;
  - a 5 MB paragraph;
  - a 10,000-column table;
  - lone surrogates escaped in the JSON;
  - control characters;
  - 10,000 combining marks;
  - bidi marks.
- Read by: the summary panel, Show Transcript at every level, Copy ▸ Transcript, and the Sessions page's search and
  detail.
- *Expect:* no crash, no hang, bounded time.

**MCR-06 · Hostile terminal output**
- *Do:* a shell tab prints:
  - 50 MB of random bytes;
  - VT sequences mutated from a corpus;
  - 10,000-character lines;
  - dense URLs.
- Read by: the VT parser, the URL scanner, the pending-input detector, and the summary jump's resolver.
- *Expect:* no crash; memory back after a settle (LKS-12).

**MCR-07 · The app's local pipes, fuzzed**
- *Targets:*
  - the hook bridge `\\.\pipe\agentmaster.<pid>` (default security: any local process can write to it);
  - the ask pipe;
  - the session database pipe.
- *Inputs,* 100,000 messages:
  - mutated wire lines: 0–100 fields, 10 MB lines, invalid UTF-8, an escape cut in half, NUL;
  - mutated JSON;
  - framing lengths of 0, 2³¹ and truncated.
- *Expect:* no crash, no hang, bounded memory; real hooks are still delivered afterwards.

**MCR-08 · All of the above under AddressSanitizer** (needs an approved build flavor, decision CG-D9)
- *What:* the P0 suite and MCR-04 to MCR-07.
- *Effect:* a report names the bad access, plus the allocating and freeing stacks.

### 8.8 LKS — leak cycles

These run in the rel-leak lane. The method is §9.

| Id | One cycle | N (warm-up) | History behind it |
|---|---|---|---|
| LKS-01 | launch a session, close it (archived) | 30 (5) | the per-tab costs must come back (MEMORY_LEAKS.md §4) |
| LKS-02 | fork, close the fork | 30 (5) | |
| LKS-03 | Deactivate, Activate | 30 (5) | the parked control and its device |
| LKS-04 | Restart session | 30 (5) | |
| LKS-05 | open a window, close it | 20 (3) | the page-teardown classes |
| LKS-06 | show and hide the Agent Manager with 20 sessions | 50 (5) | the board's persistent columns (UI_THREAD.md §12) |
| LKS-07 | scroll the side panel's GLOBAL list of 300 cards from top to bottom and back | 20 (3) | virtualization recycling (SIDE_PANEL.md §2f) |
| LKS-08 | hover 50 elements that have tooltips | 10 (2) | the per-element ToolTip leak (the 68 GB freeze) |
| LKS-09 | open and close the tab menu and a card menu | 100 (10) | eager menus, MenuFlyout children |
| LKS-10 | open the Sessions page, search, close it | 30 (5) | |
| LKS-11 | open the transcript overlay, visit every level, close it | 20 (3) | |
| LKS-12 | print 50 URL lines and run one Find | 2,000 (100) | the UText leak, 144 B per scan; positive control: `v0.7.10-prerelease` must be flagged and `v0.8.0` must pass |
| LKS-13 | one turn in a session | 200 (20) | the scanner, the summary caches, the sidecars |
| LKS-14 | nothing at all: 40 restored tabs (10 live) for 60 min, a sample each minute | 60 (10) | timers, hidden tabs |
| LKS-15 | a turn with 500 tool calls (a hook storm) | 10 (2) | the bridge's per-connection resources; forwarder processes |
| LKS-16 | a theme flip with 40 tabs | 20 (3) | |
| LKS-17 | the toolkit's injected leaks of 8, 32 and 128 KB per cycle | 30 (5) | the lane's sensitivity |

### 8.9 HMC — a hostile machine

These run in the rel-machine lane.

**HMC-01 · Commit pressure**
- *Do:* a hog leaves less than 500 MB of commit for 2 minutes while the app switches tabs and runs a turn.
- *Expect:*
  - no crash;
  - any `[ui-stall]` escalation recorded;
  - the app usable once the hog ends;
  - the Guard blames the hog (MACHINE), not the app.
- This is the class of 2026-08-22.

**HMC-02 · A full disk**
- *Do:* fill `C:` to 50 MB free; rename, tag, queue a prompt, close a window.
- *Expect:*
  - `[persist-fail]` lines; no crash; no torn or zero-length file;
  - once space returns, the next save succeeds, and a restart shows the latest state.

**HMC-03 · The VM paused for 4 minutes** (GSF-12, from the product's side)
- *Expect:* no self-restart, no lost session; the scanner resumes.

**HMC-04 · The clock jumps an hour forward and back**
- *Expect:* no false stall, no crash; the timing lines still make sense.

**HMC-05 · CPU starvation**
- *Do:* 8 busy threads above normal priority, for 2 minutes.
- *Expect:* slow, never wedged; no self-restart.

**HMC-06 · A slow profile disk**
- *Do:* put the profile on a slow volume: the Z: share, with milliseconds of latency. The host creates the profile's
  folders beforehand, because creating folders on Z: fails from the guest's Python (e2e/README.md) and may fail for
  the app too.
- *Expect:* the UI thread never waits on a file: no `[ui-stall]`, and the pings are answered. This finds UI-thread
  file I/O.

**HMC-07 · Process churn**
- *Do:* 3,000 short processes a minute in the background, for 10 minutes.
- *Expect:* the Fleet Observer's census keeps up, and its memory stays flat.

### 8.10 How the ROB flows relate

| ROB flow | Here |
|---|---|
| ROB-01 (the orphan reaper) | SDN-10; GSF-05's host case |
| ROB-05 (pointer input before the start) | MCR-04 (a), in the heap lanes |
| ROB-06 (freeze shapes) | runs under M4's pings; its 250-ms probe can be the Guard's pump ping at a faster rate |
| ROB-08 (the UText leak) | LKS-12, with the positive control |
| ROB-09 (a hung startup) | needs `startup-block` (§8.3); the Guard reads its `1460` exit as STARTUP, expected there |
| ROB-10 (a dispatch wedge) | SDN-08 |

---

## 9. The leak method

### 9.1 The cycle contract

- **A cycle is an action pair that returns the app to the same observable state:** the same windows, tabs and
  sessions, and no new dialog. The scenario checks that state after every cycle. When the state drifts, the growth
  may be legitimate, and the cycle is invalid.
- **The first W cycles are warm-up.** Caches fill, lazy objects are built, the font cache loads.

### 9.2 Measurement

After every cycle, the scenario waits for the app to settle:
- no new `hooks.log` line for 2 s;
- private bytes within 256 KB over 3 samples taken 1 s apart;
- at most 20 s.

Then it takes one sample of:
- the app's private bytes, handles, GDI objects, USER objects and threads;
- the ConPTY hosts' handles and threads (summed);
- the job's commit and its number of live processes (a process leak).

A scenario with many cheap cycles settles and samples only every k-th cycle: LKS-12 every 100, so 2,000 cycles take
minutes, not hours. N then counts the samples, not the cycles.

### 9.3 Statistics

- **Integer counters** (handles, GDI, USER, threads, processes) are nearly free of noise. A leak means they rose by at
  least N/2 over the N measured cycles, without falling in more than 20 % of the steps.
- **Private bytes are noisy.** A leak needs both:
  - the robust slope (Theil–Sen) over the N measured cycles, times N, exceeds max(threshold, 3 × the MAD of the
    cycle-to-cycle deltas);
  - the median of the last third exceeds the median of the first third.
- **A flag is a LEAK verdict** for that scenario: a failure in the leak lane, with the slope per cycle in the report.

### 9.4 Calibration and sensitivity

- **The noise floor:** each scenario runs 5 times on a known-good build. That sets its threshold.
- **The positive controls set the sensitivity:**
  - LKS-17's injected leaks of 8, 32 and 128 KB per cycle;
  - LKS-12 on `v0.7.10-prerelease`, the real UText leak.

  The report states the smallest per-cycle leak that each scenario detects at its N. A green leak lane means "no leak
  above these sizes on these paths", never "no leak".

### 9.5 Attribution

When a cycle is flagged:
1. A snapshot dump is taken at the end of the warm-up and at the end of the run (`snapdump.exe`: a PSS clone, a pause
   of under 0.3 s at test sizes).
2. On the host, with the tested build's PDBs: `heapcensus.exe` → `segheap.py --live-out` → `censustool.py live`
   ([tools/leakhunt](../../tools/leakhunt/README.md)).
3. Beside that: the handle type census, and the bridge's object counters once they are built (§8.3).

The verdict then names the classes that grew per cycle, for example "+1 `AgentTabOverlay` and +3 `ToolTip` per cycle".

⚠ Check any heap walk against objects whose count you know first, such as one live `Tab` per open tab. The 26200 LFH
bitmap misread once halved every count (MEMORY_LEAKS.md §7).

---

## 10. The corruption lanes

### 10.1 Page heap and Application Verifier

**Installing it.** Application Verifier comes with the Windows SDK. The guest gets its x64 package from the run cache
(`Z:\e2e\cache`). Its **Heaps** layer is page heap, either full, or targeted with the random and size properties. Its
other layers watch handles, locks, memory, TLS and the thread pool.

**How a stop becomes a verdict.** No debugger is attached (§4, principle 2). A stop is a breakpoint exception with no
debugger to take it, so WER writes a dump and the process ends with `0x80000003`. The Guard reads that as CORRUPTION in
these lanes, and attaches the verifier's log, exported to XML with `appverif -export log`.

**The segment heap.** The app opts into it (§3.3). With page heap on, the process may not run the segment heap at all.
*G0 checks* which heap the process actually runs under each setting, and records the exact `appverif` commands that
work in the guest.

**The cost.**
- **Time:** the lane factor (heap ×4, §5.9) stretches the Guard's budgets.
- **Memory:** full page heap gives every allocation at least a page, so the lane uses the random and size modes, and
  a guest of 12–16 GB (decision CG-D10).
- **The app's own ladder does not stretch.** Its 20-second threshold is fixed, so in these lanes a `[ui-stall]` that
  recovers is a NOTE. A self-restart still fails.

### 10.2 AddressSanitizer (decision CG-D9)

**The build.** A separate configuration of our binaries:
- what: `TerminalApp`, `TerminalControl`, `TerminalConnection`, the GUI exe, the CLI and `OpenConsole`;
- with `/fsanitize=address`, and without `/INCREMENTAL` and `/RTC`;
- the ASan runtime DLL beside the exe;
- into its own layout slot, `Q:\agentmaster-layouts\Asan`.

It is a build, so each run needs the owner's go (the Development Rules). `src/common.build.pre.props` already carries
`/fsanitize=address` in upstream's `Fuzzing` configuration: start from that pattern. That configuration also links the
static CRT and the coverage library for conhost's fuzzers; an app build keeps the app's own CRT.

**Running it.** `ASAN_OPTIONS=halt_on_error=1:log_path=C:\e2e\out\asan\am`. A report is CORRUPTION, with the access
stack plus the allocating and freeing stacks.

**What it does not see.** System code (XAML, CoreMessaging) is not instrumented; only our C++ is.

**Feasibility first.** That C++/WinRT and XAML Islands build and run cleanly under MSVC ASan is not known. The first
step is a smoke test: start the app, open a session, quit it.

### 10.3 A Debug-build lane (optional)

**Why:** the Debug CRT fills freed memory with `0xDD`, so a use-after-free crashes visibly.

**Its costs:**
- A `_ASSERTE` opens a modal "Debug Assertion Failed!" box. That is a DIALOG, treated as a CORRUPTION suspect.
- The debug heap serializes allocations, so everything is slow.
- The Debug link's CFG table issue (CLI.md §11) can crash the CLI at its exit.

### 10.4 Hardware stack protection (optional, low priority)

`Set-ProcessMitigation -Name Agentmaster.exe -Enable UserShadowStack` turns a corrupted return address into a fail-fast
with subcode 57. It needs two things:
- a `/CETCOMPAT` binary;
- a virtual CPU that exposes CET. *G0 checks* that the claude-fleet VM does.

---

## 11. Phases and acceptance

| Phase | What | Done when |
|---|---|---|
| G0 | A spike in a test desktop: measure, keep nothing | every *G0 checks* of this document is answered in `e2e/guard/G0-FACTS.md` (the full list is below) |
| G1 | The Guard's core: the plugin; the job and the ledger; the exit-code classifier; the WER monitor; the log scan; expectations; the verdict and the reports; profile integrity; the steps journal; the preflight; the driver changes (`app.py`, `waits.py`) | GSF-01, 02, 05 (claude and host), 06, 07 (synthetic), 13 and 14 pass 10 times out of 10; the existing feature suite stays green with the Guard on (GSF-15: three runs, zero unexpected incidents, or each allowlisted) |
| G2 | Liveness and resources: the pings and the UIA worker; the dialog watch; the sampler and budgets; the hang evidence; the stopping ladder; the UIA timeouts in the driver | GSF-04 (suspend form), 08, 09 (first form) and 11 pass; the test stops ≤ 5 s after a crash; a hang is decided ≤ 15 s after it starts |
| G3 | The Sentinel: heartbeat, journal, supervision, resume, export, the mock out of process, the guest settings | GSF-10 (all three); a feature run with two injected runner faults ends with every other test's result plus two RUNNER-LOST |
| G4 | The Watchdog, the host triage, the crash buckets | GSF-12; a QEMU `stop` in the middle of a run → MACHINE-STALL; a forced reset → MACHINE-REBOOT, then the run resumes on a reset desktop; GSF-16's crash symbolized and bucketed |
| G5 | The fault DLL (a tool build: the owner's go) | GSF-03; GSF-04 (DLL form); GSF-09 (second form); the GDI control |
| G6 | SDN and CST | each scenario runs; its first findings are recorded as known defects |
| G7 | LKS, and the method's validation | LKS-17 gives each scenario's sensitivity; LKS-12 flags `v0.7.10-prerelease` and passes `v0.8.0` |
| G8 | The corruption lanes: Application Verifier and page heap (MCR-01–07); ASan after CG-D9 | the heap lanes run the P0 suite to the end; each stop gives a verdict with its stack |
| G9 | The app's changes in E2E mode (NOT BUILT): the fault switches, the fast ladder, `uiBeatAgeMs`, the object counters | ROB-09, ROB-10 and SDN-08 run; GSF-07 end to end |

**G0's checks**, in one list:

- **Processes and the job:**
  - the app spawned into a job, and its tree inside the job, including the session database service;
  - the UI-stall relauncher and the relaunched app stay in the job.
- **WER:**
  - LocalDumps writes for an injected crash of each process kind, including the global minidump default;
  - event 1000 and its timing;
  - `DontShowUI`;
  - the error-mode question (GSF-01);
  - how long a full dump of a 400 MB app takes to write.
- **UI Automation:** the timeouts in the guest.
- **The host and the VM:**
  - QEMU's monitor is reachable for an E2E desktop;
  - a heartbeat write survives a guest reset;
  - a guest reboot and `system_reset` keep the desktop and `C:`;
  - what the guest's clock does across a `stop` / `cont`.
- **The machine-wide ledger:** WMI under 500 short processes a minute.
- **Tools:** `snapdump.exe` runs in the guest.
- **Instrumentation:**
  - which heap the app runs under each Application Verifier setting, and the exact commands;
  - CET availability.
- **The real crash:** microsoft/terminal#20570 reproduces.

---

## 12. Risks and limits

- **The job object could change behavior:** the session database service stays inside it; jobs nest. G0 measures it.
  The job also ends the whole tree when the runner dies, which is a feature in tests, not on a user's PC.
- **The Guard's own load can disturb timing.** On a 4–6 core VM, the pings, the samples and the UIA worker take time.
  Rates are set per lane, and the realtime lane samples less.
- **False HANGs** during long legitimate UI work: a 40-tab restore, the heap lanes. Mitigated by per-lane thresholds,
  machine-stall detection, and calibration.
- **Dumps are big:** 0.3–1 GB for an app crash at test sizes, more at scale. Mitigated by caps, zip and retention
  (CG-D13).
- **The asynchronous stop can leave the driver in the middle of an action.** The test fails anyway, the job kill
  resets the app, and the next preflight checks that the machine is clean.
- **WMI can drop events under load.** The job is the authority for the test's tree; WMI is only for what lies outside
  it.
- **Defender may block the fault toolkit's injection.** The exclusions apply in the guest only.
- **Page heap needs memory.** The guest is sized for it, and the job's tripwire watches it.
- **ASan may not build or run** with C++/WinRT and XAML Islands: feasibility first.
- **A canary also exercises the product's crash path.** Expected incidents are labeled as such, so reports never mix
  the two.
- **`hooks.log` stamps carry no date.** The Guard dates them from the test's start, and handles midnight.
- **Limits** (§1): uninstrumented corruption, leaks below a lane's sensitivity, paths no flow takes, and the VM's
  missing GPU.

---

## 13. Decisions for the owner

| Id | Question | Recommendation |
|---|---|---|
| CG-D1 | A swallowed-exception line (`[exc]`, `[wil] exception`): fail, warn or ignore? | Fail on a new signature; allowlist the known ones with a defect id. Rule #18 logs them precisely so that they get looked at |
| CG-D2 | A `claude.exe` crash during a feature test | Fail, blamed on Claude Code (or the mock), unless the scenario expects it |
| CG-D3 | A self-restart (`0x51A11`) during an ordinary test | Fail: the app survived, but it wedged |
| CG-D4 | Leftovers after the drain | Fail for our roles, warn for `tool` processes |
| CG-D5 | How faults are injected | The external toolkit first: it works on published builds with no app change. In-app switches only for the four faults it cannot make (§8.3). The UI-thread DLL needs a tool build |
| CG-D6 | Runner supervision | The Sentinel's own resume loop (evidence before the kill, one owner of process lifetimes), rather than pytest-xdist + pytest-timeout |
| CG-D7 | The mock out of process, under the Sentinel | Yes, from G3 |
| CG-D8 | PDBs for the builds under test | Test only builds whose PDBs we keep: local Release builds (E2E_TESTING.md D6), or a private CI artifact of each commit's PDBs |
| CG-D9 | An AddressSanitizer build flavor | Yes, as its own layout slot, built on request; feasibility first |
| CG-D10 | Machine-level scenarios on the user's PC: VM pause, reset, reboot; a 12–16 GB guest for the heap lanes | VM only, weekly, never while the user games |
| CG-D11 | Budgets | Calibrated from green runs (§5.9), not fixed |
| CG-D12 | Leak verdicts in ordinary tests | Hard budgets fail, soft budgets warn; slopes only in the leak lane |
| CG-D13 | Evidence retention | Runs on `Q:` for 30 days; at most 20 full dumps per run |
| CG-D14 | Keep each passing test's `guard.json` | Yes: a few KB, and it is the calibration data |
| CG-D15 | Defender in the guest | Exclusions for `C:\e2e` and the venv, not Defender off |
| CG-D16 | A canary failing at the start of a run | Continue, and mark the run UNGUARDED in its summary |
| CG-D17 | Keep the desktop after a failed run for a live look | Off by default (`--keep-on-failure`): a Windows desktop takes 4–6 GB and several cores of the user's PC |

---

## Appendix A — exit codes and statuses

The "abnormal" column was measured on this Windows (Appendix D).

| Code | Name | Job "abnormal" | Typical source | The Guard reads it as |
|---|---|---|---|---|
| `0` | success | no | the app's quit; any normal end | a quit, correct only when one was asked for |
| `1` | — | no | many programs' errors; Python's `Popen.kill` | SUDDEN-EXIT for the app |
| `3` | — | no | `abort()` without the fault report | CRASH |
| `15` | — | no | a kill through `psutil` (its SIGTERM value): today's `App.kill()` | a kill by psutil |
| `1460` | `ERROR_TIMEOUT` | no | the startup watchdog | STARTUP |
| `0x51A11` | — | no | the UI-stall self-restart | SELF-RESTART |
| `0x00E2E0D1` | proposed | no | the Guard ended the app at a test's end or on its request | expected |
| `0x00E2E0D2` | proposed | no | a scenario's kill at a chosen moment | expected |
| `0x00E2E0D3` | proposed | no | the Guard's unblock kill (§5.5) | part of a HANG |
| `0x00E2E0D4` | proposed | no | the job ended after the drain | part of an ORPHAN |
| `0xC000013A` | `STATUS_CONTROL_C_EXIT` | YES | a console process ended by `CTRL_CLOSE` or Ctrl+C; the orphan reaper | normal during a drain; otherwise an ORPHAN reap |
| `0xC0000005` | `STATUS_ACCESS_VIOLATION` | YES | a bad pointer | CRASH, or CORRUPTION (§5.6) |
| `0xC00000FD` | `STATUS_STACK_OVERFLOW` | YES | runaway recursion | CRASH |
| `0x80000003` | `STATUS_BREAKPOINT` | YES | a `DebugBreak`; a verifier stop with no debugger | CRASH; CORRUPTION in the heap lanes |
| `0xC0000409` | `STATUS_STACK_BUFFER_OVERRUN` (fail-fast) | no | `__fastfail`: GS, CFG, `terminate`, … | by subcode (§5.6) |
| `0xC000027B` | `STATUS_STOWED_EXCEPTION` | no | an unhandled WinRT/XAML error | CRASH; the stowed errors are decoded on the host |
| `0xC0000374` | `STATUS_HEAP_CORRUPTION` | no | the heap manager | CORRUPTION |
| `0xC000041D` | `STATUS_FATAL_USER_CALLBACK_EXCEPTION` | no | an exception in a kernel-to-user callback | CRASH |
| `0xE06D7363` | the C++ exception code | no | an unhandled C++ throw that reached the OS | CRASH |

## Appendix B — files, API and markers (proposed)

```text
e2e/guard/
  plugin.py        the pytest hooks; the autouse `guard` fixture; the markers; the resume selection
  testguard.py     TestGuard: phases, incidents, expectations, the verdict
  job.py           the job object (ctypes): create, spawn suspended, assign, resume, the port, peaks, terminate
  ledger.py        process records, roles, generations, exit codes
  wer.py           the dump folder, Report.wer, the event log, the minidump exception reader
  ping.py          the pump ping and the dialog watch; uiaping.py is the UI Automation worker process
  resources.py     the sampler, the budgets
  logscan.py       the app's logs: incident lines, signatures, the allowlist
  integrity.py     the profile's files, the session records' continuity
  steps.py         the @step decorator, the journal
  evidence.py      bounded collectors (today's amdriver/evidence.py folded in)
  verdict.py       kinds, precedence, rendering, guard.json, the JUnit properties
  sentinel.py      Ring 2
  policy.toml      actions per kind and lane; the allowlist
  budgets.toml     resource and time budgets per lane
e2e/host/watchdog.py, e2e/host/triage.py     Ring 3; Q:\agentmaster-e2e\crash-buckets.json
e2e/faults/
  inject.py        ctypes: remote raise, terminate, commit, handles, suspend
  amfault.cpp      the UI-thread test DLL (SetWindowsHookEx), and its injector
  hog.py, diskfill.py, qemu.py (wraps tools/install-qa/qm.py: stop, cont, system_reset, screendump, info status)
e2e/suites/reliability/
  durability.py    the durability check (§8.5)
  test_gsf_*.py, test_sdn_*.py, test_cst_*.py, test_mcr_*.py, test_lks_*.py, test_hmc_*.py
```

```python
guard.spawn(argv, env, cwd, role="app-gui") -> GuardedProcess   # inside the test's job
guard.end(proc, how="kill" | "close" | "quit-all", deadline_s=60)
guard.expect(kind, role=None, code=None, within_s=None, count=1) # a context manager; single use
guard.expect_dialog(title)
guard.allow_log(pattern, reason)
guard.tripped -> Incident | None                                 # read by wait_until / hold_true
guard.phase(name)                                                # labels the timeline ("restart", "cycle 7")
guard.measure(label) -> dict                                     # one resources sample (the leak lane)
guard.snapshot(proc, label)                                      # a PSS dump (the leak lane, a hang)
guard.processes(role=None) -> list[ProcessRecord]                # the job's ledger, alive or ended
```

```python
@pytest.mark.guard(budget_s=600, resources={"app-gui": {"private_mb": 2500}})
@pytest.mark.known_defect("CG-K1", incident="CRASH app-gui 0xC000027B")
@pytest.mark.rel_shutdown
def test_kill_during_the_startup_restore(am, guard, faults): ...
```

## Appendix C — a red test, as it would be reported

This is an example, modeled on the fork + mouse-move crash of 2026-10-05.

```text
FAILED suites/test_tab_menu.py::test_fork_session_branches_the_conversation
GUARD CRASH (CONFIRMED) — Agentmaster.exe pid 4312, generation 1 (started 13:02:05.114) exited 0xC0000005
  at 13:02:11.482, 1.8 s after step 14: tab_menu("t1").choose("Fork session", "Default")
  fault: read of 0x00000000000000D8 at Microsoft.Terminal.Control.dll+0xAE43, thread 0x1F04 (the UI thread)
  WER: full dump guard\dumps\Agentmaster.exe.4312.dmp (412 MB); Application Error event at 13:02:12.9
  the app's last line: [nav] fork-managed-done new=8afac204 from=15c8cb70
  children: 3 ConPTY hosts drained in 2.1 s, 0 left over; the profile's files parse; 0 sessions lost
  then: WaitTimeout — timed out after 30.0 s waiting for the fork's tab (a consequence of the crash)
  evidence: C:\e2e\out\tests\test_fork_session_branches_the_conversation\guard\
```

```json
{
  "test": "suites/test_tab_menu.py::test_fork_session_branches_the_conversation",
  "verdict": {"kind": "CRASH", "confidence": "CONFIRMED", "role": "app-gui", "exit": "0xC0000005"},
  "generations": [{"pid": 4312, "reason": "test-start", "exit": "0xC0000005", "expected": false}],
  "monitors": {"job": "ok", "wer": "ok", "logs": "ok", "ping": "ok", "resources": "ok"},
  "peaks": {"app-gui": {"private_mb": 312, "handles": 2140, "gdi": 61, "user": 140, "threads": 58}},
  "steps_tail": ["13:02:09.140 tab_menu t1", "13:02:09.505 submenu Fork session", "13:02:09.682 choose Default"]
}
```

```text
=============================== RELIABILITY ===============================
41 tests: 39 pass · 1 CRASH · 1 HANG
CRASH  test_fork_session_branches_the_conversation  Agentmaster.exe 0xC0000005 Microsoft.Terminal.Control.dll+0xAE43  NEW bucket CB-0004
HANG   test_sessions_page_search                    XAML ping dead 31 s, pump alive (the wedge shape); snapshot dump kept
canaries 9/9 · leftovers 0 · strays 0 · budgets: 2 warnings
```

## Appendix D — the probes behind this plan

The probes ran on the workstation (Windows 11 Pro 26200, Python 3.12, psutil 7.2.1) on 2026-10-23. They are kept out of
the repository: the request was for a plan, so G0 repeats them in a test desktop.

**The job object probe (2026-10-23).**

*Method:*
1. Create an unnamed job with a completion port, `KILL_ON_JOB_CLOSE`, and a notification-only `JobMemoryLimit` of
   64 MB (`JobObjectNotificationLimitInformation`).
2. Start 15 `python.exe` children through `subprocess.Popen(creationflags=CREATE_SUSPENDED)`, assign each to the job,
   and resume its main thread (found through psutil, opened with `THREAD_SUSPEND_RESUME`). Each child ends itself with
   `TerminateProcess(GetCurrentProcess(), <code>)`: no exception is raised, so WER is never involved.
3. Start a 16th child that commits 96 MB in 8 MB steps.
4. Read the port's messages and the job's accounting and peaks.

*Results:*

| Exit code | Messages for the child |
|---|---|
| `0`, `1`, `15`, `0x00E2E0D1`, `1460`, `0x51A11` | `NEW_PROCESS`, `EXIT_PROCESS` |
| `0xC000013A`, `0xC0000005`, `0xC00000FD`, `0x80000003` | `NEW_PROCESS`, `ABNORMAL_EXIT_PROCESS` |
| `0xC0000409`, `0xC000027B`, `0xC0000374`, `0xC000041D`, `0xE06D7363` | `NEW_PROCESS`, `EXIT_PROCESS` (not abnormal) |
| the 96 MB child | `NEW_PROCESS`, `EXIT_PROCESS`, plus a job-level `NOTIFICATION_LIMIT`, and its allocations all succeeded |

- The job-level messages (`NOTIFICATION_LIMIT`, `ACTIVE_PROCESS_ZERO`) carry pid 0.
- Accounting: 16 processes in total, 0 active at the end, 0 "terminated" (that field counts limit violations only).
- Peaks: 104.2 MB for the biggest process, 105.3 MB for the whole job.
- `Popen`'s `returncode` matched every code.

**The UI Automation probe (2026-10-23).**
- `comtypes` with the `CUIAutomation` class: `QueryInterface(IUIAutomation2)` fails (`E_NOINTERFACE`).
- With `CUIAutomation8`: `IUIAutomation2.ConnectionTimeout = 2000` and `TransactionTimeout = 20000` by default; both
  could be set (to 2,000 and 5,000) and read back.

**The kill codes (2026-10-23).** A sleeping `python.exe` child, killed:
- with `psutil.Process(pid).kill()` (what `App.kill()` does today): exit code `15`;
- with `Popen.kill()`: exit code `1`.

**Read for this plan.**
- The cached `Agentmaster_0.8.0.0_x64.zip` in the E2E desktop's persist folder: 67 entries, no `.pdb`.
- The 11 run zips in that folder: no `leftovers.txt`.
- `git tag --contains` for fa3245561, a790584c9, b59c23418 and 071031fdf (§3.7).
