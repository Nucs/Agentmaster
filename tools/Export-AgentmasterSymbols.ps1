# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
<#
.SYNOPSIS
    Find the PDB of every binary in a folder, prove it is THE PDB of that binary (same GUID and age), and copy
    the PDBs beside the binaries or into a zip - the symbols half of an Agentmaster release.

.DESCRIPTION
    Agentmaster is open source, so a release ships its symbols (doc/agentmaster/SYMBOLS.md):
      * the portable zips carry a PUBLIC PDB beside every binary (function names and addresses; written by the
        release link with /PDBSTRIPPED), so a stack trace from any install names every function;
      * Agentmaster_<version>_<arch>_symbols.zip carries the FULL PDBs (types, line numbers, locals), Source
        Linked to the release's commit.

    A PDB belongs to exactly one build of one binary: the binary's debug directory holds a CodeView ("RSDS")
    record with the PDB's file name, a GUID and an age, and a debugger loads a PDB only when its own GUID and
    age are the same. This script reads both sides and matches on them, never on a file name or a date, so a
    stale or foreign PDB is never shipped as a binary's symbols.

    For every .exe / .dll under -BinaryFolder (recursively) it reports one of:
      Matched   - a PDB with the recorded name, GUID and age was found in -PdbSearchPath;
      Mismatch  - PDBs of that name exist, but none has the binary's GUID and age;
      Missing   - no PDB of that name exists;
      NoDebugInfo - the binary carries no CodeView record (nothing could ever match it).
    A binary whose recorded PDB path lies under -RequireUnder is one WE built: anything but Matched fails the
    script. Every other binary (Microsoft.UI.Xaml.dll, duckdb.dll, TerminalThemeHelpers.dll) is a dependency we
    have no symbols for; it is reported and skipped.

    Then, optionally:
      -CopyTo  copies each matched PDB to <CopyTo>\<the binary's folder relative to -BinaryFolder>\<pdb name>,
               i.e. beside its binary when -CopyTo is -BinaryFolder itself;
      -ZipTo   writes a zip of the matched PDBs with the same relative layout (flat for today's packages: every
               binary sits at the top level), so it can be extracted straight into an install folder.

    Checks for the full release PDBs, both over every matched PDB of a binary we built:
      -RequireSourceLink <commit>  it must carry a Source Link map (the linker's "sourcelink$N" named streams)
                                   that names this commit;
      -RequireSourceServer         it must also carry a source-server ("srcsrv") stream naming that commit
                                   (build\scripts\Index-Pdbs.ps1 writes it). Measured 2026-10-23: the indexer
                                   reaches all 16 PDBs of a build, so a PDB without the stream is a broken run.

.PARAMETER BinaryFolder
    The folder whose binaries are being shipped: a package layout, or the unpacked portable folder.

.PARAMETER PdbSearchPath
    The folder(s) searched recursively for PDBs (the layout itself for the full PDBs, the /PDBSTRIPPED folder
    for the public ones).

.PARAMETER RequireUnder
    The build root (on a runner $env:GITHUB_WORKSPACE). A binary whose recorded PDB path starts with it must be
    Matched. Empty: nothing is required (a report).

.PARAMETER RequireSourceLink
    A full commit SHA that every matched PDB of a required binary must name in its Source Link map.

.PARAMETER RequireSourceServer
    Every matched PDB of a required binary must carry a srcsrv stream naming -RequireSourceLink's commit.
    Requires -RequireSourceLink.

.PARAMETER CopyTo
    Copy each matched PDB under this folder, at its binary's relative path. Existing files are overwritten.

.PARAMETER ZipTo
    Write the matched PDBs into this zip (replaced if it exists), at their binaries' relative paths. Uses
    Windows' own tar (bsdtar), the tool the release's other zips are made with. The PDBs are staged in a
    temporary folder beside the zip, hard-linked when they live on the same volume (no copy of ~1 GB of PDBs).

.PARAMETER PassThru
    Also write one object per binary to the pipeline (the table on the host is always printed).

.OUTPUTS
    A table on the host: Binary (relative path), PdbName, Status, Required, SizeMB, SourceLink, SourceServer.
    With -PassThru, the same rows as objects (plus Guid, Age, PdbPath). The script throws (a non-zero exit) when
    a required binary is not Matched, a required stream is missing, a PDB or binary cannot be read, or the copy
    or the zip fails; a partially written -ZipTo is deleted.

.EXAMPLE
    # The full symbols zip of a release build (after build\scripts\Index-Pdbs.ps1 indexed the layout):
    pwsh -File tools\Export-AgentmasterSymbols.ps1 -BinaryFolder src\cascadia\CascadiaPackage\bin\x64\Release `
         -PdbSearchPath src\cascadia\CascadiaPackage\bin\x64\Release -RequireUnder $env:GITHUB_WORKSPACE `
         -RequireSourceLink $env:GITHUB_SHA -RequireSourceServer `
         -ZipTo out\Agentmaster_0.8.1.0_x64_symbols.zip

.EXAMPLE
    # Public PDBs beside the binaries of an unpacked portable folder:
    pwsh -File tools\Export-AgentmasterSymbols.ps1 -BinaryFolder $portable -PdbSearchPath $publicPdbs `
         -RequireUnder $env:GITHUB_WORKSPACE -CopyTo $portable
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinaryFolder,
    [Parameter(Mandatory)][string[]]$PdbSearchPath,
    [string]$RequireUnder = '',
    [string]$RequireSourceLink = '',
    [switch]$RequireSourceServer,
    [string]$CopyTo = '',
    [string]$ZipTo = '',
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3
# tar's exit code is checked by hand.
$PSNativeCommandUseErrorActionPreference = $false

# The MSF 7.00 container signature every modern PDB starts with (llvm.org/docs/PDB/MsfFile.html).
$script:MsfMagic = [byte[]](0x4D, 0x69, 0x63, 0x72, 0x6F, 0x73, 0x6F, 0x66, 0x74, 0x20, 0x43, 0x2F, 0x43, 0x2B, 0x2B,
    0x20, 0x4D, 0x53, 0x46, 0x20, 0x37, 0x2E, 0x30, 0x30, 0x0D, 0x0A, 0x1A, 0x44, 0x53, 0x00, 0x00, 0x00)

<#
.SYNOPSIS
    Read exactly Count bytes at Offset of a stream.
.PARAMETER Stream
    An open, seekable stream.
.PARAMETER Offset
    The absolute position to read from.
.PARAMETER Count
    How many bytes to read.
.OUTPUTS
    [byte[]] of length Count.
.NOTES
    Throws when the stream ends first: every caller reads a structure that must be complete, and a short read
    means a truncated or foreign file.
#>
function Read-AmBytes {
    param([System.IO.Stream]$Stream, [long]$Offset, [int]$Count)
    $buf = New-Object byte[] $Count
    [void]$Stream.Seek($Offset, [System.IO.SeekOrigin]::Begin)
    $got = 0
    while ($got -lt $Count) {
        $n = $Stream.Read($buf, $got, $Count - $got)
        if ($n -le 0) { throw "unexpected end of file at offset $($Offset + $got) (wanted $Count bytes at $Offset)" }
        $got += $n
    }
    return , $buf
}

<#
.SYNOPSIS
    The CodeView record of a PE file: the PDB file name, GUID and age a debugger will look for.
.PARAMETER Path
    An .exe or .dll.
.OUTPUTS
    [pscustomobject] @{ Guid; Age; PdbPath; PdbName }, or $null when the file is not a PE image or carries no
    RSDS CodeView record (a binary linked without /DEBUG).
.NOTES
    Throws only when the file cannot be opened or is truncated inside a structure its headers announce.
    Layout: the DOS header's e_lfanew -> "PE\0\0" -> the COFF header -> the optional header (PE32 or PE32+) whose
    data directory 6 is the debug directory (an RVA, mapped to a file offset through the section table); each
    28-byte IMAGE_DEBUG_DIRECTORY entry of type 2 points (by file offset) at "RSDS" + GUID + age + UTF-8 path.
#>
function Get-AmPeCodeView {
    param([string]$Path)
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        if ($fs.Length -lt 0x40) { return $null }
        $dos = Read-AmBytes $fs 0 0x40
        if ($dos[0] -ne 0x4D -or $dos[1] -ne 0x5A) { return $null }  # "MZ"
        $pe = [BitConverter]::ToInt32($dos, 0x3C)
        if ($pe -le 0 -or $pe + 24 -gt $fs.Length) { return $null }
        $coff = Read-AmBytes $fs $pe 24
        if ([BitConverter]::ToUInt32($coff, 0) -ne 0x00004550) { return $null }  # "PE\0\0"
        $sectionCount = [BitConverter]::ToUInt16($coff, 6)
        $optSize = [BitConverter]::ToUInt16($coff, 20)
        $opt = Read-AmBytes $fs ($pe + 24) $optSize
        $magic = [BitConverter]::ToUInt16($opt, 0)
        if ($magic -eq 0x20B) { $dd = 112 } elseif ($magic -eq 0x10B) { $dd = 96 } else { return $null }
        if ($dd + 7 * 8 -gt $optSize) { return $null }  # fewer than 7 data directories: no debug directory
        $debugRva = [BitConverter]::ToUInt32($opt, $dd + 6 * 8)
        $debugSize = [BitConverter]::ToUInt32($opt, $dd + 6 * 8 + 4)
        if ($debugRva -eq 0 -or $debugSize -eq 0) { return $null }
        $sections = Read-AmBytes $fs ($pe + 24 + $optSize) (40 * $sectionCount)
        $debugOffset = -1
        for ($s = 0; $s -lt $sectionCount; $s++) {
            $va = [BitConverter]::ToUInt32($sections, 40 * $s + 12)
            $vsize = [BitConverter]::ToUInt32($sections, 40 * $s + 8)
            $rawSize = [BitConverter]::ToUInt32($sections, 40 * $s + 16)
            $rawPtr = [BitConverter]::ToUInt32($sections, 40 * $s + 20)
            $span = [Math]::Max($vsize, $rawSize)
            if ($debugRva -ge $va -and $debugRva -lt $va + $span) {
                $debugOffset = [long]$rawPtr + ($debugRva - $va)
                break
            }
        }
        if ($debugOffset -lt 0) { return $null }
        $entries = Read-AmBytes $fs $debugOffset ([int]$debugSize)
        for ($e = 0; $e + 28 -le $debugSize; $e += 28) {
            if ([BitConverter]::ToUInt32($entries, $e + 12) -ne 2) { continue }  # IMAGE_DEBUG_TYPE_CODEVIEW
            $dataSize = [BitConverter]::ToUInt32($entries, $e + 16)
            $dataPtr = [BitConverter]::ToUInt32($entries, $e + 24)
            if ($dataSize -lt 25 -or $dataPtr + $dataSize -gt $fs.Length) { continue }
            $cv = Read-AmBytes $fs $dataPtr ([int]$dataSize)
            if ([BitConverter]::ToUInt32($cv, 0) -ne 0x53445352) { continue }  # "RSDS"
            $guidBytes = New-Object byte[] 16
            [Array]::Copy($cv, 4, $guidBytes, 0, 16)
            $end = [Array]::IndexOf($cv, [byte]0, 24)
            if ($end -lt 0) { $end = $cv.Length }
            $pdbPath = [System.Text.Encoding]::UTF8.GetString($cv, 24, $end - 24)
            return [pscustomobject]@{
                Guid    = New-Object Guid -ArgumentList (, $guidBytes)
                Age     = [BitConverter]::ToUInt32($cv, 20)
                PdbPath = $pdbPath
                PdbName = [System.IO.Path]::GetFileName($pdbPath)
            }
        }
        return $null
    }
    finally {
        $fs.Dispose()
    }
}

