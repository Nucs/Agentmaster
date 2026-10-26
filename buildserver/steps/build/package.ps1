<#
.SYNOPSIS
    Turns a built layout into a build's artifacts in the share: the portable zip, the layout as a zip, the symbols zip,
    the build's logs and build.json; the last step of every build.

.DESCRIPTION
    - The portable zip: the commit's own build\scripts\New-UnpackagedTerminalDistribution.ps1 -TerminalLayout
      -PortableMode (the XAML package from the worktree's packages\Microsoft.UI.Xaml.*\tools\AppX\x64\Release, the
      22621 SDK's MakeAppx.exe), zipped with Windows' own tar, as release.yml and the e2e hand-builds made theirs. Its name
      is the package's: <identity name>_<version>_x64.zip (AgentmasterDev_... for the dev identity).
    - The layout zip: the whole package layout, for an install (the swap script registers a layout folder).
    - The symbols zip: the commit's tools\Export-AgentmasterSymbols.ps1 (each binary's PDB matched by GUID and age).
      Optional: a build whose symbols do not all match still has its zip, and says so in build.json.
    - build.log / build.log.err (copied from D:) and build.json: the commit, configuration, identity, the package's name
      and version, the files and their sizes.
    Everything is written to -Out (the share, Z:\out\<request id>), from where the coordinator moves it into the store
    and records each file's SHA-256.

.PARAMETER Worktree
    The worktree's folder.

.PARAMETER Configuration
    Release or Debug.

.PARAMETER Identity
    dev or release.

.PARAMETER Layout
    The built layout.

.PARAMETER Log
    The build's output file (copied as build.log, with its .err).

.PARAMETER Out
    The folder in the share the artifacts go to.

.OUTPUTS
    One line per artifact, then "PACKAGE {json}".

.NOTES
    PowerShell 7. Exit codes: 0 packaged; 1 the portable zip could not be made.
#>
param(
    [Parameter(Mandatory)][string]$Worktree,
    [Parameter(Mandatory)][string]$Configuration,
    [Parameter(Mandatory)][string]$Identity,
    [Parameter(Mandatory)][string]$Layout,
    [Parameter(Mandatory)][string]$Log,
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$env:TEMP = 'D:\tmp'
$env:TMP = 'D:\tmp'
$watch = [Diagnostics.Stopwatch]::StartNew()
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$stage = 'D:\tmp\package'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$manifest = [xml](Get-Content -LiteralPath "$Layout\AppxManifest.xml" -Raw)
$name = [string]$manifest.Package.Identity.Name
$version = [string]$manifest.Package.Identity.Version
$base = '{0}_{1}_x64' -f $name, $version

# The portable zip.
$xaml = Get-ChildItem "$Worktree\packages" -Directory -Filter 'Microsoft.UI.Xaml.*' -ErrorAction SilentlyContinue |
    ForEach-Object { Get-ChildItem "$($_.FullName)\tools\AppX\x64\Release" -Filter 'Microsoft.UI.Xaml.*.appx' -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if (-not $xaml) {
    Write-Output "no Microsoft.UI.Xaml appx under $Worktree\packages"
    exit 1
}
$folder = & "$Worktree\build\scripts\New-UnpackagedTerminalDistribution.ps1" -TerminalLayout $Layout -XamlAppX $xaml.FullName `
    -Destination $stage -PortableMode
$folder = @($folder | Where-Object { $_ -is [System.IO.DirectoryInfo] })
if ($folder.Count -ne 1) {
    Write-Output 'New-UnpackagedTerminalDistribution.ps1 did not return one folder'
    exit 1
}
$portable = Join-Path $Out "$base.zip"
& $tar -c --format=zip -f $portable -C $folder[0].Parent.FullName $folder[0].Name
if ($LASTEXITCODE -ne 0) {
    Write-Output "tar failed making $portable ($LASTEXITCODE)"
    exit 1
}
# The script unpacks into its own temporary folder under TEMP (D:\tmp) and leaves it: the whole folder goes.
Remove-Item -Recurse -Force $folder[0].Parent.FullName -ErrorAction SilentlyContinue

# The layout as a zip (its files at the zip's root).
$layoutZip = Join-Path $Out "${base}_layout.zip"
& $tar -c --format=zip -f $layoutZip -C $Layout .
$layoutOk = $LASTEXITCODE -eq 0

# The symbols: optional, reported.
$symbolsZip = Join-Path $Out "${base}_symbols.zip"
$symbols = 'none'
try {
    # The layout and the build tree: the script searches both, recursively, for each binary's PDB by name, GUID and age.
    $rows = @(& "$Worktree\tools\Export-AgentmasterSymbols.ps1" -BinaryFolder $Layout `
        -PdbSearchPath $Layout, "$Worktree\bin\x64\$Configuration" -RequireUnder $Worktree -ZipTo $symbolsZip -PassThru)
    $symbols = '{0} matched of {1}' -f @($rows | Where-Object { $_.Status -eq 'Matched' }).Count, $rows.Count
} catch {
    $symbols = 'failed: ' + $_.Exception.Message
    Remove-Item -LiteralPath $symbolsZip -ErrorAction SilentlyContinue
}

Copy-Item -LiteralPath $Log -Destination (Join-Path $Out 'build.log') -Force
if (Test-Path -LiteralPath "$Log.err") { Copy-Item -LiteralPath "$Log.err" -Destination (Join-Path $Out 'build.log.err') -Force }

$files = Get-ChildItem -LiteralPath $Out -File | ForEach-Object { [ordered]@{ name = $_.Name; bytes = $_.Length } }
$facts = [ordered]@{
    commit        = (& "$env:ProgramFiles\Git\cmd\git.exe" -C $Worktree rev-parse HEAD).Trim()
    configuration = $Configuration
    identity      = $Identity
    package       = $name
    version       = $version
    portable      = Split-Path $portable -Leaf
    layoutZip     = if ($layoutOk) { Split-Path $layoutZip -Leaf } else { $null }
    symbols       = $symbols
    seconds       = [math]::Round($watch.Elapsed.TotalSeconds, 1)
    files         = @($files)
}
$facts | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Out 'build.json') -Encoding utf8NoBOM
$files | ForEach-Object { Write-Output ("{0,-55} {1,8:0.0} MB" -f $_.name, ($_.bytes / 1MB)) }
Write-Output ('PACKAGE ' + ($facts | ConvertTo-Json -Compress -Depth 4))
exit 0
