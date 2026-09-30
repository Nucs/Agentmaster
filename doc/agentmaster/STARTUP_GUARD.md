# Startup guard — the NVIDIA-driver startup deadlock and the startup-hang watchdog

`src/cascadia/TerminalApp/AgentMaster/StartupGuard.h`, used by `WindowEmperor::HandleCommandlineArgs`
(`src/cascadia/WindowsTerminal/WindowEmperor.cpp`). Harness: `TestStartupGuard`.

## 1. The incident (2026-09-30)

- **2026-09-29 13:48:20** — the release instance (pid 204316, up since 09-27 18:56) died with
  `0xC0000374` STATUS_HEAP_CORRUPTION, reported in `ntdll!RtlReportFatalFailure+0x9`. That is a
  separate problem with no dump behind it; see §6.
- **2026-09-30 06:10:41** — the relaunch (pid 222128) logged its `[startup] update-check` line and
  then nothing more: no window, all 12 threads waiting, CPU flat.
- Every further launch did nothing visible either. `acquireMutexOrAttemptHandoff` finds the stuck
  instance's single-instance mutex, never finds its message window (that is created after
  `ReloadSettings`), retries for ~29 s, and then `TerminateProcess`es itself as if the handoff had
  succeeded.

## 2. Root cause — an ABBA deadlock between the loader and `windows.storage`

A full dump of pid 222128, walked with Microsoft's public PDBs (`hangwalk` + the msdl symbols):

```
MAIN thread (174376)                                   XAML GPU thread (203056, spawned by App{})
-----------------------------------------------------  -----------------------------------------------------
Microsoft.Terminal.Settings.Model!                     nvldumdx!OpenAdapter12
  _accumulateStorePowerShellInstances                    -> KERNELBASE!LoadLibraryExW   (a DLL load in flight)
  -> SHGetKnownFolderPath(FOLDERID_LocalAppData)         -> ntdll!LdrLoadDll ... NvMemMapStoragex.dll init
  -> kfapi::CKFFacade::GetFolderPath                     -> SHGetKnownFolderPath
  -> kfapi::CFolderDefinitionCache::Load   [TAKES lock]  -> kfapi::CFolderDefinitionCache::Load
  -> kfapi::CFolderDefinitionStorage::_LoadRegistry      -> RtlEnterCriticalSection     [WAITS on that lock]
  -> _tailMerge_api_ms_win_shlwapi_winrt_storage_l1_1_1_dll
  -> LdrResolveDelayLoadedAPI -> LdrpHandleProtectedDelayload
  -> LdrpLoadForwardedDll -> LdrpLoadDllInternal
  -> LdrpDrainWorkQueue       [WAITS for the other thread's DLL load to finish]
```

- **The trigger is the NVIDIA driver.** `NvMemMapStoragex.dll`, from driver 32.0.16.1656, calls a
  shell API from its DLL initialization, while the loader is busy. DLL init code must not do that.
- **It only bites when the main thread performs the process's first known-folder definition load**
  at the same moment XAML's GPU thread is loading the driver. That first load resolves a
  delay-loaded import, which needs the loader while the definition-cache lock is held.
- **Agentmaster makes that likely.** Its `AGENTMASTER_PROFILE` redirect in `FileUtils.cpp`
  `GetBaseSettingsPath` skips upstream's `SHGetKnownFolderPath(LocalAppData)`. That leaves the
  PowerShell profile generator, which runs right after `App{}`, as the first known-folder caller.
- Every earlier release launch in hooks.log passed `App{} + ReloadSettings` in 140–330 ms. One dev
  launch once stalled at the same step (hooks.log `11:38:41`, date unknown) and was replaced
  82 s later.

## 3. The fix — pre-warm the known folders before `App{}`

- **What it does.** `PrewarmKnownFolders()` replays `kStartupKnownFolderLookups` on the main thread
  before `App{}` starts any GPU thread. It covers every folder and flag combination the startup
  uses, plus RoamingAppData and Profile as belts:
  - LocalAppData with flags `0`, `FORCE_APP_DATA_REDIRECTION` and `NO_PACKAGE_REDIRECTION`
  - ProgramData with flags `0`
- **Why it works.** At that point no other thread is loading DLLs. The replay resolves
  `windows.storage`'s delay-loaded imports (their IAT slots stay patched for the process's lifetime)
  and fills the definition cache. Later lookups, ours or the driver's, then take the cache lock only
  briefly and never need the loader while holding it.
- **Cost.** A few ms. It is logged as `[startup] known-folder prewarm (6/6 resolved): Nms`.
- **Keep the list in step.** A new startup-time `SHGetKnownFolderPath` caller with a folder or flag
  combination missing from `kStartupKnownFolderLookups` re-opens the race for its first call.
