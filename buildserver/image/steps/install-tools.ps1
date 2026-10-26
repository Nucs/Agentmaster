<#
.SYNOPSIS
    Installs the rest of what an Agentmaster build runs in the am-build layer: PowerShell 7, Git, the VSSetup module, and
    long paths; a step of `images.py build-image`.

.DESCRIPTION
    - PowerShell 7.6.8 into C:\Program Files\PowerShell\7 (tools\Build-Agentmaster.ps1 is run with pwsh).
    - PortableGit 2.56.0.3 into C:\Program Files\Git (the build runs `git rev-parse`; the build step fetches the
      source from a git bundle). Git is configured system-wide: core.longpaths (the source tree's deepest paths), and
      safe.directory * (the clone on D: is made by the guest agent's user, which every build runs as anyway).
    - Both folders go on the machine PATH, so every later process (the guest agent restarts at each boot) finds them.
    - The VSSetup PowerShell module (tools\OpenConsole.psm1's Set-MsbuildDevEnvironment finds Build Tools with it)
      into C:\BuildServer\psmodules\VSSetup, copied from the share (this PC's own copy, staged by images.py): the build
      would otherwise download it from the PowerShell Gallery into the clone at its first run, which can stop on a
      prompt in a session no one answers. The build step copies it into each clone's .PowershellModules.
    - Long paths (HKLM ...\FileSystem\LongPathsEnabled = 1) for every process.

    Each download is taken from the share's image\dl folder when images.py staged it there (Q:\buildserver\cache\dl on
    the PC), else from its pinned URL, and is checked against its SHA-256 either way.

.OUTPUTS
    One line per part, then "TOOLS {json}".

.NOTES
    PowerShell 5.1. Exit codes: 0 done; 1 a download did not match its SHA-256, or a tool does not answer afterwards.
#>
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$dl = 'C:\amb\dl'
New-Item -ItemType Directory -Force -Path $dl | Out-Null

function Get-Pinned {
    <#
    .SYNOPSIS
        A pinned download: the share's staged copy, else the URL; checked against its SHA-256.
    .PARAMETER Name
        The file name (as staged in Z:\image\dl).
    .PARAMETER Url
        Where it is downloaded from when it was not staged.
    .PARAMETER Sha256
        Its SHA-256, lower-case hex.
    .OUTPUTS
        The local path of the checked file.
    .NOTES
        Throws when the file does not match: a changed download must never be installed silently.
    #>
    param([string]$Name, [string]$Url, [string]$Sha256)
    $target = Join-Path $dl $Name
    $staged = Join-Path 'Z:\image\dl' $Name
    if (Test-Path -LiteralPath $staged) {
        Copy-Item -LiteralPath $staged -Destination $target -Force
    } else {
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $target
    }
    $have = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($have -ne $Sha256) { throw "$Name has SHA-256 $have, not the pinned $Sha256" }
    return $target
}

$facts = [ordered]@{}

# PowerShell 7.
$zip = Get-Pinned 'PowerShell-7.6.8-win-x64.zip' `
    'https://github.com/PowerShell/PowerShell/releases/download/v7.6.8/PowerShell-7.6.8-win-x64.zip' `
    'b9f074f923c499b51ec99d313da8e9ae90d17a9f3adf00c73d6189f34d908528'
$pwshDir = "$env:ProgramFiles\PowerShell\7"
if (Test-Path $pwshDir) { Remove-Item -Recurse -Force $pwshDir }
Expand-Archive -LiteralPath $zip -DestinationPath $pwshDir
$facts.pwsh = (& "$pwshDir\pwsh.exe" -NoProfile -Command '$PSVersionTable.PSVersion.ToString()')

# Git (PortableGit is a 7-Zip self-extractor: -o names the folder, -y answers yes).
$exe = Get-Pinned 'PortableGit-2.56.0.3-64-bit.7z.exe' `
    'https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.3/PortableGit-2.56.0.3-64-bit.7z.exe' `
    '4fc27b779829a36df77911e3f4dea09c66c790dd6c5f7dfa3b6a5479f1fe354f'
$gitDir = "$env:ProgramFiles\Git"
if (Test-Path $gitDir) { Remove-Item -Recurse -Force $gitDir }
$extract = Start-Process -FilePath $exe -ArgumentList @("-o`"$gitDir`"", '-y') -Wait -PassThru
if ($extract.ExitCode -ne 0) { throw "PortableGit extraction failed (exit $($extract.ExitCode))" }
& "$gitDir\cmd\git.exe" config --system core.longpaths true
& "$gitDir\cmd\git.exe" config --system safe.directory '*'
$facts.git = (& "$gitDir\cmd\git.exe" --version)

# Both on the machine PATH (read fresh, so a re-run adds nothing twice).
$machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$entries = @($machinePath -split ';' | Where-Object { $_ })
foreach ($dir in @($pwshDir, "$gitDir\cmd")) {
    if ($entries -notcontains $dir) { $entries += $dir }
}
[Environment]::SetEnvironmentVariable('Path', ($entries -join ';'), 'Machine')

# The VSSetup module, from the share (this PC's copy).
$modules = 'C:\BuildServer\psmodules'
New-Item -ItemType Directory -Force -Path $modules | Out-Null
if (-not (Test-Path 'Z:\image\psmodules\VSSetup')) { throw 'the share has no image\psmodules\VSSetup (images.py stages it)' }
Copy-Item -Recurse -Force 'Z:\image\psmodules\VSSetup' $modules
$facts.vssetup = @(Get-ChildItem "$modules\VSSetup" -Directory | ForEach-Object Name) -join ','

# Long paths for every process.
Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled -Value 1 -Type DWord
$facts.longPaths = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem').LongPathsEnabled

$facts.GetEnumerator() | ForEach-Object { Write-Output ("{0,-10} {1}" -f $_.Key, $_.Value) }
Write-Output ('TOOLS ' + ($facts | ConvertTo-Json -Compress))
exit 0
