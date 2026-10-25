namespace Amb.Core.Allocation;

/// <summary>
/// What homepc's site measures of itself at one moment (DESIGN.md §3.2): the host agent reports the Windows side, the
/// distro reports the WSL side, and the coordinator adds what its own VMs' balloons could give back.
/// </summary>
/// <param name="WindowsAvailableMiB">Windows' available physical memory (Win32_OperatingSystem.FreePhysicalMemory). The
/// limit that bites on this PC: 2.5-8.9 GB during the tests of 2026-10-25.</param>
/// <param name="WslMemAvailableMiB">MemAvailable of WSL's one Linux VM (31.2 GB, shared with Docker Desktop and every
/// other session's VMs).</param>
/// <param name="FreeCommitMiB">Windows' free commit (FreeVirtualMemory): 33-44 GB during the tests, so it binds last.</param>
/// <param name="ReclaimableBalloonMiB">What the site's running VMs could give back now: each one's balloon target minus
/// its minimum, when its guest is not using that memory.</param>
/// <param name="RunningSlotVcpus">The vCPUs granted to the slots that run now.</param>
/// <param name="QFreeMiB">Free space on Q:, where the site's disk and store live.</param>
/// <param name="FullScreenGame">A full-screen game runs (QUNS_RUNNING_D3D_FULL_SCREEN): new slots wait.</param>
public sealed record HomepcFacts(
    long WindowsAvailableMiB,
    long WslMemAvailableMiB,
    long FreeCommitMiB,
    long ReclaimableBalloonMiB,
    int RunningSlotVcpus,
    long QFreeMiB,
    bool FullScreenGame);

/// <summary>
/// The amounts homepc keeps for everything else (DESIGN.md §3.2), set from the measurements of 2026-10-25.
/// </summary>
/// <param name="WindowsMiB">Windows' available memory kept for the owner's own work (6 GB: Windows had 2.5 GB available
/// while three VMs ran during the tests).</param>
/// <param name="WslMiB">WSL's MemAvailable kept for Docker Desktop and other sessions' VMs in the same Linux VM (2 GB).</param>
/// <param name="CommitMiB">Free commit kept (16 GB): the page file can grow, but never into the last of it.</param>
/// <param name="UsableVcpus">vCPUs the site may hand out in total (24 of the 32 threads).</param>
/// <param name="DiskMiB">Q: space kept for other work (15 GB: other sessions' builds and e2e runs write 4-20 GB at a
/// time on Q:).</param>
public sealed record HomepcReserves(
    long WindowsMiB = 6 * 1024,
    long WslMiB = 2 * 1024,
    long CommitMiB = 16 * 1024,
    int UsableVcpus = 24,
    long DiskMiB = 15 * 1024);

/// <summary>What dock's fastrund reports of its node (DESIGN.md §3.2, §9 F7).</summary>
/// <param name="MemAvailableMiB">MemAvailable of the node (6.0-7.1 GB all week, median 6.3 GB).</param>
/// <param name="RunningSlotVcpus">The vCPUs granted to the slots that run there now.</param>
/// <param name="DiskFreeMiB">The node's free disk.</param>
/// <param name="TakesWindowsSlots">Whether E3's changes exist there (/dev/kvm, the pod's memory limit, the disk); until
/// then dock takes no Windows slot.</param>
public sealed record DockFacts(long MemAvailableMiB, int RunningSlotVcpus, long DiskFreeMiB, bool TakesWindowsSlots);

/// <summary>The amounts dock keeps (DESIGN.md §3.2).</summary>
/// <param name="MemoryMiB">MemAvailable kept for the node's own work (2 GB).</param>
/// <param name="UsableVcpus">vCPUs the site may hand out (12 of 16: dock ran at 2-3 % all week).</param>
/// <param name="DiskMiB">Disk kept (5 GB; the thin pool's alert is at 80 %).</param>
public sealed record DockReserves(long MemoryMiB = 2 * 1024, int UsableVcpus = 12, long DiskMiB = 5 * 1024);

/// <summary>Turns what a site measures into what it has free for new slots: the measured value minus its reserve.</summary>
public static class SiteCapacity
{
    /// <summary>The name homepc's site goes by in records and placements.</summary>
    public const string Homepc = "homepc";

    /// <summary>The name dock's site goes by.</summary>
    public const string Dock = "dock";

    /// <summary>homepc's free amounts for one request (DESIGN.md §3.2).</summary>
    /// <param name="facts">What the site measured.</param>
    /// <param name="reserves">What it keeps.</param>
    /// <param name="priority">The site's priority (1.0 by default).</param>
    /// <param name="tolerant">Whether the request may be admitted on commit alone, beyond Windows' available memory (an
    /// idle build VM kept warm, a VM ballooned down to its minimum): its cold pages then live in the page file. A running
    /// build or e2e is never tolerant. This is the owner's "utilize committed memory".</param>
    /// <returns>The site's free amounts and its holds.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="facts"/> or <paramref name="reserves"/> is null.</exception>
    /// <remarks>
    /// Memory = min(Windows available − 6 GB, WSL MemAvailable − 2 GB, free commit − 16 GB) + what the site's own
    /// balloons could give back; a tolerant request leaves out the first term. Every term must hold at once: a VM's
    /// memory comes out of the WSL VM (whose growth comes out of Windows' memory) and is charged to commit.
    /// </remarks>
    public static SiteFree HomepcFree(HomepcFacts facts, HomepcReserves reserves, double priority = 1.0,
        bool tolerant = false)
    {
        ArgumentNullException.ThrowIfNull(facts);
        ArgumentNullException.ThrowIfNull(reserves);
        long wsl = facts.WslMemAvailableMiB - reserves.WslMiB;
        long commit = facts.FreeCommitMiB - reserves.CommitMiB;
        long memory = Math.Min(wsl, commit);
        if (!tolerant)
        {
            memory = Math.Min(memory, facts.WindowsAvailableMiB - reserves.WindowsMiB);
        }
        // The balloons' reclaimable memory is added after the minimum: it is memory this site holds already and can
        // hand to a new slot without asking Windows for more.
        memory += Math.Max(facts.ReclaimableBalloonMiB, 0);
        var free = new Resources(memory, reserves.UsableVcpus - facts.RunningSlotVcpus, facts.QFreeMiB - reserves.DiskMiB);
        IReadOnlyList<string> holds = facts.FullScreenGame ? ["full-screen game"] : [];
        return new SiteFree(Homepc, priority, free, holds);
    }

    /// <summary>dock's free amounts for one request (DESIGN.md §3.2).</summary>
    /// <param name="facts">What dock's fastrund reported.</param>
    /// <param name="reserves">What dock keeps.</param>
    /// <param name="windowsSlot">Whether the request needs a Windows VM (refused until E3).</param>
    /// <param name="priority">The site's priority (2.0 by default: E1, dock first).</param>
    /// <returns>The site's free amounts and its holds.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="facts"/> or <paramref name="reserves"/> is null.</exception>
    public static SiteFree DockFree(DockFacts facts, DockReserves reserves, bool windowsSlot, double priority = 2.0)
    {
        ArgumentNullException.ThrowIfNull(facts);
        ArgumentNullException.ThrowIfNull(reserves);
        var free = new Resources(facts.MemAvailableMiB - reserves.MemoryMiB,
            reserves.UsableVcpus - facts.RunningSlotVcpus, facts.DiskFreeMiB - reserves.DiskMiB);
        IReadOnlyList<string> holds = windowsSlot && !facts.TakesWindowsSlots
            ? ["no Windows slots before E3"]
            : [];
        return new SiteFree(Dock, priority, free, holds);
    }
}
