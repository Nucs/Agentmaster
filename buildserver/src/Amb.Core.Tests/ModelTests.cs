using Amb.Core.Model;
using Xunit;

namespace Amb.Core.Tests;

/// <summary>Ids and the request life of DESIGN.md §2 and §12.</summary>
public sealed class ModelTests
{
    /// <summary>Build ids count per UTC day from 1, ignoring other days and malformed names.</summary>
    [Fact]
    public void BuildIdsCountPerDay()
    {
        var day = new DateOnly(2026, 10, 25);
        Assert.Equal("b20261025-1", Ids.NextBuildId(day, []));
        Assert.Equal("b20261025-4", Ids.NextBuildId(day,
            ["b20261025-1", "b20261025-3", "b20261024-9", "b20261025-x", "b20261025-", "junk"]));
    }

    /// <summary>Request and slot ids carry their UTC time, sort as strings and are safe file names.</summary>
    [Fact]
    public void RequestAndSlotIdsAreSafeAndTimeOrdered()
    {
        var t = DateTimeOffset.Parse("2026-10-25T10:12:00+02:00");
        string id = Ids.NewRequestId(t);
        Assert.StartsWith("r20261025-081200-", id);
        Assert.True(Ids.IsSafe(id));
        Assert.True(string.CompareOrdinal(Ids.NewRequestId(t), Ids.NewRequestId(t.AddSeconds(1))) < 0);
        Assert.StartsWith("s20261025-081200-", Ids.NewSlotId(t));
    }

    /// <summary>Final states never move; the allowed moves follow DESIGN.md §12.</summary>
    [Fact]
    public void RequestLifeAllowsOnlyItsMoves()
    {
        Assert.True(RequestStates.CanMove(RequestState.Pending, RequestState.Starting));
        Assert.True(RequestStates.CanMove(RequestState.Pending, RequestState.Running));
        Assert.True(RequestStates.CanMove(RequestState.Running, RequestState.Collecting));
        Assert.True(RequestStates.CanMove(RequestState.Collecting, RequestState.Succeeded));
        Assert.False(RequestStates.CanMove(RequestState.Running, RequestState.Pending));
        foreach (RequestState final in Enum.GetValues<RequestState>().Where(RequestStates.IsFinal))
        {
            foreach (RequestState to in Enum.GetValues<RequestState>())
            {
                Assert.False(RequestStates.CanMove(final, to), $"{final} -> {to}");
            }
        }
    }
}
