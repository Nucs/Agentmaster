using Amb.Core.Allocation;
using Xunit;

namespace Amb.Core.Tests;

/// <summary>The reserves of DESIGN.md §3.2, checked against the numbers measured on 2026-10-25 (§3.6).</summary>
public sealed class SiteCapacityTests
{
    /// <summary>
    /// §3.6: Windows available 8.6 GB, the WSL VM's MemAvailable 11.1 GB, free commit 32.8 GB, 1 GB reclaimable from a
    /// test VM ballooned to 4 GB with a 3 GB minimum → min(2.6, 9.1, 16.8) + 1 = 3.6 GB.
    /// </summary>
    [Fact]
    public void HomepcMemoryMatchesTheWorkedExample()
    {
        var facts = new HomepcFacts(
            WindowsAvailableMiB: Resources.Gib(8.6),
            WslMemAvailableMiB: Resources.Gib(11.1),
            FreeCommitMiB: Resources.Gib(32.8),
            ReclaimableBalloonMiB: Resources.Gib(1),
            RunningSlotVcpus: 4,
            QFreeMiB: Resources.Gib(58.9),
            FullScreenGame: false);
        SiteFree free = SiteCapacity.HomepcFree(facts, new HomepcReserves());
        Assert.Equal(Resources.Gib(3.6), free.Free.MemoryMiB);
        Assert.Equal(20, free.Free.Vcpus);
        Assert.Equal(Resources.Gib(58.9) - 15 * 1024, free.Free.DiskMiB);
        Assert.Empty(free.Holds);
        Assert.Equal(SiteCapacity.Homepc, free.Site);
    }

    /// <summary>A tolerant request is admitted on commit and WSL alone, beyond Windows' available memory.</summary>
    [Fact]
    public void TolerantRequestsLeaveOutWindowsAvailableMemory()
    {
        var facts = new HomepcFacts(Resources.Gib(5), Resources.Gib(11.1), Resources.Gib(32.8), 0, 0,
            Resources.Gib(50), false);
        Assert.True(SiteCapacity.HomepcFree(facts, new HomepcReserves()).Free.MemoryMiB < 0);
        Assert.Equal(Resources.Gib(9.1), SiteCapacity.HomepcFree(facts, new HomepcReserves(), tolerant: true).Free.MemoryMiB);
    }

    /// <summary>A capped slot keeps 6 GB of free commit, not 16 (DESIGN.md §16.14): on the PC of 2026-10-29 (9.5 GB
    /// available, 12 GB of free commit) nothing uncapped fits, while a capped slot has min(3.5, 6.0) = 3.5 GB.</summary>
    [Fact]
    public void CappedSlotsKeepTheSmallerCommitMargin()
    {
        var facts = new HomepcFacts(Resources.Gib(9.5), Resources.Gib(29), Resources.Gib(12), 0, 0, Resources.Gib(100), false);
        Assert.Equal(Resources.Gib(-4), SiteCapacity.HomepcFree(facts, new HomepcReserves()).Free.MemoryMiB);
        Assert.Equal(Resources.Gib(3.5), SiteCapacity.HomepcFree(facts, new HomepcReserves(), capped: true).Free.MemoryMiB);
        // The margin itself: with Windows' memory to spare, the commit term decides.
        var roomy = facts with { WindowsAvailableMiB = Resources.Gib(30) };
        Assert.Equal(Resources.Gib(6), SiteCapacity.HomepcFree(roomy, new HomepcReserves(), capped: true).Free.MemoryMiB);
    }

    /// <summary>A full-screen game holds every new slot on homepc.</summary>
    [Fact]
    public void FullScreenGameHoldsHomepc()
    {
        var facts = new HomepcFacts(Resources.Gib(30), Resources.Gib(30), Resources.Gib(60), 0, 0, Resources.Gib(50), true);
        SiteFree free = SiteCapacity.HomepcFree(facts, new HomepcReserves());
        Assert.Equal(["full-screen game"], free.Holds);
    }

    /// <summary>dock: MemAvailable − 2 GB; 12 usable vCPUs; and no Windows slot before E3.</summary>
    [Fact]
    public void DockReservesAndTheE3Hold()
    {
        var facts = new DockFacts(Resources.Gib(6.3), RunningSlotVcpus: 0, DiskFreeMiB: Resources.Gib(12),
            TakesWindowsSlots: false);
        SiteFree linux = SiteCapacity.DockFree(facts, new DockReserves(), windowsSlot: false);
        Assert.Equal(Resources.Gib(4.3), linux.Free.MemoryMiB);
        Assert.Equal(12, linux.Free.Vcpus);
        Assert.Empty(linux.Holds);
        Assert.Equal(2.0, linux.Priority);
        SiteFree windows = SiteCapacity.DockFree(facts, new DockReserves(), windowsSlot: true);
        Assert.Equal(["no Windows slots before E3"], windows.Holds);
    }
}
