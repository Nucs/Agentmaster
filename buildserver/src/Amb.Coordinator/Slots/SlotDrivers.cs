using System.Diagnostics;
using System.Globalization;
using System.Text;
using Amb.Core.Allocation;

namespace Amb.Coordinator.Slots;

/// <summary>What a slot driver needs to start one slot.</summary>
/// <param name="SlotId">The slot's id (its container, folder and record share it).</param>
/// <param name="Image">The image it boots ("am-e2e", "am-base").</param>
/// <param name="Grant">What it was granted (RAM_SIZE and CPU_CORES come from it).</param>
/// <param name="SlotDir">Its own folder: the overlay over the image and its UEFI variables.</param>
/// <param name="ExchangeDir">Its share (the guest's \\host.lan\Data): the agent, slot.json, bundles in, results out.</param>
/// <param name="ImageDir">The image's read-only folder (unused by an install, which makes a new one).</param>
/// <param name="InstallVersion">For an install slot: the dockur VERSION to install from its ISO ("core11"); null for a
/// slot that boots an existing image through an overlay.</param>
/// <param name="OemDir">For an install slot: the folder dockur copies to C:\OEM and runs (install.bat) at the end of
/// the unattended install.</param>
/// <param name="Kept">A kept VM's slot (a build VM): <paramref name="SlotDir"/> is the VM's own folder under
/// <c>vms/</c>, which outlives the slot. Its overlay is made only when the folder has none yet, so the next boot finds
/// the same disk; and it is always stopped with the driver's full graceful stop, because its disks are reused.</param>
/// <param name="DataDiskGiB">When set: a raw data disk of this size (a sparse file, <c>data-disk.raw</c> in the slot
/// folder, made when missing) is attached as a second disk; the guest formats it as D: at its first boot.</param>
/// <param name="GuestMemoryMiB">When above the grant's memory: the RAM the guest sees, and the slot is CAPPED at its
/// grant (<see cref="PodmanSlotDriver.MemoryArguments"/>): the rest of the guest's RAM lives in the site's swap file.</param>
public sealed record SlotSpec(string SlotId, string Image, Resources Grant, string SlotDir, string ExchangeDir,
    string ImageDir, string? InstallVersion = null, string? OemDir = null, bool Kept = false, int? DataDiskGiB = null,
    long? GuestMemoryMiB = null);

/// <summary>What a slot's container used of the PC's memory over its life, read from its memory cgroup just before it
/// stops (the measurements behind a slot's size, DESIGN.md §16.14).</summary>
/// <param name="PeakResidentMiB">The most it held in memory at once (cgroup v1's <c>memory.max_usage_in_bytes</c>): what
/// Windows was charged for it at its peak, QEMU and dockur's own processes included.</param>
/// <param name="PeakWithSwapMiB">The most it held resident and swapped out together (<c>memory.memsw.max_usage_in_bytes</c>).</param>
/// <param name="SwappedMiB">What it had in swap when read (<c>memory.stat</c>'s <c>total_swap</c>).</param>
/// <param name="CapHits">How often it reached its limit (<c>memory.failcnt</c>); each hit made the kernel reclaim, so a
/// large count over a short run means the guest kept paging.</param>
public sealed record SlotMemoryStats(long PeakResidentMiB, long PeakWithSwapMiB, long SwappedMiB, long CapHits);

/// <summary>
/// Starts and stops slots. ONE interface for the stand-in (podman + dockur) and, later, fastrun's VM slots (DESIGN.md
/// §13 Track A), so jobs never know which one runs them.
/// </summary>
public interface ISlotDriver
{
    /// <summary>The driver's name, recorded with every slot ("podman", "local").</summary>
    string Name { get; }

    /// <summary>Whether its slots are VMs: only then may a guest's shutdown step really shut Windows down (a local
    /// slot's agent runs on the PC itself).</summary>
    bool IsVm { get; }

    /// <summary>How the guest sees its share's root (the folder a step's paths are under).</summary>
    /// <param name="spec">The slot.</param>
    /// <returns>"\\host.lan\Data" for a VM; the exchange folder itself for a local slot.</returns>
    string GuestShareRoot(SlotSpec spec);

