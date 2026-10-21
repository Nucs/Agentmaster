# Agentmaster install QA - guest-side helpers (Windows PowerShell 5.1, pure ASCII).
#
# Dot-source it in the test desktop:   . C:\amqa\amqa-guest.ps1
# It READS the install state of every account (Get-AmQaState) and, between scenarios, removes every
# Agentmaster install, registration and profile from the machine (Reset-AmQa), so each scenario
# starts from a clean Windows without a VM reset. Only for a disposable test desktop: Reset-AmQa
# deletes per-user profiles and machine-wide registrations without asking.

$ErrorActionPreference = 'Continue'

# Token elevation of another process: TokenElevation (class 20) through OpenProcessToken. The agent
# runs elevated, so it can open every account's processes; -1/-2/-3 = could not open / no token /
# query failed (reported, never guessed).
if (-not ('AmQaToken' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AmQaToken {
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr token, int cls, out int value, int length, out int returned);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public static int Elevated(int pid) {
        IntPtr p = OpenProcess(0x1000, false, pid);              // PROCESS_QUERY_LIMITED_INFORMATION
        if (p == IntPtr.Zero) return -1;
        IntPtr t;
        if (!OpenProcessToken(p, 0x8, out t)) { CloseHandle(p); return -2; }   // TOKEN_QUERY
        int v, r;
        bool ok = GetTokenInformation(t, 20, out v, 4, out r);  // TokenElevation
        CloseHandle(t); CloseHandle(p);
        return ok ? v : -3;
    }
}
'@
}

# The accounts the QA uses: the signed-in Docker plus the two test accounts (absent ones are skipped).
$AmQaAccounts = @('Docker', 'qastd', 'qaadmin')

function Get-AmQaInstallFacts([string]$dir) {
    <#
    .SYNOPSIS
    One install folder's decision files and version, or $null when the folder has no Agentmaster.exe.
    .PARAMETER dir
    The folder to read.
    .OUTPUTS
    A one-line text: version, install.path, profile.path, .portable, manifest entry count.
    #>
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'Agentmaster.exe'))) { return $null }
    # The decision files carry '#' comment lines above their one value line: show the value only.
    $read = { param($leaf) $p = Join-Path $dir $leaf; if (Test-Path -LiteralPath $p) { (Get-Content -LiteralPath $p -Encoding UTF8 | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith('#') } | Select-Object -First 1) } else { '-' } }
    $manifest = Join-Path $dir '.am-manifest'
    $count = if (Test-Path -LiteralPath $manifest) { (Get-Content -LiteralPath $manifest | Where-Object { $_ -and -not $_.StartsWith('#') }).Count } else { 0 }
    'ver={0} install.path={1} profile.path={2} portable={3} manifest={4}' -f (& $read '.am-version'), (& $read 'install.path'), (& $read 'profile.path'), (Test-Path -LiteralPath (Join-Path $dir '.portable')), $count
}

