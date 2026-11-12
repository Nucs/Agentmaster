using System.Globalization;
using Amb.Core.Allocation;
using Amb.Core.Model;

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

    /// <summary>How long dockur may wait for a slot's firmware to start booting Windows before it kills QEMU, in seconds
    /// (<c>AMB_DOCKUR_BOOT_WAIT</c>, 30-1800; unset or empty = dockur's own 30 s, which homepc's site keeps).</summary>
    /// <remarks>dockur 6.05 hard-codes the 30 s (<c>waitForBoot "$pid" 30 &amp;</c> in its /run/entry.sh). The worker
    /// machine's UEFI firmware needed 20 s to reach Windows' boot manager on a busy host, and more than 30 s twice on
    /// 2026-11-12, so those slots "stopped by themselves" a minute in; its site sets 180. When set, every slot container
    /// patches that one line before dockur starts, and refuses to start when the line is not there (a changed
    /// <see cref="DockurImage"/>): <see cref="Slots.PodmanSlotDriver.BootWaitScript"/>.</remarks>
    public int? DockurBootWaitSeconds { get; init; }

    /// <summary>How long a slot's guest agent may make no call before its slot counts as lost
    /// (<c>AMB_GUEST_SILENCE_MINUTES</c>, 1-60; default 3, homepc's).</summary>
    /// <remarks>The agent calls every 10 s (its heartbeat), so 3 minutes is many missed calls on a site whose slots reach
    /// the coordinator reliably. On the worker machine, during a cold build that shared drive 2 with other guests, the
    /// agent's calls failed on and off from 23:04 to 23:13 (connection errors and 60-s timeouts in its own log) while the
    /// VM went on compiling, and the 3-minute limit killed the build at 23:15 (2026-11-12); its site sets 10. A longer
    /// limit only delays the verdict on a slot that really died: the liveness watch still ends a stopped VM within two
    /// checks (SlotManager.WatchInterval).</remarks>
    public TimeSpan GuestSilence { get; init; } = TimeSpan.FromMinutes(3);

    /// <summary>Minutes with no active request and no slot after which the coordinator exits
    /// (<c>AMB_IDLE_EXIT_MINUTES</c>; default 10, 0 = never): the site is lazy, and the distro stops when its last process
    /// ends (DESIGN.md §5.2).</summary>
    public required int IdleExitMinutes { get; init; }

    /// <summary>The e2e suite an e2e request runs when it names none (<c>AMB_E2E_SOURCE</c>; default
    /// /mnt/k/source/Agentmaster/e2e: the repository's own e2e folder, as WSL's automount shows it). Set EMPTY on a site
    /// that cannot read K: (the worker machine's): an e2e request must then name <see cref="RequestInputs.SuiteCommit"/>,
    /// whose e2e folder comes out of the site's mirror (`amb` pushes that commit first).</summary>
    public required string E2eSourceDir { get; init; }

    /// <summary>The repository builds are made from (<c>AMB_SOURCE_REPO</c>; default /mnt/k/source/Agentmaster, the
    /// owner's checkout as WSL's automount shows it). Only read: the site's bare mirror fetches its branches and tags from
    /// it, so a build never sees uncommitted work and nothing is ever written to K: (DESIGN.md §11). Set EMPTY on a site
    /// that cannot read K: (the worker machine's): its mirror then takes commits only by push
    /// (<see cref="PushesSource"/>).</summary>
    public string SourceRepo { get; init; } = "/mnt/k/source/Agentmaster";

    /// <summary>Whether the site's mirror takes commits only by push (<c>POST /api/v1/source/bundle</c>): true when
    /// <see cref="SourceRepo"/> is empty. Then a build or an e2e suite of a commit the mirror lacks fails with "push it
    /// first"; `amb` pushes before it asks, so only an older amb (or a failed push) meets that.</summary>
    public bool PushesSource => string.IsNullOrEmpty(SourceRepo);

    /// <summary>The site's name (<c>AMB_SITE_NAME</c>; default "homepc"): placements and slot records carry it, and
    /// `amb status` prints it. "worker" on the worker machine.</summary>
    public string SiteName { get; init; } = SiteCapacity.Homepc;

    /// <summary>The letter this site's build ids start with (<c>AMB_BUILD_LETTER</c>; default "b"; "w" on the worker
    /// machine). Two sites never share a letter: a build pulled from one site into the other's store keeps its id, and
    /// the CLI tells by the letter which site holds a build (<see cref="Ids.BuildLetterOf"/>).</summary>
    public string BuildLetter { get; init; } = "b";

    /// <summary>The HOST's /proc/meminfo as a Linux site's container sees it, bind-mounted read-only
    /// (<c>AMB_HOST_MEMINFO</c>; default empty). Empty: the site is homepc's, sized by Windows' facts
    /// (<see cref="HostFactsCommand"/>). Set: the site is a Linux one (the worker machine's CT 803), sized by
    /// <see cref="SiteCapacity.LinuxFree"/> from this file, the container's own /proc/meminfo (lxcfs: its memory ceiling)
    /// and the free space under <see cref="Root"/>.</summary>
    public string HostMemInfoPath { get; init; } = "";

    /// <summary>Whether the site is a Linux one (<see cref="HostMemInfoPath"/> is set).</summary>
    public bool IsLinuxSite => !string.IsNullOrEmpty(HostMemInfoPath);

    /// <summary>A Linux site's reserves (<c>AMB_RESERVE_HOST_MEMORY_MIB</c>, <c>AMB_RESERVE_SITE_MEMORY_MIB</c>,
    /// <c>AMB_USABLE_VCPUS</c>, <c>AMB_RESERVE_DISK_MIB</c>; each defaults to <see cref="LinuxReserves"/>'s own).</summary>
    public LinuxReserves LinuxReserves { get; init; } = new();

    /// <summary>The site's own slot sizes for builds and e2e runs (<c>AMB_BUILD_MEMORY_MIB</c>, <c>AMB_BUILD_VCPUS</c>,
    /// <c>AMB_E2E_MEMORY_MIB</c>, <c>AMB_E2E_VCPUS</c> as "least:most", and <c>AMB_E2E_GUEST_MEMORY_MIB</c>); unset ones
    /// keep <see cref="RequestDefaults"/>, which homepc's measurements set. <c>AMB_CAPPED_SLOTS=0</c> says the site has no
    /// swap file for capped slots (<see cref="RequestSizing.CappedSlots"/>).</summary>
    public RequestSizing Sizing { get; init; } = new();

    /// <summary>The site's scratch folder (bundles being received, e2e suites exported from the mirror): on the site's own
    /// disk, emptied of a request's files when it ends.</summary>
    public string TmpDir => Path.Combine(Root, "tmp");

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
    /// (Swap-AgentmasterLayout.ps1 requires PowerShell 7). Set EMPTY on a site with no Windows beside it (the worker
    /// machine's): a deploy request there fails at once, and installs go through the home site, which pulls the build
    /// first.</summary>
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

    /// <summary>homepc's priority (E1; dock's 2.0 is set where dock is added). A Linux site (the worker machine's) scores
    /// its one site with the same value: a coordinator serves one site, so the number only scales the score in the
    /// placement events.</summary>
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
    /// <exception cref="FormatException">A numeric variable is not a number, a "least:most" pair is malformed,
    /// <c>AMB_BUILD_LETTER</c> is not a build-id letter, <c>AMB_DOCKUR_BOOT_WAIT</c> is not 30-1800 seconds, or
    /// <c>AMB_GUEST_SILENCE_MINUTES</c> is not 1-60 minutes.</exception>
    /// <remarks>Three variables tell UNSET from EMPTY: <c>AMB_SOURCE_REPO</c>, <c>AMB_E2E_SOURCE</c> and
    /// <c>AMB_WINDOWS_PWSH</c> (and <c>AMB_HOSTFACTS</c>, as before). Unset keeps homepc's default; set to "" turns the
    /// feature off, which is how the worker machine's site says it cannot read K: or reach Windows.</remarks>
    public static CoordinatorOptions FromEnvironment(Func<string, string?> env)
    {
        ArgumentNullException.ThrowIfNull(env);
        string root = env("AMB_ROOT") is { Length: > 0 } r ? r : "/srv/buildserver";
        string letter = env("AMB_BUILD_LETTER") is { Length: > 0 } bl ? bl : "b";
        if (!Ids.IsBuildLetter(letter))
        {
            throw new FormatException($"AMB_BUILD_LETTER must be one lowercase letter other than r and s, not '{letter}'");
        }
        var defaults = new LinuxReserves();
        return new CoordinatorOptions
        {
            Root = root,
            Listen = env("AMB_LISTEN") is { Length: > 0 } l ? l : "http://0.0.0.0:2207",
            GuestUrl = env("AMB_GUEST_URL") is { Length: > 0 } g ? g : "http://10.0.2.2:2207",
            SlotDriver = env("AMB_SLOT_DRIVER") is { Length: > 0 } d ? d : "podman",
            HostFactsCommand = env("AMB_HOSTFACTS") ?? "/mnt/q/buildserver/bin/amb-hostfacts.exe",
            GuestAgentPath = env("AMB_GUEST_AGENT") is { Length: > 0 } a ? a : Path.Combine(root, "bin", "amb-guest.exe"),
            DockurImage = env("AMB_DOCKUR_IMAGE") is { Length: > 0 } i ? i : "docker.io/dockurr/windows:6.05",
            DockurBootWaitSeconds = BootWait(env("AMB_DOCKUR_BOOT_WAIT")),
            GuestSilence = Silence(env("AMB_GUEST_SILENCE_MINUTES")),
            // Set but empty is "no default suite" (the worker machine's site): e2e suites then come from the mirror.
            E2eSourceDir = env("AMB_E2E_SOURCE") is { } e ? e : "/mnt/k/source/Agentmaster/e2e",
            IdleExitMinutes = env("AMB_IDLE_EXIT_MINUTES") is { Length: > 0 } m
                ? int.Parse(m, NumberStyles.None, CultureInfo.InvariantCulture)
                : 10,
            // Set but empty is push mode (PushesSource): the site has no repository to fetch from.
            SourceRepo = env("AMB_SOURCE_REPO") is { } s ? s : "/mnt/k/source/Agentmaster",
            BuildImage = env("AMB_BUILD_IMAGE") is { Length: > 0 } b ? b : "am-build",
            BuildDataDiskGiB = env("AMB_BUILD_DATA_DISK_GIB") is { Length: > 0 } dd
                ? int.Parse(dd, NumberStyles.None, CultureInfo.InvariantCulture)
                : 128,
            // Set but empty is "no Windows here": deploys are refused (installs go through the home site).
            WindowsPwsh = env("AMB_WINDOWS_PWSH") is { } wp ? wp : "/mnt/c/Program Files/PowerShell/7/pwsh.exe",
            InstallStage = env("AMB_INSTALL_STAGE") is { Length: > 0 } st ? st : @"Q:\buildserver\install",
            DevSlots = SlotPair(env("AMB_DEV_SLOTS"), @"Q:\agentmaster-layouts\Debug", @"Q:\agentmaster-layouts\Debug-B"),
            ProductionSlots = SlotPair(env("AMB_PRODUCTION_SLOTS"), @"Q:\agentmaster-layouts\Release", @"Q:\agentmaster-layouts\Release-B"),
            InstallScriptsRef = env("AMB_INSTALL_SCRIPTS_REF") is { Length: > 0 } sr ? sr : "refs/heads/agentmaster",
            SiteName = env("AMB_SITE_NAME") is { Length: > 0 } sn ? sn : SiteCapacity.Homepc,
            BuildLetter = letter,
            HostMemInfoPath = env("AMB_HOST_MEMINFO") ?? "",
            LinuxReserves = new LinuxReserves(
                Number(env("AMB_RESERVE_HOST_MEMORY_MIB"), defaults.HostMemoryMiB),
                Number(env("AMB_RESERVE_SITE_MEMORY_MIB"), defaults.SiteMemoryMiB),
                (int)Number(env("AMB_USABLE_VCPUS"), defaults.UsableVcpus),
                Number(env("AMB_RESERVE_DISK_MIB"), defaults.DiskMiB)),
            Sizing = new RequestSizing
            {
                Build = Sized(RequestDefaults.ResourcesOf(RequestKind.Build)!, env("AMB_BUILD_MEMORY_MIB"), env("AMB_BUILD_VCPUS")),
                E2e = Sized(RequestDefaults.ResourcesOf(RequestKind.E2e)!, env("AMB_E2E_MEMORY_MIB"), env("AMB_E2E_VCPUS")),
                E2eGuestMemoryMiB = env("AMB_E2E_GUEST_MEMORY_MIB") is { Length: > 0 } gm
                    ? long.Parse(gm, NumberStyles.None, CultureInfo.InvariantCulture)
                    : null,
                // "0" on a site without a swap file for its slots (the worker machine's): capped requests are refused.
                CappedSlots = env("AMB_CAPPED_SLOTS") is not "0",
            },
        };
    }

    /// <summary>Reads a whole non-negative number from a variable, or gives the default when it is unset or empty.</summary>
    /// <param name="value">The variable's value.</param>
    /// <param name="fallback">The default.</param>
    /// <returns>The number.</returns>
    /// <exception cref="FormatException">The value is not a whole non-negative number.</exception>
    private static long Number(string? value, long fallback) =>
        string.IsNullOrWhiteSpace(value) ? fallback : long.Parse(value.Trim(), NumberStyles.None, CultureInfo.InvariantCulture);

    /// <summary>Reads <c>AMB_GUEST_SILENCE_MINUTES</c> (<see cref="GuestSilence"/>).</summary>
    /// <param name="value">The variable's value.</param>
    /// <returns>3 minutes when it is unset or empty; else its minutes.</returns>
    /// <exception cref="FormatException">It is not a whole number from 1 to 60: under a minute a few missed heartbeats
    /// would end a healthy slot, and past an hour a dead guest would hold its slot as long.</exception>
    private static TimeSpan Silence(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return TimeSpan.FromMinutes(3);
        }
        return int.TryParse(value.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out int minutes)
            && minutes is >= 1 and <= 60
            ? TimeSpan.FromMinutes(minutes)
            : throw new FormatException($"AMB_GUEST_SILENCE_MINUTES must be whole minutes from 1 to 60, not '{value}'");
    }

    /// <summary>Reads <c>AMB_DOCKUR_BOOT_WAIT</c> (<see cref="DockurBootWaitSeconds"/>).</summary>
    /// <param name="value">The variable's value.</param>
    /// <returns>Null when it is unset or empty (dockur's own 30 s); else the seconds.</returns>
    /// <exception cref="FormatException">It is not a whole number from 30 to 1800: a wait below dockur's own would make
    /// slow boots fail sooner, and one past half an hour would only hide a firmware that hangs.</exception>
    private static int? BootWait(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }
        return int.TryParse(value.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out int seconds)
            && seconds is >= 30 and <= 1800
            ? seconds
            : throw new FormatException($"AMB_DOCKUR_BOOT_WAIT must be whole seconds from 30 to 1800, not '{value}'");
    }

    /// <summary>A kind's slot with its memory and vCPUs replaced by "least:most" variables (a single number is both).</summary>
    /// <param name="basis">The kind's homepc default (its disk is kept).</param>
    /// <param name="memory">The memory pair in MiB, or null / empty to keep the default's.</param>
    /// <param name="vcpus">The vCPU pair, or null / empty to keep the default's.</param>
    /// <returns>Null when neither variable is set (the default stands, and the record never holds a copy of it); else the
    /// sized request.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="basis"/> is null.</exception>
    /// <exception cref="FormatException">A pair is not one or two whole numbers, or its least is above its most.</exception>
    internal static ResourceRequest? Sized(ResourceRequest basis, string? memory, string? vcpus)
    {
        ArgumentNullException.ThrowIfNull(basis);
        if (string.IsNullOrWhiteSpace(memory) && string.IsNullOrWhiteSpace(vcpus))
        {
            return null;
        }
        (long leastMemory, long mostMemory) = Pair(memory, basis.Min.MemoryMiB, basis.Preferred.MemoryMiB);
        (long leastVcpus, long mostVcpus) = Pair(vcpus, basis.Min.Vcpus, basis.Preferred.Vcpus);
        return new ResourceRequest(
            basis.Min with { MemoryMiB = leastMemory, Vcpus = (int)leastVcpus },
            basis.Preferred with { MemoryMiB = mostMemory, Vcpus = (int)mostVcpus });
    }

    /// <summary>Reads "least:most" (or one number for both).</summary>
    /// <param name="value">The text, or null / empty for the defaults.</param>
    /// <param name="least">The default least.</param>
    /// <param name="most">The default most.</param>
    /// <returns>The pair.</returns>
    /// <exception cref="FormatException">The text is malformed, a number is 0, or least is above most.</exception>
    private static (long Least, long Most) Pair(string? value, long least, long most)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return (least, most);
        }
        string[] parts = value.Split(':', StringSplitOptions.TrimEntries);
        if (parts.Length is < 1 or > 2)
        {
            throw new FormatException($"expected 'least:most' or one number, not '{value}'");
        }
        long a = long.Parse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture);
        long b = parts.Length == 2 ? long.Parse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture) : a;
        // A zero would admit a slot with no memory or no vCPU, and a least above the most could never be granted.
        return a > 0 && a <= b ? (a, b) : throw new FormatException($"'{value}' needs 0 < least <= most");
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
