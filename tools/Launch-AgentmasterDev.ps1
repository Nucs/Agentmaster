#Requires -Version 5
<#
.SYNOPSIS
  Launch a deployed Agentmaster instance AND auto-answer the "Reopen your N previous
  windows?" prompt, so a scripted deploy never hangs on it.

.DESCRIPTION
  On startup the WindowEmperor raises a BLOCKING Win32 task dialog (class #32770, caption
  "Agentmaster", buttons Yes / No, a "Remember my choice and don't ask again" check box) when
  MORE THAN ONE window was open at last exit
  (src/cascadia/WindowsTerminal/WindowEmperor.cpp -> AgentMaster/ReopenWindowsPrompt.h,
  doc/agentmaster/PERSISTENCE.md section 18). When the task dialog cannot be shown, the
  original MessageBox (MB_YESNO, the same class and caption) asks instead.
  Because it blocks the startup thread, an automated `Start-Process` relaunch hangs until a
  human clicks -- and the engine (bridge/observer) doesn't finish initializing until then.

  A profile that REMEMBERS a choice (the check box was ticked, or the cog's Settings ->
  Behavior -> Startup is not "Ask every time": settings.json settings.reopenWindowsAtStart)
  is never asked: the app reopens all or starts with a single window by itself, and this watcher
  times out harmlessly (REOPEN_NODIALOG).

  This helper launches the app, then polls for that dialog and POSTs the button click so the
  deploy proceeds unattended: TDM_CLICK_BUTTON (IDYES/IDNO) to a task dialog, whose buttons are
  not child windows of the dialog, or WM_COMMAND (IDYES/IDNO) to the MessageBox fallback, whose
  buttons are. It never ticks the check box: an automated answer is never remembered. A
  single-window launch shows no prompt; the watcher simply times out harmlessly (REOPEN_NODIALOG).

  Default is -Reopen Yes (restore the previous windows -- the chosen deploy behavior). The
  caption is hardcoded "Agentmaster" for BOTH identities, so the watcher also works for the
  Release instance; pass -AppId for that.

.PARAMETER Reopen
  The answer to press: Yes (the default) reopens every window that was open at the last exit;
  No starts with a single window and leaves the others behind the Manager's "Reopen Windows (N)".
  This answer applies to this start only: the check box is never ticked, so nothing is remembered.

.PARAMETER AppId
  The AUMID started with shell:appsFolder (default: the dev identity,
  AgentmasterDev_56k4f06dsfp9r!App; the release one is Agentmaster_56k4f06dsfp9r!App). Ignored with
  -NoLaunch. Starting the RELEASE instance restarts the user's production fleet: only on purpose.

.PARAMETER TimeoutSec
  How long to watch for the question, in seconds (default 25). It appears within a few seconds when
  it appears at all; a longer wait only adds dead time to a launch that never asks.

.PARAMETER NoLaunch
  Do not start anything: only watch for the question of an instance that is starting elsewhere.

.OUTPUTS
  One line on the host: "REOPEN_CLICK=<Yes|No> hwnd=<h> pid=<p> kind=<taskdialog|messagebox>" when it
  answered, or "REOPEN_NODIALOG (...)" when no question came. The exit code is always 0 (a launch
  that does not ask is not a failure). It throws nothing of its own; Start-Process and Add-Type
  errors surface as PowerShell errors.

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
    # The reopen question is raised EARLY in WindowEmperor startup (before the windows open
    # + engine init), so it shows within a few seconds if it shows at all. A single-window
    # launch (or a remembered choice) shows none, and the watcher just waits out this window
    # once -- keep it short so a no-prompt launch doesn't add dead wall-clock to the deploy.
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
$TDM_CLICK_BUTTON = 0x0466   # WM_USER + 102: "press this button" for a task dialog (its documented programmatic click)
$btnId = if ($Reopen -eq 'No') { 7 } else { 6 }   # IDNO=7, IDYES=6
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    $h = [AM.Win]::FindWindow('#32770', 'Agentmaster')
    if ($h -ne [System.IntPtr]::Zero) {
        $opid = 0; [void][AM.Win]::GetWindowThreadProcessId($h, [ref]$opid)
        # A MessageBox's Yes/No buttons are child windows with the ids IDYES/IDNO; a task dialog's are not (they live
        # deeper, under its DirectUI host), so GetDlgItem tells the two apart and each gets the click it understands.
        $hBtn = [AM.Win]::GetDlgItem($h, $btnId)
        if ($hBtn -ne [System.IntPtr]::Zero) {
            [void][AM.Win]::PostMessage($h, $WM_COMMAND, [System.IntPtr]$btnId, $hBtn)   # BN_CLICKED via WM_COMMAND
            $kind = 'messagebox'
        }
        else {
            [void][AM.Win]::PostMessage($h, $TDM_CLICK_BUTTON, [System.IntPtr]$btnId, [System.IntPtr]::Zero)
            $kind = 'taskdialog'
        }
        Write-Host "REOPEN_CLICK=$Reopen hwnd=$h pid=$opid kind=$kind"
        exit 0
    }
    Start-Sleep -Milliseconds 400
}
Write-Host "REOPEN_NODIALOG (single-window launch, a remembered choice, or it never appeared within ${TimeoutSec}s)"
exit 0
