---
name: build-install
description: |
  Build and install Agentmaster ON THIS MACHINE — every build flavor (lib-only compile
  check, ClCompile-only for locked binaries, full Debug package, full Release package,
  the engine test harness) and both local installs: the DEV instance (AgentmasterDev,
  Debug loose layout, close → build → relaunch) and the PRODUCTION instance (Agentmaster,
  Release loose layout: build -ReleaseIdentity → stamp version → register → launch →
  profile picker), including the HOT deploy of either instance — build into the layout slot it is
  NOT registered from (Build-Agentmaster.ps1 -LayoutDir; production slots bin\x64\Release /
  Release-B, dev slots bin\x64\Debug / Debug-B) while it keeps running, then a few-second swap
  (tools\Swap-AgentmasterLayout.ps1 [-PackageName AgentmasterDev]: GUI-only stop, drain,
  re-register, launch, rollback). Encodes the machine map (two registered identities side by side, one
  repo tree, the Store WT to never touch) and the battle-tested troubleshooting: the
  self-kill check (is MY shell hosted in the instance I'm about to close?), PRI210
  mapped resources.pri, 0x80070020 same-full-name re-register collisions (orphaned
  claudes pin a removed package — bump the layout version, never kill them), stale
  build-mutex reclaim, Git-Bash msbuild invocation quirks, the ~156s `.appxsym` symbol-package
  skip (the wrapper's default `/p:AppxSymbolPackageEnabled=false`, a measured 4–10x speedup), the
  transient `D8040` / `C1083 Permission denied` obj-lock build failure (clear leftover
  MSBuild/mspdbsrv nodes, retry), and the STALE-INCREMENTAL-BUILD class after a `SessionModels.h`/
  `SessionRegistry.*`/other widely-included header or a `.idl` changes struct layout or WinRT
  properties — a plain `LNK2019` on a symbol that IS defined in source, an `/RTC1` stack-corruption
  fail-fast, or a genuine `STATUS_HEAP_CORRUPTION` (`0xC0000374`) crash on launch, all fixed the
  SAME way (purge `Generated Files` + both engine projects' `obj`, full rebuild with `-ClMpCount 6`
  to dodge a PCH virtual-memory-exhaustion failure of its own under unbounded `/m`), confirmed 3x
  in one session. Use when asked to build, rebuild, compile-check, deploy/reinstall/relaunch the
  dev or production instance, make the build faster, register the loose layout, or diagnose a
  build/registration/crash-on-launch failure.
keywords: build, install, deploy, register, relaunch, hot deploy, hot build, minimal downtime, zero downtime, blue green, layout slot, Release-B, Debug-B, PackageName, swap, LayoutDir, AgentmasterLayoutDir, Swap-AgentmasterLayout, InstallLocation, C1085, not enough space, obj junction, msbuild, TerminalAppLib, ClCompile, loose layout, Add-AppxPackage, Remove-AppxPackage, AgentmasterDev, Agentmaster, profile picker, resources.pri, PRI210, 0x80070020, 0x80073CF6, am-lock, build mutex, run-m5-tests, appxsym, AppxSymbolPackageEnabled, build speed, faster build, slow build, D8040, GenerateAppxSymbolPackage, stale build, incremental build, LNK2019, unresolved external, heap corruption, 0xC0000374, STATUS_HEAP_CORRUPTION, RTC1, stack corruption, ClMpCount, PCH, C3859, C1076, crash on launch, purge Generated Files
keywords-sparse: build release without closing it, deploy release with minimal downtime, build while production runs, swap to the new build, roll back the release, deploy dev by swap, hot deploy dev, build dev while it runs, dev swap, build the app, install it here, deploy dev, deploy production, reinstall agentmaster, relaunch the instance, compile check, register the layout, the build failed, make the build faster, build is slow, it crashes on launch, unresolved external symbol, heap corruption crash, deploy dev please, build and deploy dev
keywords-regex: \bbuild\b|\binstall\b|\bdeploy\b|\bregister\b|\brelaunch\b|\bmsbuild\b|PRI210|0x800704c8|0x80070020|0x80073CF6|resources\.pri|Add-AppxPackage|appxsym|AppxSymbolPackageEnabled|D8040|GenerateAppxSymbolPackage|LNK2019|0xC0000374|STATUS_HEAP_CORRUPTION|ClMpCount|C3859|C1076
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
| Registered from | **a layout slot** (§4a): `src\cascadia\CascadiaPackage\bin\x64\Debug\` (A), `…\bin\x64\Debug-B\` (B), or a folder outside the repo — since 2026-10-13 dev alternates between `Q:\agentmaster-layouts\Debug` and `Debug-B` — always read `(Get-AppxPackage AgentmasterDev).InstallLocation` | **a layout slot** (§4a): `…\bin\x64\Release\` (A), `…\bin\x64\Release-B\` (B), or a folder outside the repo — **since 2026-10-12 one of `Q:\agentmaster-layouts\Release` / `Release-B`** (`Release-B` since 2026-10-17; K: is short of space) — always read `(Get-AppxPackage Agentmaster).InstallLocation` |
| Launch | alias `agentmasterdev` · Start "Agentmaster Dev" · `shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App` | alias `agentmaster` · Start "Agentmaster" · `shell:appsFolder\Agentmaster_56k4f06dsfp9r!App` |
| Profile (state) | `~\.agentmaster-dev` | `~\.agentmaster` |
| Updated by | any full **Debug** package build | any full **Release** package build (incl. release pre-validation!) |

- Both are **loose registrations over build-output trees** — a full package build of that
  configuration relinks the binaries UNDER the running instance, so the close→build→relaunch
  rule applies to whichever configuration you're building. **Exception — the HOT deploy (§4a),
  for either identity:** building a layout into the slot that instance is NOT registered from
  (`Build-Agentmaster.ps1 -LayoutDir`) touches nothing it has loaded, so it keeps running for the
  whole build and only the few-second swap is downtime.
- Build intermediates live on **Q:** through junctions (K: is chronically full — §5.9):
  `obj\x64\Debug` → `Q:\tmp\agentmaster-dev-build`, `obj\x64\Release` →
  `Q:\tmp\agentmaster-release-build` (both user-chosen).
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

**Needs the user's permission** (CLAUDE.md *Development Rules*: build, deploy and install are
asked for first — the old "always auto deploy" standing authorization is REVOKED, and CLAUDE.md
wins over this skill). **Prefer the HOT deploy (§4a)**: dev keeps running for the whole build
and the swap costs seconds (8.2 s measured, 2026-10-05). The close cycle below is the fallback.

⚠ Dev runs from ONE OF TWO slots (`bin\x64\Debug` or `bin\x64\Debug-B` — the hot deploy
alternates them), so every filter and the build target follow its REGISTERED folder (`$devDir`).
A hard-coded `bin\x64\Debug` is a silent no-op while dev runs from `Debug-B`: the close finds
nothing, the build writes the unregistered folder, and the relaunch starts the old binaries.
Full cycle:

```bash
# 0. self-kill check (§1.2)  →  1. mutex (§1.1)
# 0.5. check what changed since the last deploy - decides step 3's flavor (§5.8):
git diff --stat <last-deployed-commit>..HEAD -- '**/SessionModels.h' '**/SessionRegistry.*' '**/*.idl'
# non-empty output => purge before building (§5.8), don't bother trying incremental first
```
```powershell
# 1.5. the folder dev is registered from (Debug or Debug-B):
$devDir = (Get-AppxPackage AgentmasterDev).InstallLocation.TrimEnd('\')
# 2. close the dev instance - GUI only, path filter "$devDir\*" (the trailing '\*' keeps Debug from
#    matching Debug-B; it also spares production), then wait for its OpenConsole hosts to drain
# 3. build INTO $devDir - EITHER:
#    incremental (no header/idl hit above):  pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1 -NoRestore -LayoutDir $devDir
#    OR purge+clean (header/idl hit, §5.8):  rm -rf "src/cascadia/TerminalApp/Generated Files" "obj/x64/Debug/TerminalApp" "obj/x64/Debug/TerminalAppLib"
#                                             pwsh -ExecutionPolicy Bypass -File .\tools\Build-Agentmaster.ps1 -NoRestore -ClMpCount 6 -LayoutDir $devDir
# 4. relaunch:
Start-Process "shell:appsFolder\AgentmasterDev_56k4f06dsfp9r!App"
# 4.5. verify it didn't crash on startup - wait for the window restore to genuinely finish
#      (grep hooks.log for "[rehome] window <id> resumed=N" and then "[ui-stall] watchdog armed",
#      not just "process exists 5s after launch" - a heap-corruption crash can take longer than
#      that) before calling it stable; a Debug/dev pid disappearing after a Start-Process ~= it crashed.
```
```bash
# 5. release the mutex
```

- **Re-register only when `Package-Dev.appxmanifest` changed** — and then through the swap
  (§4a, into the other slot), which stamps a version above the registered one. A fresh build
  writes 0.0.1.0 into the layout manifest, and development-mode re-registration refuses a version
  that is not newer (the `Swap-AgentmasterLayout.ps1` notes), so a bare
  `Add-AppxPackage -Register "$devDir\AppxManifest.xml" -ForceUpdateFromAnyVersion` works only
  while dev is still registered at 0.0.1.0, or after you stamp the manifest by hand.
- The close cycle itself never re-registers, and that is fine: a launch does not re-read the
  layout manifest. Checked 2026-10-05: dev ran for two days from a folder whose manifest the
  10-03 build had rewritten to 0.0.1.0 while its registration said 0.0.1.1.
- Verify: `tail ~/.agentmaster-dev/hooks.log` → `[engine] bridge listening` + observer census
  lines; `Get-AppxPackage -Name AgentmasterDev`.

## 4. Install/refresh PRODUCTION locally (the Release loose layout)

What "install regular Agentmaster on this PC" means — no GitHub involved. **For a routine
"build and deploy release" prefer the HOT deploy (§4a)** — this classic close-first flow keeps
production down for the whole build (15–30 min when a header change forces a purge).

```bash
# 0. self-kill check → 1. mutex
# 2. close ONLY production if running — GUI only (never OpenConsole.exe), path filter on its
#    CURRENT InstallLocation (slot A or B — §4a), then wait for its ConPTY hosts to drain
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
- **Caveat**: a Release package build writes slot A (`bin\x64\Release`) unless given
  `-LayoutDir` — including a `release-version` pre-validation. When production is registered
  from slot A that relinks it in place (close it first, then re-stamp the version — the build
  resets it to 0.0.1.0); when production runs from slot B, a default build is harmless to it.
- The permanent alternative: install the signed `.msixbundle` from a GitHub release (cert
  import needs an admin/UAC step) — replaces the loose registration's coupling to the repo
  tree. Same-family replace may need `Remove-AppxPackage` of the loose one first (§5.3 risk).

## 4a. HOT deploy of PRODUCTION (and DEV) — build while it runs, swap in seconds (the default)

The same two scripts serve the dev identity; its variant is at the end of this section.

Production is a loose registration over a package LAYOUT folder, and it locks only THAT folder
(loaded images + the mapped `resources.pri`). Nothing it loads comes from the intermediate
`bin\x64\Release\<Project>\` / `obj\` trees (verified: its module list is 100% the layout; the
layout holds plain copies, link count 1). So the layout alternates between two **slots**, and
each deploy builds into the slot production is NOT registered from:

| slot | folder | build it with |
|---|---|---|
| A | `src\cascadia\CascadiaPackage\bin\x64\Release\` | no `-LayoutDir` (stock) |
| B | `src\cascadia\CascadiaPackage\bin\x64\Release-B\` | `-LayoutDir .\src\cascadia\CascadiaPackage\bin\x64\Release-B` |

The mechanism: `CascadiaPackage.wapproj` sets DesktopBridge's `OutputPath` (the layout folder,
defaulted only when empty) from `/p:AgentmasterLayoutDir` BEFORE the DesktopBridge props import;
every referenced project still builds into its usual `bin\`/`obj\` (checked with msbuild
`-getProperty:OutDir` — the wapproj moves, `TerminalApp` stays at `bin\x64\Release\TerminalApp\`).
The icon files move with it: `CopyAgentmasterImagesToLayout` copies `res\terminal\images-Dev\**`
into `$(TargetDir)Images\`, i.e. into the slot. Until 2026-10-02 that target hard-coded
`bin\x64\<Config>\`, so slot B never got its `Images\`. Production ran from B (0.6.8.12, 0.6.8.14)
with a **blank taskbar and Start icon**, and its taskbar button showed nothing but the unread-count
overlay. The swap now refuses a layout that is missing any icon file its manifest names.

```bash
# 0. self-kill check — your shell must NOT be inside production (the swap stops its GUI; the
#    swap script refuses in that case). Hosted in dev / the Store WT = fine.
# 1. mutex — and `am-lock.sh refresh --token` on EVERY poll: a cold Release build outlasts the
#    30-min TTL, and an expired lock can be broken by the next agent's acquire.
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "hot-release $(git rev-parse --short HEAD)") || exit 1
# 2. target slot = the one production is NOT registered from:
powershell -NoProfile -Command "(Get-AppxPackage Agentmaster).InstallLocation"
# 3. space (K: AND Q:), the §5.8 purge check, and the target slot's resources.pri (§5.1):
rm -f src/cascadia/CascadiaPackage/bin/x64/<target-slot>/resources.pri
# 4. build into the target slot — production keeps serving the whole time:
pwsh -ExecutionPolicy Bypass -File ./tools/Build-Agentmaster.ps1 -NoRestore -Configuration Release -ReleaseIdentity -ClMpCount 6 -LayoutDir ./src/cascadia/CascadiaPackage/bin/x64/Release-B   # (or no -LayoutDir for slot A)
# 5. the swap — the ONLY downtime. Stamps registered+1, stops production's GUI (never its
#    OpenConsole hosts), waits for them to drain, re-registers the new slot, launches, times it:
pwsh -ExecutionPolicy Bypass -File ./tools/Swap-AgentmasterLayout.ps1 -LayoutDir ./src/cascadia/CascadiaPackage/bin/x64/Release-B   # -WhatIf = dry run
# 6. verify (§6: bridge line for the new pid, restore lines, no crash dump) → release the mutex
bash tools/am-lock.sh release --token "$TOKEN"
```

- **Measured (2026-09-26, first run, 0.6.8.11 → 0.6.8.12, a 3-window workspace):** production ran
  through a cold 25-min Release build untouched; the swap's downtime was **14.5 s** — GUI stop
  instant, **11.7 s** for every old ConPTY host to drain gracefully (0 left), register 2.5 s,
  launch ~1 s. The drain is the bulk: each host ends its tab's pwsh + claude.exe with CTRL_CLOSE
  (claude's SessionEnd hook runs), and it is what keeps the swap from orphaning sessions.
- **The drain grows with the number of LIVE tabs, and the register cannot start before it ends.**
  Every old ConPTY host is a process of the OLD package, and `Add-AppxPackage -Register` refuses
  while any of them lives (`0x80073D02`, "the following apps need to be closed
  Agentmaster_<old>"). On 2026-10-01 (0.6.8.13 → 0.6.8.14, a 414-tab window with 72 live tabs, 88
  claudes on the box), 72 hosts were still alive at the old 30 s drain cap and all were gone by
  ~87 s. The register failed, the script rolled back, and the rollback instance (still on its
  splash) had to be stopped for a second swap, which then took 1.5 s. Production was down ~2 min
  in total. The script now drains up to `-DrainTimeoutSec` (default **120**) and then RETRIES the
  register every 3 s while the old package still reads in use, for up to `-RegisterRetrySec`
  (default **120**), before it rolls back. Its result reports `RegisterAttempts`. If a swap ever
  rolls back anyway, re-check slot A/B for old `OpenConsole.exe` hosts. Once they are gone,
  rerun the swap at once, while the rollback instance is still on its splash and has restored
  nothing.
- **The new instance can then PARK on the "Reopen your N previous Agentmaster windows?" dialog**
  (a `#32770` window, nothing else — the engine/bridge start only after the answer; it appears when
  >1 window was open at the stop). The measured downtime ends at the new GUI's START, not at the
  answer, so tell the user to click it (or ask before clicking it for them).
- **Rollback is one command**: the previous slot is untouched, so
  `Swap-AgentmasterLayout.ps1 -LayoutDir <previous slot>` re-registers it (the next version
  number) — seconds, no rebuild. If the swap's register step itself fails, the script relaunches
  the still-registered old version on its own before rethrowing.
- **Close / verify production by its InstallLocation, never by a hardcoded `bin\x64\Release`** —
  a filter on `*\bin\x64\Release\*` does NOT match slot B (`Release-B\`).
- The next deploy's target is the OTHER slot. Before building into a slot, make sure nothing
  still runs from it (a straggler ConPTY host of the previous instance would lock it — the swap
  prints `HostsLeft`).
- The version keeps `Major.Minor.Build` and bumps the revision (0.6.8.11 → 0.6.8.12): the in-app
  updater compares the first three against GitHub releases.

### The DEV variant (first run 2026-10-05)

| slot | folder | build it with |
|---|---|---|
| A | `src\cascadia\CascadiaPackage\bin\x64\Debug\` | no `-LayoutDir` (stock) |
| B | `src\cascadia\CascadiaPackage\bin\x64\Debug-B\` | `-LayoutDir .\src\cascadia\CascadiaPackage\bin\x64\Debug-B` |

```bash
# 0. self-kill check: your shell must NOT be inside DEV (inside production is fine - the dev
#    swap stops only dev's GUI). 1. mutex, refreshed on every poll, as above.
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "hot-dev $(git rev-parse --short HEAD)") || exit 1
# 2. target slot = the one dev is NOT registered from:
powershell -NoProfile -Command "(Get-AppxPackage AgentmasterDev).InstallLocation"
# 3. the §5.8 purge check (purge if it hits), and the target slot's resources.pri (§5.1):
rm -f src/cascadia/CascadiaPackage/bin/x64/<target-slot>/resources.pri
# 4. build the Debug package (NO -ReleaseIdentity) into the target slot - dev keeps serving:
pwsh -ExecutionPolicy Bypass -File ./tools/Build-Agentmaster.ps1 -NoRestore -MaxCpu 4 -ClMpCount 6 -LayoutDir 'K:\source\Agentmaster\src\cascadia\CascadiaPackage\bin\x64\Debug-B'
# 5. before the swap: the swap ends each dev claude with CTRL_CLOSE, so check none is mid-turn
#    (agentmasterdev sessions --json: derivedState / turnInFlight / presence). Then dry-run it:
pwsh -ExecutionPolicy Bypass -File ./tools/Swap-AgentmasterLayout.ps1 -LayoutDir 'K:\source\Agentmaster\src\cascadia\CascadiaPackage\bin\x64\Debug-B' -PackageName AgentmasterDev -WhatIf
pwsh -ExecutionPolicy Bypass -File ./tools/Swap-AgentmasterLayout.ps1 -LayoutDir 'K:\source\Agentmaster\src\cascadia\CascadiaPackage\bin\x64\Debug-B' -PackageName AgentmasterDev
# 6. verify (~/.agentmaster-dev/hooks.log: the new pid's "bridge listening", "[rehome] ...
#    resumed=N", "[ui-stall] watchdog armed"; no crash dump; no Application Error) -> release
bash tools/am-lock.sh release --token "$TOKEN"
```

- **Measured (2026-10-05, dev 0.0.1.1 @ `Debug` → 0.0.1.2 @ `Debug-B`, 42 tabs, 16 live
  claudes, all at rest):** a purged build (header + `.idl` changes) took 295 s with
  `-MaxCpu 4 -ClMpCount 6` and 44 GB of commit free (lowest sampled free 35.9 GB, polled every
  45 s; no C1002) while dev kept serving. The swap's downtime was **8.2 s**: GUI stop, the 16 hosts drained by themselves in
  5.8 s (0 left), register 2.1 s at the first attempt, launch. The window restore re-homed all 42
  tabs, and no claude was orphaned.
- `-WhatIf` prints ~45 `What if: ... Set Alias` lines first (the Appx module's alias creation under
  WhatIfPreference) — noise, not a problem.
- **The dev CLI can show a PRODUCTION claude as a live dev session.** A conversation with a record
  in both profiles (here `a77419cf`) is bound by claude's shared presence heartbeat, so
  `agentmasterdev sessions` listed production's claude (pid 16640) under the dev record. After
  the swap that pid was still alive and read like an orphan. Before calling a leftover claude an
  orphan, check its `--settings` path (`.agentmaster` = production, `.agentmaster-dev` = dev) and
  its parent chain (CLI.md §11).
- Dev, like production, then runs from the other slot. The next dev deploy targets the old one,
  or uses the §3 close cycle, which follows the registered folder (`$devDir`).

## 5. Troubleshooting (all field-verified on this machine)

1. **`PRI210 / 0x800704c8` — "File move failed … resources.pri"** during a package build:
   the REGISTERED layout keeps `resources.pri` memory-mapped (no owning process visible).
   Fix: `rm src/cascadia/CascadiaPackage/bin/x64/<Config>/resources.pri` and rebuild —
   MakePri then *creates* instead of overwriting. **Recurs reliably, not just "first occurrence
   per layout"** — hit on the FIRST build of a deploy cycle twice in one session, both times
   after the dev instance had been running a while before being closed. Cheap prevention: delete
   it proactively BEFORE the first build of a cycle (same idiom as §5.7's daemon-clear) instead
   of waiting to hit the error and retry:
   ```bash
   rm -f "src/cascadia/CascadiaPackage/bin/x64/Debug/resources.pri"
   ```
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
8. **A binary that BUILDS clean but then crashes on launch, or a build that LINK-fails on a
   symbol you can `grep` and find fully defined in source — after `AgentMaster/SessionModels.h`,
   `SessionRegistry.{h,cpp}`, or another widely-included engine header changed struct layout, OR
   after a `.idl` gained/changed a WinRT property.** This is MSBuild's incremental up-to-date
   check under-tracking a dependency — some TUs get recompiled against the NEW layout/ABI, others
   keep a STALE `.obj` compiled against the old one, and the mismatch either fails to link
   (`LNK2019` on a symbol that genuinely exists — confirmed twice: `_OnPullDraftClicked`/
   `_UpdateDraftPullButton` both defined in `AgentManagerContent.AutoTesting.cpp`, unresolved
   anyway) or links fine and corrupts memory at runtime (confirmed twice more: an `/RTC1` stack
   guard-byte failure inside `TabHeaderControl`'s ctor after `TerminalTabStatus.idl` gained new
   properties — the NATIVE `.g.h`/`.g.cpp` WinRT glue was stale while the consumer projection
   wasn't, proven by comparing `Generated Files\*.g.cpp` timestamps; and a genuine
   `STATUS_HEAP_CORRUPTION` (`0xC0000374`) inside `TerminalPage`'s ctor / `TabRowControl`
   activation, 100% reproducible with IDENTICAL registers across two separate launches of the
   same binary). **Don't try to diagnose these individually — the fix is the same and cheap:
   purge the generated glue + both engine projects' `obj`, then a FULL rebuild:**
   ```bash
   rm -rf "src/cascadia/TerminalApp/Generated Files" "obj/x64/<Config>/TerminalApp" "obj/x64/<Config>/TerminalAppLib"
   pwsh -ExecutionPolicy Bypass -File ./tools/Build-Agentmaster.ps1 -NoRestore -ClMpCount 6
   ```
   **`-ClMpCount 6` is not optional here** — purging forces the WHOLE project to recompile in one
   shot, and unbounded `/m` parallelism against every PCH-consuming TU at once measured a HARD
   failure of its own (`error C3859: Failed to create virtual memory for PCH` + `error C1076:
   compiler limit: internal heap limit reached`, tens of TUs at once) on this 32-thread/64GB
   machine; throttled to 6 it's clean. Costs ~150–250s instead of the usual ~20–50s, but is the
   only reliable fix — confirmed 3 separate times in one session: every purge+clean-rebuild
   produced a stable binary (verified by watching a 45–50-tab window-restore run to completion,
   `Responding=True`, no new `%LOCALAPPDATA%\CrashDumps\WindowsTerminal.exe.*.dmp`), and every
   plain incremental rebuild attempted instead of a purge, after one of these header/idl changes,
   reproduced a failure. **Default to purging whenever `git diff` on the commits since your last
   known-good deploy touches `SessionModels.h`, `SessionRegistry.{h,cpp}`, or any `.idl` file** —
   don't wait for the crash to prove it's needed.

9. **`C1085` / `C1088` / `MSB3191` "There is not enough space on the disk"** (a different failure
   from the PCH-*memory* `C3859`/`C1076`): K: is chronically near-full, and a cold/purged Release
   build writes ~20 GB of intermediates. Both configurations' `obj\x64\<Config>` are therefore
   **junctions to Q:** — `Debug` → `Q:\tmp\agentmaster-dev-build`, `Release` →
   `Q:\tmp\agentmaster-release-build` (user-chosen; `cmd /c dir /AL obj\x64` shows them). If a
   build dies on space anyway: check `Get-PSDrive K,Q`; confirm the junctions still exist (on
   2026-09-26 the whole `obj\` folder — junctions included — was deleted by something outside
   the build while K: sat at 0 bytes; the Q: targets survived); recreate a missing one with
   `New-Item -ItemType Junction -Path K:\source\Agentmaster\obj\x64\<Config> -Target <Q: dir>`.
   Before rebuilding after a disk-full failure, **purge every project the log names in a
   disk-full error** (its `obj\x64\<Config>\<Project>`) plus any purged-and-regenerating project
   and its `Generated Files` — a truncated `.pch`/`.obj` with a fresh timestamp can pass the
   incremental up-to-date check.

## 6. Post-install verification checklist

- `Get-AppxPackage -Name Agentmaster` / `-Name AgentmasterDev` → expected `PackageFullName` +
  `InstallLocation`.
- The registered layout (InstallLocation) has its `Images\` folder: 192 files from
  `res\terminal\images-Dev`. Without it the app runs normally, but its taskbar and Start icons are
  blank; a taskbar button showing ONLY the red unread-count disc means exactly this. The swap
  script refuses such a layout; a plain `Add-AppxPackage -Register` does not.
- `~\.agentmaster.profiles` has one line per install pointing at the right profile.
- `<profile>\hooks.log` shows the bridge + observer alive; `<profile>\bridge.json` carries the
  CURRENT pid's pipe; the regenerated `<profile>\agentmaster-hook.ps1` discovery line points
  INSIDE that profile (`$disc = '<profile>\bridge.json'`).
- Both instances running simultaneously: each sees the other's claudes as EXTERNAL
  (observe-only) on its Triage Board — correct, by design.
