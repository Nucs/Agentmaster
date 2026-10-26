using System.IO.Compression;
using Amb.Coordinator;
using Amb.Coordinator.Jobs;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;
using Xunit;

namespace Amb.Coordinator.Tests;

/// <summary>The request book's life and events, the import's helpers and the e2e summary.</summary>
public sealed class RequestBookTests : IDisposable
{
    /// <summary>The test's own folder under %TEMP% (a test's files are small; nothing is built there), deleted after it.</summary>
    private readonly string _root = Path.Combine(Path.GetTempPath(), "amb-book-" + Guid.NewGuid().ToString("N"));

    /// <summary>Deletes the test's folder.</summary>
    public void Dispose()
    {
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }

    /// <summary>A book over a fresh record store.</summary>
    /// <returns>The book.</returns>
    private RequestBook Book() => new(new RecordStore(Path.Combine(_root, "records")), TimeProvider.System);

    /// <summary>Requests with missing or unsafe inputs, and kinds not served yet, are refused.</summary>
    [Fact]
    public void BadRequestsAreRefused()
    {
        RequestBook book = Book();
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.Import)));
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.E2e,
            new RequestInputs { BuildId = "../etc" })));
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.Build)));
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.Layer,
            new RequestInputs { Image = "am-base", NewImage = "am-e2e" })));
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.Smoke,
            new RequestInputs { Stage = [new StageItem(@"C:\x", "../outside")] })));
        Assert.Empty(book.Active);
    }

    /// <summary>A request gets its kind's defaults, moves only along its life, and logs every move.</summary>
    [Fact]
    public void LifeAndEvents()
    {
        RequestBook book = Book();
        RequestRecord created = book.Create(new CreateRequest(RequestKind.E2e, new RequestInputs { BuildId = "b20261025-1" }));
        Assert.Equal(RequestState.Pending, created.State);
        Assert.Equal(TimeSpan.FromMinutes(120), created.WaitTimeout);
        Assert.Equal(8 * 1024, created.Resources!.Preferred.MemoryMiB);
        Assert.Null(book.Move(created.Id, RequestState.Collecting, "not allowed from pending"));
        Assert.NotNull(book.Move(created.Id, RequestState.Starting, "placed"));
        RequestRecord? running = book.Move(created.Id, RequestState.Running, "guest ready");
        Assert.NotNull(running?.StartedAt);
        RequestRecord? done = book.Move(created.Id, RequestState.Succeeded, "49 passed", r => r with { Outcome = "49 passed" });
        Assert.NotNull(done?.EndedAt);
        Assert.Equal("49 passed", done!.Outcome);
        Assert.Null(book.Move(created.Id, RequestState.Running, "a late heartbeat must not revive it"));
        Assert.Empty(book.Active);
        IReadOnlyList<RequestEvent> events = book.Events(created.Id, 0);
        Assert.Equal(["accepted: E2e", "placed", "guest ready", "49 passed"],
            events.Where(e => e.Kind == "state").Select(e => e.Message));
        Assert.Single(book.Events(created.Id, events.Count - 1));
    }

    /// <summary>An event is one line without NULs: UTF-16 output read as UTF-8 (MakePri's warnings in a build's package
    /// step) arrives with a NUL beside every character, and a multi-line message is folded.</summary>
    [Fact]
    public void EventLinesAreCleaned()
    {
        RequestBook book = Book();
        RequestRecord created = book.Create(new CreateRequest(RequestKind.Smoke));
        book.Event(created.Id, "output", "package: \0W\0A\0R\0N\0I\0N\0G\0");
        book.Event(created.Id, "error", "first\r\nsecond");
        Assert.Equal(["package: WARNING", "first | second"],
            book.Events(created.Id, 0).Where(e => e.Kind != "state").Select(e => e.Message));
    }

    /// <summary>A reopened book knows every request that had not ended, and nothing that had.</summary>
    [Fact]
    public void ReopenedBookKeepsTheActiveOnes()
    {
        RequestBook book = Book();
        RequestRecord waiting = book.Create(new CreateRequest(RequestKind.Smoke));
        RequestRecord ended = book.Create(new CreateRequest(RequestKind.Smoke));
        book.Move(ended.Id, RequestState.Cancelled, "cancelled");
        RequestBook reopened = Book();
        Assert.Equal([waiting.Id], reopened.Active.Select(r => r.Id));
        Assert.Equal(RequestState.Cancelled, reopened.Get(ended.Id)!.State);
    }

    /// <summary>A patch changes fields but never the state.</summary>
    [Fact]
    public void PatchKeepsTheState()
    {
        RequestBook book = Book();
        RequestRecord created = book.Create(new CreateRequest(RequestKind.Smoke));
        RequestRecord? patched = book.Patch(created.Id, r => r with { Outcome = "waiting: memory", State = RequestState.Succeeded });
        Assert.Equal(RequestState.Pending, patched!.State);
        Assert.Equal("waiting: memory", patched.Outcome);
    }

    /// <summary>The import reads a portable zip's version from its top folder and hashes what it copies.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task ImportHelpers()
    {
        Directory.CreateDirectory(_root);
        string zip = Path.Combine(_root, "Agentmaster_0.8.1.1_x64.zip");
        using (ZipArchive archive = ZipFile.Open(zip, ZipArchiveMode.Create))
        {
            using (var writer = new StreamWriter(archive.CreateEntry("agentmaster-0.8.1.1/.am-version").Open()))
            {
                writer.Write("0.8.1.1\n");
            }
        }
        Assert.Equal("0.8.1.1", ImportJob.ReadPortableVersion(zip));
        string notZip = Path.Combine(_root, "plain.txt");
        await File.WriteAllTextAsync(notZip, "not a zip");
        Assert.Null(ImportJob.ReadPortableVersion(notZip));
        (string sha, long bytes) = await ImportJob.CopyHashedAsync(notZip, Path.Combine(_root, "copy.txt"), CancellationToken.None);
        Assert.Equal(9, bytes);
        Assert.Equal(Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData("not a zip"u8.ToArray())), sha);
        Assert.False(File.Exists(Path.Combine(_root, "copy.txt.part")));
    }

    /// <summary>The e2e outcome comes from run_suite.py's status file, and a missing one is a failure that says so.</summary>
    [Fact]
    public void E2eSummary()
    {
        Directory.CreateDirectory(_root);
        string status = Path.Combine(_root, "r1.status.json");
        File.WriteAllText(status, """{"state": "done", "exit": 1, "tests": 49, "failures": 1, "errors": 0, "skipped": 2}""");
        (bool passed, string summary) = E2eJob.Summarize(status, 1);
        Assert.False(passed);
        Assert.Equal("49 tests: 46 passed, 1 failed, 0 errors, 2 skipped (exit 1)", summary);
        Assert.StartsWith("exit 5: no tests were collected", E2eJob.Summarize(Path.Combine(_root, "none.json"), 5).Summary);
        Assert.Contains("; no status file", E2eJob.Summarize(Path.Combine(_root, "none.json"), 5).Summary);
        File.WriteAllText(status, """{"state": "done", "exit": 4, "tests": 0, "failures": 0, "errors": 0, "skipped": 0}""");
        Assert.EndsWith("(exit 4: pytest's usage error (a test path that does not exist, an unknown option); see pytest.log in the results)",
            E2eJob.Summarize(status, 4).Summary);
    }
}
