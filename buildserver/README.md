# Agentmaster build server

A service that builds, tests and e2e-runs Agentmaster on demand, in slots sized to what this PC (and later dock) has
free. The design, the owner's decisions and every measured number are in [`DESIGN.md`](DESIGN.md); this file says what
exists so far and how to use it. **Status (2026-10-25): P0.1 passed; Phase 1's coordinator, guest agent and host-facts
helper exist and pass their chain test on this PC without a VM; the images and a VM run are next.**

**Phase 1, as built, differs from DESIGN.md §10 in one way:** there is no always-on host agent yet. The coordinator reads
Windows' numbers itself (it runs `amb-hostfacts.exe` through WSL's interop, which works from a daemon after the call that
started it ended: measured), and the CLI will wake the site by starting the coordinator. The host agent's other duties
(the bare mirror, copying artifacts to Windows, installs, a LAN port for Caddy) come with Phases 3 and 4.

## What exists

| Path | What it is | State |
|---|---|---|
| [`site/sitectl.py`](site/sitectl.py) | makes and inspects this PC's site: the WSL distro `buildserver`, its disk in `Q:\buildserver\wsl` (E2) | in use: the site was made with it on 2026-10-25 |
| [`site/files/`](site/files) | the configuration it installs (`/etc/wsl.conf`, podman's `containers.conf`), each file documented | in use |
| [`measure/`](measure/README.md) | the measurements and proofs behind DESIGN.md §16, with their raw results | P0.1 passed (§16.9) |
| [`src/Amb.Core`](src/Amb.Core) | the shared core: the allocation rule (§3), the request / build / slot model, the guest and API protocol, the atomic record store (§6) | 27 unit tests pass (`src/Amb.Core.Tests`) |
| [`src/Amb.Coordinator`](src/Amb.Coordinator) | `amb-coordinator`: the API, the scheduler (admission by priority × availability, wait and run timeouts, cancel), the guest broker, the jobs (import, smoke, install, layer, e2e), the slot manager and its drivers (podman + dockur, the stand-in of §13; local, for tests) | the local chain passes (`tests/local_chain.py`); not yet run in the site with a VM |
| [`src/Amb.GuestAgent`](src/Amb.GuestAgent) | `amb-guest.exe` (Native AOT, 7 MB): the agent in every VM; hello, heartbeat with the guest's memory, one step at a time by long poll (run, fetch, expand, put, shutdown) | the local chain passes |
| [`src/Amb.HostFacts`](src/Amb.HostFacts) | `amb-hostfacts.exe` (Native AOT): Windows' available memory, free commit, Q: free space and a full-screen game, as one line of JSON; the coordinator runs it every 10 s through WSL's interop | the local chain reads it |
| [`tests/local_chain.py`](tests/local_chain.py) | the whole chain on this PC without a VM: the coordinator with the local slot driver, the real guest agent, the real API | 15 checks pass |

## Build and test

```text
dotnet test buildserver/src/Buildserver.slnx            # build everything + the unit tests (output in Q:\tmp\buildserver-build)
python buildserver/tests/local_chain.py                 # the chain without a VM (Q:\tmp\buildserver-test, removed after)
PATH="/c/Program Files (x86)/Microsoft Visual Studio/Installer:$PATH" \
  dotnet publish buildserver/src/Amb.GuestAgent -c Release -o Q:\tmp\buildserver-build\publish\amb-guest   # Native AOT needs vswhere
```

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
