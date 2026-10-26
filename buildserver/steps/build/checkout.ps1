<#
.SYNOPSIS
    Puts a commit into the build VM's worktree from the git bundle the coordinator wrote into the share; a step of every
    build (DESIGN.md §11).

.DESCRIPTION
    The worktree (D:\src\<configuration>-<identity>) is a plain clone kept on the VM's data disk, so the next build of
    the same worktree checks out only the files that changed and msbuild rebuilds only what depends on them (a fresh
    copy of the tree would give every file a new time and rebuild everything).

    1. The repository: unless -Bundle is empty (the clone holds the commit already), the bundle is verified first; one
       whose prerequisite commits the clone lacks (an incremental bundle for a clone that was made anew) exits 3, and the
       coordinator sends the whole history instead. Then the bundle's ref is fetched and the commit checked out,
       detached and forced (the tree's own build outputs are ignored files, which a checkout leaves alone).
    2. The submodule (refs\claude-agent-sdk-cpp): its own clone inside the worktree, fed from its bundle the same way,
       checked out at the commit the superproject pins.
    3. The VSSetup module: tools\OpenConsole.psm1 finds Build Tools with it and would otherwise download it from the
       PowerShell Gallery into the clone's .PowershellModules at the first build; the am-build image carries a copy.

.PARAMETER Worktree
    The worktree's folder.

.PARAMETER Commit
    The commit to check out (a full SHA).

.PARAMETER Bundle
    The repository's bundle (Z:\src\agentmaster.bundle), or '' when the clone holds the commit already.

.PARAMETER BundleRef
    The ref the bundle carries.

.PARAMETER SdkCommit
    The submodule's pinned commit, or '' when the commit has no submodule.

.PARAMETER SdkBundle
    The submodule's bundle, or '' when its clone holds the commit already.

.PARAMETER SdkRef
    The ref the submodule's bundle carries.

.OUTPUTS
    One line per part, then "CHECKOUT {json}".

.NOTES
    PowerShell 7. Exit codes: 0 checked out; 1 a git command failed; 2 HEAD is not the requested commit afterwards;
    3 the bundle needs commits the clone lacks (the coordinator retries with a full bundle).
#>
param(
    [Parameter(Mandatory)][string]$Worktree,
    [Parameter(Mandatory)][string]$Commit,
    [string]$Bundle = '',
    [string]$BundleRef = '',
    [string]$SdkCommit = '',
    [string]$SdkBundle = '',
    [string]$SdkRef = ''
)
$ErrorActionPreference = 'Stop'
$git = "$env:ProgramFiles\Git\cmd\git.exe"
$watch = [Diagnostics.Stopwatch]::StartNew()

function Invoke-Git {
    <#
    .SYNOPSIS
        Runs git in a repository and stops the script with exit 1 when it fails.
    .PARAMETER Repo
        The repository's folder (git's -C).
    .PARAMETER GitArgs
        git's arguments.
    .OUTPUTS
        git's standard output.
    #>
    param([string]$Repo, [string[]]$GitArgs)
    $out = & $git -C $Repo @GitArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Output "git $($GitArgs -join ' ') failed ($LASTEXITCODE): $out"
        exit 1
    }
    return $out
}

function Update-Clone {
    <#
    .SYNOPSIS
        Fetches a bundle's ref into a clone (made when missing) and checks a commit out, detached.
    .PARAMETER Repo
        The clone's folder.
    .PARAMETER BundlePath
        The bundle, or '' when the clone holds the commit already.
    .PARAMETER Ref
        The ref the bundle carries.
    .PARAMETER Sha
        The commit to check out.
    .OUTPUTS
        None; exits 3 when the bundle needs commits the clone lacks.
    #>
    param([string]$Repo, [string]$BundlePath, [string]$Ref, [string]$Sha)
    if (-not (Test-Path (Join-Path $Repo '.git'))) {
        New-Item -ItemType Directory -Force -Path $Repo | Out-Null
        Invoke-Git $Repo @('init', '-q') | Out-Null
    }
    if ($BundlePath) {
        # A clone made anew (a new data disk) cannot take an incremental bundle: say so with exit 3.
        & $git -C $Repo bundle verify -q $BundlePath 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Output "the bundle $BundlePath needs commits $Repo lacks"
            exit 3
        }
        Invoke-Git $Repo @('fetch', '-q', '--no-tags', $BundlePath, "+${Ref}:refs/amb/build") | Out-Null
    }
    Invoke-Git $Repo @('checkout', '-q', '--force', '--detach', $Sha) | Out-Null
}

Update-Clone -Repo $Worktree -BundlePath $Bundle -Ref $BundleRef -Sha $Commit
$head = (Invoke-Git $Worktree @('rev-parse', 'HEAD')).Trim()
if ($head -ne $Commit) {
    Write-Output "HEAD is $head, not $Commit"
    exit 2
}
Write-Output "repository at $head ($([math]::Round($watch.Elapsed.TotalSeconds, 1)) s)"

$sdkHead = ''
if ($SdkCommit) {
    $sdkDir = Join-Path $Worktree 'refs\claude-agent-sdk-cpp'
    Update-Clone -Repo $sdkDir -BundlePath $SdkBundle -Ref $SdkRef -Sha $SdkCommit
    $sdkHead = (Invoke-Git $sdkDir @('rev-parse', 'HEAD')).Trim()
    if ($sdkHead -ne $SdkCommit) {
        Write-Output "the submodule is at $sdkHead, not $SdkCommit"
        exit 2
    }
    Write-Output "submodule at $sdkHead"
}

$modules = Join-Path $Worktree '.PowershellModules'
if (-not (Test-Path (Join-Path $modules 'VSSetup'))) {
    New-Item -ItemType Directory -Force -Path $modules | Out-Null
    Copy-Item -Recurse -Force 'C:\BuildServer\psmodules\VSSetup' $modules
}

$facts = [ordered]@{
    commit    = $head
    sdk       = $sdkHead
    bundle    = [bool]$Bundle
    seconds   = [math]::Round($watch.Elapsed.TotalSeconds, 1)
    freeD_GB  = [math]::Round((Get-PSDrive D).Free / 1GB, 1)
}
Write-Output ('CHECKOUT ' + ($facts | ConvertTo-Json -Compress))
exit 0
