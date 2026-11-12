using Amb.Core.Allocation;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Xunit;

namespace Amb.Core.Tests;

/// <summary>The core rules of the worker machine's site (since 2026-11-12): build ids that name their site by a letter,
/// and a Linux site's free amounts.</summary>
public sealed class WorkerSiteCoreTests
{
    /// <summary>Each site counts its own day: a worker id never takes homepc's numbers, and "b" stays the default.</summary>
    [Fact]
    public void BuildIdsCountPerSiteLetter()
    {
        var day = new DateOnly(2026, 11, 12);
        string[] existing = ["b20261112-1", "b20261112-2", "w20261112-1"];
        Assert.Equal("b20261112-3", Ids.NextBuildId(day, existing));
        Assert.Equal("w20261112-2", Ids.NextBuildId(day, existing, "w"));
        Assert.Equal("w20261112-1", Ids.NextBuildId(day, ["b20261112-7"], "w"));
        foreach (string bad in new[] { "r", "s", "W", "wk", "", "1" })
        {
            Assert.Throws<ArgumentException>(() => Ids.NextBuildId(day, [], bad));
        }
    }

    /// <summary>A build id's letter is read from its shape only; anything else names no site.</summary>
    [Fact]
    public void BuildLettersNameTheSite()
    {
        Assert.Equal("w", Ids.BuildLetterOf("w20261112-1"));
        Assert.Equal("b", Ids.BuildLetterOf("b20261025-14"));
        foreach (string? bad in new[] { null, "", "w2026111-1", "w20261112-", "w20261112x1", "r20261112-1", "x2026111a-1",
            "W20261112-1", "w20261112-1a" })
        {
            Assert.Null(Ids.BuildLetterOf(bad));
        }
        Assert.True(Ids.IsBuildLetter("w"));
        Assert.False(Ids.IsBuildLetter("s"));
        Assert.False(Ids.IsBuildLetter(null));
    }

    /// <summary>A Linux site keeps both reserves at once (the host's for the other guests, its container's own) and
    /// hands out vCPUs up to its usable count; it never holds for a game.</summary>
    [Fact]
    public void LinuxSiteKeepsBothReserves()
    {
        var reserves = new LinuxReserves(HostMemoryMiB: 8192, SiteMemoryMiB: 2048, UsableVcpus: 20, DiskMiB: 51200);
        var at = DateTimeOffset.Parse("2026-11-12T10:00:00Z");
        // The host is the tighter one: other guests use most of it.
        SiteFree host = SiteCapacity.LinuxFree(new LinuxHostFacts(at, 20000, 128000, 90000, 900_000), reserves, 0, "worker");
        Assert.Equal(new Resources(20000 - 8192, 20, 900_000 - 51200), host.Free);
        Assert.Empty(host.Holds);
        Assert.Equal("worker", host.Site);
        // The container's ceiling is the tighter one: the site uses most of its own share.
        SiteFree site = SiteCapacity.LinuxFree(new LinuxHostFacts(at, 100000, 128000, 10000, 900_000), reserves, 16, "worker");
        Assert.Equal(new Resources(10000 - 2048, 4, 900_000 - 51200), site.Free);
        Assert.Throws<ArgumentNullException>(() => SiteCapacity.LinuxFree(null!, reserves, 0, "worker"));
    }
}
