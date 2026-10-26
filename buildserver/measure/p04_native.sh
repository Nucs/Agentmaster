#!/usr/bin/env bash
# P0.4's native reference (DESIGN.md §16.12): the build VM's build of a commit, repeated on this PC with the same
# commit, the same wrapper and the same parallelism, so the two times compare (W4: above 2x native, builds would take the
# host lane). It builds from a private clone on Q:, never from the shared tree, and holds the build lock for the whole
# build, since a full build competes with every other build on this PC.
#
# The clone of 2026-10-26, as found after the runs (nothing of it is on K:; it only read the repository there):
#   - /q/tmp/p04-native/src: a clone of 'K:\source\Agentmaster' (origin), core.longpaths true; checked out at 5f5d53fa9
#     for the cold build, then at ea28331ae for the incremental one (its reflog: clone, checkout, checkout);
#   - refs/claude-agent-sdk-cpp: a clone of 'K:\source\Agentmaster\.git\modules\refs\claude-agent-sdk-cpp' at the
#     commits' gitlink 9fd287244 (the superproject never ran `submodule init`; the build needs only the files);
#   - .PowershellModules\VSSetup 2.2.16 copied in from the repository (it is gitignored; the build VM's checkout step
#     copies it the same way, else Build-Agentmaster.ps1 downloads it from the PowerShell Gallery).
#
# The build's flags are the build VM's at 8.8 GB (BuildJob.Parallelism: /m:2, CL_MPCount 2); TEMP is on Q: and msbuild's
# node reuse is off, as in buildserver/steps/build/build.ps1. Times come from the wrapper's own "Build OK in N s" line,
# the same line the VM's build log carries.
#
# Usage: bash p04_native.sh <label>     (cold, incr, ...: names the log /q/tmp/p04-native/build-<label>.log)
# Exit code: 0 always once the lock was taken (the build's own exit code is printed); 3 when the lock was not taken.
set -u
label="$1"
cd /k/source/Agentmaster
TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "p04-native $label") || { echo "no lock"; exit 3; }
mkdir -p /q/tmp/p04-native/tmp
start=$(date +%s)
MSBUILDDISABLENODEREUSE=1 TEMP='Q:\tmp\p04-native\tmp' TMP='Q:\tmp\p04-native\tmp' \
  pwsh -NoProfile -ExecutionPolicy Bypass -File 'Q:\tmp\p04-native\src\tools\Build-Agentmaster.ps1' \
  -Configuration Release -MaxCpu 2 -ClMpCount 2 -LayoutDir 'Q:\tmp\p04-native\layout' \
  > "/q/tmp/p04-native/build-$label.log" 2>&1
code=$?
end=$(date +%s)
# Released before the summary, so a failing grep can never keep the lock (a cold build outlives its 30-min TTL anyway:
# refresh it from another shell with `am-lock.sh refresh --token <token> --ttl-min 120`).
bash tools/am-lock.sh release --token "$TOKEN"
echo "native $label: exit $code in $((end - start)) s"
grep -a -E "Build OK in|Build FAILED|error" "/q/tmp/p04-native/build-$label.log" | tail -5