- **Pinned by the harness.** It checks that the locally spelled `FOLDERID_*` GUIDs resolve to the
  folders their names claim. They are spelled locally so the EXE needs no `uuid.lib`.

## 4. The belt — the startup-hang watchdog

`HangWatchdog` is armed over the pre-warm, `App{}` and `ReloadSettings`, a UI-free phase that
normally takes about 0.3 s, and is disarmed right after.

**What a hung process can still do.** Measured with a DllMain parked inside the loader
(`LoadLibrary` on another thread):

| From a thread that exists before the hang | Result |
|---|---|
| append a log line (`std::ofstream`) | works |
| `CreateProcessW` (self-relaunch) | **blocks** |
| create a new thread | **never runs** (its thread-attach needs the loader) |
| `TerminateProcess` | works |

So a stuck process cannot relaunch itself. What the watchdog does instead, on its 25 s deadline:

- **It logs a line**, then terminates the process with exit 1460 (`ERROR_TIMEOUT`). The line reads
  `[startup-hang] "known-folder prewarm + App{} + ReloadSettings" still running after 25000ms …`.
  Terminating frees the single-instance mutex.
- **A launch the user already started while the hang lasted takes over automatically.** That launch
  is still inside its ~29 s handoff retry loop, so it becomes the instance. The 25 s deadline was
  chosen below that loop's length for this reason. Any later launch simply works.
- **It kills at most once per episode.** Before terminating it writes
  `<profile>\startup-hang.marker`. A launch that finds the marker is armed log-only: it waits
  without a deadline, so a machine whose settings load is legitimately slower than 25 s can never
  be locked out. A launch that gets through normally deletes the marker and logs
  `[startup-hang] recovered: …`.
- **It never kills a debuggee.** With a debugger attached it only logs.
- **A late timer can never kill a healthy launch.** Arming, disarming and firing form one atomic
  state machine: Armed → Disarmed, or Armed → Fired. A phase that finishes at the deadline instant
  is either disarmed or fired, never both; the harness hammers this race 200 times.
- **A guard failure never fails the launch.** If the watchdog can't arm (event or thread creation
  fails), it logs `[startup-guard] … runs without the startup-hang watchdog` and the launch
  proceeds unguarded.

## 5. Diagnosing a startup hang next time

1. In `<profile>\hooks.log`, the last `[startup]` line names the phase that never finished. A
   `[startup-hang]` line means the watchdog fired.
2. Check whether the process is alive with zero visible windows: enumerate `EnumWindows` for its
   pid, collecting the rows into a list (output written inside the callback is lost).
3. Take a full dump before ending it. `MiniDumpWriteDump` works out-of-process; the scratch
   `writedump.ps1` recipe is in the memory notes.
4. Walk it with `hangwalk` against Microsoft's public PDBs. Download them offline from msdl by the
   image's RSDS GUID/age, and never point a symbol server at a live instance (livesample's warning).
   Match each thread's `Ldr*` / `RtlEnterCriticalSection` frames to their partner thread.

## 6. The 2026-09-29 heap-corruption crash — why there was no dump

- **Crash dumps stopped when the exe was renamed.** HKLM
  `SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps` has a `WindowsTerminal.exe` key
  (DumpType 2 = full, DumpCount 5, default folder `%LOCALAPPDATA%\CrashDumps`). It kept every
  earlier crash analyzable, but the GUI binary is now `Agentmaster.exe`, so WER no longer matches
  it and the crash left only an event-1000 record.
- **The remedy needs admin.** Add the same key for `Agentmaster.exe`:

  ```
  reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Agentmaster.exe" /v DumpType /t REG_DWORD /d 2 /f
  reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Agentmaster.exe" /v DumpCount /t REG_DWORD /d 5 /f
  ```

- **What the crash itself shows.** The fault is `ntdll!RtlReportFatalFailure+0x9`, and WER's P9
  frame is `ntdll!NtWaitForMultipleObjects+0x14`, which is WER's own wait. Neither names the
  corrupting code.
- **What the app's log shows.** hooks.log for that run has no `[exc]` / `[wil]` lines, and the fleet
  was idle; only 5-minute census keepalives were logged from 13:22 on.
- **What was injected.** Only NVIDIA's own capture hook (`nvspcap64.dll`) besides the display
  driver.
- **Conclusion.** The cause stays undetermined until a dump exists. With the key above, the next
  occurrence leaves a full dump for the `0xC0000374` playbook in the `debug-dumps` skill
  (`dumpmem` on the `HEAP_FAILURE_INFORMATION`).