    /// <summary>Starts a slot whose folders the slot manager has prepared.</summary>
    /// <param name="spec">The slot.</param>
    /// <param name="cancel">Cancels the start.</param>
    /// <returns>The driver's handle of the slot (a container name, a process id).</returns>
    /// <exception cref="InvalidOperationException">The slot could not be started (the message says why).</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    Task<string> StartAsync(SlotSpec spec, CancellationToken cancel);

    /// <summary>Stops a slot and removes what the driver made for it (never the slot manager's folders).</summary>
    /// <param name="handle">The slot's handle.</param>
    /// <param name="graceful">Shut the guest down first (Windows takes 8-15 s, measured); false kills it at once.</param>
    /// <param name="cancel">Cancels the wait (the slot is then killed).</param>
    /// <returns>A task that ends when the slot is gone.</returns>
    Task StopAsync(string handle, bool graceful, CancellationToken cancel);

    /// <summary>Whether a slot still runs.</summary>
    /// <param name="handle">The slot's handle.</param>
    /// <param name="cancel">Cancels the check.</param>
    /// <returns>True while it runs.</returns>
    Task<bool> IsAliveAsync(string handle, CancellationToken cancel);

    /// <summary>Sets a VM's balloon target (DESIGN.md §3.4).</summary>
    /// <param name="handle">The slot's handle.</param>
    /// <param name="targetMiB">The memory the guest may keep.</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>True when the target was set; false when the driver has no balloon (a local slot).</returns>
    Task<bool> SetBalloonAsync(string handle, long targetMiB, CancellationToken cancel);

    /// <summary>Reads what a running slot has used of the PC's memory so far (its peaks), for the request's events.</summary>
    /// <param name="handle">The slot's handle.</param>
    /// <param name="cancel">Cancels the read.</param>
    /// <returns>The figures, or null when the driver cannot tell (no cgroup: a local slot, a test driver) or the read
    /// failed: they are a report, never a reason to fail the slot.</returns>
    Task<SlotMemoryStats?> ReadMemoryAsync(string handle, CancellationToken cancel) => Task.FromResult<SlotMemoryStats?>(null);
}

/// <summary>A program run by a slot driver, with its output.</summary>
/// <param name="ExitCode">Its exit code.</param>
/// <param name="Output">Standard output.</param>
/// <param name="Error">Standard error.</param>
public sealed record CommandResult(int ExitCode, string Output, string Error);

/// <summary>Runs the programs slot drivers need (podman, qemu-img) and returns their output.</summary>
public static class Commands
{
    /// <summary>Runs a program to its end.</summary>
    /// <param name="program">The program.</param>
    /// <param name="args">Its arguments (each passed as one argument; no shell).</param>
    /// <param name="timeout">The longest wait; then the program's tree is killed.</param>
    /// <param name="cancel">Cancels the wait (the program's tree is killed).</param>
    /// <param name="stdin">Text for its standard input, or null.</param>
    /// <param name="env">Variables to add, or null.</param>
    /// <returns>The exit code and both outputs.</returns>
    /// <exception cref="System.ComponentModel.Win32Exception">The program could not be started.</exception>
    /// <exception cref="TimeoutException">It did not end within the timeout.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    public static async Task<CommandResult> RunAsync(string program, IEnumerable<string> args, TimeSpan timeout,
        CancellationToken cancel, string? stdin = null, IReadOnlyDictionary<string, string>? env = null)
    {
        var info = new ProcessStartInfo(program)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            CreateNoWindow = true,
        };
        foreach (string arg in args)
        {
            info.ArgumentList.Add(arg);
        }
        foreach ((string name, string value) in env ?? new Dictionary<string, string>())
        {
            info.Environment[name] = value;
        }
        using var process = new Process { StartInfo = info };
        process.Start();
        if (stdin is not null)
        {
            await process.StandardInput.WriteAsync(stdin.AsMemory(), cancel).ConfigureAwait(false);
        }
        process.StandardInput.Close();
        Task<string> output = process.StandardOutput.ReadToEndAsync(CancellationToken.None);
        Task<string> error = process.StandardError.ReadToEndAsync(CancellationToken.None);
        try
        {
            await process.WaitForExitAsync(cancel).WaitAsync(timeout, cancel).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is TimeoutException or OperationCanceledException)
        {
            // Cleanup-on-failure: a hung podman call must not outlive the wait that gave up on it.
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException)
            {
                // It ended between the timeout and the kill.
            }
            throw;
        }
        return new CommandResult(process.ExitCode, await output.ConfigureAwait(false), await error.ConfigureAwait(false));
    }
}

