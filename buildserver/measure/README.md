# Build server measurements: the kit

The scripts behind every number in [`../DESIGN.md`](../DESIGN.md) §16, as they ran on 2026-10-24 and 2026-10-25. Nothing
here is part of the build server: they are the measurements' proof, kept so that each number can be measured again.

## Where they ran

- **The host:** this PC (Windows 11 Pro 26200, i9-13900K, 63.7 GB of RAM), Docker Desktop on WSL 2.3.24.0
  (kernel 5.15.153.1-2).
- **A Docker Desktop VM:** dockur `dockurr/windows:6.05` (QEMU 11.1.0) with `VERSION=core11` (Tiny11 Core 25H2), in the
  container `bs-core11-install`, its disk in the Docker volume of the same name (inside `docker_data.vhdx` on C:).
- **The dedicated distro:** an Alpine 3.22 minirootfs imported with `wsl --import bs-measure <folder> <tar>`, its disk
  `ext4.vhdx` in the test folder `Q:\buildserver\_measure\wsl`; podman 5.6.2 + crun 1.23.1 in it (`apk add podman crun
  passt fuse-overlayfs qemu-img slirp4netns`, settings: [`podman-containers.conf`](podman-containers.conf)).
- **A slot in the distro:** [`run-slot.sh`](run-slot.sh) boots dockur with an overlay over a copy of the installed
  image (`/srv/bs/golden/core11`) and extra test disks.

## The scripts

| File | What it measures | Where it runs |
|---|---|---|
| [`fio-mounts.sh`](fio-mounts.sh) | host-level disk speed (fio, O_DIRECT and buffered) of a Docker volume and two Windows folder binds | an Alpine container |
| [`fio-one.sh`](fio-one.sh) | the same four O_DIRECT tests on one folder (the dedicated distro's disk; an integrated distro's disk bound into a Docker Desktop container) | an Alpine container |
| [`bench.ps1`](bench.ps1) | CPU (managed SHA-256 on 1 and N threads), process starts, 5,000 small files, DiskSpd random 4K / sequential 1M | the host and every guest, the same script |
| [`compile-bench.ps1`](compile-bench.ps1) | the engine test harness (63 C++20 files, no PCH) compiled and linked from a `git archive` copy, at a set `/MP` | the host and a guest with Build Tools |
| [`hmp.py`](hmp.py) | one QEMU monitor command (the balloon: `info balloon`, `balloon <MiB>`; `screendump`) | inside a dockur container |
| [`ppm2png.py`](ppm2png.py) | converts a `screendump` to PNG with the standard library | inside a dockur container |
| [`dockur-skip.sh`](dockur-skip.sh) | prints dockur's "is Windows installed" logic | the dockur image |
| [`ksm-test.sh`](ksm-test.sh) | KSM's savings on idle Windows VMs (not run: KSM is one switch for the whole WSL VM, and another session was measuring memory in it) | a WSL distro, as root |
| [`oem/`](oem) | the image's measurement agent: `install.bat` → `setup.ps1` registers `agent.ps1` as a logon task; the agent runs `jobs\<n>.ps1` from the share and writes `results\` | every guest |
| [`guest/`](guest) | the jobs: `facts.ps1` (what Tiny11 Core contains), `bench-c.ps1`, `bench-disks.ps1`, `alloc.ps1` (new space vs overwrites), `attach.ps1` (four QEMU disk attachments), `app-portable.ps1` (Agentmaster v0.8.0 portable), `vs-install.ps1` (VS 2022 Build Tools), `compile.ps1` / `compile-sampled.ps1` / `compile-quiet.ps1` (the compile benchmark in the guest: twice, with memory and CPU samples, and with Windows' servicing stopped), `update-agent.ps1` | a guest, through the agent |
| [`results-2026-10-25/`](results-2026-10-25) | every result file the numbers in DESIGN.md §16 come from: `docker-vm-*` (the VM in Docker Desktop), `distro-slot-*` (the slot in the test distro), `host-*` (this PC), and the screenshot of Agentmaster on Tiny11 Core | — |

## Traps met on the way (each one is in DESIGN.md too)

- **A host-written file on a Docker Desktop bind is never listed by the guest.** dockur's Samba gets no change
  notification across the bind, so the guest's SMB client keeps serving its cached listing under its directory lease.
  The agent therefore asks for the next job by its exact name (`jobs\<n>.ps1`); a real guest agent talks HTTP.
- **dockur deletes a VM folder's disks when it holds no `windows.boot`:** without that empty marker file (and without
  the install ISO), dockur takes the folder for an unfinished install, removes `data.*` and `windows.*`, and installs
  again (`cleanupStorage` in `/run/install.sh`). Copy the marker with every image.
- **`wsl.exe -d <distro> -- sh -c '<script>'` is re-parsed by the distro's default shell**, which expands the script's
  `$VARS` and `$(...)` before `sh -c` runs it. `wsl.exe -d <distro> -e sh -c '<script>'` passes it intact.
- **Rootful podman has no pasta networking** ("pasta networking is only supported for rootless mode");
  `--network slirp4netns` gives a container its own network namespace without a bridge or firewall rule in the one
  WSL shares with Docker Desktop.
- **Pipes are slow:** a 12 GB image copied `docker run … tar -cf - | wsl.exe … tar -xf -` ran at 25 MB/s (480.6 s).
- **Reads of a sparse file's holes are not disk reads:** DiskSpd's random-read number on a freshly created test file
  (14,000+ IOPS) was mostly holes; `alloc.ps1` writes its file first.
- **After the Build Tools install, TiWorker (Windows' servicing worker) kept one vCPU busy for over 30 minutes** on
  Tiny11 Core; the compiles made meanwhile were 8–47 % slower. `compile-quiet.ps1` stops the TrustedInstaller service
  first.
- **`wsl --manage <distro> --set-sparse true` is refused while WSL's VM runs** ("the vhdx is currently in use"), even
  right after `wsl --import`, and so is `fsutil sparse setflag`; with Docker Desktop keeping WSL up, it needs a WSL
  restart.
