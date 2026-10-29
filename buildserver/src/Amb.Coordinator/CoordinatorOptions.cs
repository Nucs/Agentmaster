using System.Globalization;
using Amb.Core.Allocation;

namespace Amb.Coordinator;

/// <summary>
/// The coordinator's settings, read from environment variables only (like fastrund), each with the default the site
/// uses, so the start script sets nothing in the ordinary case.
/// </summary>
public sealed record CoordinatorOptions
{
    /// <summary>The site's root folder (<c>AMB_ROOT</c>; default /srv/buildserver): records, store, slots, images,
    /// exchange and bin live under it (DESIGN.md §8.1).</summary>
    public required string Root { get; init; }

    /// <summary>The address the API listens on (<c>AMB_LISTEN</c>; default http://0.0.0.0:2207). IPv4 on purpose:
    /// WSL's localhost forwarding passes only IPv4 listeners, so Windows reaches 127.0.0.1:2207 (DESIGN.md §16.9).</summary>
    public required string Listen { get; init; }

    /// <summary>The coordinator's URL as a slot's guest reaches it (<c>AMB_GUEST_URL</c>; default http://10.0.2.2:2207:
    /// slirp4netns maps 10.0.2.2 to the site distro's loopback when the slot runs with allow_host_loopback).</summary>
    public required string GuestUrl { get; init; }

    /// <summary>The slot driver (<c>AMB_SLOT_DRIVER</c>): "podman" (dockur VMs, the stand-in for fastrun's VM slots) or
    /// "local" (amb-guest started as a process on this machine: the tests' slot, no VM).</summary>
    public required string SlotDriver { get; init; }

    /// <summary>The host-facts helper and its argument (<c>AMB_HOSTFACTS</c>; default
    /// /mnt/q/buildserver/bin/amb-hostfacts.exe), run every 10 s through WSL's interop; empty = no host facts (tests).</summary>
    public required string HostFactsCommand { get; init; }

    /// <summary>The guest agent copied into every slot's share (<c>AMB_GUEST_AGENT</c>; default
    /// &lt;root&gt;/bin/amb-guest.exe).</summary>
    public required string GuestAgentPath { get; init; }

    /// <summary>The dockur image the podman driver runs (<c>AMB_DOCKUR_IMAGE</c>; default docker.io/dockurr/windows:6.05,
    /// the version every measurement used).</summary>
    public required string DockurImage { get; init; }

    /// <summary>Minutes with no active request and no slot after which the coordinator exits
    /// (<c>AMB_IDLE_EXIT_MINUTES</c>; default 10, 0 = never): the site is lazy, and the distro stops when its last process
    /// ends (DESIGN.md §5.2).</summary>
    public required int IdleExitMinutes { get; init; }

    /// <summary>The e2e suite an e2e request runs when it names none (<c>AMB_E2E_SOURCE</c>; default
    /// /mnt/k/source/Agentmaster/e2e: the repository's own e2e folder, as WSL's automount shows it).</summary>
    public required string E2eSourceDir { get; init; }

    /// <summary>The repository builds are made from (<c>AMB_SOURCE_REPO</c>; default /mnt/k/source/Agentmaster, the
    /// owner's checkout as WSL's automount shows it). Only read: the site's bare mirror fetches its branches and tags from
    /// it, so a build never sees uncommitted work and nothing is ever written to K: (DESIGN.md §11).</summary>
    public string SourceRepo { get; init; } = "/mnt/k/source/Agentmaster";

    /// <summary>The image build VMs boot (<c>AMB_BUILD_IMAGE</c>; default am-build).</summary>
    public string BuildImage { get; init; } = "am-build";

    /// <summary>The kept build VM's name: one VM, so builds run one at a time (the scheduler holds the next one).</summary>
    public string BuildVmName { get; init; } = "build-1";

    /// <summary>The build VM's raw data disk, in GiB (<c>AMB_BUILD_DATA_DISK_GIB</c>; default 128). A sparse file: only what
    /// the guest writes takes space (a Release tree with its caches ~25-40 GB, DESIGN.md §8.2), and what it deletes is
    /// returned (discard=unmap).</summary>
    public int BuildDataDiskGiB { get; init; } = 128;