/// <summary>
/// The stand-in slot driver (DESIGN.md §13): each slot is a dockur container run by rootful podman in the site distro,
/// booting a copy-on-write overlay over a read-only image, in a slirp4netns network namespace of its own.
/// </summary>
/// <remarks>
/// <list type="bullet">
/// <item>The overlay: <c>qemu-img create -b &lt;image&gt;/data.qcow2</c> (under 1 s, measured); the image folder is mounted
/// read-only at the same path, so the overlay's absolute backing path resolves inside the container.</item>
/// <item>Every other file of the image folder (UEFI code and variables, dockur's state files, <c>windows.boot</c>) is
/// copied into the slot: each slot gets its own UEFI variables. ⚠ Without <c>windows.boot</c> dockur takes the folder for
/// an unfinished install and deletes its disks (measured), so a start without it is refused here.</item>
/// <item>The version: a boot passes the VERSION the image was installed from (<see cref="ImageVersion"/>); with any other
/// dockur moves the overlay aside and installs Windows afresh (measured), so a start without it is refused too.</item>
/// <item>The network: <c>slirp4netns:allow_host_loopback=true</c>, so the guest reaches the coordinator at 10.0.2.2,
/// which slirp4netns maps to the distro's loopback (measured: without the option nothing answers there). Nothing is
/// added to the network namespace WSL shares with Docker Desktop.</item>
/// <item>A virtio balloon device is always attached (dockur's own balloon controller stays off: BALLOONING=N is its
/// default); the coordinator sets it through QEMU's monitor (<c>hmp.py</c>). The screen is 1600×900, the claude-fleet
/// desktops' size (<see cref="QemuArguments"/>).</item>
/// </list>
/// </remarks>
public sealed class PodmanSlotDriver(CoordinatorOptions options, ILogger<PodmanSlotDriver> log) : ISlotDriver
{
    /// <summary>The prefix of every slot container's name: the coordinator's start-up cleanup finds its own by it.</summary>
    public const string ContainerPrefix = "amb-slot-";

    private static readonly IReadOnlyDictionary<string, string> Env = new Dictionary<string, string>
    {
        // podman warns on every call under WSL's cgroups v1; the warning would fill the logs.
        ["PODMAN_IGNORE_CGROUPSV1_WARNING"] = "1",
    };

    /// <summary>QEMU's extra arguments for every slot (dockur's <c>ARGUMENTS</c>): the balloon device, and the screen
    /// size the guest's display driver picks at boot.</summary>
    /// <remarks>
    /// <para>The balloon: dockur's own balloon controller stays off (BALLOONING=N is its default); the coordinator sets
    /// the device's target through QEMU's monitor (<c>hmp.py</c>).</para>
    /// <para>The screen: Windows' VirtIO GPU driver offers a fixed mode list plus the size the device announces in its
    /// EDID, and picks that one at boot; <c>-global</c> sets it on dockur's own <c>-vga virtio</c> device. 1600×900 is
    /// the claude-fleet desktops' size (claude-desktops fleet/windows_host.py), which the e2e suite was written against.
    /// Without it a slot came up at ~1024×768 (a maximized window 995×721, measured 2026-10-25) and SP-06's side-panel
    /// button was off screen.</para>
    /// </remarks>
    internal const string QemuArguments =
        "-device virtio-balloon-pci,id=balloon0,deflate-on-oom=on -global virtio-vga.xres=1600 -global virtio-vga.yres=900";

    /// <summary>The raw data disk's file name in a slot folder.</summary>
    public const string DataDiskFile = "data-disk.raw";

    /// <summary>The site's swap file, on its own disk (the ext4.vhdx on Q:): a capped slot's guest RAM above its cap
    /// lives there. coordinator-up.sh makes it (16 GB, fallocate: nothing is written until something is swapped out) and
    /// turns it on at priority 10, ahead of WSL's own swap on C:, at every start of the coordinator.</summary>
    public const string SwapFile = "/srv/buildserver/swap/slots.swap";

