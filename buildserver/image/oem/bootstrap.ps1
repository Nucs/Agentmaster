<#
.SYNOPSIS
    Starts the build server's guest agent in a slot: runs at every logon (scheduled task AmbBootstrap, elevated).

.DESCRIPTION
    1. Maps the slot's share (dockur's \\host.lan\Data) to Z:, because the e2e engine's guest half expects its folders
       there (Z:\e2e\src, Z:\e2e\out, Z:\e2e\builds: e2e/guest/run_suite.py and provision.ps1 run unchanged).
    2. Turns off the SMB client's directory and not-found caches: the share is written by the coordinator behind
       Samba's back, so a cached listing or a cached "not found" would hide a file that exists (measured 2026-10-25: a
       host-written file was never listed). Everything is still opened by its exact path.
    3. Copies amb-guest.exe from Z:\amb\ to C:\amb\ (the share's copy is the slot's own version, so a new agent needs no
       new image) and runs it with Z:\amb\slot.json, waiting while the share is not up yet (it answered 24-32 s after
       boot, measured).
    4. If the agent ends with an error (not after a shutdown step: Windows is going down then), it is started again
       after a pause, so a crash of the agent does not leave the slot silent until the coordinator gives up on it.

.OUTPUTS
    None. C:\amb\bootstrap.log gets one line per step and per agent start.

.NOTES
    PowerShell 5.1 (the guest's own); no network other than the share is used.
#>
$ErrorActionPreference = 'Continue'
$log = 'C:\amb\bootstrap.log'
New-Item -ItemType Directory -Force -Path 'C:\amb' | Out-Null

function Write-Log([string]$message) {
    <#
    .SYNOPSIS
        Appends one stamped line to C:\amb\bootstrap.log.
    .PARAMETER message
        The line.
    #>
    "$((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ')) $message" | Add-Content -Encoding utf8 $log
}

Write-Log 'bootstrap starting'
try {
    Set-SmbClientConfiguration -DirectoryCacheLifetime 0 -FileNotFoundCacheLifetime 0 -FileInfoCacheLifetime 0 -Force `
        -ErrorAction Stop | Out-Null
} catch {
    Write-Log "smb caches: $($_.Exception.Message)"
}

$deadline = (Get-Date).AddMinutes(10)
while (-not (Test-Path 'Z:\amb\slot.json')) {
    if (-not (Test-Path 'Z:\')) {
        & net.exe use Z: '\\host.lan\Data' /persistent:no 2>&1 | Out-Null
    }
    if ((Get-Date) -gt $deadline) {
        Write-Log 'no Z:\amb\slot.json after 10 minutes: giving up'
        exit 2
    }
    Start-Sleep -Seconds 2
}
Write-Log 'share mapped; Z:\amb\slot.json found'

for ($start = 1; $start -le 20; $start++) {
    try {
        Copy-Item -Force 'Z:\amb\amb-guest.exe' 'C:\amb\amb-guest.exe' -ErrorAction Stop
    } catch {
        Write-Log "copy of the agent failed: $($_.Exception.Message); keeping the last copy"
    }
    Write-Log "starting amb-guest (start $start)"
    $process = Start-Process -FilePath 'C:\amb\amb-guest.exe' -ArgumentList @('--config', 'Z:\amb\slot.json', '--work', 'C:\amb') `
        -NoNewWindow -Wait -PassThru
    Write-Log "amb-guest ended with exit code $($process.ExitCode)"
    if ($process.ExitCode -eq 0) {
        # A clean end: a shutdown step (Windows is going down) or a stop by hand. Nothing to restart.
        break
    }
    Start-Sleep -Seconds 5
}
