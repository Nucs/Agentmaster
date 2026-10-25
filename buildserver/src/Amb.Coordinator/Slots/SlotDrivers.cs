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
public sealed record SlotSpec(string SlotId, string Image, Resources Grant, string SlotDir, string ExchangeDir,
    string ImageDir, string? InstallVersion = null, string? OemDir = null);

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
/// default); the coordinator sets it through QEMU's monitor (<c>hmp.py</c>).</item>
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
            "-e", "RAM_SIZE=" + spec.Grant.MemoryMiB.ToString(CultureInfo.InvariantCulture) + "M",
            "-e", "CPU_CORES=" + spec.Grant.Vcpus.ToString(CultureInfo.InvariantCulture),
            "-e", "DISK_FMT=qcow2",
            "-e", "NETWORK=user",
            "-e", "TZ=UTC",
            "-e", "ARGUMENTS=-device virtio-balloon-pci,id=balloon0,deflate-on-oom=on",
        ];
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
            await PrepareOverlayAsync(spec, cancel).ConfigureAwait(false);
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