    /// <summary>The swap a capped slot may use beyond its guest's RAM (QEMU's and dockur's own pages), MiB.</summary>
    internal const long CappedSwapMarginMiB = 1024;

    /// <summary>The podman arguments that size a slot's memory: RAM_SIZE, and for a capped slot its container's cap.</summary>
    /// <param name="spec">The slot.</param>
    /// <returns>The arguments, to add before the image's name.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="spec"/> is null.</exception>
    /// <remarks>
    /// <para>Uncapped (no <see cref="SlotSpec.GuestMemoryMiB"/>, or one not above the grant): RAM_SIZE is the grant, and
    /// the PC holds all of it once the guest has touched it (a Windows guest touches all of its RAM within 1-2 minutes of
    /// its boot, DESIGN.md §16).</para>
    /// <para>Capped: RAM_SIZE is the guest's RAM; <c>--memory</c> is the grant, the most the container keeps resident
    /// (cgroup v1's memory.limit_in_bytes: the WSL kernel has the memory controller and swap accounting, measured
    /// 2026-10-29); <c>--memory-swap</c> is resident plus swapped (memsw): the grant, the guest's RAM and
    /// <see cref="CappedSwapMarginMiB"/>, so at its cap the kernel swaps the guest's coldest pages to the site's swap
    /// file instead of killing QEMU. RAM_CHECK=N, or dockur would judge RAM_SIZE against the memory it sees free.
    /// ⚠ Without any swap turned on, a capped slot is killed at its cap (memsw then equals memory).</para>
    /// </remarks>
    internal static IReadOnlyList<string> MemoryArguments(SlotSpec spec)
    {
        ArgumentNullException.ThrowIfNull(spec);
        long grant = spec.Grant.MemoryMiB;
        if (spec.GuestMemoryMiB is not long guest || guest <= grant)
        {
            return ["-e", "RAM_SIZE=" + grant.ToString(CultureInfo.InvariantCulture) + "M"];
        }
        long residentAndSwapped = grant + guest + CappedSwapMarginMiB;
        return
        [
            "-e", "RAM_SIZE=" + guest.ToString(CultureInfo.InvariantCulture) + "M",
            "-e", "RAM_CHECK=N",
            "--memory", grant.ToString(CultureInfo.InvariantCulture) + "m",
            "--memory-swap", residentAndSwapped.ToString(CultureInfo.InvariantCulture) + "m",
        ];
    }

    /// <summary>QEMU's arguments that attach a slot's raw data disk (the measured attachment of
    /// buildserver/measure/run-slot.sh: a virtio-scsi controller of its own, a raw sparse file, O_DIRECT, discard).</summary>
    /// <remarks>discard=unmap passes the guest's TRIMs through, so what a build deletes (a purged tree) is punched out of
    /// the sparse file again (measured: a 64 GB data disk held 92 MB after its files were deleted).</remarks>
    internal const string DataDiskArguments =
        " -device virtio-scsi-pci,id=ambdata -drive file=/storage/" + DataDiskFile
        + ",id=ambdata0,format=raw,cache=none,aio=native,discard=unmap,if=none"
        + " -device scsi-hd,drive=ambdata0,bus=ambdata.0,rotation_rate=1";

    /// <inheritdoc/>
    public string Name => "podman";

    /// <inheritdoc/>
    public bool IsVm => true;

    /// <inheritdoc/>
    public string GuestShareRoot(SlotSpec spec) => @"\\host.lan\Data";

