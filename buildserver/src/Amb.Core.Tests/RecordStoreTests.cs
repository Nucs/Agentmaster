using System.Text.Json;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Records;
using Xunit;

namespace Amb.Core.Tests;

/// <summary>The record store of DESIGN.md §6: atomic writes, compare-and-swap, leftovers, event logs, safe ids.</summary>
public sealed class RecordStoreTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), "amb-records-" + Guid.NewGuid().ToString("N"));

    /// <summary>Deletes the test's store folder.</summary>
    public void Dispose()
    {
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }

    /// <summary>A request record to store.</summary>
    /// <param name="id">Its id.</param>
    /// <param name="state">Its state.</param>
    /// <returns>The record.</returns>
    private static RequestRecord Request(string id, RequestState state = RequestState.Pending) => new()
    {
        Id = id,
        Kind = RequestKind.E2e,
        State = state,
        Inputs = new RequestInputs { BuildId = "b20261025-1", SuiteArgs = ["-v"] },
        CreatedAt = DateTimeOffset.Parse("2026-10-25T08:00:00Z"),
        UpdatedAt = DateTimeOffset.Parse("2026-10-25T08:00:00Z"),
        WaitTimeout = TimeSpan.FromMinutes(120),
        RunTimeout = TimeSpan.FromHours(2),
    };

    /// <summary>A record written with version 0 reads back with version 1 and the same value.</summary>
    [Fact]
    public void WriteThenReadRoundTrips()
    {
        var store = new RecordStore(_root);
        long version = store.Write("requests", "r1", Request("r1"), 0, AmbJson.Default.EnvelopeRequestRecord);
        Assert.Equal(1, version);
        Versioned<RequestRecord>? read = store.Read("requests", "r1", AmbJson.Default.EnvelopeRequestRecord);
        Assert.NotNull(read);
        Assert.Equal(1, read.Version);
        Assert.Equal(RequestKind.E2e, read.Value.Kind);
        Assert.Equal(["-v"], read.Value.Inputs.SuiteArgs);
        Assert.Equal(TimeSpan.FromHours(2), read.Value.RunTimeout);
    }

    /// <summary>A write that names an old version is refused and changes nothing.</summary>
    [Fact]
    public void StaleVersionIsAConflict()
    {
        var store = new RecordStore(_root);
        store.Write("requests", "r1", Request("r1"), 0, AmbJson.Default.EnvelopeRequestRecord);
        store.Write("requests", "r1", Request("r1", RequestState.Starting), 1, AmbJson.Default.EnvelopeRequestRecord);
        RecordConflictException conflict = Assert.Throws<RecordConflictException>(() =>
            store.Write("requests", "r1", Request("r1", RequestState.Cancelled), 1, AmbJson.Default.EnvelopeRequestRecord));
        Assert.Equal(2, conflict.Actual);
        Assert.Equal(RequestState.Starting, store.Read("requests", "r1", AmbJson.Default.EnvelopeRequestRecord)!.Value.State);
        // Creating a record that exists is a conflict too (expected version 0).
        Assert.Throws<RecordConflictException>(() =>
            store.Write("requests", "r1", Request("r1"), 0, AmbJson.Default.EnvelopeRequestRecord));
    }

    /// <summary>Update reads, changes and writes; a change that returns null leaves the record as it is.</summary>
    [Fact]
    public void UpdateAppliesOrLeavesTheRecord()
    {
        var store = new RecordStore(_root);
        Versioned<RequestRecord>? created = store.Update<RequestRecord>("requests", "r1",
            current => current ?? Request("r1"), AmbJson.Default.EnvelopeRequestRecord);
        Assert.Equal(1, created!.Version);
        Versioned<RequestRecord>? moved = store.Update<RequestRecord>("requests", "r1",
            current => current! with { State = RequestState.Starting }, AmbJson.Default.EnvelopeRequestRecord);
        Assert.Equal(2, moved!.Version);
        Versioned<RequestRecord>? unchanged = store.Update<RequestRecord>("requests", "r1",
            _ => null, AmbJson.Default.EnvelopeRequestRecord);
        Assert.Equal(2, unchanged!.Version);
        Assert.Equal(RequestState.Starting, unchanged.Value.State);
    }

    /// <summary>Many threads updating one record never lose an update (the per-record lock serializes them).</summary>
    [Fact]
    public void ConcurrentUpdatesAreSerialized()
    {
        var store = new RecordStore(_root);
        store.Write("builds", "counter", new BuildRecord
        {
            Id = "counter", Source = BuildSource.Imported, CreatedAt = DateTimeOffset.UnixEpoch, RequestId = "r0",
            Artifacts = [],
        }, 0, AmbJson.Default.EnvelopeBuildRecord);
        Parallel.For(0, 40, _ => store.Update<BuildRecord>("builds", "counter",
            current => current! with { Version = ((int.TryParse(current.Version, out int n) ? n : 0) + 1).ToString() },
            AmbJson.Default.EnvelopeBuildRecord));
        Versioned<BuildRecord>? final = store.Read("builds", "counter", AmbJson.Default.EnvelopeBuildRecord);
        Assert.Equal("40", final!.Value.Version);
        Assert.Equal(41, final.Version);
    }

    /// <summary>Temporary files a crash left behind are deleted when the store opens, and are never listed.</summary>
    [Fact]
    public void LeftoverTemporaryFilesAreDeletedAtOpen()
    {
        var store = new RecordStore(_root);
        store.Write("requests", "r1", Request("r1"), 0, AmbJson.Default.EnvelopeRequestRecord);
        string leftover = store.PathOf("requests", "r1") + ".tmp-999-1";
        File.WriteAllText(leftover, "{ half a reco");
        Assert.Equal(["r1"], store.List("requests"));
        _ = new RecordStore(_root);
        Assert.False(File.Exists(leftover));
        Assert.Equal(RequestState.Pending, store.Read("requests", "r1", AmbJson.Default.EnvelopeRequestRecord)!.Value.State);
    }

    /// <summary>An id that could leave the folder is refused before it reaches a path.</summary>
    /// <param name="id">An unsafe id: a climb, a separator, empty, or a name with a dot.</param>
    [Theory]
    [InlineData("../escape")]
    [InlineData("a/b")]
    [InlineData("a\\b")]
    [InlineData("")]
    [InlineData("name.json")]
    public void UnsafeIdsAreRefused(string id)
    {
        var store = new RecordStore(_root);
        Assert.Throws<ArgumentException>(() => store.PathOf("requests", id));
    }

    /// <summary>An unknown collection is refused.</summary>
    [Fact]
    public void UnknownCollectionIsRefused()
    {
        var store = new RecordStore(_root);
        Assert.Throws<ArgumentException>(() => store.List("passwords"));
    }

    /// <summary>Event lines append in order; a partial last line is left for the next read; skip resumes.</summary>
    [Fact]
    public void EventLinesAppendAndResume()
    {
        var store = new RecordStore(_root);
        string Line(string message) => JsonSerializer.Serialize(
            new RequestEvent(DateTimeOffset.UnixEpoch, "log", message), AmbJsonLine.Default.RequestEvent);
        store.AppendEventLine("r1", Line("one"));
        store.AppendEventLine("r1", Line("two"));
        File.AppendAllText(Path.Combine(store.Root, "requests", "r1.events.jsonl"), "{\"partial");
        IReadOnlyList<string> all = store.ReadEventLines("r1");
        Assert.Equal(2, all.Count);
        Assert.Contains("\"one\"", all[0]);
        IReadOnlyList<string> after = store.ReadEventLines("r1", skip: 1);
        Assert.Single(after);
        Assert.Contains("\"two\"", after[0]);
        Assert.Throws<ArgumentException>(() => store.AppendEventLine("r1", "a\nb"));
        Assert.Empty(store.ReadEventLines("nobody"));
    }

    /// <summary>A damaged record is reported, not read as missing (so it can never be silently overwritten).</summary>
    [Fact]
    public void DamagedRecordThrows()
    {
        var store = new RecordStore(_root);
        File.WriteAllText(store.PathOf("requests", "bad"), "not json");
        Assert.ThrowsAny<JsonException>(() => store.Read("requests", "bad", AmbJson.Default.EnvelopeRequestRecord));
        Assert.ThrowsAny<JsonException>(() =>
            store.Write("requests", "bad", Request("bad"), 0, AmbJson.Default.EnvelopeRequestRecord));
    }
}