<#
.SYNOPSIS
    The identity and named streams of a PDB: its GUID, the age the debugger matches, and the named streams.
.PARAMETER Path
    A PDB file (MSF 7.00).
.PARAMETER ReadStreams
    Names of named streams (exact, or a prefix ending in '*', e.g. 'sourcelink*') whose bytes to return.
.OUTPUTS
    [pscustomobject] @{ Guid; Age; InfoAge; StreamNames; Streams } - Age is the DBI stream's age, the one a
    binary's CodeView record carries; InfoAge is the PDB info stream's own counter; Streams maps every
    requested stream name present to its bytes ([byte[]]).
.NOTES
    Throws when the file is not an MSF 7.00 PDB or is truncated. The layout follows llvm.org/docs/PDB: the
    superblock (block size @32, directory byte count @44, block-map address @52), the stream directory (stream
    count, sizes, then each stream's block list), stream 1 = PDB info (version, signature, age, GUID, then the
    named-stream hash table), stream 3 = DBI (age @8). Only the needed blocks are read: a 230 MB PDB costs a few
    hundred KB of I/O.
#>
function Get-AmPdbInfo {
    param([string]$Path, [string[]]$ReadStreams = @())
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $sb = Read-AmBytes $fs 0 56
        for ($i = 0; $i -lt 32; $i++) {
            if ($sb[$i] -ne $script:MsfMagic[$i]) { throw "$Path is not an MSF 7.00 PDB" }
        }
        $blockSize = [BitConverter]::ToUInt32($sb, 32)
        $dirBytes = [BitConverter]::ToUInt32($sb, 44)
        $blockMap = [BitConverter]::ToUInt32($sb, 52)
        $dirBlocks = [int][Math]::Ceiling($dirBytes / $blockSize)
        $mapBytes = Read-AmBytes $fs ([long]$blockMap * $blockSize) (4 * $dirBlocks)
        $dir = New-Object byte[] ($dirBlocks * $blockSize)
        for ($b = 0; $b -lt $dirBlocks; $b++) {
            $blk = Read-AmBytes $fs ([long][BitConverter]::ToUInt32($mapBytes, 4 * $b) * $blockSize) ([int]$blockSize)
            [Array]::Copy($blk, 0, $dir, $b * $blockSize, $blockSize)
        }
        $streamCount = [BitConverter]::ToUInt32($dir, 0)
        # Where each stream's block list starts inside the directory: after the sizes, in stream order; a nil
        # stream (size 0xFFFFFFFF) has no blocks.
        $sizes = New-Object 'uint32[]' $streamCount
        $listAt = New-Object 'int[]' $streamCount
        $cursor = 4 + 4 * $streamCount
        for ($s = 0; $s -lt $streamCount; $s++) {
            $size = [BitConverter]::ToUInt32($dir, 4 + 4 * $s)
            if ($size -eq [uint32]::MaxValue) { $size = 0 }
            $sizes[$s] = $size
            $listAt[$s] = $cursor
            $cursor += 4 * [int][Math]::Ceiling($size / $blockSize)
        }
        $readStream = {
            param([int]$Index, [int]$Limit)
            $size = [int]$sizes[$Index]
            if ($Limit -gt 0 -and $Limit -lt $size) { $size = $Limit }
            $out = New-Object byte[] $size
            $done = 0
            $k = 0
            while ($done -lt $size) {
                $block = [BitConverter]::ToUInt32($dir, $listAt[$Index] + 4 * $k)
                $take = [Math]::Min([int]$blockSize, $size - $done)
                $part = Read-AmBytes $fs ([long]$block * $blockSize) $take
                [Array]::Copy($part, 0, $out, $done, $take)
                $done += $take
                $k++
            }
            return , $out
        }
        $info = & $readStream 1 0
        $infoAge = [BitConverter]::ToUInt32($info, 8)
        $guidBytes = New-Object byte[] 16
        [Array]::Copy($info, 12, $guidBytes, 0, 16)
        $dbiHeader = & $readStream 3 64
        $dbiAge = [BitConverter]::ToUInt32($dbiHeader, 8)
        # The named-stream map: the string buffer, then a hash table (size, capacity, present and deleted bit
        # vectors, then one (string offset, stream index) pair per present bucket).
        $o = 28
        $strSize = [BitConverter]::ToUInt32($info, $o); $o += 4
        $strStart = $o; $o += $strSize
        $capacity = [BitConverter]::ToUInt32($info, $o + 4); $o += 8
        $presentWords = [BitConverter]::ToUInt32($info, $o); $o += 4
        $present = New-Object 'uint32[]' $presentWords
        for ($w = 0; $w -lt $presentWords; $w++) { $present[$w] = [BitConverter]::ToUInt32($info, $o + 4 * $w) }
        $o += 4 * $presentWords
        $deletedWords = [BitConverter]::ToUInt32($info, $o); $o += 4 + 4 * $deletedWords
        $names = [ordered]@{}
        for ($bucket = 0; $bucket -lt $capacity; $bucket++) {
            $word = [int]($bucket -shr 5)
            if ($word -ge $presentWords) { break }
            if (([uint64]$present[$word] -band ([uint64]1 -shl ($bucket -band 31))) -eq 0) { continue }
            $key = [BitConverter]::ToUInt32($info, $o)
            $value = [BitConverter]::ToUInt32($info, $o + 4)
            $o += 8
            $end = [Array]::IndexOf($info, [byte]0, [int]($strStart + $key))
            $names[[System.Text.Encoding]::UTF8.GetString($info, [int]($strStart + $key), [int]($end - $strStart - $key))] = [int]$value
        }
        $streams = @{}
        foreach ($name in $names.Keys) {
            foreach ($want in $ReadStreams) {
                $hit = if ($want.EndsWith('*')) { $name.StartsWith($want.TrimEnd('*'), [StringComparison]::Ordinal) } else { $name -ceq $want }
                if ($hit -and $names[$name] -lt $streamCount) {
                    $streams[$name] = & $readStream $names[$name] 0
                    break
                }
            }
        }
        return [pscustomobject]@{
            Guid        = New-Object Guid -ArgumentList (, $guidBytes)
            Age         = $dbiAge
            InfoAge     = $infoAge
            StreamNames = @($names.Keys)
            Streams     = $streams
        }
    }
    finally {
        $fs.Dispose()
    }
}