    /// <inheritdoc/>
    public async Task<string> StartAsync(SlotSpec spec, CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(spec);
        Directory.CreateDirectory(spec.SlotDir);
        string name = ContainerPrefix + spec.SlotId;
        string arguments = QemuArguments;
        if (spec.DataDiskGiB is int dataGiB)
        {
            EnsureDataDisk(Path.Combine(spec.SlotDir, DataDiskFile), dataGiB);
            arguments += DataDiskArguments;
        }
        List<string> args =
        [
            "run", "-d", "--name", name,
            "--label", "amb.slot=" + spec.SlotId,
            "--device", "/dev/kvm",
            "--network", "slirp4netns:allow_host_loopback=true",
            // dockur forwards a stop to Windows as an ACPI shutdown and waits for QEMU; 120 s covers 8-15 s measured.
            "--stop-timeout", "120",
            "-v", spec.SlotDir + ":/storage",
            "-v", spec.ExchangeDir + ":/shared",
            "-e", "CPU_CORES=" + spec.Grant.Vcpus.ToString(CultureInfo.InvariantCulture),
            "-e", "DISK_FMT=qcow2",
            "-e", "NETWORK=user",
            "-e", "TZ=UTC",
            "-e", "ARGUMENTS=" + arguments,
        ];
        // RAM_SIZE, and for a capped slot the container's cap and its swap allowance.
        args.AddRange(MemoryArguments(spec));
        if (spec.InstallVersion is not null)
        {
            // An install: dockur downloads the version's ISO into /storage, installs unattended onto a new 64 GB
            // (sparse) disk, copies /oem to C:\OEM and runs its install.bat at the end; windows.boot marks success.
            args.AddRange(["-e", "VERSION=" + spec.InstallVersion, "-e", "DISK_SIZE=64G"]);
            if (spec.OemDir is not null)
            {
                args.AddRange(["-v", spec.OemDir + ":/oem:ro"]);
            }
        }
        else
        {
            // A boot: the SAME VERSION the image was installed from, or dockur takes the slot for a changed install and
            // reinstalls over it (ImageVersion's remarks). Read before the overlay is made, so a bad image costs nothing.
            string version = ImageVersion(spec.ImageDir);
            // A kept VM boots the overlay it has: a new one would throw away every build tree it holds.
            if (!(spec.Kept && File.Exists(Path.Combine(spec.SlotDir, "data.qcow2"))))
            {
                await PrepareOverlayAsync(spec, cancel).ConfigureAwait(false);
            }
            string imagesRoot = Path.GetDirectoryName(spec.ImageDir)!;
            args.AddRange(["-e", "VERSION=" + version, "-v", imagesRoot + ":" + imagesRoot + ":ro"]);
        }
        args.Add(options.DockurImage);
        CommandResult run = await Commands.RunAsync("podman", args, TimeSpan.FromMinutes(2), cancel, env: Env)
            .ConfigureAwait(false);
        if (run.ExitCode != 0)
        {
            throw new InvalidOperationException($"podman run failed: {run.Error.Trim()}");
        }
        log.LogInformation("slot {Slot}: container {Name} started ({Grant})", spec.SlotId, name, spec.Grant);
        return name;
    }

    /// <summary>The dockur VERSION an image was installed from, read from dockur's own record of it.</summary>
    /// <param name="imageDir">The image's folder.</param>
    /// <returns>The version ("core11" for an image whose <c>windows.base</c> says "core11.iso").</returns>
    /// <exception cref="InvalidOperationException">The image has no <c>windows.base</c>, or it does not name an ISO by a
    /// plain dockur version (letters, digits, '.', '_', '-').</exception>
    /// <exception cref="IOException">The file could not be read.</exception>
    /// <remarks>
    /// <para>dockur writes the ISO's file name to <c>windows.base</c> when it installs (the canonical version: "11" is
    /// recorded as "win11x64.iso", "core11" as "core11.iso"). At every start it compares that name with the ISO its
    /// VERSION variable names, and on a mismatch it moves the disk to <c>backups/</c> and installs afresh, so a slot that
    /// boots an image must pass the same VERSION.</para>
    /// <para>⚠ Measured on 2026-10-25: a boot of am-base without VERSION (dockur's default is "11") downloaded the 7.7 GB
    /// Windows 11 ISO into the slot and started its setup; no guest agent ever said hello. The image's layers keep their
    /// base's <c>windows.base</c> (the slot copies it), so this reads right for a layer too.</para>
    /// </remarks>
    internal static string ImageVersion(string imageDir)
    {
        string file = Path.Combine(imageDir, "windows.base");
        if (!File.Exists(file))
        {
            throw new InvalidOperationException($"image {Path.GetFileName(imageDir)} has no windows.base: the dockur "
                + "version it was installed from is unknown, and a boot with another version would reinstall over it");
        }
        string iso = File.ReadAllText(file).Trim();
        string version = iso.EndsWith(".iso", StringComparison.OrdinalIgnoreCase) ? iso[..^4] : "";
        if (version.Length == 0 || !version.All(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or '-'))
        {
            throw new InvalidOperationException(
                $"image {Path.GetFileName(imageDir)}'s windows.base does not name a dockur version's ISO: {iso}");
        }
        return version;
    }

