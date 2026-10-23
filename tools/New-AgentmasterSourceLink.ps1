# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
<#
.SYNOPSIS
    Write the Source Link map that a release build embeds in every PDB, so a debugger downloads each source
    file of a crash from GitHub at the exact commit that was built.

.DESCRIPTION
    Agentmaster is open source, so its release PDBs are full and public (doc/agentmaster/SYMBOLS.md). A PDB
    names its source files by the ABSOLUTE paths of the machine that built it (on a GitHub runner
    D:\a\Agentmaster\Agentmaster\src\...), which exist nowhere else. A Source Link map (JSON, the format of
    github.com/dotnet/designs accepted/2020/diagnostics/source-link.md) tells Visual Studio and WinDbg how to
    turn such a path into a URL: "<build root>\*" -> "https://raw.githubusercontent.com/<repo>/<commit>/*".

    The map written here holds one entry for the repository and one for every GitHub submodule, which lives in
    its own repository at its own pinned commit (refs\claude-agent-sdk-cpp -> 0xeb/claude-agent-sdk-cpp). The
    Source Link rules resolve the most specific key first, so a submodule file never falls back to the main
    repository's URL. A submodule hosted anywhere but GitHub gets no entry (a warning says so): its files then
    simply have no download link.

    The build passes the file to every link through /p:AgentmasterSourceLinkFile=<file> (src\
    common.build.post.props -> link /SOURCELINK), so the map lands in each module PDB as the named stream
    "sourcelink$1". Files that are not in the repository (vcpkg and NuGet headers, generated sources) also
    match the repository key; a debugger then gets a 404 for them and shows "source not available" - no harm.

.PARAMETER RepoRoot
    The folder the build compiled from: its absolute path is the prefix every source path in the PDBs starts
    with (on a GitHub runner $env:GITHUB_WORKSPACE). Default: the parent of this script's folder.

.PARAMETER Repository
    The GitHub repository as <owner>/<name> whose raw files the main key points at (on a runner
    $env:GITHUB_REPOSITORY, so a fork's release points at the fork).

.PARAMETER Commit
    The full commit SHA that was built (on a runner $env:GITHUB_SHA). A short or symbolic name is refused: the
    URLs must name exactly the source of the binaries for as long as the release exists.

.PARAMETER OutFile
    Where to write the map. The folder must exist. Written as UTF-8 without a byte-order mark, the encoding the
    linker embeds verbatim.

.OUTPUTS
    The map's entries, one line each (key -> URL), on the host. The script throws - a non-zero exit, so the
    workflow step fails - when the commit is not a 40-hex SHA, the repository is not <owner>/<name>, git cannot
    read the submodule list, or the file cannot be written.

.EXAMPLE
    pwsh -File tools\New-AgentmasterSourceLink.ps1 -RepoRoot $env:GITHUB_WORKSPACE -Repository $env:GITHUB_REPOSITORY `
         -Commit $env:GITHUB_SHA -OutFile "$env:RUNNER_TEMP\agentmaster-sourcelink.json"
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$Commit,
    [Parameter(Mandatory)][string]$OutFile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3
# git's exit codes are checked by hand below (exit 1 of `git config --get-regexp` only means "no submodules"),
# so PowerShell 7's optional throw-on-native-failure must stay off whatever the caller's profile says.
$PSNativeCommandUseErrorActionPreference = $false

<#
.SYNOPSIS
    The <owner>/<name> of a GitHub remote URL, or $null for any other host.
.PARAMETER Url
    A submodule URL from .gitmodules: https://github.com/<owner>/<name>[.git][/] or git@github.com:<owner>/<name>[.git].
.OUTPUTS
    [string] "<owner>/<name>", or $null when the URL is not a GitHub repository (no raw.githubusercontent.com
    address exists for it). Never throws.
#>
function Get-GitHubRepositoryName {
    param([string]$Url)
    if ($Url -match '^(?:https?://github\.com/|git@github\.com:|ssh://git@github\.com/)([^/]+)/([^/]+?)(?:\.git)?/?$') {
        return "$($Matches[1])/$($Matches[2])"
    }
    return $null
}

<#
.SYNOPSIS
    The Source Link key for every file under a folder: its absolute path with a trailing "\*".
.PARAMETER Folder
    An absolute folder path (any trailing separator is dropped; forward slashes become backslashes, the form
    MSVC records in a PDB).
.OUTPUTS
    [string] "<folder>\*". Never throws.
#>
function Get-SourceLinkKey {
    param([string]$Folder)
    return ($Folder -replace '/', '\').TrimEnd('\') + '\*'
}

if ($Commit -notmatch '^[0-9a-fA-F]{40}$') {
    throw "-Commit must be a full 40-hex commit SHA (got '$Commit'): the URLs have to name exactly the source that was built."
}
if ($Repository -notmatch '^[^/\s]+/[^/\s]+$') {
    throw "-Repository must be <owner>/<name> (got '$Repository')."
}
$root = (Resolve-Path -LiteralPath $RepoRoot).ProviderPath

# The map, most general key first; the debugger picks the most specific match whatever the order.
$documents = [ordered]@{}
$documents[(Get-SourceLinkKey $root)] = "https://raw.githubusercontent.com/$Repository/$($Commit.ToLowerInvariant())/*"

# Every submodule: its path, its URL, and the commit the superproject pins (ls-tree of HEAD, never the
# checked-out state, so the map describes the committed source even in a dirty local tree).
$gitmodules = Join-Path $root '.gitmodules'
if (Test-Path -LiteralPath $gitmodules) {
    $pathLines = @(& git -C $root config -f .gitmodules --get-regexp '^submodule\..*\.path$')
    if ($LASTEXITCODE -gt 1) {  # 1 = no match (an empty .gitmodules), anything above = git failed
        throw "git could not read $gitmodules (exit $LASTEXITCODE)."
    }
    foreach ($line in $pathLines) {
        # "submodule.<name>.path <path>"; the name itself may hold dots and slashes (refs/claude-agent-sdk-cpp).
        $pm = [regex]::Match([string]$line, '^submodule\.(.+)\.path\s+(.+)$')
        if (-not $pm.Success) { continue }
        $name = $pm.Groups[1].Value
        $subPath = $pm.Groups[2].Value.Trim()
        $url = [string](& git -C $root config -f .gitmodules --get "submodule.$name.url")
        $ghName = Get-GitHubRepositoryName $url.Trim()
        if (-not $ghName) {
            Write-Warning "submodule '$subPath' ($url) is not on GitHub: its files get no Source Link entry."
            continue
        }
        $tree = [string](& git -C $root ls-tree HEAD -- $subPath)
        $tm = [regex]::Match($tree, '^160000 commit ([0-9a-f]{40})\t')
        if ($LASTEXITCODE -ne 0 -or -not $tm.Success) {
            throw "git ls-tree HEAD -- '$subPath' did not name a submodule commit (got '$tree')."
        }
        $documents[(Get-SourceLinkKey (Join-Path $root $subPath))] = "https://raw.githubusercontent.com/$ghName/$($tm.Groups[1].Value)/*"
    }
}

$json = ConvertTo-Json -InputObject ([ordered]@{ documents = $documents }) -Depth 3
[IO.File]::WriteAllText($OutFile, $json, (New-Object System.Text.UTF8Encoding($false)))
foreach ($k in $documents.Keys) {
    Write-Host ("Source Link: {0} -> {1}" -f $k, $documents[$k])
}
