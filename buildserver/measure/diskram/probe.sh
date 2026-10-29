#!/bin/sh
# One sample of the experiment container, as key=value lines (read by sample.py): its state, its memory cgroup's
# resident and swapped bytes, the peak, the times it hit the cap, and how much of the swap file on Q: is in use.
NAME=amb-exp-diskram
export PODMAN_IGNORE_CGROUPSV1_WARNING=1
state=$(podman inspect -f '{{.State.Status}} oom={{.State.OOMKilled}}' "$NAME" 2>/dev/null || echo "gone")
echo "state=$state"
cg=$(podman inspect -f '{{.State.CgroupPath}}' "$NAME" 2>/dev/null)
dir=/sys/fs/cgroup/memory$cg
if [ -n "$cg" ] && [ -d "$dir" ]; then
  echo "mem=$(cat "$dir/memory.usage_in_bytes")"
  echo "memsw=$(cat "$dir/memory.memsw.usage_in_bytes")"
  echo "peak=$(cat "$dir/memory.max_usage_in_bytes")"
  echo "failcnt=$(cat "$dir/memory.failcnt")"
  awk '$1=="total_rss"||$1=="total_cache"||$1=="total_swap"||$1=="total_mapped_file"{print $1"="$2}' "$dir/memory.stat"
fi
awk '$1=="/srv/buildserver/swap/exp.swap"{print "swapfile_used_kb="$4}' /proc/swaps
awk '$1=="MemAvailable:"{print "wsl_avail_kb="$2}' /proc/meminfo