    /// <summary>Makes a slot's raw data disk unless it exists: a sparse file of the given size.</summary>
    /// <param name="path">The disk file.</param>
    /// <param name="giB">Its virtual size.</param>
    /// <exception cref="IOException">The file could not be made.</exception>
    /// <remarks>Setting the length of a new file on ext4 allocates nothing (a sparse file), so a 128 GiB disk costs only
    /// what the guest writes. An existing disk is never resized or touched: it holds a kept VM's build trees.</remarks>
    internal static void EnsureDataDisk(string path, int giB)
    {
        if (File.Exists(path))
        {
            return;
        }
        using var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        file.SetLength(giB * 1024L * 1024L * 1024L);
    }

    /// <summary>Makes a slot's overlay over its image and copies the image's other files (UEFI, dockur's state).</summary>
    /// <param name="spec">The slot.</param>
    /// <param name="cancel">Cancels qemu-img.</param>
    /// <returns>A task that ends when the slot folder is ready.</returns>
    /// <exception cref="InvalidOperationException">The image is not ready, or qemu-img failed.</exception>
    /// <exception cref="IOException">A file could not be copied.</exception>
    private static async Task PrepareOverlayAsync(SlotSpec spec, CancellationToken cancel)
    {
        string baseDisk = Path.Combine(spec.ImageDir, "data.qcow2");
        if (!File.Exists(baseDisk) || !File.Exists(Path.Combine(spec.ImageDir, "windows.boot")))
        {
            throw new InvalidOperationException(
                $"image {spec.Image} is not ready: {spec.ImageDir} needs data.qcow2 and windows.boot");
        }
        foreach (string file in Directory.EnumerateFiles(spec.ImageDir))
        {
            if (!Path.GetFileName(file).StartsWith("data.", StringComparison.Ordinal))
            {
                File.Copy(file, Path.Combine(spec.SlotDir, Path.GetFileName(file)), overwrite: true);
            }
        }
        CommandResult overlay = await Commands.RunAsync("qemu-img",
            ["create", "-q", "-f", "qcow2", "-F", "qcow2", "-b", baseDisk, Path.Combine(spec.SlotDir, "data.qcow2")],
            TimeSpan.FromSeconds(60), cancel).ConfigureAwait(false);
        if (overlay.ExitCode != 0)
        {
            throw new InvalidOperationException($"qemu-img create failed: {overlay.Error.Trim()}");
        }
    }

    /// <inheritdoc/>
    public async Task StopAsync(string handle, bool graceful, CancellationToken cancel)
    {
        if (graceful)
        {
            try
            {
                await Commands.RunAsync("podman", ["stop", "-t", "120", handle], TimeSpan.FromMinutes(3), cancel, env: Env)
                    .ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is TimeoutException or OperationCanceledException)
            {
                log.LogWarning("slot {Handle}: a graceful stop did not finish; removing it by force", handle);
            }
        }
        // rm -f either way: a graceful stop leaves the stopped container, and a failed one leaves a running one.
        await Commands.RunAsync("podman", ["rm", "-f", "-t", "0", handle], TimeSpan.FromMinutes(1), CancellationToken.None,
            env: Env).ConfigureAwait(false);
    }

    /// <inheritdoc/>
    public async Task<bool> IsAliveAsync(string handle, CancellationToken cancel)
    {
        CommandResult result = await Commands.RunAsync("podman",
            ["inspect", "-f", "{{.State.Running}}", handle], TimeSpan.FromSeconds(30), cancel, env: Env).ConfigureAwait(false);
        return result.ExitCode == 0 && result.Output.Trim() == "true";
    }

