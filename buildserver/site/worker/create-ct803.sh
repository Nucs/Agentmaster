#!/bin/bash
# Create CT 803 "buildserver" on the worker machine: the build server's site there (buildserver/README.md, "The worker
# site"). Runs on the worker host (K:\source\proxmox\workermachine), detached (its CLAUDE.md rule 13), from the folder
# buildserver/site/worker.py uploads to /root/buildserver-site:
#   ssh worker 'nohup setsid bash /root/buildserver-site/create-ct803.sh </dev/null >/root/buildserver-ct803.log 2>&1 &'
#
# What it makes, and why each choice:
#   - A PRIVILEGED Alpine container with nesting: inside it rootful podman runs dockur's QEMU, so every Windows VM is
#     a plain KVM guest of the host's kernel (no nested virtualization: at home the VMs run nested in WSL's VM at ~2,000
#     IOPS, DESIGN.md §16.6). Privileged, because rootful podman's overlay storage, its cgroups and /dev/kvm in a
#     container inside an unprivileged one are each a known failure; the site runs only this repository's code and
#     Windows guests that KVM isolates.
#   - /dev/kvm (dockur's QEMU) and /dev/net/tun (slirp4netns makes a tap device in each slot's network namespace)
#     passed in with --devN, which also allows them in the container's device cgroup.
#   - The host's /proc/meminfo bind-mounted read-only at /mnt/hostproc/meminfo (optional: the container starts without
#     it). /proc/meminfo inside a container is lxcfs's view of the container alone; the coordinator admits VMs by what
#     the whole HOST has available (TraderLab's worker and the planned Windows Server VM share it), so it reads the
#     host's own file. Without it the coordinator holds every slot ("no host facts"): a safe failure.
#   - Disks: the rootfs (Alpine, podman, 8 GiB) on drive 1's local-lvm, which is backed up; EVERYTHING the site holds
#     (images, the kept build VM's disks, slots, the store, the git mirror, the records, podman's storage) on drive 2
#     through mp0 (/mnt/drive2/data/buildserver -> /srv/buildserver, backup=0): the owner's split of 2026-11-12, drive 2
#     for "building and holding artifacts", data that may be lost (workermachine/CLAUDE.md rule 10: every build id can
#     be built again, every image made again).
#   - The network: 192.168.50.21 on vmbr1, the home LAN extension (the owner, 2026-11-12: "that worker gets assigned an
#     ip"), gateway the host .18. The link home carries .21 since that day (workermachine/link.md); its internet leaves
#     from the worker's own public address (the host masquerades vmbr1's guests), DNS 1.1.1.1/9.9.9.9, so downloads
#     never depend on the link.
#   - Memory: a 100 GiB ceiling (not a reservation: the coordinator admits by the host's available memory), no swap
#     (a VM's pages must never be swapped to drive 1). CPU: every thread, default cpuunits (TraderLab's worker has 50,
#     so a build wins contention 2:1, as that worker was designed to back off).
#   - onboot, after the link (order 30; CT 802 is 10).
# It installs nothing inside: worker.py's `provision` does (packages, podman's configuration, the coordinator's
# service). It refuses when CT 803 exists, drive 2 is not mounted, or vmbr1 is not up.
#
# Exit codes: 0 created and running; 1 refused or a step failed (the log says which).
set -euo pipefail

CTID=803
TEMPLATE_NAME=alpine-3.24-default_20261105_amd64.tar.xz
TEMPLATE=local:vztmpl/$TEMPLATE_NAME
DATA=/mnt/drive2/data/buildserver
ADDRESS=192.168.50.21

log() { printf '\n=== %s  %s\n' "$(date -u +%T)Z" "$*"; }

# Runs a command inside the container. Arguments: the command and its arguments. Returns: its exit status.
in_ct() { pct exec "$CTID" -- "$@"; }

if pct status "$CTID" >/dev/null 2>&1; then echo "refusing: CT $CTID exists"; exit 1; fi
mountpoint -q /mnt/drive2 || { echo "refusing: drive 2 is not mounted at /mnt/drive2"; exit 1; }
ip -4 -o addr show dev vmbr1 2>/dev/null | grep -qF ' 192.168.50.18/32 ' || { echo "refusing: vmbr1 isn't up"; exit 1; }
ip route show "$ADDRESS" | grep -q vmbr1 || { echo "refusing: $ADDRESS has no route on vmbr1 (link-host-rules GUESTS)"; exit 1; }

log "template $TEMPLATE_NAME"
if [ ! -e "/var/lib/vz/template/cache/$TEMPLATE_NAME" ]; then
  pveam update >/dev/null
  pveam download local "$TEMPLATE_NAME"
fi

log "drive 2's folder $DATA"
install -d -m 0755 "$DATA"

log "create CT $CTID"
pct create "$CTID" "$TEMPLATE" \
  --hostname buildserver \
  --description "The build server's site on the worker machine: amb-coordinator, rootful podman and dockur's KVM VMs (am-build, am-e2e), the store of build ids. Its data is on drive 2 (/mnt/drive2/data/buildserver). Code and runbook: buildserver/ in K:\\source\\Agentmaster (README.md, \"The worker site\")." \
  --tags "buildserver" \
  --unprivileged 0 --features nesting=1 \
  --memory 102400 --swap 0 \
  --rootfs local-lvm:8 \
  --mp0 "$DATA,mp=/srv/buildserver,backup=0" \
  --net0 "name=eth0,bridge=vmbr1,ip=$ADDRESS/24,gw=192.168.50.18,type=veth" \
  --nameserver "1.1.1.1 9.9.9.9" \
  --timezone Asia/Jerusalem \
  --onboot 1 --startup order=30 \
  --dev0 /dev/kvm --dev1 /dev/net/tun
# The host's own memory figures (see the header). "optional": a missing source never stops the container's start.
echo "lxc.mount.entry: /proc/meminfo mnt/hostproc/meminfo none bind,ro,optional,create=file" >> "/etc/pve/lxc/$CTID.conf"
# Capabilities: Alpine's LXC config (/usr/share/lxc/config/alpine.common.conf) drops mknod, so crun cannot make a slot's
# /dev/null or /dev/kvm ("mknodat: Operation not permitted", measured 2026-11-12), and sys_resource, which a container's
# raised limits need. An empty drop resets the list; the second line drops everything common.conf and Alpine's list
# drop except those two.
printf '%s\n' "lxc.cap.drop: " \
  "lxc.cap.drop: audit_write ipc_owner sys_pacct sys_rawio sys_tty_config syslog wake_alarm mac_admin mac_override sys_time sys_module" \
  >> "/etc/pve/lxc/$CTID.conf"
pct start "$CTID"

log "wait for the network"
for _ in $(seq 1 30); do in_ct ping -c1 -W2 1.1.1.1 >/dev/null 2>&1 && break; sleep 2; done
in_ct ping -c1 -W2 1.1.1.1 >/dev/null
in_ct ping -c1 -W2 192.168.50.100 >/dev/null && echo "home's Technitium answers through the link"

log "facts"
in_ct sh -c 'cat /etc/alpine-release; uname -r; ls -l /dev/kvm /dev/net/tun; grep MemAvailable /mnt/hostproc/meminfo /proc/meminfo; df -h /srv/buildserver | tail -1'
pct config "$CTID"
echo "created CT $CTID"
