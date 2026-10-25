#!/bin/sh
# Disk benchmark for the build server's E2 decision (where a VM's disks live).
# Runs one fio set on each mount and prints one line per test: read/write IOPS and KB/s.
#   /vol    a Docker named volume (inside docker_data.vhdx on C:)
#   /qbind  a bind mount of the dedicated folder Q:\buildserver\_measure (Docker Desktop's Windows file sharing)
#   /cbind  a bind mount of C:\buildserver-bench (the same sharing path, on C:)
# direct=1 is how QEMU opens a disk with dockur's default DISK_CACHE=none / DISK_IO=native (O_DIRECT);
# direct=0 goes through the page cache. A mount that refuses O_DIRECT shows an ERROR line for direct=1.
# Each test file is 2 GB and is deleted when its mount is done.
apk add --no-cache fio >/dev/null 2>&1 || { echo "fio install failed"; exit 1; }
for m in /vol /qbind /cbind; do
  for t in "randwrite 4k 32" "randread 4k 32" "write 1m 4" "read 1m 4" "randwrite 64k 8"; do
    set -- $t
    for d in 1 0; do
      out=$(fio --name=t --filename=$m/fio.dat --size=2g --rw=$1 --bs=$2 --iodepth=$3 --ioengine=libaio --direct=$d \
        --runtime=15 --time_based --group_reporting --output-format=terse 2>&1 | tail -1)
      case "$out" in
        *";"*) r_iops=$(echo "$out" | cut -d';' -f8); r_bw=$(echo "$out" | cut -d';' -f7)
               w_iops=$(echo "$out" | cut -d';' -f49); w_bw=$(echo "$out" | cut -d';' -f48)
               echo "$m $1 bs=$2 qd=$3 direct=$d read_iops=$r_iops read_KBps=$r_bw write_iops=$w_iops write_KBps=$w_bw" ;;
        *) echo "$m $1 bs=$2 qd=$3 direct=$d ERROR: $(echo "$out" | head -c 160)" ;;
      esac
    done
  done
  rm -f $m/fio.dat
done
