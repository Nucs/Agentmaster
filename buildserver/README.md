# Agentmaster build server

A service that builds, tests and e2e-runs Agentmaster on demand, in slots sized to what this PC (and later dock) has
free. The design, the owner's decisions and every measured number are in [`DESIGN.md`](DESIGN.md); this file says what
exists so far and how to use it. **Status: Phase 0 of DESIGN.md §13 is under way; nothing serves requests yet.**

## What exists

| Path | What it is | State |
|---|---|---|
| [`site/sitectl.py`](site/sitectl.py) | makes and inspects this PC's site: the WSL distro `buildserver`, its disk in `Q:\buildserver\wsl` (E2) | in use: the site was made with it on 2026-10-25 |
| [`site/files/`](site/files) | the configuration it installs (`/etc/wsl.conf`, podman's `containers.conf`), each file documented | in use |
| [`measure/`](measure/README.md) | the measurements and proofs behind DESIGN.md §16, with their raw results | P0.1 passed (§16.9) |

## The site

```text
python buildserver/site/sitectl.py create       # once: Alpine 3.22 minirootfs (checked by SHA-256) → wsl --import → packages
python buildserver/site/sitectl.py provision    # re-apply packages, folders and configuration (idempotent)
python buildserver/site/sitectl.py status       # JSON: state, disk size; free space and memory when it is running
python buildserver/site/sitectl.py run -- podman ps
```

- **What it holds:** podman (rootful, cgroupfs, overlay), crun, slirp4netns, qemu-img, python3; the folders of
  DESIGN.md §8.1 under `/srv/buildserver` (images, slots, vms, exchange, records, store, cache, fastrun).
- **Never `wsl --shutdown`** for the site: it ends every Docker container and other sessions' desktops. The script only
  ever terminates its own distro (`wsl --terminate buildserver`).
- **Reach the site from Windows at the distro's address** (`ip -4 addr show eth0` inside; it changes with every start
  of WSL's VM): WSL's localhost forwarding passes only IPv4 listeners, and fastrund listens on the IPv6 wildcard.
- **Something that must keep running goes into a podman container:** a background job of a `wsl.exe -e` call ends with
  that call.
- **The registry credentials** fastrund pulls with live in `/srv/buildserver/fastrun/regcreds` (root only), copied from
  Docker Desktop's credential store by `measure/p01_fastrund.py`; nothing prints them.
