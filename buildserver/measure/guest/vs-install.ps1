<#
.SYNOPSIS
    Measurement job: does Visual Studio 2022 Build Tools, with what an Agentmaster build needs, install on Tiny11 Core
    25H2 (which cannot be serviced), and what does it take: download, time, disk?

.DESCRIPTION
    Downloads the Build Tools bootstrapper and installs, unattended, into D:\BuildTools: the C++ build tools workload
    (MSVC x64, MSBuild) and the UWP build tools workload with their recommended components, the Windows 11 SDKs
    22621 and 26100, vcpkg and the .NET SDK (--nocache: the package cache is not kept). Writes
    results\vs-install.json: the bootstrapper's exit code (0 = done, 3010 = done but wants a reboot), the time, the
    space used on C: and D:, the folders' sizes, and whether cl.exe, msbuild.exe and the SDK's headers are there. On a
    failure the installer's logs are copied to results\vs-logs.

.OUTPUTS
    None; results\vs-install.json (and results\vs-logs on a failure) on the share.
#>
$ErrorActionPreference = 'Continue'
$share = '\\host.lan\Data'

function Get-FreeGB {
    <#
    .SYNOPSIS
        A drive's free space in GB.
    .PARAMETER Letter
        The drive letter.
    .OUTPUTS
        [double] rounded to 2 places.
    #>
    param([string]$Letter)
    [math]::Round((Get-PSDrive $Letter).Free / 1GB, 2)
}

function Get-FolderSizeGB {
    <#
    .SYNOPSIS
        The total size of a folder's files in GB; $null when it does not exist.
    .PARAMETER Path
        The folder.
    .OUTPUTS
        [double] rounded to 2 places, or $null.
    #>
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    [math]::Round((Get-ChildItem $Path -Recurse -Force -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1GB, 2)
}

$freeC0 = Get-FreeGB C; $freeD0 = Get-FreeGB D
New-Item -ItemType Directory -Force -Path D:\vs | Out-Null
$watch = [Diagnostics.Stopwatch]::StartNew()
Invoke-WebRequest -UseBasicParsing -Uri 'https://aka.ms/vs/17/release/vs_buildtools.exe' -OutFile D:\vs\vs_buildtools.exe
$bootstrapSeconds = [math]::Round($watch.Elapsed.TotalSeconds, 1)
$arguments = @(
    '--quiet', '--wait', '--norestart', '--nocache', '--installPath', 'D:\BuildTools',
    '--add', 'Microsoft.VisualStudio.Workload.VCTools',
    '--add', 'Microsoft.VisualStudio.Workload.UniversalBuildTools',
    '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621',
    '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.26100',
    '--add', 'Microsoft.VisualStudio.Component.Vcpkg',
    '--add', 'Microsoft.NetCore.Component.SDK',
    '--includeRecommended'
)
$watch.Restart()
$proc = Start-Process -FilePath D:\vs\vs_buildtools.exe -ArgumentList $arguments -Wait -PassThru
$installSeconds = [math]::Round($watch.Elapsed.TotalSeconds, 1)
$cl = Get-ChildItem 'D:\BuildTools\VC\Tools\MSVC' -Recurse -Filter cl.exe -ErrorAction SilentlyContinue |
    Where-Object FullName -like '*Hostx64\x64*' | Select-Object -First 1
$msbuild = Get-ChildItem 'D:\BuildTools\MSBuild' -Recurse -Filter MSBuild.exe -ErrorAction SilentlyContinue | Select-Object -First 1
$result = [ordered]@{
    exitCode          = $proc.ExitCode
    bootstrapSeconds  = $bootstrapSeconds
    installSeconds    = $installSeconds
    usedOnC_GB        = [math]::Round($freeC0 - (Get-FreeGB C), 2)
    usedOnD_GB        = [math]::Round($freeD0 - (Get-FreeGB D), 2)
    sizesGB           = [ordered]@{
        BuildTools   = Get-FolderSizeGB 'D:\BuildTools'
        WindowsKits  = Get-FolderSizeGB "${env:ProgramFiles(x86)}\Windows Kits"
        VSShared     = Get-FolderSizeGB "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
        PackageCache = Get-FolderSizeGB "$env:ProgramData\Package Cache"
        DotNet       = Get-FolderSizeGB "$env:ProgramFiles\dotnet"
    }
    cl                = if ($cl) { $cl.FullName } else { $null }
    msbuild           = if ($msbuild) { $msbuild.FullName } else { $null }
    sdkIncludes       = @(Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include" -Directory -ErrorAction SilentlyContinue | ForEach-Object Name)
    dotnetSdks        = if (Test-Path "$env:ProgramFiles\dotnet\dotnet.exe") { @(& "$env:ProgramFiles\dotnet\dotnet.exe" --list-sdks) } else { @() }
}
$result | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$share\results\vs-install.json"
if ($proc.ExitCode -notin 0, 3010) {
    New-Item -ItemType Directory -Force -Path "$share\results\vs-logs" | Out-Null
    Get-ChildItem $env:TEMP -Filter 'dd_*.log' -ErrorAction SilentlyContinue | Copy-Item -Destination "$share\results\vs-logs" -Force
}
