#!/bin/sh
# Boots one measurement slot of the build server in the dedicated distro (bs-measure), with podman:
#   sh run-slot.sh <slot> <ram, e.g. 8G> <vcpus> [extra QEMU arguments]
# The slot folder /srv/bs/slots/<slot> holds the VM's own files: data.qcow2 (an overlay over the golden image in
# /srv/bs/golden, mounted read-only at the same path so the overlay's backing path resolves), the UEFI files, and
# d2.raw, a sparse raw data disk attached as a second SCSI disk on its own controller. /srv/bs/shared-<slot> is the
# guest's \\host.lan\Data (jobs, results, tools). Networking is slirp4netns: the container gets its own network
# namespace, so nothing is added to the namespace WSL shares with Docker Desktop. A virtio balloon device is always
# attached (BALLOONING=N keeps dockur's own controller off, so the balloon is set only by hand, through the monitor).
# Prints the container id; the start time (epoch seconds) goes to /srv/bs/slots/<slot>.t0.
set -e
slot="$1"; ram="$2"; cpus="$3"; shift 3
dir="/srv/bs/slots/$slot"
[ -f "$dir/data.qcow2" ] || { echo "no slot at $dir"; exit 2; }
mkdir -p "/srv/bs/shared-$slot/jobs" "/srv/bs/shared-$slot/results" "/srv/bs/shared-$slot/tools"
date +%s > "/srv/bs/slots/$slot.t0"
export PODMAN_IGNORE_CGROUPSV1_WARNING=1
podman run -d --name "bs-$slot" --device /dev/kvm --stop-timeout 120 \
  -v "$dir:/storage" -v /srv/bs/golden:/srv/bs/golden:ro -v "/srv/bs/shared-$slot:/shared" \
  -e VERSION=core11 -e RAM_SIZE="$ram" -e CPU_CORES="$cpus" -e DISK_SIZE=64G -e DISK_FMT=qcow2 \
  -e NETWORK=user -e TZ=Asia/Jerusalem \
  -e "ARGUMENTS=-device virtio-balloon-pci,id=balloon0,deflate-on-oom=on -device virtio-scsi-pci,id=bsd2 -drive file=/storage/d2.raw,id=d2,format=raw,cache=none,aio=native,discard=unmap,if=none -device scsi-hd,drive=d2,bus=bsd2.0,rotation_rate=1 $*" \
  docker.io/dockurr/windows:6.05
