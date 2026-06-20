---
name: build-install
description: |
  Build and install Agentmaster ON THIS MACHINE — every build flavor (lib-only compile
  check, ClCompile-only for locked binaries, full Debug package, full Release package,
  the engine test harness) and both local installs: the DEV instance (AgentmasterDev,
  Debug loose layout, close → build → relaunch) and the PRODUCTION instance (Agentmaster,
  Release loose layout: build -ReleaseIdentity → stamp version → register → launch →
  profile picker). Encodes the machine map (two registered identities side by side, one
  repo tree, the Store WT to never touch) and the battle-tested troubleshooting: the
  self-kill check (is MY shell hosted in the instance I'm about to close?), PRI210
  mapped resources.pri, 0x80070020 same-full-name re-register collisions (orphaned
  claudes pin a removed package — bump the layout version, never kill them), stale
  build-mutex reclaim, Git-Bash msbuild invocation quirks, the ~156s `.appxsym` symbol-package
  skip (the wrapper's default `/p:AppxSymbolPackageEnabled=false`, a measured 4–10x speedup), and
  the transient `D8040` / `C1083 Permission denied` obj-lock build failure (clear leftover
  MSBuild/mspdbsrv nodes, retry). Use when asked to build, rebuild, compile-check,
  deploy/reinstall/relaunch the dev or production instance, make the build faster,
  register the loose layout, or diagnose a build/registration failure.
keywords: build, install, deploy, register, relaunch, msbuild, TerminalAppLib, ClCompile, loose layout, Add-AppxPackage, Remove-AppxPackage, AgentmasterDev, Agentmaster, profile picker, resources.pri, PRI210, 0x80070020, 0x80073CF6, am-lock, build mutex, run-m5-tests, appxsym, AppxSymbolPackageEnabled, build speed, faster build, slow build, D8040, GenerateAppxSymbolPackage
keywords-sparse: build the app, install it here, deploy dev, deploy production, reinstall agentmaster, relaunch the instance, compile check, register the layout, the build failed, make the build faster, build is slow
keywords-regex: \bbuild\b|\binstall\b|\bdeploy\b|\bregister\b|\brelaunch\b|\bmsbuild\b|PRI210|0x800704c8|0x80070020|0x80073CF6|resources\.pri|Add-AppxPackage|appxsym|AppxSymbolPackageEnabled|D8040|GenerateAppxSymbolPackage
---

# Agentmaster — build & install on THIS machine

The runnable runbook for every local build flavor and both local installs. The
authoritative source is **`CLAUDE.md`** (*Building FAST*, *Deploy & run*, *Gotchas*) and
**`doc/agentmaster/PROFILES.md`**; if this skill ever disagrees, they win — re-read them.
For cutting a public GitHub release, use the **`release-version`** skill instead (it shares
these build recipes but adds the pipeline/draft/publish flow).

---

## 0. The machine map (what exists here)

| | DEV instance | PRODUCTION instance |
|---|---|---|
| Package | `AgentmasterDev_56k4f06dsfp9r` | `Agentmaster_56k4f06dsfp9r` |
| Manifest source | `Package-Dev.appxmanifest` (default) | `Package-Rel.appxmanifest` (`/p:AgentmasterPackageIdentity=Release`) |
| Registered from | `src\cascadia\CascadiaPackage\bin\x64\Debug\` | `src\cascadia\CascadiaPackage\bin\x64\Release\` |
| Launch | alias `agentmasterdev` · Start "Agentmaster Dev" · `shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App` | alias `agentmaster` · Start "Agentmaster" · `shell:appsFolder\Agentmaster_56k4f06dsfp9r!App` |
| Profile (state) | `~\.agentmaster-dev` | `~\.agentmaster` |
| Updated by | any full **Debug** package build | any full **Release** package build (incl. release pre-validation!) |

- Both are **loose registrations over build-output trees** — a full package build of that
  configuration relinks the binaries UNDER the running instance, so the close→build→relaunch
  rule applies to whichever configuration you're building.
- The per-install profile choice lives in `~\.agentmaster.profiles` (`PFN=dir` lines).
- The user's **Store Windows Terminal** (`Microsoft.WindowsTerminal_…8wekyb3d8bbwe`, under
  `Program Files\WindowsApps`) hosts their live shells — **NEVER close it**. The separate
  `K:\source\windowsterminal` checkout owns `WindowsTerminalDev` — never reuse that identity.
- Build hardware: i9-13900K / 32 threads / 64 GB — unbounded `/m` + `/MP` is fine.

## 1. Golden rules

1. **Mutex around any full package build, launch, register, or instance close** (lib-only
   compile checks and the test harness need NO lock):
   ```bash
   TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "deploy $(git rev-parse --short HEAD)") || exit 1
   # ... the whole cycle ...
   bash tools/am-lock.sh release --token "$TOKEN"     # ALWAYS, even on failure
   ```
   Stale lock from a dead shell (e.g. you killed your own host — see rule 2): `status` shows
   `HELD-BUT-STALE`; `release --token <saved>` then re-acquire (an `acquire` also auto-breaks
   a lock past its 30-min TTL).
2. **THE SELF-KILL CHECK — before closing ANY instance, verify your own shell isn't hosted
   in it.** Claude sessions on this machine often run INSIDE Agentmaster; closing it kills
   you mid-deploy (it happened — exit code 137, half-finished cycle, stale mutex). Walk your
   ancestry first; proceed only if the terminal ancestor is the Store WT (or anything not
   under `K:\source\Agentmaster\*`):
   ```bash
   WINPID=$(ps -p $$ | awk 'NR==2{print $4}')
   powershell -NoProfile -Command "\$p = Get-CimInstance Win32_Process -Filter \"ProcessId=$WINPID\"; while (\$p) { Write-Host (\$p.ProcessId.ToString() + '  ' + \$p.Name + '  ' + \$p.ExecutablePath); \$pp = \$p.ParentProcessId; \$p = if (\$pp) { Get-CimInstance Win32_Process -Filter \"ProcessId=\$pp\" } else { \$null } }"
   ```
   If you ARE hosted in the target instance: stop, tell the user to rehost you (Store WT)
   or to run the close themselves.
3. **Close by path filter, never by image name** (spares the Store WT):
   ```powershell
   Get-CimInstance Win32_Process -Filter "Name='WindowsTerminal.exe' OR Name='OpenConsole.exe'" |
     ? { $_.ExecutablePath -like 'K:\source\Agentmaster\*' } | % { Stop-Process -Id $_.ProcessId -Force }
   ```
   To close ONE instance only, additionally match the tree: `-like '*\bin\x64\Debug\*'` (dev)
   or `'*\bin\x64\Release\*'` (production). Orphaned `claude.exe` children survive the close —
   they are the user's LIVE sessions; **never kill them** (see §5.3 for the side effect).
4. **msbuild from Git Bash**: call the full exe with **dash-style switches** (`-m -v:m
   -p:...`) — `/m`-style gets MSYS-path-mangled into `M:/`. Building a `.vcxproj` directly
   REQUIRES `-p:SolutionDir=K:\source\Agentmaster\` (trailing backslash):
   ```bash
   "/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" \
     'K:\source\Agentmaster\src\cascadia\TerminalApp\TerminalAppLib.vcxproj' \
     -m -v:m -nologo -p:Configuration=Debug -p:Platform=x64 '-p:SolutionDir=K:\source\Agentmaster\'
   ```
   The pwsh wrapper (`tools/Build-Agentmaster.ps1`) handles all of this for package builds.
5. **Background long builds** and grep the tail for `Build OK|Build FAILED|error`. Pipe through
   `grep -ivE 'PRI263|: warning'` — PRI263 is chronic noise. Expected times **with the wrapper's
   default appxsym-skip** (§2): an incremental Debug deploy is **~20–50s** (a 1-TU change ≈ 20s),
   a cold first build still 3–6 min (compile + cppwinrt projection), cold Release 5–10 min. If you
   see a build sit ~2.5 min on a single MSBuild node AFTER the `.msix` line, you forgot the flag —
   that's the ~156s `GenerateAppxSymbolPackage` zip (§2).

## 2. Build recipes (pick the cheapest that answers your question)

| Goal | Recipe | Lock? |
|---|---|---|
| Validate TerminalApp/engine code compiles | `TerminalAppLib.vcxproj` full build (static lib — nothing loaded by a running app) | no |
| Validate exe-side code (WindowEmperor/main) | `WindowsTerminal.vcxproj` **`-t:ClCompile`** (objects only — the exe itself is locked by the running instance) | no |
| Validate a loaded-DLL project (e.g. TerminalSettingsModel) | its `*Lib.vcxproj` full build, or the dll project `-t:ClCompile` — never relink a dll the running app has loaded | no |
| Engine tests (604 checks) | `cmd //c "K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\tests\run-m5-tests.bat"` | no |
| Ship code into the DEV instance | full Debug package: `pwsh -ExecutionPolicy Bypass -File ./tools/Build-Agentmaster.ps1 -NoRestore` | **yes** |
| Ship code into PRODUCTION / pre-validate a release | full Release package: `... Build-Agentmaster.ps1 -NoRestore -Configuration Release -ReleaseIdentity` | **yes** |

First-ever build on a machine: drop `-NoRestore` once (nuget restore; the bundled nuget.exe
can't parse `.slnx` — the wrapper restores `dep\nuget\packages.config` itself).

> 🚀 **The wrapper skips the `.appxsym` symbol package by default — this is the single biggest
> speedup.** A binlog `PerformanceSummary` measured `GenerateAppxSymbolPackage` at **~156s of a
> ~204s Debug package build (77%)** — it zips every full Debug PDB (hundreds of MB) into a
> Store-submission bundle we never use (we ship the `.msix` + portable zips via GitHub; the PDBs
> still emit next to the binaries, so debugging is unaffected). It's a FIXED tax on every build.
> `Build-Agentmaster.ps1` now passes `/p:AppxSymbolPackageEnabled=false`, taking the same build to
> **~20–50s (validated 20.5s for a 1-TU change)**. A **raw `msbuild`** call (the §3/§4 `# or:`
> alternatives, or building the `.slnx` directly) must add `/p:AppxSymbolPackageEnabled=false`
> ITSELF — the flag lives in the wrapper, not the project. Pass `-WithSymbolPackage` to restore the
> `.appxsym` (only for a genuine Store symbol bundle; the GitHub `release.yml` path doesn't need it
> either — it bundles the `.msix`). The remaining variable cost is **header fan-out**: a hot engine
> header (e.g. `ProcessInspect.h`, included by dozens of TUs) recompiles them all — that's the `CL`
> time, not the build system.

## 3. Deploy the DEV instance (the inner loop)

Standing-authorized ("always auto deploy") — no prompting. Full cycle:

```bash
# 0. self-kill check (§1.2)  →  1. mutex (§1.1)
```
```powershell
# 2. close the dev instance (path filter; add '*\bin\x64\Debug\*' to spare production)
# 3. build:  pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1 -NoRestore
# 4. relaunch:
Start-Process "shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App"
```
```bash
# 5. release the mutex
```

- **Re-register only when `Package-Dev.appxmanifest` changed**:
  `Add-AppxPackage -Register "K:\source\Agentmaster\src\cascadia\CascadiaPackage\bin\x64\Debug\AppxManifest.xml" -ForceUpdateFromAnyVersion`
- Verify: `tail ~/.agentmaster-dev/hooks.log` → `[engine] bridge listening` + observer census
  lines; `Get-AppxPackage -Name AgentmasterDev`.

## 4. Install/refresh PRODUCTION locally (the Release loose layout)

What "install regular Agentmaster on this PC" means — no GitHub involved:

```bash
# 0. self-kill check → 1. mutex
# 2. close ONLY production if running (path filter on '*\bin\x64\Release\*')
# 3. build the Release-identity package:
pwsh -ExecutionPolicy Bypass -File ./tools/Build-Agentmaster.ps1 -NoRestore -Configuration Release -ReleaseIdentity
```
```powershell
# 4. stamp a real version into the LAYOUT manifest (build output regenerates it each build —
#    re-stamp after every Release build; also the fix for the §5.3 collision):
$p = 'K:\source\Agentmaster\src\cascadia\CascadiaPackage\bin\x64\Release\AppxManifest.xml'
[xml]$m = Get-Content $p; $m.Package.Identity.Version = '0.2.0.0'; $m.Save($p)
# 5. register + launch:
Add-AppxPackage -Register $p -ForceUpdateFromAnyVersion
Start-Process "shell:appsFolder\Agentmaster_56k4f06dsfp9r!App"
```
```bash
# 6. release the mutex
```

- **First launch shows the profile picker** (GUI — only the user can click; poll
  `~\.agentmaster.profiles` for the `Agentmaster_…=` line to know it landed). Production
  picks **Production** (`~\.agentmaster`). Headless bypass: set `AGENTMASTER_PROFILE=<dir>`
  on the process.
- **Pre-seed terminal settings for a fresh profile** (the seeder copies from the package's
  own LocalState, which is EMPTY for a never-run package → WT defaults). Before the user
  clicks, copy a known-good pair — the seed/copy semantics are copy-if-absent, so pre-placed
  files win:
  ```bash
  mkdir -p "$USERPROFILE/.agentmaster/terminal" && cp -n "$USERPROFILE/.agentmaster-dev/terminal/"{settings.json,state.json} "$USERPROFILE/.agentmaster/terminal/" 2>/dev/null
  ```
- **Caveat**: this production install updates in place on ANY future Release package build
  (including a `release-version` pre-validation) — close production first for those, then
  re-stamp the version (step 4) since the build resets it to 0.0.1.0.
- The permanent alternative: install the signed `.msixbundle` from a GitHub release (cert
  import needs an admin/UAC step) — replaces the loose registration's coupling to the repo
  tree. Same-family replace may need `Remove-AppxPackage` of the loose one first (§5.3 risk).

## 5. Troubleshooting (all field-verified on this machine)

1. **`PRI210 / 0x800704c8` — "File move failed … resources.pri"** during a package build:
   the REGISTERED layout keeps `resources.pri` memory-mapped (no owning process visible).
   Fix: `rm src/cascadia/CascadiaPackage/bin/x64/<Config>/resources.pri` and rebuild —
   MakePri then *creates* instead of overwriting. (First occurrence per layout is ~normal.)
2. **Exe link fails / `LNK1104` on `WindowsTerminal.exe`**: that configuration's instance is
   still running — you skipped the close (or another window respawned). Re-run the path-
   filtered close for THAT tree, rebuild.
3. **`Add-AppxPackage -Register` fails `0x80073CF6` with inner `0x80070020`** ("activatable-
   Class.collector … file in use"): you're registering a package whose **full name**
   (`Name_Version_x64__hash`) equals one removed EARLIER whose AppRepository staging folder
   is still pinned — orphaned `claude.exe` children of the removed instance keep it alive,
   and Windows defers cleanup. Diagnose: `Get-AppPackageLog -ActivityID <guid from the
   error>`. Fix: **bump the layout manifest's `Identity Version`** (different full name —
   §4 step 4) and re-register. **Never kill the orphaned claudes** (the user's live
   sessions); the debris clears when they exit or after a reboot.
4. **Stale `build-launch` mutex** (`HELD-BUT-STALE` — usually a dead shell, often the
   self-kill): `bash tools/am-lock.sh release --token <its token>` if you have it, else
   `acquire --wait` auto-breaks past TTL. Don't `--force` a FRESH lock — that's another
   agent mid-build.
5. **Picker never appears on first launch**: it only shows when there's no saved choice for
   that PFN in `~\.agentmaster.profiles`, no `AGENTMASTER_PROFILE` env, and no `.portable`
   marker — and never for `-Embedding` (defterm) activations. Delete that PFN's line to
   re-ask; check the dialog exists via an `EnumWindows` for class `#32770` on the new pid.
6. **Which instance is which / is it up?**
   ```powershell
   Get-CimInstance Win32_Process -Filter "Name='WindowsTerminal.exe'" |
     ? { $_.ExecutablePath -like 'K:\source\Agentmaster\*' } | select ProcessId, ExecutablePath
   ```
   `…\bin\x64\Debug\…` = dev, `…\bin\x64\Release\…` = production. Engine proof: tail
   `<profile>\hooks.log` for `[engine] bridge listening` / `[observer] census`.
7. **Build fails with `error C1083: Cannot open compiler generated file: '…\X.obj': Permission
   denied` + `cl : command line error D8040: error creating or communicating with child process`**
   — often MASKED by the wapproj's generic `error MSB4181: The "MSBuild" task returned false but did
   not log an error` (grep the full log past `MSB4181` for the real `C1083`/`D8040`). This is a
   TRANSIENT toolchain-contention failure, **not a code error** (the same source builds clean on
   retry): leftover idle `MSBuild.exe` worker nodes (node-reuse keeps ~11 resident after a build) +
   `mspdbsrv.exe` holding locks on the `obj\` outputs, racing a Defender scan. Fix — kill the leftover
   build daemons (one CreationDate cluster == one build; you hold the mutex so no FOREIGN build should
   be running), then rebuild incrementally (up-to-date check recompiles only the failed TUs):
   ```powershell
   Get-CimInstance Win32_Process -Filter "Name='MSBuild.exe' OR Name='mspdbsrv.exe' OR Name='cl.exe'" |
     % { Stop-Process -Id $_.ProcessId -Force }   # idle build nodes; a running VS respawns its own
   ```
   Hit + recovered live this session (first build failed, retry after the kill went clean). Cheap
   prevention: a pre-build `Get-Process MSBuild,mspdbsrv,cl` check before the FIRST build of a cycle
   (if any are resident from a prior aborted build, clear them first).

## 6. Post-install verification checklist

- `Get-AppxPackage -Name Agentmaster` / `-Name AgentmasterDev` → expected `PackageFullName` +
  `InstallLocation`.
- `~\.agentmaster.profiles` has one line per install pointing at the right profile.
- `<profile>\hooks.log` shows the bridge + observer alive; `<profile>\bridge.json` carries the
  CURRENT pid's pipe; the regenerated `<profile>\agentmaster-hook.ps1` discovery line points
  INSIDE that profile (`$disc = '<profile>\bridge.json'`).
- Both instances running simultaneously: each sees the other's claudes as EXTERNAL
  (observe-only) on its Triage Board — correct, by design.
