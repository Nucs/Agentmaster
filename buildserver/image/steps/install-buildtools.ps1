<#
.SYNOPSIS
    Installs Visual Studio 2022 Build Tools with what an Agentmaster build needs; a step of the am-build layer
    (DESIGN.md §4.2), run once by `images.py build-image` in a layer slot over am-base.

.DESCRIPTION
    Downloads the Build Tools bootstrapper (the current VS 2022 release) and installs, unattended, into C:\BuildTools:
    the C++ build tools workload (MSVC x64, MSBuild) and the UWP build tools workload with their recommended components,
    the Windows 11 SDKs 22621 and 26100, vcpkg and the .NET SDK. These are the measured arguments of
    buildserver/measure/guest/vs-install.ps1 (exit 0 on Tiny11 Core, 28 min with its 4.8 GB download, DESIGN.md §16.5),
    with the install path moved from the measurement's data disk to C:, which the image keeps.

    Then checks what the build needs and prints one line per fact: cl.exe (Hostx64\x64), MSBuild.exe, the two SDKs'
    headers, the 22621 SDK's MakeAppx.exe (New-UnpackagedTerminalDistribution.ps1's default) and the vcpkg component.
    A missing one fails the step: an image without it would only fail later, in every build.

.OUTPUTS
    Progress lines, then "BUILDTOOLS {json}". On a failure the installer's dd_*.log files are copied to
    Z:\image\logs (the slot's share), so they outlive the slot.

.NOTES
    PowerShell 5.1 (the guest's own). Exit codes: 0 installed and checked; 1 the bootstrapper failed (its exit code is
    printed: 3010 = installed but wants a reboot, accepted); 2 installed but a required file is missing.
#>
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$dl = 'C:\amb\dl'
$installPath = 'C:\BuildTools'
New-Item -ItemType Directory -Force -Path $dl | Out-Null

$watch = [Diagnostics.Stopwatch]::StartNew()
Invoke-WebRequest -UseBasicParsing -Uri 'https://aka.ms/vs/17/release/vs_buildtools.exe' -OutFile "$dl\vs_buildtools.exe"
Write-Output ("bootstrapper downloaded in {0:0.0} s" -f $watch.Elapsed.TotalSeconds)

$arguments = @(
    '--quiet', '--wait', '--norestart', '--nocache', '--installPath', $installPath,
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
$installer = Start-Process -FilePath "$dl\vs_buildtools.exe" -ArgumentList $arguments -Wait -PassThru
Write-Output ("bootstrapper exit {0} after {1:0.0} min" -f $installer.ExitCode, $watch.Elapsed.TotalMinutes)
if ($installer.ExitCode -notin 0, 3010) {
    # Cleanup-on-failure: the installer's own logs are the only account of why; the slot (and its TEMP) goes away.
    New-Item -ItemType Directory -Force -Path 'Z:\image\logs' | Out-Null
    Get-ChildItem $env:TEMP -Filter 'dd_*.log' -ErrorAction SilentlyContinue | Copy-Item -Destination 'Z:\image\logs' -Force
    exit 1
}

$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$cl = Get-ChildItem "$installPath\VC\Tools\MSVC" -Recurse -Filter cl.exe -ErrorAction SilentlyContinue |
    Where-Object FullName -like '*Hostx64\x64*' | Select-Object -First 1
$msbuild = Get-ChildItem "$installPath\MSBuild" -Recurse -Filter MSBuild.exe -ErrorAction SilentlyContinue |
    Where-Object FullName -like '*\amd64\*' | Select-Object -First 1
$facts = [ordered]@{
    exitCode      = $installer.ExitCode
    minutes       = [math]::Round($watch.Elapsed.TotalMinutes, 1)
    cl            = if ($cl) { $cl.FullName } else { $null }
    msbuild       = if ($msbuild) { $msbuild.FullName } else { $null }
    sdk22621      = Test-Path "$kits\Include\10.0.22621.0\um\windows.h"
    sdk26100      = Test-Path "$kits\Include\10.0.26100.0\um\windows.h"
    makeAppx      = Test-Path "$kits\bin\10.0.22621.0\x64\MakeAppx.exe"
    vcpkg         = Test-Path "$installPath\VC\vcpkg\vcpkg.exe"
    freeC_GB      = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
}
$facts.GetEnumerator() | ForEach-Object { Write-Output ("{0,-10} {1}" -f $_.Key, $_.Value) }
Write-Output ('BUILDTOOLS ' + ($facts | ConvertTo-Json -Compress))
$missing = @('cl', 'msbuild') | Where-Object { -not $facts[$_] }
$missing += @('sdk22621', 'sdk26100', 'makeAppx', 'vcpkg') | Where-Object { -not $facts[$_] }
if ($missing) {
    Write-Output ('missing: ' + ($missing -join ', '))
    exit 2
}
exit 0
