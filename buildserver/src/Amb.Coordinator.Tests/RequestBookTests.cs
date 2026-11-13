using System.IO.Compression;
using Amb.Coordinator;
using Amb.Coordinator.Jobs;
using Amb.Core.Allocation;
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

    /// <summary>An e2e or smoke VM sized on its own (DESIGN.md §16.14): a given memory and vCPU count become both its
    /// minimum and its preferred amount; a guest RAM alone lowers the memory range to it; a size for another kind, one out
    /// of range, or a guest RAM under the cap is refused; the book's record carries the sized resources.</summary>
    [Fact]
    public void SlotSizeOverridesTheKindsDefaults()
    {
        ResourceRequest e2e = RequestDefaults.ResourcesOf(RequestKind.E2e)!;
        ResourceRequest capped = RequestBook.SizeSlot(RequestKind.E2e, e2e,
            new RequestInputs { MemoryMiB = 2560, GuestMemoryMiB = 4096, Vcpus = 4 })!;
        Assert.Equal(new Resources(2560, 4, e2e.Min.DiskMiB), capped.Min);
        Assert.Equal(new Resources(2560, 4, e2e.Preferred.DiskMiB), capped.Preferred);
        // A guest RAM under the default range lowers it: never more memory than the guest sees.
        ResourceRequest guestOnly = RequestBook.SizeSlot(RequestKind.E2e, e2e, new RequestInputs { GuestMemoryMiB = 1536 })!;
        Assert.Equal(1536, guestOnly.Min.MemoryMiB);
        Assert.Equal(1536, guestOnly.Preferred.MemoryMiB);
        Assert.Same(e2e, RequestBook.SizeSlot(RequestKind.E2e, e2e, new RequestInputs()));
        Assert.Throws<RequestRejectedException>(() => RequestBook.SizeSlot(RequestKind.Build,
            RequestDefaults.ResourcesOf(RequestKind.Build), new RequestInputs { MemoryMiB = 4096 }));
        Assert.Throws<RequestRejectedException>(() => RequestBook.SizeSlot(RequestKind.E2e, e2e,
            new RequestInputs { MemoryMiB = 512 }));
        Assert.Throws<RequestRejectedException>(() => RequestBook.SizeSlot(RequestKind.E2e, e2e,
            new RequestInputs { Vcpus = 0 }));
        Assert.Throws<RequestRejectedException>(() => RequestBook.SizeSlot(RequestKind.E2e, e2e,
            new RequestInputs { MemoryMiB = 4096, GuestMemoryMiB = 2048 }));

        RequestRecord record = Book().Create(new CreateRequest(RequestKind.Smoke,
            new RequestInputs { MemoryMiB = 2048, GuestMemoryMiB = 3072 }));
        Assert.Equal(2048, record.Resources!.Min.MemoryMiB);
        Assert.Equal(2048, record.Resources.Preferred.MemoryMiB);
        Assert.True(Scheduler.IsCapped(record));
    }

    /// <summary>An e2e request that names no size gets the tuned capped VM (DESIGN.md §16.15): a 2 GB cap on a 3 GB guest
    /// with 4 vCPUs; one that names its memory and no guest RAM gets an uncapped VM of that size; another kind keeps its
    /// own defaults and no guest RAM.</summary>
    [Fact]
    public void E2eDefaultsToTheTunedCappedVm()
    {
        RequestBook book = Book();
        RequestRecord tuned = book.Create(new CreateRequest(RequestKind.E2e, new RequestInputs { BuildId = "b20261029-1" }));
        Assert.Equal(RequestDefaults.E2eGuestMemoryMiB, tuned.Inputs.GuestMemoryMiB);
        Assert.Equal(new Resources(2048, 4, 1024), tuned.Resources!.Min);
        Assert.Equal(2048, tuned.Resources.Preferred.MemoryMiB);
        Assert.True(Scheduler.IsCapped(tuned));

        RequestRecord uncapped = book.Create(new CreateRequest(RequestKind.E2e,
            new RequestInputs { BuildId = "b20261029-1", MemoryMiB = 8192 }));
        Assert.Null(uncapped.Inputs.GuestMemoryMiB);
        Assert.Equal(8192, uncapped.Resources!.Preferred.MemoryMiB);
        Assert.False(Scheduler.IsCapped(uncapped));

        RequestRecord smoke = book.Create(new CreateRequest(RequestKind.Smoke, new RequestInputs()));
        Assert.Null(smoke.Inputs.GuestMemoryMiB);
        Assert.False(Scheduler.IsCapped(smoke));
    }

    /// <summary>A request gets its kind's defaults, moves only along its life, and logs every move.</summary>
    [Fact]
    public void LifeAndEvents()
    {
        RequestBook book = Book();
        RequestRecord created = book.Create(new CreateRequest(RequestKind.E2e, new RequestInputs { BuildId = "b20261025-1" }));
        Assert.Equal(RequestState.Pending, created.State);
        Assert.Equal(TimeSpan.FromMinutes(120), created.WaitTimeout);
        Assert.Equal(2 * 1024, created.Resources!.Preferred.MemoryMiB);
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

    /// <summary>A test request (`amb test`, since 2026-11-13) names a build and a served kind: the kind defaults to m5 and
    /// is lower-cased; no build id, an unsafe one, or a kind not served is refused; its slot is the kind's small default
    /// VM, and its record waits and runs by the test kind's timeouts.</summary>
    [Fact]
    public void TestRequestsNameABuildAndAServedKind()
    {
        RequestBook book = Book();
        RequestRecord record = book.Create(new CreateRequest(RequestKind.Test, new RequestInputs { BuildId = "w20261113-3" }));
        Assert.Equal("m5", record.Inputs.TestKind);
        Assert.Equal(RequestDefaults.ResourcesOf(RequestKind.Test), record.Resources);
        Assert.Equal(TimeSpan.FromMinutes(30), record.WaitTimeout);
        Assert.Equal(TimeSpan.FromMinutes(60), record.RunTimeout);
        Assert.Equal("m5", RequestBook.NormalizeTest(new RequestInputs { BuildId = "b1", TestKind = " M5 " }).TestKind);
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.Test)));
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.Test,
            new RequestInputs { BuildId = "../etc" })));
        RequestRejectedException kind = Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(
            RequestKind.Test, new RequestInputs { BuildId = "w20261113-3", TestKind = "syntax" })));
        Assert.Contains("must be m5", kind.Message, StringComparison.Ordinal);
        // A test VM is sized by its kind only: --ram and --vcpus size e2e and smoke VMs.
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.Test,
            new RequestInputs { BuildId = "w20261113-3", MemoryMiB = 8192 })));
        Assert.Single(book.Active);
    }

    /// <summary>The m5 outcome comes from the harness's own last summary line: a pass needs exit 0 and no failures, and a
    /// run without the line (a crash, a stop) says so instead of passing nothing.</summary>
    [Fact]
    public void M5Summary()
    {
        string[] failing = ["=== Agentmaster engine tests ===", "  [FAIL] disc: the rim", "", "12946 checks, 3 failures - FAILURES"];
        (bool passed, string summary) = TestJob.Summarize(failing, 1, 99.4);
        Assert.False(passed);
        Assert.Equal("m5: 12946 checks, 3 failures (exit 1), 99 s", summary);
        (passed, summary) = TestJob.Summarize(["12946 checks, 0 failures - ALL PASS"], 0, 120);
        Assert.True(passed);
        Assert.Equal("m5: 12946 checks, 0 failures (exit 0), 120 s", summary);
        // No summary line: the harness ended early (or was stopped), whatever its exit code.
        (passed, summary) = TestJob.Summarize(["=== Agentmaster engine tests ===", "-- PendingInput --"], 0, 3);
        Assert.False(passed);
        Assert.Equal("m5: no summary line (exit 0: the harness ended early), 3 s", summary);
        Assert.Contains("(exit -1: stopped)", TestJob.Summarize([], -1, 3540).Summary, StringComparison.Ordinal);
        Assert.Equal(@"Z:\test\out\m5-output.txt", TestJob.GuestPathOnShare("test/out/m5-output.txt"));
    }
}
