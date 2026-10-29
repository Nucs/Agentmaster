#!/bin/sh
# Experiment (2026-10-29): a test VM whose RAM lives mostly on disk. The am-e2e image boots exactly as the coordinator's
# PodmanSlotDriver starts a slot, plus a memory cap on the container (--memory): whatever the VM uses beyond the cap is
# swapped to a swap file on the site's own disk (the ext4.vhdx on Q:), so Windows' commit is charged only for the cap.
# Runs inside the buildserver distro as root. Idempotent up to the container start; refuses to start a second one.
# From Windows (Git Bash):  tr -d '\r' < setup.sh | wsl.exe -d buildserver -u root -e sh -s
# Then sample.py SECONDS watches it, the QEMU monitor's `screendump /storage/screen.ppm` (../hmp.py inside the container)
# with ../ppm2png.py shows the guest's screen, swaprate.py measures the swap traffic, and cleanup.sh ends it all
# (DESIGN.md §16.14).
set -eu
EXP=/srv/buildserver/exp/diskram
SWAP=/srv/buildserver/swap/exp.swap
NAME=amb-exp-diskram
IMG=/srv/buildserver/images/am-e2e
export PODMAN_IGNORE_CGROUPSV1_WARNING=1

if podman container exists "$NAME"; then
  echo "refused: container $NAME exists already (remove it with cleanup.sh first)" >&2
  exit 1
fi

mkdir -p "$EXP/storage" "$EXP/shared" "$(dirname "$SWAP")"
# The slot's own copies of the image's UEFI and dockur state files (never its disks), as PrepareOverlayAsync does.
for f in "$IMG"/*; do
  case "$(basename "$f")" in data.*) ;; *) cp -f "$f" "$EXP/storage/" ;; esac
done
# A fresh overlay over the image: the experiment never writes into the image itself.
rm -f "$EXP/storage/data.qcow2"
qemu-img create -q -f qcow2 -F qcow2 -b "$IMG/data.qcow2" "$EXP/storage/data.qcow2"

# The swap file on the site's disk. fallocate writes nothing, so the vhdx on Q: grows only by what is swapped out.
# Priority 10 puts it ahead of WSL's own swap (/dev/sdb, priority -2, a vhdx on C:).
if ! grep -q "^$SWAP " /proc/swaps; then
  if [ ! -f "$SWAP" ]; then
    fallocate -l 8G "$SWAP"
    chmod 600 "$SWAP"
    mkswap "$SWAP" >/dev/null
  fi
  swapon -p 10 "$SWAP"
fi

# The coordinator's own slot arguments (SlotDrivers.cs), 4 GB of guest RAM, 4 vCPUs, and the cap: 1.5 GB resident,
# up to 8 GB more in swap (memsw = RAM + swap). RAM_CHECK=N: dockur must not judge RAM_SIZE against the cap.
podman run -d --name "$NAME" --label amb.exp=diskram \
  --device /dev/kvm \
  --network slirp4netns:allow_host_loopback=true \
  --stop-timeout 120 \
  --memory 1536m --memory-swap 9728m \
  -v "$EXP/storage:/storage" \
  -v "$EXP/shared:/shared" \
  -e RAM_SIZE=4096M -e CPU_CORES=4 -e DISK_FMT=qcow2 -e NETWORK=user -e TZ=UTC -e RAM_CHECK=N \
  -e "ARGUMENTS=-device virtio-balloon-pci,id=balloon0,deflate-on-oom=on -global virtio-vga.xres=1600 -global virtio-vga.yres=900" \
  -e VERSION=core11 \
  -v /srv/buildserver/images:/srv/buildserver/images:ro \
  docker.io/dockurr/windows:6.05 >/dev/null

echo "started $NAME"
echo "cgroup: $(podman inspect -f '{{.State.CgroupPath}}' "$NAME" 2>/dev/null)"
cat /proc/swaps
