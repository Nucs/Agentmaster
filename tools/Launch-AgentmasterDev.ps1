#Requires -Version 5
<#
.SYNOPSIS
  Launch a deployed Agentmaster instance AND auto-answer the "Reopen your N previous
  windows?" prompt, so a scripted deploy never hangs on it.

.DESCRIPTION
  On startup the WindowEmperor raises a BLOCKING Win32 MessageBox (MB_YESNO, class #32770,
  caption "Agentmaster") when MORE THAN ONE window was open at last exit
  (src/cascadia/WindowsTerminal/WindowEmperor.cpp -> ::MessageBoxW(... MB_YESNO ...)).
  Because it blocks the startup thread, an automated `Start-Process` relaunch hangs until a
  human clicks -- and the engine (bridge/observer) doesn't finish initializing until then.

  This helper launches the app, then polls for that MessageBox and POSTs the button click
  (WM_COMMAND -> IDYES/IDNO) so the deploy proceeds unattended. A single-window launch shows
  no prompt; the watcher simply times out harmlessly (REOPEN_NODIALOG).

  Default is -Reopen Yes (restore the previous windows -- the chosen deploy behavior). The
  MessageBox caption is hardcoded "Agentmaster" for BOTH identities, so the watcher also works
  for the Release instance; pass -AppId for that.

.EXAMPLE
  pwsh -ExecutionPolicy Bypass -File .\tools\Launch-AgentmasterDev.ps1            # launch dev, auto-Yes
.EXAMPLE
  pwsh -ExecutionPolicy Bypass -File .\tools\Launch-AgentmasterDev.ps1 -Reopen No # single window
.EXAMPLE
  # App already launching elsewhere; just answer the prompt:
  pwsh -ExecutionPolicy Bypass -File .\tools\Launch-AgentmasterDev.ps1 -NoLaunch
#>
[CmdletBinding()]
param(
    [ValidateSet('Yes', 'No')][string]$Reopen = 'Yes',
    [string]$AppId = 'AgentmasterDev_56k4f06dsfp9r!App',
    # The reopen MessageBox is raised EARLY in WindowEmperor startup (before the windows open
    # + engine init), so it shows within a few seconds if it shows at all. A single-window
    # launch shows none, and the watcher just waits out this window once -- keep it short so a
    # no-prompt launch doesn't add dead wall-clock to the deploy.
    [int]$TimeoutSec = 25,
    [switch]$NoLaunch
)

if (-not $NoLaunch) {
    Start-Process ("shell:appsFolder\{0}" -f $AppId)
    Write-Host "launched $AppId"
}

Add-Type -Namespace AM -Name Win -MemberDefinition @'
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern System.IntPtr FindWindow(string c, string n);
[DllImport("user32.dll")] public static extern System.IntPtr GetDlgItem(System.IntPtr h, int id);
[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
'@

$WM_COMMAND = 0x0111
$btnId = if ($Reopen -eq 'No') { 7 } else { 6 }   # IDNO=7, IDYES=6
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    $h = [AM.Win]::FindWindow('#32770', 'Agentmaster')
    if ($h -ne [System.IntPtr]::Zero) {
        $opid = 0; [void][AM.Win]::GetWindowThreadProcessId($h, [ref]$opid)
        $hBtn = [AM.Win]::GetDlgItem($h, $btnId)              # the Yes/No button (BN_CLICKED via WM_COMMAND)
        [void][AM.Win]::PostMessage($h, $WM_COMMAND, [System.IntPtr]$btnId, $hBtn)
        Write-Host "REOPEN_CLICK=$Reopen hwnd=$h pid=$opid"
        exit 0
    }
    Start-Sleep -Milliseconds 400
}
Write-Host "REOPEN_NODIALOG (single-window launch, or it never appeared within ${TimeoutSec}s)"
exit 0