    /// <summary>The build job's guest scripts, deployed with the coordinator (<c>&lt;root&gt;/bin/steps/build</c>, from the
    /// repository's buildserver/steps/build): staged into every build slot's share.</summary>
    public string BuildStepsDir => Path.Combine(Root, "bin", "steps", "build");

    /// <summary>The folder of kept build VMs: each VM's overlay, UEFI files and raw data disk.</summary>
    public string VmsDir => Path.Combine(Root, "vms");

    /// <summary>The folder of the bare mirrors (the repository and its submodule) builds are bundled from.</summary>
    public string GitDir => Path.Combine(Root, "git");

    /// <summary>The deploy job's Windows-side script, deployed with the coordinator (<c>&lt;root&gt;/bin/steps/install</c>,
    /// from the repository's buildserver/steps/install).</summary>
    public string InstallStepsDir => Path.Combine(Root, "bin", "steps", "install");

    /// <summary>PowerShell 7 on Windows as the site sees it (<c>AMB_WINDOWS_PWSH</c>; default
    /// /mnt/c/Program Files/PowerShell/7/pwsh.exe): the deploy job runs its Windows side with it through WSL's interop
    /// (Swap-AgentmasterLayout.ps1 requires PowerShell 7).</summary>
    public string WindowsPwsh { get; init; } = "/mnt/c/Program Files/PowerShell/7/pwsh.exe";

    /// <summary>Where a deploy stages its files on Windows (<c>AMB_INSTALL_STAGE</c>; default Q:\buildserver\install): one
    /// folder per request (the layout zip, the scripts, the log), removed when the request ends.</summary>
    public string InstallStage { get; init; } = @"Q:\buildserver\install";

    /// <summary>The two layout folders of the dev instance (<c>AMB_DEV_SLOTS</c>, ';'-separated; default
    /// Q:\agentmaster-layouts\Debug;Q:\agentmaster-layouts\Debug-B): an install goes into the one it does not run from
    /// (CLAUDE.md "Hot deploy").</summary>
    public IReadOnlyList<string> DevSlots { get; init; } = [@"Q:\agentmaster-layouts\Debug", @"Q:\agentmaster-layouts\Debug-B"];

    /// <summary>The two layout folders of the production instance (<c>AMB_PRODUCTION_SLOTS</c>; default
    /// Q:\agentmaster-layouts\Release;Q:\agentmaster-layouts\Release-B).</summary>
    public IReadOnlyList<string> ProductionSlots { get; init; } = [@"Q:\agentmaster-layouts\Release", @"Q:\agentmaster-layouts\Release-B"];

    /// <summary>The ref of the site's mirror whose tools\Swap-AgentmasterLayout.ps1 and tools\am-lock.sh a deploy runs
    /// (<c>AMB_INSTALL_SCRIPTS_REF</c>; default refs/heads/agentmaster): the newest COMMITTED copies, never the working
    /// tree's (another session's half-done edit must not run a swap), and never a build's own older copy (an install of
    /// an old build still gets today's rollback fixes).</summary>
    public string InstallScriptsRef { get; init; } = "refs/heads/agentmaster";

    /// <summary>homepc's reserves (DESIGN.md §3.2).</summary>
    public HomepcReserves Reserves { get; init; } = new();

    /// <summary>homepc's priority (E1; dock's 2.0 is set where dock is added).</summary>
    public double HomepcPriority { get; init; } = 1.0;

    /// <summary>The records' folder.</summary>
    public string RecordsDir => Path.Combine(Root, "records");

    /// <summary>The artifact store's folder.</summary>
    public string StoreDir => Path.Combine(Root, "store");

    /// <summary>The folder of slots' disks.</summary>
    public string SlotsDir => Path.Combine(Root, "slots");

    /// <summary>The folder of read-only images.</summary>
    public string ImagesDir => Path.Combine(Root, "images");

    /// <summary>The folder of slots' shares.</summary>
    public string ExchangeDir => Path.Combine(Root, "exchange");

