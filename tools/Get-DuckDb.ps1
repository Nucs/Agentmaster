# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
<#
.SYNOPSIS
    Fetch the pinned official DuckDB C library (libduckdb) that the session database service links.

.DESCRIPTION
    Agentmaster's session database (doc/agentmaster/SESSIONS_DB.md) runs in its own process,
    agentmaster-sessiondb.exe, which links DuckDB's official Windows C library: duckdb.h,
    duckdb.lib (the import library) and duckdb.dll (shipped beside the service). The library is
    NOT in the repository (~37 MB per architecture). This script downloads the pinned release
    archive from DuckDB's GitHub releases, refuses it unless its SHA-256 equals the pin below, and
    extracts the three files into
        dep\duckdb\<version>\<arch>\
    which agentmaster-sessiondb.vcxproj and its standalone twin (_compile.bat) read.

    The pin lives HERE and only here (version + one digest per architecture). Moving to a new
    DuckDB release = edit $Pins, run the script, build. The C API is stable across 1.x and 2.0, so
    the service code does not change with the pin; the database file written by an older version
    is read by a newer one (DuckDB's storage reads forward), never the other way round.

    Why 1.5.6 and not the design's 2.0 (SESSIONS_DB.md D2): 2.0.0 ships on 2026-10-21 and had no
    C-library build when this was written (2026-10-15). Bump the pin once it does.

    Idempotent: a folder that already holds the three files with a matching .sha256 stamp is left
    alone (no network). -Force re-downloads.

.PARAMETER Arch
    x64, arm64 or all (default all). The build needs the architecture it targets; CI passes the
    matrix arch.

.PARAMETER Root
    The repository root (default: the parent of this script's folder).

.PARAMETER Force
    Download and extract again even when the folder is current.

.OUTPUTS
    One line per architecture: the folder and whether it was fetched or already current.
    Exit code 0 on success; the script throws (non-zero exit) on a download failure or a digest
    mismatch, and never leaves a half-written folder behind (it extracts into a temporary folder
    first and moves it into place).

.EXAMPLE
    pwsh -File tools\Get-DuckDb.ps1 -Arch x64
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64', 'all')]
    [string]$Arch = 'all',
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

# THE PIN. The digests are the SHA-256 of the release assets as published by DuckDB
# (https://github.com/duckdb/duckdb/releases/tag/v1.5.6), checked again on every download.
$Pins = @{
    Version = '1.5.6'
    Assets  = @{
        x64   = @{ File = 'libduckdb-windows-amd64.zip'; Sha256 = '44cf59583f9951d2cb09b1bf115a63ecb2d8901e363903029d86c7d8683fe96a' }
        arm64 = @{ File = 'libduckdb-windows-arm64.zip'; Sha256 = 'c63285647e94c46d309b60d5dd1ff3cb2cdf9ca03e415a818ff7d8dbf838e0e8' }
    }
}

# The three files the build consumes; anything else in the archive is ignored.
$Wanted = @('duckdb.h', 'duckdb.lib', 'duckdb.dll')

function Get-PinnedDuckDb {
    <#
    .SYNOPSIS
        Fetch, verify and extract one architecture's library into dep\duckdb\<version>\<arch>\.
    .PARAMETER Architecture
        x64 or arm64 (a key of $Pins.Assets).
    .OUTPUTS
        A status line.
    #>
    param([Parameter(Mandatory)][string]$Architecture)

    $asset = $Pins.Assets[$Architecture]
    $dest = Join-Path $Root "dep\duckdb\$($Pins.Version)\$Architecture"
    $stamp = Join-Path $dest '.sha256'

    # Current already: every wanted file present and the stamp names this pin's digest. A stamp is
    # written only after a verified extract, so it cannot vouch for a half-copied folder.
    if (-not $Force -and (Test-Path $stamp)) {
        $have = (Get-Content -Raw $stamp).Trim()
        $complete = $true
        foreach ($f in $Wanted) { if (-not (Test-Path (Join-Path $dest $f))) { $complete = $false } }
        if ($complete -and $have -eq $asset.Sha256) {
            return "duckdb $($Pins.Version) $Architecture current: $dest"
        }
    }

    $url = "https://github.com/duckdb/duckdb/releases/download/v$($Pins.Version)/$($asset.File)"
    $work = Join-Path ([IO.Path]::GetTempPath()) ("am-duckdb-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work | Out-Null
    try {
        $zip = Join-Path $work $asset.File
        # -UseBasicParsing: Windows PowerShell 5.1 would otherwise start IE's engine; harmless on 7.
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        $got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
        if ($got -ne $asset.Sha256) {
            # Never extract an archive that is not the pinned one: the DLL runs in our process.
            throw "duckdb $Architecture digest mismatch: expected $($asset.Sha256), got $got ($url)"
        }
        $unz = Join-Path $work 'x'
        Expand-Archive -Path $zip -DestinationPath $unz
        $staged = Join-Path $work 'staged'
        New-Item -ItemType Directory -Path $staged | Out-Null
        foreach ($f in $Wanted) {
            $src = Get-ChildItem -Path $unz -Recurse -File -Filter $f | Select-Object -First 1
            if (-not $src) { throw "duckdb $Architecture archive lacks $f ($url)" }
            Copy-Item $src.FullName (Join-Path $staged $f)
        }
        Set-Content -Path (Join-Path $staged '.sha256') -Value $asset.Sha256 -NoNewline
        # Swap into place: a stale folder is removed first, then the staged one moves in whole, so
        # a reader never sees a mix of two versions.
        if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
        New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
        Move-Item $staged $dest
        return "duckdb $($Pins.Version) $Architecture fetched: $dest"
    }
    finally {
        if (Test-Path $work) { Remove-Item -Recurse -Force $work }
    }
}

$targets = if ($Arch -eq 'all') { @('x64', 'arm64') } else { @($Arch) }
foreach ($a in $targets) { Get-PinnedDuckDb -Architecture $a }
