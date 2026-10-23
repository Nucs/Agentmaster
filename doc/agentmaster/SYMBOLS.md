# Release symbols — every PDB, for every binary, in every release

> Status (2026-10-23): **implemented in the release pipeline, not run on GitHub yet** — the next `release.yml`
> run (or a `ci.yml` run of a push) is the first to execute it. Every piece was checked on this machine against
> real build outputs, without building the app (§6). The owner's decision of 2026-10-23: *"We are opensource, we
> should provide maximum pdbs at any possibility"*, then, after the measurements of §2, **public PDBs inside the
> portable zips + the full PDBs in a separate symbols zip; the msixbundle unchanged; local Debug and Release
> builds keep a PDB next to every binary (never shipped)**.

## 1. What ships where

| Where | What | Size (x64, measured 2026-10-23) |
|---|---|---|
| `Agentmaster_<ver>_<arch>.zip` (portable) | every binary **plus its PUBLIC PDB beside it** (function names and addresses; listed in `.am-manifest` like every shipped file) | ~27 MB → ~42 MB (est.) |
| `Agentmaster_<ver>_<arch>_symbols.zip` (new) | the **FULL PDB** of every binary we build (types, line numbers, locals), flat, **Source-Linked and srcsrv-indexed** to the release's commit | ~190 MB (1 GB unpacked) |
| `Agentmaster_<ver>.msixbundle` | unchanged: no PDBs (the owner's choice) | ~50 MB |
| local builds (`bin\`, `Q:\agentmaster-layouts\*`) | a PDB beside every binary: full in Release, FASTLINK in Debug (§5); never shipped | — |

What has no PDB at all: `Microsoft.UI.Xaml.dll` (Microsoft publishes its symbols on its own server,
`srv*https://msdl.microsoft.com/download/symbols`), `duckdb.dll` (DuckDB's release carries no debug record) and
`TerminalThemeHelpers.dll` (a NuGet binary without a PDB).

The asset name `…_symbols.zip` matches none of the patterns that pick release assets: the in-app updater takes
`*_x64.zip` / `*_arm64.zip` / `*.msixbundle` / `*.cer` by suffix (`Updater.h` `ParseReleaseObj`), and
`Install-Agentmaster.ps1` takes `_<arch>\.zip$`. That holds for every build already installed, too.

## 2. Why this shape — the measurements

**The size.** A Release build makes **959 MB of full PDBs per architecture** (17 files; `TerminalApp.pdb` alone is
230 MB, `Microsoft.Terminal.Settings.Editor.pdb` 184 MB, `…Settings.Model.pdb` 144 MB). Zipped with the release's
own zip tool (Windows' bsdtar): **~190–195 MB**. The portable zip it would join is 27 MB.

**A PDB cannot be embedded in a native binary.** MSVC always writes debug information to a separate `.pdb`; the
`.exe` / `.dll` carries only a CodeView ("RSDS") record: the PDB's file name, a GUID and an age, which a debugger
matches. An embedded PDB exists only for .NET (portable PDBs); Visual C++ 6's in-image CodeView (`/PDB:NONE`) was
removed in 2005. The closest equivalent is a PDB beside its binary, in each architecture's own package — each
architecture's build job makes and packages its own PDBs, so nothing is shared across architectures.

**"Our code only" does not shrink it.** Measured with a PDB-format reader written for this (`pdbsize.py` /
`pdbowners.py`, kept out of the repository: they read the MSF container, the TPI / IPI type records, the DBI
module list and every symbol record, and attribute each byte to the namespace it describes):

- `TerminalApp.pdb` (230 MB): **type records 44%** (101 MB), per-module symbols 29%, global/public symbols 10%,
  the `/names` string table 7%, the `/debugtype:fixup` stream 2.5%.
- Of the 101 MB of types, **C++/WinRT's `winrt::impl` is 51%, the STL 26%**, the Windows projection 7%; the
  Windows Terminal + Agentmaster classes and the engine together ~7%.
- Dependency **libraries** linked in (vcpkg, the C runtime and STL libraries, SDK import libraries): ~3 MB of the
  230. Dependency **binaries**: we have no PDBs for them anyway (above).
- Across all 17 PDBs, ~666 of the 959 MB describe dependency **headers** (C++/WinRT, STL, WIL, SDK) — but that
  code is compiled INTO our functions (template instantiations, inlined calls), and our own classes are made of
  those types (`std::wstring` members, `com_ptr`s). MSVC has no "debug information for my code only" switch, and
  cutting it would lose the frames many crashes sit in (STL iterators, `winrt::impl` calls) and the ability to
  inspect our own objects.

**What does shrink a PDB: a public copy.** `link /PDBSTRIPPED:<file>` writes, in the same link, a second PDB with
the same GUID and age (so it matches the binary) that keeps the name and address of every function — including
static functions and lambdas (their `S_LPROC32` records and the `S_LPROCREF`s to them) — and drops types, line
numbers, locals and inline frames. Measured on `agentmaster-cli` (a scratch relink of its Release objects):
**0.89 MB vs 11.6 MB (7.6%)**, zipping to 0.14 MB. Estimated for all 17: ~80–90 MB unpacked, **~15 MB zipped**.

The three shapes the owner weighed: full PDBs in a separate zip only (downloads unchanged) · **public PDBs in the
portable zips + the full ones in a separate zip (chosen)** · full PDBs inside the packages (portable zip ~220 MB,
~960 MB more disk per install, every in-app update 8× larger; msixbundle ~440 MB).

## 3. How a release makes them

Everything is in `.github/workflows/release.yml`'s `build` job (per architecture), mirrored by `ci.yml`:

1. **Prepare** — `tools\New-AgentmasterSourceLink.ps1` writes the Source Link map
   (`$RUNNER_TEMP\agentmaster-sourcelink.json`): `"<workspace>\*" → raw.githubusercontent.com/<repo>/<commit>/*`,
   and every GitHub submodule at its own pin (`refs\claude-agent-sdk-cpp\*` → `0xeb/claude-agent-sdk-cpp/<pin>/*`;
   the most specific key wins). It also creates the public-PDB folder.
2. **Build** — two properties, read only by `src\common.build.post.props`'s Agentmaster block (a local build sets
   neither, so its links are unchanged):
   - `/p:AgentmasterSourceLinkFile=<map>` → `link /SOURCELINK:<map>` on every `.exe` / `.dll`. The map lands in the
     PDB as the named stream `sourcelink$1` (the linker's own naming; `sourcelink$2` is empty).
   - `/p:AgentmasterPublicPdbDir=<folder>` → the Link task's `StripPrivateSymbols` → `/PDBSTRIPPED:<folder>\
     <TargetName>.pdb` (named like the full PDB, the name the binary records; no project overrides
     `ProgramDatabaseFile`). A target creates the folder before `Link`.
   - `/p:AppxSymbolPackageEnabled=false`: DesktopBridge's own `.appxsym` — an unindexed zip of the same full PDBs
     that the workflow always built and threw away — is skipped.
3. **Source-index (srcsrv)** — upstream's `build\scripts\Index-Pdbs.ps1` (with `pdbstr` / `srctool` from
   `build\packages.config`, restored by the existing restore step from the TerminalDependencies feed) writes a
   `srcsrv` stream into each full PDB of the package layout (`src\cascadia\CascadiaPackage\bin\<Platform>\
   Release\`): every git-tracked source file → `raw.githubusercontent.com/<owner>/<repo>/<commit>/<path>`, for
   cdb and classic WinDbg. GUID and age stay as they were. The indexer's long output goes to
   `index-pdbs-<arch>.log`, uploaded with the binlog.
4. **The full symbols zip** — `tools\Export-AgentmasterSymbols.ps1` reads each binary's CodeView record, finds the
   PDB with that name, GUID and age, and zips the matches flat into `_symbols\Agentmaster_<ver>_<arch>_symbols.zip`
   (artifact `symbols-<arch>`). It **fails the release** unless every binary whose recorded PDB path lies in the
   workspace is matched and its PDB carries both the Source Link map and the srcsrv stream naming the commit.
5. **The portable zip** — `New-UnpackagedTerminalDistribution.ps1 -SymbolsPath <public folder>
   -RequireSymbolsUnder <workspace>` copies each binary's public PDB beside it (the same matcher) **before**
   `.am-manifest` is written, so the manifest lists the PDBs: the in-app updater and the installers replace them
   with the next version's and remove them at uninstall, like every file a release ships.
6. **The release** downloads `symbols-*` with the other artifacts; `dist/*.zip` attaches them. The release body
   has a "Debugging symbols" section.

`ci.yml` runs steps 1–3 and the two checks (full PDBs with both indexes; a public PDB for every binary) on every
push and pull request, without zipping or uploading, so a broken symbols pipeline shows before a release.

## 4. Using them

- **Any portable, per-user or all-users install** has the public PDBs beside its binaries. Visual Studio looks
  in a module's own folder, so attaching to the app or opening a WER dump on that machine names every function
  with nothing to configure; WinDbg / cdb need the folder on the symbol path (`.sympath+ <install folder>`,
  `.reload`).
- **Full symbols:** extract `Agentmaster_<ver>_<arch>_symbols.zip` into the folder that holds `Agentmaster.exe`.
  It replaces the public PDBs (same names, so they are manifest files: the next update brings that version's
  public PDBs, an uninstall removes them; a full PDB is never left beside a newer binary). An all-users install
  (`C:\Program Files\Agentmaster`) needs administrator rights to write there.
- **An MSIX install** (`C:\Program Files\WindowsApps\…`, read-only) — and any dump analyzed elsewhere: extract the
  zip anywhere and put that folder on the debugger's symbol path (Visual Studio: Tools → Options → Debugging →
  Symbols; WinDbg: `.sympath+ <folder>` then `.reload`). Add `srv*https://msdl.microsoft.com/download/symbols` for
  Windows and WinUI.
- **Source code:** Visual Studio uses the Source Link map by default ("Enable Source Link support") and downloads
  each file of the release's commit from GitHub; WinDbg reads Source Link too; cdb / classic WinDbg use the srcsrv
  stream (enable source server: `.srcfix` / `.srcpath+ srv*`). Files outside the repository (vcpkg and NuGet
  headers, generated sources) also match the repository key and get a 404 — "source not available", no harm.
- **This repository's own crash tools** (the debug-dumps skill's `diasym.exe <pdb> <rva>`, `dumpourscan`): give
  them the full PDB from the symbols zip of the release that crashed. An `[exc]` line's `Module.dll+0xRVA` resolves
  against it directly.

## 5. Local builds keep a PDB beside every binary

Checked 2026-10-23 (the owner's requirement: local Debug and Release builds produce PDBs next to the binaries;
they are just not shipped):

- **Release:** full PDBs (`GenerateDebugInformation=DebugFull`, `common.build.pre.props`) beside every binary in
  `bin\x64\Release\…` and in the layout — 17 PDBs for the 16 binaries we build in `Q:\agentmaster-layouts\Release`
  (the 17th, `console.pdb`, belongs to a `console.dll` the package does not ship).
- **Debug:** a PDB beside every binary as well (17 in each of `Q:\agentmaster-layouts\Debug` and `Debug-B`), but
  **FASTLINK** (`DebugFastLink`, upstream's choice for link speed; `mspdbcmf /CHECK` and DIA's
  `IDiaSessionEx::isFastLinkPDB` both say so). A FASTLINK PDB keeps types and line tables in the object files;
  the next Debug build rewrites those (the hot deploy builds into the other slot while dev runs), and from then on
  the running dev instance's PDBs — and the rollback slot's — resolve little more than names. Making Debug links
  full (`DebugFull`) would make every dev layout self-contained at the cost of slower Debug links (not measured:
  the machine had ~1.3 GB of free commit that day, too little for a full Debug link of `TerminalApp.dll`).
  **Open — the owner's decision.**
- The production package installed on this machine is the published **0.8.0 MSIX** (`Get-AppxPackage
  Agentmaster`, 2026-10-23), which has no PDBs; releases from this change on have the symbols zip.
- `tools\Build-Agentmaster.ps1` still skips the local `.appxsym` (`AppxSymbolPackageEnabled=false`, the
  ~156 s symbol-package zip): the PDBs stay beside the binaries regardless.

## 6. How it was checked (2026-10-23, no app build)

- **Source Link map:** `New-AgentmasterSourceLink.ps1` in PowerShell 7 and 5.1 — identical JSON, no byte-order
  mark, the submodule at its pin; a short SHA and a malformed repository name exit 1 and write nothing.
- **The linker switches:** the CLI's recorded Release link (`link.command.1.tlog`) replayed into a scratch folder
  with `/PDBSTRIPPED` and `/SOURCELINK` added: exit 0 in 11 s; full PDB 11.6 MB, public PDB 0.89 MB, both GUID
  `5ae3a56e-…` age 1; the map in the stream `sourcelink$1`, verbatim.
- **The MSBuild wiring:** a probe target injected with `ForceImportAfterCppTargets` printed the Link item
  definitions of `agentmaster-cli`, `TerminalApp` (a C++/WinRT DLL) and `WindowsTerminal` (the GUI exe, target
  `Agentmaster`): without the properties nothing changes; with them `/SOURCELINK:"<map>"` is appended and
  `StripPrivateSymbols=<folder>\<TargetName>.pdb` is set.
- **A debugger's reader:** a DIA SDK probe (`IDiaSessionEx::getSourceLinkInfo(nullptr)`, msdia140.dll — the PDB
  engine of Visual Studio's debugger) read the map back from the relinked PDB, also after `pdbstr` had written the
  srcsrv stream; a normal local PDB reports none.
- **srcsrv:** `Index-Pdbs.ps1` over copies of all 17 Release PDBs: 8 s, every PDB indexed with this commit, every
  one still matching its binary.
- **The matcher:** `Export-AgentmasterSymbols.ps1` on the Release layout: 16 binaries matched by GUID + age,
  `duckdb.dll` (no debug record) and `TerminalThemeHelpers.dll` (PDB outside the tree) reported, not required;
  the zip: 16 PDBs, 189.8 MB, 30 s. Negative cases: a PDB of another build → `Mismatch`, a public PDB under
  `-RequireSourceLink` → refused, a wrong `-RequireUnder` → "nothing was checked"; all exit 1. Windows PowerShell
  5.1 gives the same matches.
- **The portable packager:** `New-UnpackagedTerminalDistribution.ps1 -SymbolsPath bin\x64\Release
  -RequireSymbolsUnder <repo>` over the day's Release `.msix`: exit 0 in 53 s; the zip holds the 16 PDBs beside
  their binaries (picked by GUID among several same-named copies) and `.am-manifest` lists all 16.
- **The workflows:** both parse as YAML, and every `pwsh` step's script parses with PowerShell's own parser.

## 7. Open

- The first GitHub run (CI or release) has not happened: the x64 and arm64 links with `/SOURCELINK` +
  `/PDBSTRIPPED`, and the runner's `pdbstr` restore, are proven only by the local equivalents above.
- Debug FASTLINK vs full PDBs (§5).
- The msixbundle stays without PDBs (the owner's choice); public PDBs there would need the package payload's PDB
  items swapped for the `/PDBSTRIPPED` copies (`AppxPackageIncludePrivateSymbols=true` would ship the full ones).
- The e2e harness (`E2E_CRASH_GUARD.md`: "the published 0.8.0 portable zip has NO PDBs") can now take the public
  PDBs from the zip under test and the full ones from its symbols zip.
- No symbol server: Microsoft's MSDL is not open to us, and GitHub release assets are not a symstore layout; a
  symbols zip per release is the archive.