    /// <summary>The admin token file (created at first start, mode 0600 on Linux).</summary>
    public string AdminTokenFile => Path.Combine(Root, "coordinator", "admin.token");

    /// <summary>Reads the settings from the environment.</summary>
    /// <param name="env">Reads one variable (Environment.GetEnvironmentVariable in production; a dictionary in tests).</param>
    /// <returns>The settings.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="env"/> is null.</exception>
    /// <exception cref="FormatException">A numeric variable is not a number.</exception>
    public static CoordinatorOptions FromEnvironment(Func<string, string?> env)
    {
        ArgumentNullException.ThrowIfNull(env);
        string root = env("AMB_ROOT") is { Length: > 0 } r ? r : "/srv/buildserver";
        return new CoordinatorOptions
        {
            Root = root,
            Listen = env("AMB_LISTEN") is { Length: > 0 } l ? l : "http://0.0.0.0:2207",
            GuestUrl = env("AMB_GUEST_URL") is { Length: > 0 } g ? g : "http://10.0.2.2:2207",
            SlotDriver = env("AMB_SLOT_DRIVER") is { Length: > 0 } d ? d : "podman",
            HostFactsCommand = env("AMB_HOSTFACTS") ?? "/mnt/q/buildserver/bin/amb-hostfacts.exe",
            GuestAgentPath = env("AMB_GUEST_AGENT") is { Length: > 0 } a ? a : Path.Combine(root, "bin", "amb-guest.exe"),
            DockurImage = env("AMB_DOCKUR_IMAGE") is { Length: > 0 } i ? i : "docker.io/dockurr/windows:6.05",
            E2eSourceDir = env("AMB_E2E_SOURCE") is { Length: > 0 } e ? e : "/mnt/k/source/Agentmaster/e2e",
            IdleExitMinutes = env("AMB_IDLE_EXIT_MINUTES") is { Length: > 0 } m
                ? int.Parse(m, NumberStyles.None, CultureInfo.InvariantCulture)
                : 10,
            SourceRepo = env("AMB_SOURCE_REPO") is { Length: > 0 } s ? s : "/mnt/k/source/Agentmaster",
            BuildImage = env("AMB_BUILD_IMAGE") is { Length: > 0 } b ? b : "am-build",
            BuildDataDiskGiB = env("AMB_BUILD_DATA_DISK_GIB") is { Length: > 0 } dd
                ? int.Parse(dd, NumberStyles.None, CultureInfo.InvariantCulture)
                : 128,
            WindowsPwsh = env("AMB_WINDOWS_PWSH") is { Length: > 0 } wp ? wp : "/mnt/c/Program Files/PowerShell/7/pwsh.exe",
            InstallStage = env("AMB_INSTALL_STAGE") is { Length: > 0 } st ? st : @"Q:\buildserver\install",
            DevSlots = SlotPair(env("AMB_DEV_SLOTS"), @"Q:\agentmaster-layouts\Debug", @"Q:\agentmaster-layouts\Debug-B"),
            ProductionSlots = SlotPair(env("AMB_PRODUCTION_SLOTS"), @"Q:\agentmaster-layouts\Release", @"Q:\agentmaster-layouts\Release-B"),
            InstallScriptsRef = env("AMB_INSTALL_SCRIPTS_REF") is { Length: > 0 } sr ? sr : "refs/heads/agentmaster",
        };
    }

    /// <summary>Reads an instance's two layout folders from a ';'-separated variable.</summary>
    /// <param name="value">The variable's value, or null when unset.</param>
    /// <param name="first">The default first folder.</param>
    /// <param name="second">The default second folder.</param>
    /// <returns>The two folders.</returns>
    /// <exception cref="FormatException">The variable names other than exactly two folders: an install needs a pair, the
    /// folder the instance runs from and the one it moves to.</exception>
    private static IReadOnlyList<string> SlotPair(string? value, string first, string second)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return [first, second];
        }
        string[] parts = value.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        return parts.Length == 2 ? parts : throw new FormatException($"expected two ';'-separated layout folders, not '{value}'");
    }
}
