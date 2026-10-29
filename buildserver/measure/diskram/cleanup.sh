#!/bin/sh
# Ends the experiment: removes the container (its swapped pages are freed with it), turns the swap file off and deletes
# it, and deletes the experiment's folder. Safe to run twice.
NAME=amb-exp-diskram
SWAP=/srv/buildserver/swap/exp.swap
export PODMAN_IGNORE_CGROUPSV1_WARNING=1
podman rm -f -t 0 "$NAME" >/dev/null 2>&1 && echo "removed $NAME" || echo "no container $NAME"
if grep -q "^$SWAP " /proc/swaps; then swapoff "$SWAP" && echo "swapoff $SWAP"; fi
rm -f "$SWAP" && rmdir /srv/buildserver/swap 2>/dev/null
rm -rf /srv/buildserver/exp/diskram && rmdir /srv/buildserver/exp 2>/dev/null
cat /proc/swaps
