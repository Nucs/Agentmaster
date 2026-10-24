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
      C:\e2e\venv\          a Python venv with the runner's packages (pytest, comtypes, psutil, pillow), filled by the
                            base Python's pip (the venv has no pip of its own since 2026-10-24).
      Defender exclusions   C:\e2e\pwsh, C:\e2e\venv and C:\e2e\git, the test tooling, are not scanned (the build under
                            test and every folder an install writes still are).
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

.PARAMETER Lane
    Standard (the default: everything above, for the suites that drive the app on a test profile) or Install (the
    install lane, doc/agentmaster/E2E_TESTING.md s.8.5: the scenarios of flows/19 that install Agentmaster FOR REAL).
    The install lane skips Claude Code, Git and the mock's user environment - no install scenario starts a session -
    and adds two steps: 'kit' (the build under test extracted to C:\e2e\kit\build WITH its `.portable` marker, the
    pristine portable copy every scenario copies and stamps) and 'lane' (C:\e2e\install-lane.json, the file whose
    presence lets an install test run at all, plus api.github.com in the hosts guard so the installed app's startup
    update check fails at once instead of spending GitHub's 60 requests an hour, which this PC shares with its
    desktops).

.PARAMETER Isolation
    The install lane's isolation, written into install-lane.json for the tests: Reset (the host resets the desktop
    before every scenario: the tests only check that the machine is clean), Clean (one desktop for every scenario:
    each test first removes every Agentmaster install with tools\install-qa\amqa-guest.ps1's Reset-AmQa, then checks)
    or None (nothing is removed: a scenario on a machine that is not clean fails at once). Ignored by the Standard lane.

.PARAMETER Accounts
    The install lane only: also make the other accounts the scenarios need (flows/19's Machine tags "std" and "adm2"):
    the STANDARD local user `qastd` and the ADMINISTRATOR `qaadmin`, with generated passwords kept in
    C:\e2e\qa-accounts.json readable by administrators only (the tests run as the signed-in administrator; the passwords
    never leave the guest). An existing account gets a new password when that file is missing. A disposable desktop's
    test accounts, not anybody's credentials.

.PARAMETER Old
    The install lane only: also extract an OLDER release (flows/19's Machine tag "old") to C:\e2e\kit\old, as
    published: a build from before the release manifest and `--register-user` (v0.7.10 by default). The scenarios make
    their zips of it with and without a manifest (amdriver.install.Kit.old_zip).

.PARAMETER OldZip
    A local path or an https URL of that older release's portable zip. Default: v0.7.10-prerelease's x64 zip.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File Z:\e2e\src\guest\provision.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File Z:\e2e\src\guest\provision.ps1 -AgentmasterZip Z:\e2e\builds\Agentmaster_0.8.1.0_x64.zip -Force
    powershell -NoProfile -ExecutionPolicy Bypass -File Z:\e2e\src\guest\provision.ps1 -Lane Install -Isolation Reset -Pwsh

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
    [switch]$Force,
    [ValidateSet('Standard', 'Install')]
    [string]$Lane = 'Standard',
    [ValidateSet('Reset', 'Clean', 'None')]
    [string]$Isolation = 'Reset',
    [switch]$Accounts,
    [switch]$Old,
    [string]$OldZip = 'https://github.com/Nucs/Agentmaster/releases/download/v0.7.10-prerelease/Agentmaster_0.7.10.0_x64.zip'
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
    # Each step's time goes on its line (not into the summary, which the host reads by its words): on a loaded PC a
    # reset desktop's provisioning took 7 min, and which step costs what decides what is worth making faster.
    $clock = [Diagnostics.Stopwatch]::StartNew()
    try {
        $outcome = & $body
        if (-not $outcome) { $outcome = 'ok' }
        Write-Output ("{0,-10} {1}  [{2:N1} s]" -f $name, $outcome, $clock.Elapsed.TotalSeconds)
        $summary[$name] = "$outcome"
    }
    catch {
        Write-Output ("{0,-10} FAILED: {1}  [{2:N1} s]" -f $name, $_.Exception.Message, $clock.Elapsed.TotalSeconds)
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

function Expand-Zip([string]$zip, [string]$dest) {
    <#
    .SYNOPSIS
        Extract a zip into a folder that holds no files yet, with .NET's ZipFile instead of Expand-Archive.
    .PARAMETER zip
        The zip file (a full path: .NET resolves a relative one against the process's folder, not PowerShell's).
    .PARAMETER dest
        The destination folder (a full path); made when missing. The callers remove what an earlier run left there
        first: ZipFile refuses to overwrite a file, where Expand-Archive -Force would have mixed two builds.
    .OUTPUTS
        None. Throws when the zip cannot be read, a file is in the way, or an entry would land outside the destination.
    .NOTES
        Windows PowerShell 5.1's Expand-Archive copies every entry in an interpreted PowerShell loop. Measured on a
        freshly reset test desktop on 2026-10-24, the PowerShell 7 zip (101 MB, 658 files) took 146 s with it inside a
        provisioning and 12.4 s with ZipFile (7.6 s with Defender scanning nothing) - from a copy on the local disk.
        Straight from the persist share it took 49.9 s: ZipFile reads in 4 KB pieces, each one a round trip to the host,
        while copying the whole zip took 0.6 s. So a zip that is not on the system drive is copied to %TEMP% first.
    #>
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $source = $zip
    $copy = $null
    if (-not $zip.StartsWith($env:SystemDrive, [StringComparison]::OrdinalIgnoreCase)) {
        $copy = Join-Path $env:TEMP ('e2e-' + [guid]::NewGuid().ToString('N') + '.zip')
        Copy-Item -LiteralPath $zip -Destination $copy
        $source = $copy
    }
    try {
        [IO.Compression.ZipFile]::ExtractToDirectory($source, $dest)
    }
    finally {
        # The copy goes whether the extraction worked or not (a failed one throws on to the step, which reports it).
        if ($copy) { Remove-Item -LiteralPath $copy -Force -ErrorAction SilentlyContinue }
    }
}

# How long Windows PowerShell took to start and reach this line. After a reset the first step began 85 s after
# provision.cmd started PowerShell (2026-10-24, a loaded PC); this line tells a slow start from a slow step. Not a step:
# the summary the host reads holds steps only.
$startedAt = (Get-Process -Id $PID).StartTime
Write-Output ("{0,-10} PowerShell {1} reached the script {2:N1} s after its process started" -f 'start', $PSVersionTable.PSVersion, ((Get-Date) - $startedAt).TotalSeconds)

New-Item -ItemType Directory -Force $Root, $Cache, "$Root\dumps", "$Root\out", "$Root\p", "$Root\c", "$Root\w" | Out-Null

$installLane = $Lane -eq 'Install'

function Get-BuildZip {
    <#
    .SYNOPSIS
        The local path of the Agentmaster zip under test (downloaded into the cache once when -AgentmasterZip is a URL).
    .OUTPUTS
        The zip's full path.
    .NOTES
        Throws when the zip is missing. Shared by the 'agentmaster' and the 'kit' steps, so both lanes test the same
        build from the same cached download.
    #>
    $zip = $AgentmasterZip
    if ($zip -match '^https?://') { $zip = Get-Cached $zip (Split-Path $zip -Leaf) }
    if (-not (Test-Path $zip)) { throw "no Agentmaster zip at $zip" }
    return (Get-Item $zip).FullName
}

function Skip-InLane([string]$name, [string]$why) {
    <#
    .SYNOPSIS
        Record a step the install lane leaves out, in the step's own output line and in the summary.
    .PARAMETER name
        The step's name (the summary key).
    .PARAMETER why
        Why the install lane needs nothing from it.
    .OUTPUTS
        None.
    #>
    $outcome = "skip (install lane): $why"
    Write-Output ("{0,-10} {1}" -f $name, $outcome)
    $summary[$name] = $outcome
}

# ---- Microsoft Defender: the test tooling unscanned ---------------------------------------------
# Only the TOOLING the runner and the mock use (PowerShell 7, the venv, Git). Defender scanned every file the extractions
# and pip wrote, most of a provisioning after a reset (measured 2026-10-24 on a freshly reset desktop: its engine spent
# 22 s of CPU on a 28 s pip install and 18 s on a 25 s `python -m venv`). The build under test (C:\e2e\am, C:\e2e\kit),
# the copies the scenarios start and every folder an install writes stay scanned, as on a user's PC. A desktop without
# Defender's cmdlets, or one that refuses the change, is only reported: the steps work either way, slower.
Step 'defender' {
    $want = @("$Root\pwsh", "$Root\venv", "$Root\git")
    try {
        $have = @((Get-MpPreference -ErrorAction Stop).ExclusionPath)
        $add = @($want | Where-Object { $have -notcontains $_ })
        if (-not $add) { return 'skip: the tooling folders are excluded' }
        Add-MpPreference -ExclusionPath $add -ErrorAction Stop
        return "excluded $($add -join ', ') from scanning"
    }
    catch {
        return "skip: not changed ($($_.Exception.Message))"
    }
}

# ---- Agentmaster ------------------------------------------------------------------------------
# The install lane never runs this copy (each install scenario copies the pristine kit instead), so it skips it.
if ($installLane) { Skip-InLane 'agentmaster' 'each scenario copies the kit instead' } else { Step 'agentmaster' {
    $zip = Get-BuildZip
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
    Expand-Zip $zip $tmp
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
} }

# ---- Claude Code ------------------------------------------------------------------------------
if ($installLane) { Skip-InLane 'claude' 'no install scenario starts a session' } else { Step 'claude' {
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
} }

# ---- Git (Claude Code's Bash tool) ------------------------------------------------------------
if ($installLane) { Skip-InLane 'git' 'no install scenario starts a session' } else { Step 'git' {
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
} }

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
        Expand-Zip $cached.FullName "$Root\pwsh"
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
        # Without a pip of its own: ensurepip installing pip into every new venv cost 25 s of a provisioning after a
        # reset (0.3 s without it, measured 2026-10-24). The base Python's pip fills it below (`pip --python`), and
        # nothing else ever runs pip in it; `python -m ensurepip` adds one by hand when wanted.
        $r = Invoke-Native $py @('-m', 'venv', '--without-pip', "$Root\venv")
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
    # The base Python's pip installs INTO the venv (`--python`, pip 22.3+; the golden image has pip 26.2): a venv
    # made before 2026-10-24 has a pip of its own, and this works for both. The wheel cache first, without the
    # network (a reset desktop's case, 5 s of PyPI questions saved); PyPI fills the cache only when it lacks something.
    $install = @('-m', 'pip', '--python', $vpy, 'install', '--quiet', '--disable-pip-version-check', '--no-index', '--find-links', $wheels) + $missing
    $r = Invoke-Native $py $install
    if ($r.Code -eq 0) { return "installed $($missing -join ', ') from the wheel cache" }
    $r = Invoke-Native $py (@('-m', 'pip', 'download', '--quiet', '--disable-pip-version-check', '--dest', $wheels) + $missing)
    if ($r.Code -ne 0) { Write-Output "  (pip download exit $($r.Code): $($r.Output); installing from the cache only)" }
    $r = Invoke-Native $py $install
    if ($r.Code -ne 0) { throw "pip install failed ($($r.Code)): $($r.Output)" }
    return "installed $($missing -join ', ')"
}

# ---- Leak guard (hosts file) ------------------------------------------------------------------
Step 'hosts' {
    $hostsFile = "$env:SystemRoot\System32\drivers\etc\hosts"
    $names = @('api.anthropic.com', 'statsig.anthropic.com', 'console.anthropic.com', 'mcp-proxy.anthropic.com')
    # The install lane: every installed copy checks GitHub for an update when it starts (Updater.h), and the
    # 60 unauthenticated API requests an hour are shared by this PC and its desktops. Refused at once, the check
    # ends quietly ("offline", flows/19 IS-88); a scenario that needs the real releases lifts the line itself.
    if ($installLane) { $names += 'api.github.com' }
    $text = Get-Content $hostsFile -Raw -ErrorAction SilentlyContinue
    if ($null -eq $text) { $text = '' }
    $add = @($names | Where-Object { $text -notmatch "(?m)^127\.0\.0\.1\s+$([regex]::Escape($_))\s*$" })
    if (-not $add) { return 'skip: present' }
    $lines = ($add | ForEach-Object { "127.0.0.1 $_" }) -join "`r`n"
    Add-Content -Path $hostsFile -Value "`r`n# agentmaster-e2e leak guard`r`n$lines" -Encoding ASCII
    return "guarded $($add -join ', ')"
}

# ---- User environment (registry) --------------------------------------------------------------
if ($installLane) { Skip-InLane 'userenv' 'the mock environment is for sessions' } else { Step 'userenv' {
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
} }

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

# ---- The install lane's kit and marker ----------------------------------------------------------
$laneFile = "$Root\install-lane.json"
if ($installLane) {
    Step 'kit' {
        $zip = Get-BuildZip
        $kit = "$Root\kit"
        $build = "$kit\build"
        $stamp = Join-Path $kit '.e2e-source'
        $want = "$zip|$((Get-Item $zip).Length)|$((Get-Item $zip).LastWriteTimeUtc.Ticks)"
        if ((Test-Path "$build\Agentmaster.exe") -and (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $want) -and -not $Force) {
            return "skip: $(Split-Path $zip -Leaf) already extracted"
        }
        # Never replace the kit under a copy that runs from it (its files are locked; a half-replaced kit is worse than none).
        $running = Get-Process -Name Agentmaster -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$kit\*" }
        if ($running) { throw "Agentmaster is running from $kit (pid $($running.Id -join ',')); close it first" }
        if (Test-Path $kit) { Remove-Item -Recurse -Force $kit }
        New-Item -ItemType Directory -Force $kit | Out-Null
        $tmp = "$kit\extract"
        Expand-Zip $zip $tmp
        # A release zip holds one top folder (agentmaster-<ver>\...); a hand-made zip may hold the files directly.
        $exe = Get-ChildItem $tmp -Recurse -Filter Agentmaster.exe | Select-Object -First 1
        if (-not $exe) { throw "no Agentmaster.exe inside $zip" }
        Move-Item $exe.Directory.FullName $build
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        # The marker is what makes a copy's first launch ask "Where should Agentmaster live?" - the very thing the
        # scenarios test - so the kit keeps it (the Standard lane removes it from C:\e2e\am). A zip made without
        # -PortableMode has none and cannot stand for a release.
        if (-not (Test-Path "$build\.portable")) { throw "the zip's copy has no .portable marker (made without New-UnpackagedTerminalDistribution.ps1 -PortableMode)" }
        # A downloaded zip's mark would bring SmartScreen to every copy a scenario starts through Explorer (IS-56 tests
        # the mark on purpose, on a zip of its own).
        Get-ChildItem $build -Recurse -File | Unblock-File
        Set-Content -Path $stamp -Value $want -Encoding ASCII
        $ver = if (Test-Path "$build\.am-version") { (Get-Content "$build\.am-version" -Raw).Trim() } else { 'unknown' }
        return "extracted $(Split-Path $zip -Leaf) to $build (version $ver)"
    }
    $accountsFile = "$Root\qa-accounts.json"
    if ($Accounts) {
        Step 'accounts' {
            # The Machine tags "std" (a standard user) and "adm2" (a second administrator), the install QA's own names.
            $plan = [ordered]@{ std = @{ name = 'qastd'; groups = @('S-1-5-32-545') }; adm2 = @{ name = 'qaadmin'; groups = @('S-1-5-32-545', 'S-1-5-32-544') } }
            $present = @($plan.Values | Where-Object { Get-LocalUser -Name $_.name -ErrorAction SilentlyContinue })
            if ($present.Count -eq $plan.Count -and (Test-Path $accountsFile) -and -not $Force) { return 'skip: qastd, qaadmin present' }
            $json = [ordered]@{}
            foreach ($tag in $plan.Keys) {
                $name = $plan[$tag].name
                # A generated password: 20 letters and digits from a cryptographic source, then one character of each
                # class a complexity rule could ask for. It exists only here (a disposable desktop's test account).
                $chars = [char[]]'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789'
                $bytes = New-Object byte[] 20
                [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
                $pw = (-join ($bytes | ForEach-Object { $chars[$_ % $chars.Length] })) + 'a7#Q'
                $secure = ConvertTo-SecureString $pw -AsPlainText -Force
                if (Get-LocalUser -Name $name -ErrorAction SilentlyContinue) {
                    Set-LocalUser -Name $name -Password $secure -PasswordNeverExpires $true
                } else {
                    New-LocalUser -Name $name -Password $secure -PasswordNeverExpires -AccountNeverExpires `
                        -Description "Agentmaster e2e install lane: the '$tag' account" | Out-Null
                }
                # Users (S-1-5-32-545) for both; Administrators (S-1-5-32-544) only for adm2. With UAC off (the
                # golden image), an administrator's start is elevated: "another administrator, elevated" (IS-52).
                foreach ($sid in $plan[$tag].groups) { Add-LocalGroupMember -SID $sid -Member $name -ErrorAction SilentlyContinue }
                $json[$tag] = [ordered]@{ name = $name; password = $pw }
            }
            [IO.File]::WriteAllText($accountsFile, ($json | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
            # Administrators and SYSTEM only: the tests run as the signed-in administrator, and no test account reads it.
            $null = icacls $accountsFile /inheritance:r /grant:r '*S-1-5-32-544:F' '*S-1-5-18:F'
            return "made qastd (standard) and qaadmin (administrator); their passwords in $accountsFile (administrators only)"
        }
    }
    $oldDir = "$Root\kit\old"
    if ($Old) {
        Step 'oldkit' {
            $zip = $OldZip
            if ($zip -match '^https?://') { $zip = Get-Cached $zip (Split-Path $zip -Leaf) }
            if (-not (Test-Path $zip)) { throw "no older Agentmaster zip at $zip" }
            $stamp = Join-Path $oldDir '.e2e-source'
            $want = "$((Get-Item $zip).FullName)|$((Get-Item $zip).Length)"
            if ((Test-Path "$oldDir\Agentmaster.exe") -and (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $want) -and -not $Force) {
                return "skip: $(Split-Path $zip -Leaf) already extracted"
            }
            if (Test-Path $oldDir) { Remove-Item -Recurse -Force $oldDir }
            $tmp = "$Root\kit\old.extract"
            if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
            Expand-Zip (Get-Item $zip).FullName $tmp
            $exe = Get-ChildItem $tmp -Recurse -Filter Agentmaster.exe | Select-Object -First 1
            if (-not $exe) { throw "no Agentmaster.exe inside $zip" }
            Move-Item $exe.Directory.FullName $oldDir
            if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
            # As published: what it lacks (a manifest, --register-user) is what the scenarios test, so nothing is added.
            Get-ChildItem $oldDir -Recurse -File | Unblock-File
            Set-Content -Path $stamp -Value $want -Encoding ASCII
            $ver = if (Test-Path "$oldDir\.am-version") { (Get-Content "$oldDir\.am-version" -Raw).Trim() } else { 'unknown' }
            return "extracted $(Split-Path $zip -Leaf) to $oldDir (version $ver, manifest $(Test-Path "$oldDir\.am-manifest"))"
        }
    }
    Step 'lane' {
        $build = "$Root\kit\build"
        $ver = if (Test-Path "$build\.am-version") { (Get-Content "$build\.am-version" -Raw).Trim() } else { '' }
        $pwshExe = if (Test-Path "$Root\pwsh\pwsh.exe") { "$Root\pwsh\pwsh.exe" } else { '' }
        $facts = [ordered]@{
            lane        = 'install'
            isolation   = $Isolation.ToLowerInvariant()
            kit         = "$Root\kit"
            build       = $build
            version     = $ver
            zip         = (Get-BuildZip)
            pwsh        = $pwshExe
            accounts    = $(if (Test-Path $accountsFile) { $accountsFile } else { '' })
            old         = $(if (Test-Path "$oldDir\Agentmaster.exe") { $oldDir } else { '' })
            provisioned = (Get-Date).ToString('o')
        }
        # UTF-8 without a BOM: Python's json reads it as is (5.1's -Encoding UTF8 would write a BOM).
        [IO.File]::WriteAllText($laneFile, ($facts | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
        return "wrote $laneFile (isolation $Isolation, build $ver)"
    }
}
else {
    # A desktop the install lane prepared and nobody reset since must not let a later Standard run start the install
    # scenarios (they install for real): the marker that admits them goes.
    Step 'lane' {
        if (-not (Test-Path $laneFile)) { return 'skip: no install lane marker' }
        Remove-Item -Force $laneFile
        return "removed $laneFile (the install scenarios stay deselected)"
    }
}

Write-Output ('PROVISION ' + ($summary | ConvertTo-Json -Compress))
exit 0
