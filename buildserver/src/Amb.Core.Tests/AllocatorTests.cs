using Amb.Core.Allocation;
using Xunit;

namespace Amb.Core.Tests;

/// <summary>The admission rule of DESIGN.md §3.3, including the worked example of §3.6 with its measured numbers.</summary>
public sealed class AllocatorTests
{
    /// <summary>An e2e VM's request (§3.1): 3 / 8 GB, 2 / 6 vCPUs, 1 / 3 GB of overlay.</summary>
    private static readonly ResourceRequest E2e = new(
        new Resources(Resources.Gib(3), 2, Resources.Gib(1)),
        new Resources(Resources.Gib(8), 6, Resources.Gib(3)));

    /// <summary>A Debug build VM's request (§3.1): 8 / 16 GB, 4 / 8 vCPUs, 35 GB of data disk.</summary>
    private static readonly ResourceRequest DebugBuild = new(
        new Resources(Resources.Gib(8), 4, Resources.Gib(35)),
        new Resources(Resources.Gib(16), 8, Resources.Gib(35)));

    /// <summary>Builds a site with only memory varying (plenty of vCPUs and disk).</summary>
    /// <param name="name">The site's name.</param>
    /// <param name="priority">Its priority.</param>
    /// <param name="memoryGib">Its free memory, GiB.</param>
    /// <returns>The site.</returns>
    private static SiteFree Site(string name, double priority, double memoryGib) =>
        new(name, priority, new Resources(Resources.Gib(memoryGib), 20, Resources.Gib(100)), []);

    /// <summary>Below the minimum nothing counts; above it the fit is free / preferred, capped at 1.</summary>
    [Fact]
    public void FitIsZeroBelowTheMinimumAndCappedAtOne()
    {
        Assert.Equal(0.0, Allocator.Fit(free: 2, min: 3, preferred: 8));
        Assert.Equal(3.0 / 8, Allocator.Fit(free: 3, min: 3, preferred: 8), 10);
        Assert.Equal(1.0, Allocator.Fit(free: 20, min: 3, preferred: 8));
        Assert.Equal(1.0, Allocator.Fit(free: 0, min: 0, preferred: 0));
    }

    /// <summary>§3.6: homepc had 3.6 GB free, dock (the week's median) 4.3 GB; an e2e VM goes to dock at 4.3 GB.</summary>
    [Fact]
    public void WorkedExampleSendsE2eToDock()
    {
        SiteFree homepc = Site("homepc", 1.0, 3.6);
        SiteFree dock = Site("dock", 2.0, 4.3);
        Placement onHomepc = Allocator.Score(E2e, homepc);
        Placement onDock = Allocator.Score(E2e, dock);
        // 3.6 / 8 = 0.45 on homepc; 2 × 4.3 / 8 = 1.075 on dock (§3.6 prints it as 1.08; whole MiB make it 1.07495).
        Assert.Equal(0.45, onHomepc.Score, 2);
        Assert.InRange(onDock.Score, 1.07, 1.08);
        Placement? chosen = Allocator.Choose(E2e, [homepc, dock]);
        Assert.NotNull(chosen);
        Assert.Equal("dock", chosen.Site);
        Assert.Equal(Resources.Gib(4.3), chosen.Grant.MemoryMiB);
        Assert.Equal(6, chosen.Grant.Vcpus);
    }

    /// <summary>§3.6 before E3: dock holds Windows slots, so the e2e VM goes to homepc at its 3.6 GB.</summary>
    [Fact]
    public void BeforeE3TheE2eVmGoesToHomepc()
    {
        SiteFree homepc = Site("homepc", 1.0, 3.6);
        SiteFree dock = new("dock", 2.0, new Resources(Resources.Gib(4.3), 12, Resources.Gib(20)),
            ["no Windows slots before E3"]);
        Placement? chosen = Allocator.Choose(E2e, [homepc, dock]);
        Assert.NotNull(chosen);
        Assert.Equal("homepc", chosen.Site);
        Assert.Equal(Resources.Gib(3.6), chosen.Grant.MemoryMiB);
        Assert.StartsWith("hold:", Allocator.Score(E2e, dock).Shortage);
    }

    /// <summary>§3.6: a Debug build (8 GB minimum) fits neither site and waits, naming memory as what is short.</summary>
    [Fact]
    public void DebugBuildWaitsAndNamesMemory()
    {
        SiteFree homepc = Site("homepc", 1.0, 3.6);
        SiteFree dock = Site("dock", 2.0, 4.3);
        Assert.Null(Allocator.Choose(DebugBuild, [homepc, dock]));
        Assert.Equal("homepc: memory; dock: memory", Allocator.ExplainShortage(DebugBuild, [homepc, dock]));
    }

    /// <summary>The product rule: dock wins while it can give at least half of what homepc gives in full.</summary>
    [Fact]
    public void DockWinsAtHalfOfWhatHomepcGives()
    {
        // homepc gives the full 8 GB (fit 1, score 1); dock gives 4 GB (fit 0.5, score 1.0): a tie goes to dock.
        Placement? tie = Allocator.Choose(E2e, [Site("homepc", 1.0, 8), Site("dock", 2.0, 4)]);
        Assert.Equal("dock", tie?.Site);
        // dock with 3.5 GB (fit 0.4375, score 0.875) loses to homepc's full grant.
        Placement? less = Allocator.Choose(E2e, [Site("homepc", 1.0, 8), Site("dock", 2.0, 3.5)]);
        Assert.Equal("homepc", less?.Site);
    }

    /// <summary>The smallest fit decides: plenty of memory does not help when vCPUs are short.</summary>
    [Fact]
    public void AvailabilityIsTheSmallestFit()
    {
        var site = new SiteFree("homepc", 1.0, new Resources(Resources.Gib(32), 3, Resources.Gib(100)), []);
        Placement placement = Allocator.Score(E2e, site);
        Assert.Equal(0.5, placement.Availability, 10);
        Assert.Equal(3, placement.Grant.Vcpus);
        var noCpu = site with { Free = site.Free with { Vcpus = 1 } };
        Assert.Equal("vcpus", Allocator.Score(E2e, noCpu).Shortage);
    }

    /// <summary>A request whose minimum exceeds its preferred amount is refused when it is made.</summary>
    [Fact]
    public void MinimumAbovePreferredIsRefused()
    {
        Assert.Throws<ArgumentException>(() => new ResourceRequest(
            new Resources(Resources.Gib(8), 2, 0), new Resources(Resources.Gib(4), 2, 0)));
    }
}
