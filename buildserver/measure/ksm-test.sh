#!/bin/sh
# KSM measurement for the build server: how much host memory does page merging take back from idle Windows VMs?
#   sh ksm-test.sh <seconds> <qemu-pid> [<qemu-pid> ...]
# Run as root in a WSL distro: KSM is one switch for the whole WSL VM (Docker Desktop's containers included). The
# script records the starting settings, turns KSM on with zero-page merging and a fast scan (2,000 pages every
# 20 ms, ~400 MB/s), prints every 15 s the VM's MemAvailable, KSM's counters, ksmd's CPU time and each QEMU's RSS,
# and at the end restores the starting settings with run=2, which unmerges every page (the state before the test).
# QEMU marks guest RAM mergeable by default (mem-merge=on); dockur does not turn that off.
set -e
seconds="$1"; shift
k=/sys/kernel/mm/ksm
old_run=$(cat $k/run); old_scan=$(cat $k/pages_to_scan); old_sleep=$(cat $k/sleep_millisecs); old_zero=$(cat $k/use_zero_pages)
# restore: puts KSM back as the test found it, merged pages included (run=2 unmerges them all). Takes no arguments,
# returns nothing; every write is best effort (|| true), so a refused value never skips the ones after it. Runs on
# every exit through the trap, including Ctrl+C and a failed step.
restore() {
  # run=2 first (unmerge everything; it is refused while run=1 is changing, so stop scanning in between).
  echo 2 > $k/run || true
  echo "$old_zero" > $k/use_zero_pages || true
  echo "$old_scan" > $k/pages_to_scan || true
  echo "$old_sleep" > $k/sleep_millisecs || true
  echo "$old_run" > $k/run || true
  echo "restored: run=$(cat $k/run) pages_to_scan=$(cat $k/pages_to_scan) sleep=$(cat $k/sleep_millisecs) use_zero_pages=$(cat $k/use_zero_pages)"
}
trap restore EXIT
# report <t> <pid>...: prints one line for the elapsed time <t>: the VM's MemAvailable, KSM's counters (pages that
# share a page, pages shared, finished scans), ksmd's CPU seconds so far, and the RSS of each QEMU pid given. Returns
# nothing; a pid that has exited prints an empty RSS instead of failing.
report() {
  t="$1"; shift
  avail=$(awk '/MemAvailable/{printf "%.2f", $2/1048576}' /proc/meminfo)
  ksmd=$(pgrep -x ksmd | head -1)
  cpu=$(awk '{print ($14+$15)/100}' /proc/$ksmd/stat 2>/dev/null)
  rss=""
  for p in "$@"; do rss="$rss $(awk '/VmRSS/{printf "%.2f", $2/1048576}' /proc/$p/status 2>/dev/null)G"; done
  echo "t=${t}s avail=${avail}G sharing=$(cat $k/pages_sharing) shared=$(cat $k/pages_shared) full_scans=$(cat $k/full_scans) ksmd_cpu=${cpu}s qemu_rss:$rss"
}
start=$(date +%s)
pids="$*"
report 0 $pids
echo 1 > $k/use_zero_pages; echo 2000 > $k/pages_to_scan; echo 20 > $k/sleep_millisecs; echo 1 > $k/run
while [ $(( $(date +%s) - start )) -lt "$seconds" ]; do
  sleep 15
  report $(( $(date +%s) - start )) $pids
done
