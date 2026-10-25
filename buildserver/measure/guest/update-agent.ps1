<#
.SYNOPSIS
    Measurement job: replaces the guest's agent with the new agent.ps1 and restarts it at once; also reports Windows'
    own boot-performance records of the boots so far.

.DESCRIPTION
    - Copies \\host.lan\Data\tools\agent.ps1 over C:\OEM\agent.ps1 (the logon task's script).
    - Reads Microsoft-Windows-Diagnostics-Performance/Operational event 100 (one per boot: BootTime, MainPathBootTime,
      BootPostBootTime in ms) into results\boot-events.json, when the trimmed image keeps that log.
    - Stops the agent that started this job (its parent process) and starts the logon task again, so the new agent
      runs without a reboot. The old agent therefore never writes this job's .exit file; the new agent's
      results\agent-start-*.json is the sign that it worked.

.OUTPUTS
    None; results\boot-events.json on the share.
#>
$ErrorActionPreference = 'Continue'
Copy-Item '\\host.lan\Data\tools\agent.ps1' 'C:\OEM\agent.ps1' -Force
$events = try {
    Get-WinEvent -LogName 'Microsoft-Windows-Diagnostics-Performance/Operational' -FilterXPath '*[System[EventID=100]]' `
        -ErrorAction Stop | ForEach-Object {
        $x = [xml]$_.ToXml()
        $d = @{}
        foreach ($n in $x.Event.EventData.Data) { $d[$n.Name] = $n.'#text' }
        [ordered]@{
            time = $_.TimeCreated.ToString('o'); bootMs = $d['BootTime']; mainPathMs = $d['MainPathBootTime']
            postBootMs = $d['BootPostBootTime']
        }
    }
} catch { "unavailable: $($_.Exception.Message)" }
[ordered]@{ agentUpdated = (Get-Item 'C:\OEM\agent.ps1').LastWriteTime.ToString('o'); bootEvents = @($events) } |
    ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 '\\host.lan\Data\results\boot-events.json'
# The agent that runs this job is this process's parent; the task allows one instance, so it must go first.
$parent = (Get-CimInstance Win32_Process -Filter "ProcessId=$PID").ParentProcessId
Stop-Process -Id $parent -Force
Start-Sleep -Seconds 1
schtasks.exe /Run /TN BsAgent | Out-Null