    /// <inheritdoc/>
    public async Task<bool> SetBalloonAsync(string handle, long targetMiB, CancellationToken cancel)
    {
        string script = Path.Combine(options.Root, "bin", "hmp.py");
        if (!File.Exists(script))
        {
            return false;
        }
        CommandResult result = await Commands.RunAsync("podman",
            ["exec", "-i", handle, "python3", "-", "balloon " + targetMiB.ToString(CultureInfo.InvariantCulture)],
            TimeSpan.FromSeconds(30), cancel, stdin: await File.ReadAllTextAsync(script, Encoding.UTF8, cancel)
                .ConfigureAwait(false), env: Env).ConfigureAwait(false);
        return result.ExitCode == 0;
    }

    /// <inheritdoc/>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    /// <remarks>The container's cgroup comes from podman (<c>{{.State.CgroupPath}}</c>, e.g. /libpod_parent/libpod-&lt;id&gt;);
    /// the coordinator runs as root in the site, so it reads cgroup v1's files under /sys/fs/cgroup/memory itself, or, on
    /// cgroup v2 (the worker machine's CT 803), <see cref="ReadMemoryV2"/>'s. Any other failure (the container gone, a file
    /// missing or unreadable) answers null and is logged.</remarks>
    public async Task<SlotMemoryStats?> ReadMemoryAsync(string handle, CancellationToken cancel)
    {
        try
        {
            CommandResult inspect = await Commands.RunAsync("podman", ["inspect", "-f", "{{.State.CgroupPath}}", handle],
                TimeSpan.FromSeconds(30), cancel, env: Env).ConfigureAwait(false);
            string path = inspect.Output.Trim();
            // The path comes from podman, but it is joined under /sys: never let it climb out.
            if (inspect.ExitCode != 0 || !path.StartsWith('/') || path.Contains("..", StringComparison.Ordinal))
            {
                return null;
            }
            string dir = "/sys/fs/cgroup/memory" + path;
            if (!Directory.Exists("/sys/fs/cgroup/memory") && File.Exists("/sys/fs/cgroup" + path + "/memory.peak"))
            {
                // cgroup v2: one hierarchy, other file names.
                return ReadMemoryV2("/sys/fs/cgroup" + path);
            }
            long swapped = 0;
            foreach (string line in File.ReadLines(Path.Combine(dir, "memory.stat")))
            {
                if (line.StartsWith("total_swap ", StringComparison.Ordinal))
                {
                    swapped = long.Parse(line["total_swap ".Length..], NumberStyles.None, CultureInfo.InvariantCulture);
                }
            }
            const long MiB = 1024 * 1024;
            return new SlotMemoryStats(ReadCount(dir, "memory.max_usage_in_bytes") / MiB,
                ReadCount(dir, "memory.memsw.max_usage_in_bytes") / MiB, swapped / MiB, ReadCount(dir, "memory.failcnt"));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or FormatException or OverflowException
            or InvalidOperationException or System.ComponentModel.Win32Exception or TimeoutException)
        {
            // A report only: a slot whose figures cannot be read is stopped all the same.
            log.LogInformation("slot {Handle}: its memory figures could not be read: {Error}", handle, ex.Message);
            return null;
        }
    }

    /// <summary>Reads one cgroup file that holds a single number.</summary>
    /// <param name="dir">The cgroup's folder.</param>
    /// <param name="file">The file (memory.failcnt, memory.max_usage_in_bytes, …).</param>
    /// <returns>Its number.</returns>
    /// <exception cref="IOException">The file could not be read.</exception>
    /// <exception cref="UnauthorizedAccessException">The file may not be read.</exception>
    /// <exception cref="FormatException">It does not hold a plain number.</exception>
    /// <exception cref="OverflowException">Its number does not fit a long.</exception>
    private static long ReadCount(string dir, string file) =>
        long.Parse(File.ReadAllText(Path.Combine(dir, file)).Trim(), NumberStyles.None, CultureInfo.InvariantCulture);

