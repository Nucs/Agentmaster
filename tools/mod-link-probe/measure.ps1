<#
.SYNOPSIS
  Measures what a loaded mod costs one claude.exe process: runs one headless Haiku turn that holds the
  process alive in a 20 s Bash sleep, samples the process's private bytes and working set every 500 ms,
  and prints the median over the steady window (6–18 s after start) plus the peaks.
.PARAMETER PluginDir
  Plugin folders to load, separated by ';', each passed as its own --plugin-dir; empty runs with no
  plugin at all.
.PARAMETER Label
  A name for this run, used in the output files and the summary line.
.PARAMETER OutDir
  Where the run's stdout/stderr and samples are written.
.PARAMETER Tcp
  The probe server's base URL, handed to the mod as AMP_TCP (unused when no plugin is loaded).
.NOTES
  Agentmaster's own variables are removed first, so the child is never mistaken for this tab's session,
  and --setting-sources project (from a folder with no settings) keeps every user hook and installed
  plugin out of the comparison.
#>
param([string]$PluginDir = "", [string]$Label, [string]$OutDir, [string]$Tcp = "")

foreach ($name in 'CCMGR_HOOK_PIPE', 'CCMGR_SESSION_ID', 'AM_SESSION', 'WT_SESSION', 'AGENTMASTER_PROFILE', 'CLAUDE_CODE_PLUGIN_DIRS') {
    Remove-Item "Env:$name" -ErrorAction Ignore
}
$env:AMP_TCP = $Tcp

$exe = Join-Path $env:USERPROFILE '.local\bin\claude.exe'
$argList = @('-p', '"Use the Bash tool to run exactly: sleep 20 . Then reply DONE."', '--model', 'haiku',
    '--setting-sources', 'project', '--no-session-persistence', '--allowedTools', 'Bash')
# One --plugin-dir per folder: the flag repeats, it never takes a list.
foreach ($dir in ($PluginDir -split ';' | Where-Object { $_ -ne '' })) { $argList += @('--plugin-dir', "`"$dir`"") }

$proc = Start-Process -FilePath $exe -ArgumentList $argList -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $OutDir "$Label.out.txt") -RedirectStandardError (Join-Path $OutDir "$Label.err.txt")
$t0 = Get-Date
$samples = New-Object System.Collections.Generic.List[object]
while (-not $proc.HasExited) {
    Start-Sleep -Milliseconds 500
    try {
        $proc.Refresh()
        $samples.Add([pscustomobject]@{ t = ((Get-Date) - $t0).TotalSeconds; ws = $proc.WorkingSet64; priv = $proc.PrivateMemorySize64; threads = $proc.Threads.Count })
    } catch {
        # The process can exit between HasExited and Refresh: that sample is simply skipped.
    }
}
$samples | Export-Csv -NoTypeInformation (Join-Path $OutDir "$Label.samples.csv")

$steady = @($samples | Where-Object { $_.t -ge 6 -and $_.t -le 18 })
function Median([double[]]$v) { $s = $v | Sort-Object; if ($s.Count -eq 0) { return 0 }; return $s[[int][math]::Floor($s.Count / 2)] }
$mb = 1MB
"{0,-10} n={1,3} privMed={2,6:N1} MB  wsMed={3,6:N1} MB  privPeak={4,6:N1} MB  wsPeak={5,6:N1} MB  threadsMed={6}  exit={7}  secs={8:N1}" -f `
    $Label, $steady.Count, ((Median ($steady | ForEach-Object priv)) / $mb), ((Median ($steady | ForEach-Object ws)) / $mb), `
    (($samples | Measure-Object priv -Maximum).Maximum / $mb), (($samples | Measure-Object ws -Maximum).Maximum / $mb), `
    (Median ($steady | ForEach-Object threads)), $proc.ExitCode, ((Get-Date) - $t0).TotalSeconds
