#!/bin/sh
# Disk benchmark of ONE folder for the build server's E2 decision, inside an Alpine container:
#   sh fio-one.sh <folder> [size]
# Four fio tests with O_DIRECT (how QEMU opens a disk with dockur's DISK_CACHE=none): random 4K writes and reads at
# queue depth 32, sequential 1M writes and reads at depth 4, 10 s each, on one test file of <size> (default 256m),
# deleted at the end. Prints one line per test: read/write IOPS and KB/s (fio's terse fields 8/7 and 49/48).
dir="$1"
size="${2:-256m}"
[ -d "$dir" ] || { echo "no such folder: $dir"; exit 2; }
apk add --no-cache fio >/dev/null 2>&1 || { echo "fio install failed"; exit 1; }
for t in "randwrite 4k 32" "randread 4k 32" "write 1m 4" "read 1m 4"; do
  set -- $t
  out=$(fio --name=t --filename="$dir/fio.dat" --size="$size" --rw="$1" --bs="$2" --iodepth="$3" --ioengine=libaio \
    --direct=1 --runtime=10 --time_based --output-format=terse 2>&1 | tail -1)
  case "$out" in
    *";"*) echo "$1 bs=$2 qd=$3 read_iops=$(echo "$out" | cut -d';' -f8) read_KBps=$(echo "$out" | cut -d';' -f7)" \
              "write_iops=$(echo "$out" | cut -d';' -f49) write_KBps=$(echo "$out" | cut -d';' -f48)" ;;
    *) echo "$1 bs=$2 qd=$3 ERROR: $(echo "$out" | head -c 160)" ;;
  esac
done
rm -f "$dir/fio.dat"
