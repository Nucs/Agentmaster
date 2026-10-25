<#
.SYNOPSIS
    Measurement job: reports what a freshly installed build-server test VM contains and what it costs at rest.

.DESCRIPTION
    Run by agent.ps1 inside the guest. Writes results\facts.json on the shared folder with:
    - memory at rest (available, committed, pools, file cache) and the 15 largest processes by working set;
    - every running service and the start type of the services an image decision depends on (servicing, MSI,
      AppX deployment, Windows Update, Defender, search, telemetry);
    - the system disk's use, with the sizes of Windows, WinSxS, the page file and the other large files;
    - the runtime pieces a build or e2e slot needs (.NET Framework release, PowerShell, the VC++ runtime DLLs,
      WebView2, winget, Edge) and whether optional features can be queried (Tiny11 Core is not serviceable).
    Every probe is wrapped, so a component the trimmed image removed reads as missing instead of ending the job.

.OUTPUTS
    None. results\facts.json on \\host.lan\Data.

.NOTES
    Read-only: it changes nothing in the guest except its own JSON file on the share.
#>
$ErrorActionPreference = 'Continue'
$share = '\\host.lan\Data'

function Get-FolderSizeGB {
    <#
    .SYNOPSIS
        Sums the sizes of every file under a folder, skipping what access is denied to.
    .PARAMETER Path
        The folder to measure.
    .OUTPUTS
        [double] gigabytes rounded to 2 places, or $null when the folder does not exist.
    #>
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    $sum = (Get-ChildItem $Path -Recurse -Force -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    [math]::Round($sum / 1GB, 2)
}

function Get-ServiceFacts {
    <#
    .SYNOPSIS
        The status and start type of one service, or 'missing' when the image removed it.
    .PARAMETER Name
        The service name.
    .OUTPUTS
        [string] such as 'Running/Automatic', 'Stopped/Manual' or 'missing'.
    #>
    param([string]$Name)
    $s = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if (-not $s) { return 'missing' }
    "$($s.Status)/$($s.StartType)"
}

$counters = @{}
foreach ($c in '\Memory\Available MBytes', '\Memory\Committed Bytes', '\Memory\Pool Nonpaged Bytes',
    '\Memory\Pool Paged Bytes', '\Memory\Cache Bytes', '\Memory\Modified Page List Bytes') {
    $v = (Get-Counter $c -ErrorAction SilentlyContinue).CounterSamples.CookedValue
    $counters[$c] = if ($c -like '*MBytes') { [int]$v } else { [int]($v / 1MB) }
}

$top = Get-Process | Sort-Object WorkingSet64 -Descending | Select-Object -First 15 |
    ForEach-Object { '{0} {1} MB' -f $_.ProcessName, [int]($_.WorkingSet64 / 1MB) }

$servicesOfInterest = 'TrustedInstaller', 'msiserver', 'AppXSvc', 'ClipSVC', 'wuauserv', 'UsoSvc', 'BITS',
    'WinDefend', 'WdNisSvc', 'SecurityHealthService', 'WSearch', 'SysMain', 'DiagTrack', 'Spooler', 'Winmgmt',
    'Schedule', 'LanmanWorkstation', 'TermService', 'Audiosrv', 'StateRepository', 'TextInputManagementService',
    'TabletInputService', 'UIAutomationCore', 'Themes', 'vmicguestinterface', 'QEMU-GA', 'VirtioFsSvc'
$serviceMap = [ordered]@{}
foreach ($n in $servicesOfInterest) { $serviceMap[$n] = Get-ServiceFacts $n }

$featuresProbe = try {
    $f = Get-WindowsOptionalFeature -Online -ErrorAction Stop
    "ok: $(@($f | Where-Object State -eq 'Enabled').Count) enabled of $(@($f).Count)"
} catch { "failed: $($_.Exception.Message)" }

$ndp = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full' -ErrorAction SilentlyContinue
$webView = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}' `
    -ErrorAction SilentlyContinue
$disk = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='C:'"
$pageFile = Get-CimInstance Win32_PageFileUsage -ErrorAction SilentlyContinue
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$os = Get-CimInstance Win32_OperatingSystem

$facts = [ordered]@{
    now               = (Get-Date).ToString('o')
    uptimeSeconds     = [int]((Get-Date) - $os.LastBootUpTime).TotalSeconds
    caption           = $os.Caption
    build             = "$($os.BuildNumber).$((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').UBR)"
    cpu               = "$($cpu.Name) ($($cpu.NumberOfLogicalProcessors) logical)"
    ramTotalMB        = [int]($os.TotalVisibleMemorySize / 1KB)
    memoryCountersMB  = $counters
    topProcesses      = $top
    processes         = @(Get-Process).Count
    servicesRunning   = @(Get-Service | Where-Object Status -eq 'Running').Count
    runningServices   = @(Get-Service | Where-Object Status -eq 'Running' | Sort-Object Name | ForEach-Object Name)
    services          = $serviceMap
    optionalFeatures  = $featuresProbe
    dotnetFxRelease   = $ndp.Release
    powershell        = $PSVersionTable.PSVersion.ToString()
    vcRuntime         = [ordered]@{
        vcruntime140    = Test-Path "$env:windir\System32\vcruntime140.dll"
        vcruntime140_1  = Test-Path "$env:windir\System32\vcruntime140_1.dll"
        msvcp140        = Test-Path "$env:windir\System32\msvcp140.dll"
    }
    webView2          = if ($webView) { $webView.pv } else { 'missing' }
    edge              = Test-Path "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
    winget            = [bool](Get-Command winget.exe -ErrorAction SilentlyContinue)
    pageFile          = @($pageFile | ForEach-Object { "$($_.Name) alloc $($_.AllocatedBaseSize) MB used $($_.CurrentUsage) MB" })
    diskSizeGB        = [math]::Round($disk.Size / 1GB, 2)
    diskFreeGB        = [math]::Round($disk.FreeSpace / 1GB, 2)
    sizesGB           = [ordered]@{
        Windows        = Get-FolderSizeGB "$env:windir"
        WinSxS         = Get-FolderSizeGB "$env:windir\WinSxS"
        ProgramFiles   = Get-FolderSizeGB "$env:ProgramFiles"
        ProgramFilesX86 = Get-FolderSizeGB "${env:ProgramFiles(x86)}"
        ProgramData    = Get-FolderSizeGB "$env:ProgramData"
        Users          = Get-FolderSizeGB 'C:\Users'
    }
    rootFiles         = @(Get-ChildItem C:\ -Force -File -ErrorAction SilentlyContinue |
        ForEach-Object { '{0} {1:N2} GB' -f $_.Name, ($_.Length / 1GB) })
    hibernation       = (powercfg /a 2>&1 | Out-String).Trim()
}
$facts | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$share\results\facts.json"
