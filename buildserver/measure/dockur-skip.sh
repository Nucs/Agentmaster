#!/bin/sh
# Prints the parts of dockur's scripts that decide whether Windows is already installed (and so whether a start
# boots the disk or installs anew), for the build server's image design. Run inside the dockur image:
#   podman run --rm --network none --entrypoint sh -v <tools>:/t:ro docker.io/dockurr/windows:6.05 /t/dockur-skip.sh
grep -n "windows.boot\|windows.base\|skipInstall\|hasDisk()" /run/*.sh | head -40
echo "-----"
# The function that skips the install, in full.
awk '/^skipInstall\(\)/,/^}/' /run/install.sh