    /// <summary>A container's memory figures from cgroup v2's files (the worker machine's site; homepc's WSL distro runs
    /// cgroup v1).</summary>
    /// <param name="dir">The container's cgroup folder under /sys/fs/cgroup.</param>
    /// <returns>The peak (<c>memory.peak</c>); the peak with swap as the memory peak plus the swap peak
    /// (<c>memory.swap.peak</c>: v2 keeps no combined peak, so this is an upper bound); the swap in use
    /// (<c>memory.swap.current</c>); and the times the limit was hit (<c>memory.events</c>' "max").</returns>
    /// <exception cref="IOException">A file could not be read.</exception>
    /// <exception cref="UnauthorizedAccessException">A file may not be read.</exception>
    /// <exception cref="FormatException">A file does not hold a plain number.</exception>
    /// <exception cref="OverflowException">A number does not fit a long.</exception>
    /// <remarks>The swap files exist only where the kernel accounts swap: without them both swap figures are 0.</remarks>
    internal static SlotMemoryStats ReadMemoryV2(string dir)
    {
        const long MiB = 1024 * 1024;
        long peak = ReadCount(dir, "memory.peak");
        long swapPeak = File.Exists(Path.Combine(dir, "memory.swap.peak")) ? ReadCount(dir, "memory.swap.peak") : 0;
        long swapNow = File.Exists(Path.Combine(dir, "memory.swap.current")) ? ReadCount(dir, "memory.swap.current") : 0;
        long hits = 0;
        foreach (string line in File.ReadLines(Path.Combine(dir, "memory.events")))
        {
            // "max N": how often the cgroup reached memory.max and the kernel reclaimed (v1's failcnt).
            if (line.StartsWith("max ", StringComparison.Ordinal))
            {
                hits = long.Parse(line["max ".Length..], NumberStyles.None, CultureInfo.InvariantCulture);
            }
        }
        return new SlotMemoryStats(peak / MiB, (peak + swapPeak) / MiB, swapNow / MiB, hits);
    }
}

/// <summary>
/// The tests' slot driver: a "slot" is amb-guest started as a process on this machine, with its share being the
/// exchange folder itself. No VM, no image, no balloon: it proves the coordinator and the guest agent talk correctly.
/// </summary>
public sealed class LocalSlotDriver(CoordinatorOptions options, ILogger<LocalSlotDriver> log) : ISlotDriver
{
    /// <inheritdoc/>
    public string Name => "local";

    /// <inheritdoc/>
    public bool IsVm => false;

    /// <inheritdoc/>
    public string GuestShareRoot(SlotSpec spec) => spec.ExchangeDir;

    /// <inheritdoc/>
    public Task<string> StartAsync(SlotSpec spec, CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(spec);
        Directory.CreateDirectory(spec.SlotDir);
        var info = new ProcessStartInfo(options.GuestAgentPath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        info.ArgumentList.Add("--config");
        info.ArgumentList.Add(Path.Combine(spec.ExchangeDir, "amb", "slot.json"));
        info.ArgumentList.Add("--work");
        info.ArgumentList.Add(spec.SlotDir);
        Process process = Process.Start(info)
            ?? throw new InvalidOperationException($"{options.GuestAgentPath} did not start");
        log.LogInformation("slot {Slot}: local agent pid {Pid}", spec.SlotId, process.Id);
        return Task.FromResult(process.Id.ToString(CultureInfo.InvariantCulture));
    }

    /// <inheritdoc/>
    public Task StopAsync(string handle, bool graceful, CancellationToken cancel)
    {
        try
        {
            using Process process = Process.GetProcessById(int.Parse(handle, CultureInfo.InvariantCulture));
            // A graceful stop of a local agent is its shutdown step (which a test never sends): both cases kill it.
            process.Kill(entireProcessTree: true);
            process.WaitForExit(10_000);
        }
        catch (ArgumentException)
        {
            // Expected: the agent ended already.
        }
        catch (InvalidOperationException)
        {
            // Expected: it ended between the lookup and the kill.
        }
        return Task.CompletedTask;
    }

    /// <inheritdoc/>
    public Task<bool> IsAliveAsync(string handle, CancellationToken cancel)
    {
        try
        {
            using Process process = Process.GetProcessById(int.Parse(handle, CultureInfo.InvariantCulture));
            return Task.FromResult(!process.HasExited);
        }
        catch (ArgumentException)
        {
            return Task.FromResult(false);
        }
    }

    /// <inheritdoc/>
    public Task<bool> SetBalloonAsync(string handle, long targetMiB, CancellationToken cancel) => Task.FromResult(false);
}
