<#
.SYNOPSIS
    Prepare a claude-fleet Windows 11 test desktop for Agentmaster's end-to-end suite (idempotent).

.DESCRIPTION
    Runs INSIDE the guest (the desktop agent's run_command, as the signed-in administrator "Docker"; UAC is off
    there). Each step checks whether its result is already in place and skips itself, so a second run on the
    same machine costs seconds. Everything downloaded is cached in -Cache (normally Z:\e2e\cache, the desktop's
    persist share on the host), so a reset desktop provisions again without the network for the big files.

    What it puts in place (doc/agentmaster/E2E_TESTING.md s.5.2, Appendix A):
      C:\e2e\am\            the Agentmaster build under test (a portable zip, extracted). Its `.portable` marker
                            is REMOVED: a test always passes AGENTMASTER_PROFILE, and without the marker the copy
                            never asks where to install itself and never runs the startup update check.
      %USERPROFILE%\.local\bin\claude.exe
                            Claude Code at a pinned version, verified against the release manifest's SHA-256
                            (where Agentmaster's ResolveClaudeExe looks after PATH).
      C:\e2e\git\           PortableGit (Claude Code's Bash tool needs Git Bash on Windows).
      C:\e2e\pwsh\          PowerShell 7 (only with -Pwsh; without it managed sessions run under Windows
                            PowerShell 5.1 - Agentmaster's ResolvePwshLauncher fallback).
      C:\e2e\venv\          a Python venv with the runner's packages (pytest, comtypes, psutil, pillow).
      hosts file            api.anthropic.com and the telemetry hosts point at 127.0.0.1, so anything that
                            bypasses the mock's base URL fails at once instead of reaching the real API.
      user environment      the mock URL + a dummy token and the anti-update variables in HKCU\Environment, so
                            a claude started from a shell tab (which rebuilds its environment from the
                            registry) also reaches the mock and never replaces itself.
      WER LocalDumps        full dumps of Agentmaster.exe / OpenConsole.exe / claude.exe into C:\e2e\dumps.

    !! No real credential ever enters the guest: the token is a dummy, the mock listens on 127.0.0.1.

.PARAMETER Cache
    Folder for downloads (created when missing). Default Z:\e2e\cache.

.PARAMETER AgentmasterZip
    A local path or an https URL of the portable zip to test. Default: the v0.8.0 x64 release on GitHub.

.PARAMETER ClaudeVersion
    The Claude Code version to install (the pinned lane). Default 2.1.291 (the workstation's version on
    2026-10-23). "latest" or "stable" resolve through downloads.claude.ai.

.PARAMETER Pwsh
    Also install PowerShell 7 (the common user setup; Agentmaster prefers it for managed sessions).

.PARAMETER MockPort
    The port the mock will listen on (written into the user environment). Default 8787.

.PARAMETER Force
    Redo every step even when its result is in place (re-extract the build, reinstall claude.exe, ...).

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File Z:\e2e\src\guest\provision.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File Z:\e2e\src\guest\provision.ps1 -AgentmasterZip Z:\e2e\builds\Agentmaster_0.8.1.0_x64.zip -Force

.NOTES
    Exit code 0 when everything is in place; 1 on the first failed step (the error says which). Prints one
    line per step ("ok", "skip" or what it did) and a final JSON summary line starting with "PROVISION ".
    PowerShell 5.1 compatible (the guest's default shell).
#>
[CmdletBinding()]
param(
    [string]$Cache = 'Z:\e2e\cache',
    [string]$AgentmasterZip = 'https://github.com/Nucs/Agentmaster/releases/download/v0.8.0/Agentmaster_0.8.0.0_x64.zip',
    [string]$ClaudeVersion = '2.1.291',
    [switch]$Pwsh,
    [int]$MockPort = 8787,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest's progress bar makes 5.1 downloads 10x slower
Set-StrictMode -Version 2

$Root = 'C:\e2e'
$summary = [ordered]@{}

function Step([string]$name, [scriptblock]$body) {
    <#
    .SYNOPSIS
        Run one provisioning step, print its outcome and record it in the summary.
    .PARAMETER name
        The step's name (one word; the summary key).
    .PARAMETER body
        The step; it returns a short outcome text ("skip: ..." when nothing had to be done).
    .OUTPUTS
        None. A failure prints the step and rethrows, which ends the script with exit code 1 (the caller
        must know WHICH step failed, not just that something did).
    #>
    try {
        $outcome = & $body
        if (-not $outcome) { $outcome = 'ok' }
        Write-Output ("{0,-10} {1}" -f $name, $outcome)
        $summary[$name] = "$outcome"
    }
    catch {
        Write-Output ("{0,-10} FAILED: {1}" -f $name, $_.Exception.Message)
        $summary[$name] = "FAILED: $($_.Exception.Message)"
        Write-Output ('PROVISION ' + ($summary | ConvertTo-Json -Compress))
        exit 1
    }
}

function Invoke-Native([string]$exe, [string[]]$arguments) {
    <#
    .SYNOPSIS
        Run a native program and return its exit code and output, without PowerShell 5.1 turning its stderr
        into terminating errors.
    .PARAMETER exe
        The program.
    .PARAMETER arguments
        Its arguments.
    .OUTPUTS
        A [pscustomobject] with Code (the exit code) and Output (stdout + stderr lines as one string).
    .NOTES
        Under $ErrorActionPreference = 'Stop', Windows PowerShell 5.1 raises a NativeCommandError for the first
        line a native program writes to stderr once stderr is redirected (pip's "[notice] A new release of
        pip" line ended the venv step with an EMPTY message on 2026-10-23). The preference is lowered for the
        call only, and the exit code decides.
    #>
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $exe @arguments 2>&1 | ForEach-Object { "$_" }
        return [pscustomobject]@{ Code = $LASTEXITCODE; Output = ($out -join "`n") }
    }
    finally {
        $ErrorActionPreference = $saved
    }
}

function Get-Cached([string]$url, [string]$leaf) {
    <#
    .SYNOPSIS
        Download a URL into the cache once; return the cached file's path.
    .PARAMETER url
        The https URL.
    .PARAMETER leaf
        The cache file's name.
    .OUTPUTS
        The full path of the cached file.
    .NOTES
        Downloads to a .part file first and renames it, so an interrupted download never leaves a truncated
        file that a later run would take for complete.
    #>
    $dest = Join-Path $Cache $leaf
    if ((Test-Path $dest) -and -not $Force) { return $dest }
    $part = "$dest.part"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $part
    Move-Item -Force $part $dest
    return $dest
}

New-Item -ItemType Directory -Force $Root, $Cache, "$Root\dumps", "$Root\out", "$Root\p", "$Root\c", "$Root\w" | Out-Null

# ---- Agentmaster ------------------------------------------------------------------------------
Step 'agentmaster' {
    $zip = $AgentmasterZip
    if ($zip -match '^https?://') { $zip = Get-Cached $zip (Split-Path $zip -Leaf) }
    if (-not (Test-Path $zip)) { throw "no Agentmaster zip at $zip" }
    $stamp = Join-Path "$Root\am" '.e2e-source'
    $want = "$((Get-Item $zip).FullName)|$((Get-Item $zip).Length)|$((Get-Item $zip).LastWriteTimeUtc.Ticks)"
    if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $want) -and -not $Force) {
        return "skip: $(Split-Path $zip -Leaf) already extracted"
    }
    # Never extract over a running build (its files are locked, and a half-replaced build is worse than none).
    $running = Get-Process -Name Agentmaster -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Root\am\*" }
    if ($running) { throw "Agentmaster is running from $Root\am (pid $($running.Id -join ',')); close it first" }
    if (Test-Path "$Root\am") { Remove-Item -Recurse -Force "$Root\am" }
    $tmp = "$Root\am.extract"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive -Path $zip -DestinationPath $tmp -Force
    # A release zip holds one top folder (agentmaster-<ver>\...); a hand-made zip may hold the files directly.
    $exe = Get-ChildItem $tmp -Recurse -Filter Agentmaster.exe | Select-Object -First 1
    if (-not $exe) { throw "no Agentmaster.exe inside $zip" }
    Move-Item $exe.Directory.FullName "$Root\am"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    # The portable marker makes a copy ask where to install itself and check GitHub for updates at start;
    # a test always passes AGENTMASTER_PROFILE, so without the marker it simply runs (PROFILES.md s.2b).
    Remove-Item -Force -ErrorAction SilentlyContinue "$Root\am\.portable"
    # The download mark would make Windows prompt for helpers started through the shell.
    Get-ChildItem "$Root\am" -Recurse -File | Unblock-File
    Set-Content -Path $stamp -Value $want -Encoding ASCII
    $ver = if (Test-Path "$Root\am\.am-version") { (Get-Content "$Root\am\.am-version" -Raw).Trim() } else { 'unknown' }
    return "extracted $(Split-Path $zip -Leaf) (version $ver)"
}

# ---- Claude Code ------------------------------------------------------------------------------
Step 'claude' {
    $base = 'https://downloads.claude.ai/claude-code-releases'
    $ver = $ClaudeVersion
    if ($ver -in @('latest', 'stable')) { $ver = (Invoke-RestMethod "$base/$ver").ToString().Trim() }
    $manifest = Invoke-RestMethod "$base/$ver/manifest.json"
    $entry = $manifest.platforms.'win32-x64'
    if (-not $entry) { throw "the $ver manifest has no win32-x64 entry" }
    $binDir = Join-Path $env:USERPROFILE '.local\bin'
    $dest = Join-Path $binDir 'claude.exe'
    New-Item -ItemType Directory -Force $binDir | Out-Null
    if ((Test-Path $dest) -and -not $Force) {
        $have = (Get-FileHash $dest -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($have -eq $entry.checksum) { return "skip: $ver already installed" }
    }
    $file = Get-Cached "$base/$ver/win32-x64/claude.exe" "claude-$ver.exe"
    $hash = (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $entry.checksum) {
        Remove-Item -Force $file
        throw "claude.exe $ver checksum mismatch ($hash, the manifest says $($entry.checksum)); the cached copy was deleted"
    }
    Copy-Item -Force $file $dest
    return "installed $ver"
}

# ---- Git (Claude Code's Bash tool) ------------------------------------------------------------
Step 'git' {
    if ((Test-Path "$Root\git\bin\bash.exe") -and -not $Force) { return 'skip: present' }
    $cached = Get-ChildItem $Cache -Filter 'PortableGit-*-64-bit.7z.exe' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $cached) {
        $rel = Invoke-RestMethod 'https://api.github.com/repos/git-for-windows/git/releases/latest' -Headers @{ 'User-Agent' = 'agentmaster-e2e' }
        $asset = $rel.assets | Where-Object { $_.name -match '^PortableGit-[\d.]+-64-bit\.7z\.exe$' } | Select-Object -First 1
        if (-not $asset) { throw 'no PortableGit asset in the latest git-for-windows release' }
        $cached = Get-Item (Get-Cached $asset.browser_download_url $asset.name)
    }
    if (Test-Path "$Root\git") { Remove-Item -Recurse -Force "$Root\git" }
    # Run the self-extractor from the LOCAL disk: started from the network share (Z:) Windows shows its
    # "Open File - Security Warning" dialog for a program on a network location, and a hidden window then
    # waits for a click forever (measured 2026-10-23: no extraction in 120 s, nothing on screen to answer).
    $local = Join-Path $env:TEMP $cached.Name
    Copy-Item -Force $cached.FullName $local
    # A 7-Zip self-extractor: -o<dir> -y, no space after -o.
    $p = Start-Process -FilePath $local -ArgumentList @("-o$Root\git", '-y') -Wait -PassThru -WindowStyle Hidden
    Remove-Item -Force -ErrorAction SilentlyContinue $local
    if ($p.ExitCode -ne 0 -or -not (Test-Path "$Root\git\bin\bash.exe")) { throw "PortableGit extraction failed (exit $($p.ExitCode))" }
    return "extracted $($cached.Name)"
}

# ---- PowerShell 7 (optional) ------------------------------------------------------------------
if ($Pwsh) {
    Step 'pwsh' {
        if ((Test-Path "$Root\pwsh\pwsh.exe") -and -not $Force) { return 'skip: present' }
        $cached = Get-ChildItem $Cache -Filter 'PowerShell-*-win-x64.zip' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | Select-Object -First 1
        if (-not $cached) {
            $rel = Invoke-RestMethod 'https://api.github.com/repos/PowerShell/PowerShell/releases/latest' -Headers @{ 'User-Agent' = 'agentmaster-e2e' }
            $asset = $rel.assets | Where-Object { $_.name -match '^PowerShell-[\d.]+-win-x64\.zip$' } | Select-Object -First 1
            if (-not $asset) { throw 'no win-x64 zip in the latest PowerShell release' }
            $cached = Get-Item (Get-Cached $asset.browser_download_url $asset.name)
        }
        if (Test-Path "$Root\pwsh") { Remove-Item -Recurse -Force "$Root\pwsh" }
        Expand-Archive -Path $cached.FullName -DestinationPath "$Root\pwsh" -Force
        return "extracted $($cached.Name)"
    }
}

# ---- Python venv ------------------------------------------------------------------------------
Step 'venv' {
    $py = 'C:\Program Files\Python313\python.exe'
    if (-not (Test-Path $py)) { throw "no Python 3.13 at $py (the golden image has it)" }
    $vpy = "$Root\venv\Scripts\python.exe"
    if (-not (Test-Path $vpy) -or $Force) {
        if (Test-Path "$Root\venv") { Remove-Item -Recurse -Force "$Root\venv" }
        $r = Invoke-Native $py @('-m', 'venv', "$Root\venv")
        if ($r.Code -ne 0) { throw "python -m venv failed ($($r.Code)): $($r.Output)" }
    }
    $need = @('pytest', 'comtypes', 'psutil', 'pillow')
    $missing = @()
    foreach ($pkg in $need) {
        $module = if ($pkg -eq 'pillow') { 'PIL' } else { $pkg }
        $r = Invoke-Native $vpy @('-c', "import importlib.util,sys; sys.exit(0 if importlib.util.find_spec('$module') else 1)")
        if ($r.Code -ne 0) { $missing += $pkg }
    }
    if (-not $missing) { return 'skip: packages present' }
    $wheels = Join-Path $Cache 'wheels'
    New-Item -ItemType Directory -Force $wheels | Out-Null
    # Fill the wheel cache from PyPI (a no-op for wheels already there), then install from it, so a reset
    # desktop installs without the network.
    $r = Invoke-Native $vpy (@('-m', 'pip', 'download', '--quiet', '--disable-pip-version-check', '--dest', $wheels) + $missing)
    if ($r.Code -ne 0) { Write-Output "  (pip download exit $($r.Code): $($r.Output); installing from the cache only)" }
    $r = Invoke-Native $vpy (@('-m', 'pip', 'install', '--quiet', '--disable-pip-version-check', '--no-index', '--find-links', $wheels) + $missing)
    if ($r.Code -ne 0) { throw "pip install failed ($($r.Code)): $($r.Output)" }
    return "installed $($missing -join ', ')"
}

# ---- Leak guard (hosts file) ------------------------------------------------------------------
Step 'hosts' {
    $hostsFile = "$env:SystemRoot\System32\drivers\etc\hosts"
    $names = @('api.anthropic.com', 'statsig.anthropic.com', 'console.anthropic.com', 'mcp-proxy.anthropic.com')
    $text = Get-Content $hostsFile -Raw -ErrorAction SilentlyContinue
    if ($null -eq $text) { $text = '' }
    $add = @($names | Where-Object { $text -notmatch "(?m)^127\.0\.0\.1\s+$([regex]::Escape($_))\s*$" })
    if (-not $add) { return 'skip: present' }
    $lines = ($add | ForEach-Object { "127.0.0.1 $_" }) -join "`r`n"
    Add-Content -Path $hostsFile -Value "`r`n# agentmaster-e2e leak guard`r`n$lines" -Encoding ASCII
    return "guarded $($add -join ', ')"
}

# ---- User environment (registry) --------------------------------------------------------------
Step 'userenv' {
    $vars = [ordered]@{
        ANTHROPIC_BASE_URL                       = "http://127.0.0.1:$MockPort"
        ANTHROPIC_AUTH_TOKEN                     = 'mock-token-not-a-secret'
        DISABLE_AUTOUPDATER                      = '1'
        CLAUDE_CODE_SKIP_AUTO_UPDATE             = '1'
        CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC = '1'
        CLAUDE_CODE_GIT_BASH_PATH                = "$Root\git\bin\bash.exe"
        CLAUDE_CODE_RESUME_INTERRUPTED_TURN      = '1'
    }
    $changed = @()
    foreach ($k in $vars.Keys) {
        if ([Environment]::GetEnvironmentVariable($k, 'User') -ne $vars[$k]) {
            [Environment]::SetEnvironmentVariable($k, $vars[$k], 'User')
            $changed += $k
        }
    }
    # PATH: Git's cmd folder (a shell tab's `git`), and PowerShell 7 when installed (Agentmaster prefers pwsh).
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ($null -eq $userPath) { $userPath = '' }
    $want = @("$Root\git\cmd")
    if ($Pwsh) { $want += "$Root\pwsh" }
    $parts = @($userPath -split ';' | Where-Object { $_ })
    $newParts = @($want | Where-Object { $parts -notcontains $_ }) + $parts
    if ($newParts.Count -ne $parts.Count) {
        [Environment]::SetEnvironmentVariable('Path', ($newParts -join ';'), 'User')
        $changed += 'Path'
    }
    if (-not $changed) { return 'skip: set' }
    return "set $($changed -join ', ')"
}

# ---- Crash dumps ------------------------------------------------------------------------------
Step 'dumps' {
    $key = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps'
    $done = @()
    foreach ($exe in @('Agentmaster.exe', 'OpenConsole.exe', 'claude.exe')) {
        $k = Join-Path $key $exe
        if (-not (Test-Path $k)) { New-Item -Force $k | Out-Null; $done += $exe }
        Set-ItemProperty $k -Name DumpFolder -Value "$Root\dumps" -Type ExpandString
        Set-ItemProperty $k -Name DumpType -Value 2 -Type DWord
        Set-ItemProperty $k -Name DumpCount -Value 5 -Type DWord
    }
    if (-not $done) { return 'skip: set' }
    return "set for $($done -join ', ')"
}

Write-Output ('PROVISION ' + ($summary | ConvertTo-Json -Compress))
exit 0