<#
.SYNOPSIS
    Whether a path lies inside a folder (case-insensitive, separator-normalized - Windows path rules).
.PARAMETER Path
    The path to test (here: a PDB path recorded in a binary).
.PARAMETER Folder
    The folder; empty = never.
.OUTPUTS
    [bool]. Never throws.
#>
function Test-AmUnder {
    param([string]$Path, [string]$Folder)
    if (-not $Folder) { return $false }
    $p = ($Path -replace '/', '\')
    $f = ($Folder -replace '/', '\').TrimEnd('\') + '\'
    return $p.StartsWith($f, [StringComparison]::OrdinalIgnoreCase)
}

$binRoot = (Resolve-Path -LiteralPath $BinaryFolder).ProviderPath.TrimEnd('\')
if ($RequireSourceServer -and -not $RequireSourceLink) {
    throw "-RequireSourceServer needs -RequireSourceLink (the commit the srcsrv stream must name)."
}
if ($RequireSourceLink -and $RequireSourceLink -notmatch '^[0-9a-fA-F]{40}$') {
    throw "-RequireSourceLink must be a full 40-hex commit SHA (got '$RequireSourceLink')."
}
$commit = $RequireSourceLink.ToLowerInvariant()

# Every candidate PDB by file name (case-insensitive: Windows file names), with its identity read once.
$pdbsByName = @{}
foreach ($dirPath in $PdbSearchPath) {
    if (-not (Test-Path -LiteralPath $dirPath -PathType Container)) {
        throw "-PdbSearchPath folder '$dirPath' does not exist."
    }
    foreach ($f in Get-ChildItem -LiteralPath $dirPath -Recurse -File -Filter '*.pdb') {
        $key = $f.Name.ToLowerInvariant()
        if (-not $pdbsByName.ContainsKey($key)) { $pdbsByName[$key] = New-Object System.Collections.ArrayList }
        [void]$pdbsByName[$key].Add($f.FullName)
    }
}
$identityCache = @{}

$results = New-Object System.Collections.ArrayList
$binaries = Get-ChildItem -LiteralPath $binRoot -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' } | Sort-Object FullName
foreach ($bin in $binaries) {
    $rel = $bin.FullName.Substring($binRoot.Length).TrimStart('\')
    $cv = Get-AmPeCodeView $bin.FullName
    $row = [ordered]@{
        Binary = $rel; PdbName = ''; Guid = $null; Age = $null; Status = 'NoDebugInfo'; Required = $false
        PdbPath = ''; SizeMB = 0.0; SourceLink = ''; SourceServer = ''
    }
    if ($cv) {
        $row.PdbName = $cv.PdbName
        $row.Guid = $cv.Guid
        $row.Age = $cv.Age
        $row.Required = Test-AmUnder $cv.PdbPath $RequireUnder
        $row.Status = 'Missing'
        $candidates = $pdbsByName[$cv.PdbName.ToLowerInvariant()]
        if ($candidates) {
            $row.Status = 'Mismatch'
            foreach ($cand in $candidates) {
                if (-not $identityCache.ContainsKey($cand)) {
                    $identityCache[$cand] = Get-AmPdbInfo $cand -ReadStreams @('sourcelink*', 'srcsrv')
                }
                $id = $identityCache[$cand]
                if ($id.Guid -eq $cv.Guid -and $id.Age -eq $cv.Age) {
                    $row.Status = 'Matched'
                    $row.PdbPath = $cand
                    $row.SizeMB = [Math]::Round((Get-Item -LiteralPath $cand).Length / 1MB, 1)
                    $sl = ''
                    foreach ($k in $id.Streams.Keys) {
                        if ($k -like 'sourcelink*') { $sl += [System.Text.Encoding]::UTF8.GetString($id.Streams[$k]) }
                    }
                    $row.SourceLink = if (-not $sl) { 'none' } elseif ($commit -and $sl.ToLowerInvariant().Contains($commit)) { 'commit' } else { 'other' }
                    $ss = if ($id.Streams.ContainsKey('srcsrv')) { [System.Text.Encoding]::UTF8.GetString($id.Streams['srcsrv']) } else { '' }
                    $row.SourceServer = if (-not $ss) { 'none' } elseif ($commit -and $ss.ToLowerInvariant().Contains($commit)) { 'commit' } else { 'other' }
                    break
                }
            }
        }
    }
    [void]$results.Add([pscustomobject]$row)
}

# Rendered wide on purpose: a CI log (and a narrow console) would otherwise wrap every column into a ribbon.
$results | Format-Table -AutoSize Binary, PdbName, Status, Required, SizeMB, SourceLink, SourceServer |
    Out-String -Width 220 | Out-Host

# The checks, all reported together so one run shows every problem.
$problems = New-Object System.Collections.ArrayList
foreach ($r in $results) {
    if ($r.Required -and $r.Status -ne 'Matched') {
        [void]$problems.Add("$($r.Binary): $($r.Status) - no PDB named $($r.PdbName) with GUID $($r.Guid) age $($r.Age) in $($PdbSearchPath -join '; ')")
    }
    if ($r.Required -and $r.Status -eq 'Matched' -and $commit -and $r.SourceLink -ne 'commit') {
        [void]$problems.Add("$($r.Binary): $($r.PdbName) has no Source Link map naming $commit (found: $($r.SourceLink))")
    }
    if ($r.Required -and $r.Status -eq 'Matched' -and $RequireSourceServer -and $r.SourceServer -ne 'commit') {
        [void]$problems.Add("$($r.Binary): $($r.PdbName) has no srcsrv stream naming $commit (found: $($r.SourceServer))")
    }
}
if (($RequireSourceLink -or $RequireSourceServer) -and @($results | Where-Object { $_.Required }).Count -eq 0) {
    # A stream check that saw nothing proves nothing (a wrong -RequireUnder would otherwise pass silently).
    [void]$problems.Add("no binary's PDB path lies under -RequireUnder '$RequireUnder': nothing was checked")
}
if ($problems.Count -gt 0) {
    throw ("symbols check failed:`n  " + ($problems -join "`n  "))
}

$matched = @($results | Where-Object { $_.Status -eq 'Matched' })
if ($CopyTo) {
    $copied = 0
    foreach ($m in $matched) {
        $relDir = Split-Path -Parent $m.Binary
        $destDir = if ($relDir) { Join-Path $CopyTo $relDir } else { $CopyTo }
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
        $dest = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath((Join-Path $destDir $m.PdbName))
        # Already in place (-CopyTo is the folder the PDB was found in): Copy-Item would refuse to copy it onto itself.
        if ($dest -ieq $m.PdbPath) { continue }
        Copy-Item -LiteralPath $m.PdbPath -Destination $dest -Force
        $copied++
    }
    Write-Host ("copied {0} of {1} matched PDB(s) under {2}" -f $copied, $matched.Count, $CopyTo)
}
if ($ZipTo) {
    if ($matched.Count -eq 0) { throw "no PDB matched: refusing to write an empty $ZipTo" }
    # Resolved against PowerShell's current location, which .NET's process directory (GetFullPath) need not follow.
    $zipFull = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ZipTo)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $zipFull) | Out-Null
    if (Test-Path -LiteralPath $zipFull) { Remove-Item -LiteralPath $zipFull -Force }
    # Windows' own bsdtar (never a Git-for-Windows GNU tar that may come first on PATH: it cannot write zips).
    # Each PDB is first staged at its binary's relative folder in a temporary folder (the PDB may sit anywhere under
    # -PdbSearchPath), and tar zips the staging folder with entry names relative to it.
    $tarExe = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (-not (Test-Path -LiteralPath $tarExe)) { $tarExe = 'tar.exe' }
    # Beside the zip, not in %TEMP%: the zip's folder is usually on the PDBs' volume, where hard links work.
    $stage = Join-Path (Split-Path -Parent $zipFull) ("am-symbols-stage-" + [Guid]::NewGuid().ToString('N'))
    try {
        foreach ($m in $matched) {
            $relDir = Split-Path -Parent $m.Binary
            $destDir = if ($relDir) { Join-Path $stage $relDir } else { $stage }
            New-Item -ItemType Directory -Force -Path $destDir | Out-Null
            # A hard link when the volume allows it (no 1 GB copy of the full PDBs), else a copy.
            $dest = Join-Path $destDir $m.PdbName
            $linked = $false
            try {
                New-Item -ItemType HardLink -Path $dest -Target $m.PdbPath -ErrorAction Stop | Out-Null
                $linked = $true
            }
            catch {
                Write-Verbose "hard link of $($m.PdbPath) refused ($($_.Exception.Message)); copying"
            }
            if (-not $linked) { Copy-Item -LiteralPath $m.PdbPath -Destination $dest -Force }
        }
        $entries = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1) })
        & $tarExe -c --format=zip -f $zipFull -C $stage @entries
        if ($LASTEXITCODE -ne 0) {
            Remove-Item -LiteralPath $zipFull -Force -ErrorAction SilentlyContinue
            throw "tar failed (exit $LASTEXITCODE) writing $zipFull"
        }
    }
    finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host ("wrote {0}: {1} PDB(s), {2:N1} MB" -f $zipFull, $matched.Count, ((Get-Item -LiteralPath $zipFull).Length / 1MB))
}

if ($PassThru) { $results }