function Get-AmQaState {
    <#
    .SYNOPSIS
    Print every Agentmaster install, registration, shortcut, PATH entry, verb, profile and process.
    .DESCRIPTION
    Read-only. Covers the per-user installs of each QA account (the old script's copy in
    %LOCALAPPDATA%\Programs\Agentmaster too), the all-users install, the MSIX packages, HKCU (each
    loaded user hive) and HKLM entries with their display names, the user and machine PATH, the
    right-click verbs, the shortcuts on both desktops and both Start menus, each account's profile + its
    last [install] lines, and the running Agentmaster.exe processes with their account and token elevation.
    .OUTPUTS
    Text lines (one fact per line).
    #>
    "== installs"
    foreach ($a in $AmQaAccounts) {
        $d = "C:\Users\$a\.agentmaster\bin"
        $f = Get-AmQaInstallFacts $d
        if ($f) { "  per-user ${a}: $d  $f" }
        # Where Install-Agentmaster.ps1 v0.7.10 and older put its portable copy (no registry entry of its own).
        $old = "C:\Users\$a\AppData\Local\Programs\Agentmaster"
        $of = Get-AmQaInstallFacts $old
        if ($of) { "  old script copy ${a}: $old  $of" }
    }
    $m = Get-AmQaInstallFacts 'C:\Program Files\Agentmaster'
    if ($m) { "  all-users: C:\Program Files\Agentmaster  $m" }
    # The MSIX packages, every user's (the agent runs elevated).
    Get-AppxPackage -AllUsers -Name 'Agentmaster*' -ErrorAction SilentlyContinue | ForEach-Object {
        $users = ($_.PackageUserInformation | ForEach-Object { "$($_.UserSecurityId.Username)=$($_.InstallState)" }) -join ','
        "  package: $($_.PackageFullName) at $($_.InstallLocation) users=[$users]"
    }
    "== uninstall entries"
    # The all-users entry's own key (AgentmasterAllUsers since 2026-10-21) and the name it had before.
    foreach ($leaf in 'AgentmasterAllUsers', 'Agentmaster') {
        $hk = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$leaf"
        if (Test-Path $hk) { $e = Get-ItemProperty $hk; "  HKLM {0}: name='{1}' ver={2} loc={3} uninstall={4}" -f $leaf, $e.DisplayName, $e.DisplayVersion, $e.InstallLocation, $e.UninstallString }
    }
    foreach ($a in $AmQaAccounts) {
        $sid = try { (New-Object System.Security.Principal.NTAccount($a)).Translate([System.Security.Principal.SecurityIdentifier]).Value } catch { $null }
        if (-not $sid) { continue }
        $uk = "Registry::HKEY_USERS\$sid\Software\Microsoft\Windows\CurrentVersion\Uninstall\Agentmaster"
        if (Test-Path $uk) { $e = Get-ItemProperty $uk; "  HKCU ${a}: name='{0}' ver={1} loc={2}" -f $e.DisplayName, $e.DisplayVersion, $e.InstallLocation }
        $env = "Registry::HKEY_USERS\$sid\Environment"
        if (Test-Path $env) {
            $p = (Get-ItemProperty $env -ErrorAction SilentlyContinue).Path
            if ($p) { $p -split ';' | Where-Object { $_ -match 'agentmaster' } | ForEach-Object { "  user PATH ${a}: $_" } }
        }
        foreach ($k in 'Directory\shell\Agentmaster', 'Directory\Background\shell\Agentmaster', 'Drive\shell\Agentmaster') {
            $vk = "Registry::HKEY_USERS\$sid\Software\Classes\$k"
            if (Test-Path $vk) { "  verb HKCU ${a}: $k" }
        }
    }
    $mp = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $mp -split ';' | Where-Object { $_ -match 'agentmaster' } | ForEach-Object { "  machine PATH: $_" }
    foreach ($k in 'Directory\shell\Agentmaster', 'Directory\Background\shell\Agentmaster', 'Drive\shell\Agentmaster') {
        if (Test-Path "HKLM:\Software\Classes\$k") { "  verb HKLM: $k" }
    }
    "== shortcuts"
    $places = @('C:\Users\Public\Desktop', 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs')
    foreach ($a in $AmQaAccounts) { $places += "C:\Users\$a\Desktop"; $places += "C:\Users\$a\AppData\Roaming\Microsoft\Windows\Start Menu\Programs" }
    foreach ($pl in $places) {
        if (Test-Path -LiteralPath $pl) {
            Get-ChildItem -LiteralPath $pl -Filter 'Agentmaster*.lnk' -ErrorAction SilentlyContinue | ForEach-Object {
                $t = (New-Object -ComObject WScript.Shell).CreateShortcut($_.FullName).TargetPath
                "  $($_.FullName) -> $t"
            }
        }
    }
    "== profiles"
    foreach ($a in $AmQaAccounts) {
        $pd = "C:\Users\$a\.agentmaster"
        if (Test-Path -LiteralPath $pd) {
            $items = (Get-ChildItem -LiteralPath $pd -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ','
            "  ${a}: $pd [$items]"
            $hl = Join-Path $pd 'hooks.log'
            # hooks.log is UTF-8 without a BOM; 5.1's Get-Content would read it as ANSI (an em dash -> mojibake).
            if (Test-Path -LiteralPath $hl) { Get-Content -LiteralPath $hl -Tail 400 -Encoding UTF8 | Where-Object { $_ -match '\[install\]' } | Select-Object -Last 6 | ForEach-Object { "    $_" } }
        }
        $map = "C:\Users\$a\.agentmaster.profiles"
        if (Test-Path -LiteralPath $map) { "  ${a} home map: " + ((Get-Content -LiteralPath $map -Raw) -replace '\s+', ' ') }
    }
    "== processes"
    Get-CimInstance Win32_Process -Filter "Name='Agentmaster.exe' OR Name='WindowsTerminal.exe' OR Name='powershell.exe'" | ForEach-Object {
        # Only the app's own updater / uninstaller scripts; never this very query (its command line names Agentmaster too).
        if ($_.Name -eq 'powershell.exe' -and ($_.ProcessId -eq $PID -or $_.CommandLine -notmatch 'am-update|am-uninstall|Install-Agentmaster')) { return }
        $o = Invoke-CimMethod -InputObject $_ -MethodName GetOwner
        "  pid={0} {1} user={2} session={3} elevated={4} path={5}" -f $_.ProcessId, $_.Name, $o.User, $_.SessionId, [AmQaToken]::Elevated([int]$_.ProcessId), $_.ExecutablePath
    }
}

function Reset-AmQa {
    <#
    .SYNOPSIS
    Remove every Agentmaster install, registration, shortcut, PATH entry, profile and process.
    .DESCRIPTION
    For a disposable test desktop only: ends Agentmaster.exe / its am-update powershell, removes the
    MSIX packages (every user's), deletes the per-user installs, the old script's copies
    (%LOCALAPPDATA%\Programs\Agentmaster) and the profiles of the QA accounts, the all-users install,
    the HKLM / each loaded HKCU uninstall entry, the right-click verbs, the PATH entries and the
    shortcuts. The QA copies under C:\amqa stay (each scenario still starts from a FRESH copy of them).
    .OUTPUTS
    One line per removed item.
    #>
    Get-CimInstance Win32_Process -Filter "Name='Agentmaster.exe' OR Name='WindowsTerminal.exe'" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "ended pid $($_.ProcessId)" }
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -match 'am-update' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "ended updater pid $($_.ProcessId)" }
    Start-Sleep -Milliseconds 800
    # The MSIX packages, for every user (the agent runs elevated). Their profile data is removed below.
    Get-AppxPackage -AllUsers -Name 'Agentmaster*' -ErrorAction SilentlyContinue | ForEach-Object {
        try { Remove-AppxPackage -Package $_.PackageFullName -AllUsers -ErrorAction Stop; "removed package $($_.PackageFullName)" } catch { "package $($_.PackageFullName) NOT removed: $($_.Exception.Message)" }
    }
    foreach ($a in $AmQaAccounts) {
        foreach ($d in "C:\Users\$a\.agentmaster", "C:\Users\$a\.agentmaster-dev", "C:\Users\$a\AppData\Local\Programs\Agentmaster") {
            if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue; "removed $d" }
        }
        $map = "C:\Users\$a\.agentmaster.profiles"
        if (Test-Path -LiteralPath $map) { Remove-Item -LiteralPath $map -Force; "removed $map" }
        foreach ($pl in "C:\Users\$a\Desktop", "C:\Users\$a\AppData\Roaming\Microsoft\Windows\Start Menu\Programs") {
            Get-ChildItem -LiteralPath $pl -Filter 'Agentmaster*.lnk' -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force; "removed $($_.FullName)" }
        }
        $sid = try { (New-Object System.Security.Principal.NTAccount($a)).Translate([System.Security.Principal.SecurityIdentifier]).Value } catch { $null }
        if (-not $sid -or -not (Test-Path "Registry::HKEY_USERS\$sid")) { continue }
        foreach ($k in 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Agentmaster', 'Software\Classes\Directory\shell\Agentmaster', 'Software\Classes\Directory\Background\shell\Agentmaster', 'Software\Classes\Drive\shell\Agentmaster') {
            $rk = "Registry::HKEY_USERS\$sid\$k"
            if (Test-Path $rk) { Remove-Item $rk -Recurse -Force; "removed HKCU($a)\$k" }
        }
        $env = "Registry::HKEY_USERS\$sid\Environment"
        $p = (Get-ItemProperty $env -ErrorAction SilentlyContinue).Path
        if ($p -and $p -match 'agentmaster') {
            $kept = ($p -split ';' | Where-Object { $_ -and $_ -notmatch 'agentmaster' }) -join ';'
            Set-ItemProperty $env -Name Path -Value $kept; "cleaned user PATH ($a)"
        }
    }
    if (Test-Path 'C:\Program Files\Agentmaster') { Remove-Item 'C:\Program Files\Agentmaster' -Recurse -Force -ErrorAction SilentlyContinue; 'removed C:\Program Files\Agentmaster' }
    foreach ($k in 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AgentmasterAllUsers', 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Agentmaster', 'HKLM:\Software\Classes\Directory\shell\Agentmaster', 'HKLM:\Software\Classes\Directory\Background\shell\Agentmaster', 'HKLM:\Software\Classes\Drive\shell\Agentmaster') {
        if (Test-Path $k) { Remove-Item $k -Recurse -Force; "removed $k" }
    }
    $mp = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    if ($mp -match 'agentmaster') {
        [Environment]::SetEnvironmentVariable('Path', (($mp -split ';' | Where-Object { $_ -and $_ -notmatch 'agentmaster' }) -join ';'), 'Machine'); 'cleaned machine PATH'
    }
    foreach ($pl in 'C:\Users\Public\Desktop', 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs') {
        Get-ChildItem -LiteralPath $pl -Filter 'Agentmaster*.lnk' -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force; "removed $($_.FullName)" }
    }
    Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object { $_.TaskName -like 'Agentmaster*' -or $_.TaskName -like 'amqa-*' } | ForEach-Object { Unregister-ScheduledTask -TaskName $_.TaskName -Confirm:$false; "removed task $($_.TaskName)" }
}

function New-AmQaCopy([string]$version, [string]$name) {
    <#
    .SYNOPSIS
    A FRESH pristine copy of the QA build under C:\amqa\run\<name>, stamped with a version.
    .PARAMETER version
    The .am-version text, e.g. 0.8.0.0 (what the app reads as this copy's version).
    .PARAMETER name
    The copy's folder name.
    .OUTPUTS
    The copy's Agentmaster.exe path.
    #>
    $dst = "C:\amqa\run\$name"
    if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
    New-Item -ItemType Directory -Force -Path 'C:\amqa\run' | Out-Null
    Copy-Item -Recurse -Force -LiteralPath 'C:\amqa\build' -Destination $dst
    Set-Content -LiteralPath (Join-Path $dst '.am-version') -Value $version -Encoding Ascii
    # Every test account may run the copy (and a hand-over writes beside it as another user).
    icacls $dst /grant '*S-1-1-0:(OI)(CI)M' | Out-Null
    Join-Path $dst 'Agentmaster.exe'
}
