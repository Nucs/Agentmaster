<#
.SYNOPSIS
    Turns off what a build-server VM does not need (DESIGN.md §4.4); a step of the am-e2e and am-build layers.

.DESCRIPTION
    Tiny11 Core still runs services a test or build VM never uses, each costing memory and CPU in every slot. This
    script disables them (startup type Disabled, and stops them now), leaving alone what Agentmaster or its tests need:
    text input (the keyboard), push notifications (Agentmaster's toasts), themes, UI Automation, the balloon service.

    Disabled: Windows Search (WSearch), SysMain (prefetch), DiagTrack (telemetry), the print spooler, Delivery
    Optimization (DoSvc), geolocation (lfsvc), the connected-devices platform (CDPSvc and the per-user CDPUserSvc
    template), Remote Desktop (TermService: dockur's console is VNC).

    A service that does not exist (Tiny11 Core removed it) is skipped; one Windows refuses to change (a protected
    service) is reported and skipped. Nothing here can make the image unbootable: only services are changed.

.OUTPUTS
    One line per service: "disabled", "absent" or "refused: <reason>"; then "TRIMS {json}".

.NOTES
    PowerShell 5.1. Exit code 0 even when some services were refused (a trim is an optimization, not a requirement).
#>
$ErrorActionPreference = 'Continue'
$summary = [ordered]@{}
$services = @('WSearch', 'SysMain', 'DiagTrack', 'Spooler', 'DoSvc', 'lfsvc', 'CDPSvc', 'TermService')
foreach ($name in $services) {
    $service = Get-Service -Name $name -ErrorAction SilentlyContinue
    if (-not $service) {
        $summary[$name] = 'absent'
    } else {
        try {
            if ($service.Status -ne 'Stopped') {
                Stop-Service -Name $name -Force -ErrorAction Stop
            }
            Set-Service -Name $name -StartupType Disabled -ErrorAction Stop
            $summary[$name] = 'disabled'
        } catch {
            # Some services are protected (DoSvc on recent builds): the registry's Start value is the way that remains.
            try {
                Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\$name" -Name Start -Value 4 -ErrorAction Stop
                $summary[$name] = 'disabled (registry)'
            } catch {
                $summary[$name] = "refused: $($_.Exception.Message)"
            }
        }
    }
    Write-Output ("{0,-12} {1}" -f $name, $summary[$name])
}
# CDPUserSvc is a per-user service template: its instances (CDPUserSvc_xxxxx) start from the template's Start value.
$template = 'HKLM:\SYSTEM\CurrentControlSet\Services\CDPUserSvc'
if (Test-Path $template) {
    Set-ItemProperty -Path $template -Name Start -Value 4 -ErrorAction SilentlyContinue
    $summary['CDPUserSvc'] = 'disabled (template)'
} else {
    $summary['CDPUserSvc'] = 'absent'
}
Write-Output ("{0,-12} {1}" -f 'CDPUserSvc', $summary['CDPUserSvc'])
Write-Output ('TRIMS ' + ($summary | ConvertTo-Json -Compress))
exit 0
