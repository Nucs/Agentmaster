using System.IO.Compression;
using Amb.Coordinator.Slots;
using Amb.Core.Allocation;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Time.Testing;
using Xunit;

namespace Amb.Coordinator.Tests;

/// <summary>
/// The scheduler's admission order (Scheduler.Admit, DESIGN.md §3.5): a request that has waited past the pass limit for
/// capacity the site's own slots hold keeps the next free capacity for itself, yet never holds up a request that needs
/// no slot, holds nothing back when even an idle site could not fit it, and a request held behind it still ends at its
/// own wait timeout. A capped slot (DESIGN.md §16.14) is scored with the smaller capped commit margin.
/// </summary>
/// <remarks>Found 2026-10-29: an import of the v0.8.1 release zip waited behind a build that waited hours for memory,
/// past its own 5-minute wait timeout and never looked at, because the admission pass stopped at the build; and that
/// build, which needed 24 GB of free commit on a PC that had 7-13 GB, held every e2e run behind it. Every test runs
/// single passes on a fake clock; the driver starts only the slots a test asks for.</remarks>
public sealed class SchedulerTests : IDisposable
{
    /// <summary>The memory homepc has free beyond its reserves unless a test says otherwise: a smoke check (2 GB at
    /// least) would fit, the tests' e2e run (an uncapped 3 GB VM, asked for by its memory) does not.</summary>
    private const double FreeGb = 2.5;

    /// <summary>The test's own site root under %TEMP% (records, store, the import's zip), deleted after it.</summary>
    private readonly string _root = Path.Combine(Path.GetTempPath(), "amb-sched-" + Guid.NewGuid().ToString("N"));

    /// <summary>The clock the requests' ages, wait timeouts and the slots' warm-up run on.</summary>
    private readonly FakeTimeProvider _clock = new(DateTimeOffset.Parse("2026-10-29T06:00:00Z"));

    /// <summary>The driver: it refuses every start unless a test lets one through, and counts them all.</summary>
    private readonly CountingDriver _driver = new();

    /// <summary>The request book the scheduler admits from.</summary>
    private readonly RequestBook _book;

    /// <summary>The slot manager (a test's busy-site slot is started through it).</summary>
    private readonly SlotManager _slots;

    /// <summary>homepc's capacity, fed by <see cref="Report"/> instead of the host-facts helper.</summary>
    private readonly HostCapacity _capacity;

    /// <summary>The scheduler under test; never started as a service, so each test runs its passes itself.</summary>
    private readonly Scheduler _scheduler;

    /// <summary>Makes a site root with a stand-in guest agent file, and a scheduler over it.</summary>
    public SchedulerTests()
    {
        Directory.CreateDirectory(_root);
        string agent = Path.Combine(_root, "amb-guest.exe");
        File.WriteAllText(agent, "a stand-in: the driver never runs it");
        var options = new CoordinatorOptions
        {
            Root = _root,
            Listen = "http://127.0.0.1:0",
            GuestUrl = "http://127.0.0.1:0",
            SlotDriver = "fake",
            HostFactsCommand = "",
            GuestAgentPath = agent,
            DockurImage = "none",
            IdleExitMinutes = 0,
            E2eSourceDir = _root,
        };
        var store = new RecordStore(options.RecordsDir);
        _book = new RequestBook(store, _clock);
        _slots = new SlotManager(options, store, new GuestBroker(_clock), _driver, _clock, NullLogger<SlotManager>.Instance);
        _capacity = new HostCapacity(options, _slots, _clock, NullLogger<HostCapacity>.Instance);
        _scheduler = new Scheduler(_book, _capacity, _slots, store, options, _clock, NullLoggerFactory.Instance);
    }

    /// <summary>Disposes the scheduler and deletes the site root (every job a test started has ended by then).</summary>
    public void Dispose()
    {
        _scheduler.Dispose();
        Directory.Delete(_root, recursive: true);
    }

    /// <summary>An import that comes while an older e2e run has waited past the pass limit starts at once and imports its
    /// zip, while a younger smoke check that would fit is held behind the e2e run, which keeps waiting: the site's own
    /// slot is what the e2e run lacks.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task ImportStartsBehindARequestThatWaitedPastThePassLimit()
    {
        await BusySiteAsync();
        RequestRecord e2e = _book.Create(new CreateRequest(RequestKind.E2e, new RequestInputs { BuildId = "b20261029-1", MemoryMiB = 3072 }));
        Report();
        _scheduler.Admit();
        RequestRecord waiting = _book.Get(e2e.Id)!;
        Assert.Equal(RequestState.Pending, waiting.State);
        Assert.Contains("memory", waiting.Outcome, StringComparison.Ordinal);

        _clock.Advance(Scheduler.PassLimit + TimeSpan.FromMinutes(1));
        Report();
        RequestRecord import = _book.Create(new CreateRequest(RequestKind.Import, new RequestInputs
        {
            Source = PortableZip("0.8.1.0"),
            Commit = "605e314dca26cae0606863996b5321e2e7939f1d",
        }));
        RequestRecord smoke = _book.Create(new CreateRequest(RequestKind.Smoke, new RequestInputs()));
        _scheduler.Admit();

        RequestRecord imported = await EndedAsync(import.Id);
        Assert.Equal(RequestState.Succeeded, imported.State);
        Assert.NotNull(imported.BuildId);
        RequestRecord held = _book.Get(smoke.Id)!;
        Assert.Equal(RequestState.Pending, held.State);
        Assert.Equal($"waiting: behind {e2e.Id}, which has waited longer for capacity", held.Outcome);
        Assert.Equal(RequestState.Pending, _book.Get(e2e.Id)!.State);
        Assert.Equal(1, _driver.Starts);
    }

    /// <summary>A request held behind an older one that waited past the pass limit still ends no-capacity at its own wait
    /// timeout, naming what it waited behind; the older one, whose wait timeout is longer, waits on.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task RequestHeldBehindStillEndsAtItsOwnWaitTimeout()
    {
        await BusySiteAsync();
        RequestRecord e2e = _book.Create(new CreateRequest(RequestKind.E2e, new RequestInputs { BuildId = "b20261029-1", MemoryMiB = 3072 }));
        _clock.Advance(Scheduler.PassLimit + TimeSpan.FromMinutes(1));
        RequestRecord smoke = _book.Create(new CreateRequest(RequestKind.Smoke, new RequestInputs()));
        Report();
        _scheduler.Admit();
        Assert.Equal(RequestState.Pending, _book.Get(smoke.Id)!.State);

        // Past the smoke check's 30-minute wait timeout, well inside the e2e run's 120 minutes.
        _clock.Advance(RequestDefaults.WaitOf(RequestKind.Smoke) + TimeSpan.FromMinutes(1));
        Report();
        _scheduler.Admit();
        RequestRecord ended = _book.Get(smoke.Id)!;
        Assert.Equal(RequestState.NoCapacity, ended.State);
        Assert.Equal($"no-capacity: behind {e2e.Id}, which has waited longer for capacity", ended.Outcome);
        Assert.Equal(RequestState.Pending, _book.Get(e2e.Id)!.State);
        Assert.Equal(1, _driver.Starts);
    }

    /// <summary>An older request that would not fit even with every slot of the site stopped holds nothing back: the
    /// owner's own programs hold what it lacks, so a younger smoke check that fits is admitted (its slot is started).</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task OlderRequestThatCannotFitOnAnIdleSiteHoldsNothingBack()
    {
        RequestRecord e2e = _book.Create(new CreateRequest(RequestKind.E2e, new RequestInputs { BuildId = "b20261029-1", MemoryMiB = 3072 }));
        _clock.Advance(Scheduler.PassLimit + TimeSpan.FromMinutes(1));
        RequestRecord smoke = _book.Create(new CreateRequest(RequestKind.Smoke, new RequestInputs()));
        Report();
        _scheduler.Admit();

        RequestRecord admitted = await EndedAsync(smoke.Id);
        Assert.Equal(1, _driver.Starts);
        Assert.Equal(RequestState.Failed, admitted.State);   // the driver refused its slot: it was admitted all the same
        RequestRecord older = _book.Get(e2e.Id)!;
        Assert.Equal(RequestState.Pending, older.State);
        Assert.Contains("memory", older.Outcome, StringComparison.Ordinal);
    }

    /// <summary>A capped slot is admitted where an uncapped one of the same size waits: on a PC with 9.5 GB available and
    /// 17 GB of free commit, an uncapped smoke check has min(3.5, 1) = 1 GB, a capped one min(3.5, 11) = 3.5 GB.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task CappedRequestFitsWhereAnUncappedOneWaits()
    {
        RequestRecord uncapped = _book.Create(new CreateRequest(RequestKind.Smoke, new RequestInputs { MemoryMiB = 2048 }));
        RequestRecord capped = _book.Create(new CreateRequest(RequestKind.Smoke,
            new RequestInputs { MemoryMiB = 2048, GuestMemoryMiB = 4096 }));
        Assert.False(Scheduler.IsCapped(uncapped));
        Assert.True(Scheduler.IsCapped(capped));
        Report(availableGb: 9.5, freeCommitGb: 17);
        _scheduler.Admit();

        RequestRecord admitted = await EndedAsync(capped.Id);
        Assert.Equal(RequestState.Failed, admitted.State);   // admitted; the driver refused its slot
        Assert.Equal(1, _driver.Starts);
        RequestRecord waiting = _book.Get(uncapped.Id)!;
        Assert.Equal(RequestState.Pending, waiting.State);
        Assert.Contains("memory", waiting.Outcome, StringComparison.Ordinal);
    }

    /// <summary>Gives homepc a fresh host-facts report (one older than HostCapacity.Stale holds every slot).</summary>
    /// <param name="availableGb">Windows' available memory; by default <see cref="FreeGb"/> above its 6 GB reserve.</param>
    /// <param name="freeCommitGb">Windows' free commit; by default <see cref="FreeGb"/> above the 16 GB reserve.</param>
    private void Report(double availableGb = 6 + FreeGb, double freeCommitGb = 16 + FreeGb) =>
        _capacity.Override(new HostFactsReport(_clock.GetUtcNow(), AvailableMiB: Resources.Gib(availableGb),
            FreeCommitMiB: Resources.Gib(freeCommitGb), QFreeMiB: Resources.Gib(200), FullScreenGame: false, UserState: 5));

    /// <summary>Starts one 4 GB slot for a request outside the book, so the site's own slots hold memory: an older request
    /// that does not fit would then fit on an idle site, and the pass limit holds younger requests behind it.</summary>
    /// <returns>A task that ends when the slot runs.</returns>
    /// <exception cref="InvalidOperationException">The slot could not be started.</exception>
    private async Task BusySiteAsync()
    {
        var running = new RequestRecord
        {
            Id = "r20261029-050000-busy01",
            Kind = RequestKind.Smoke,
            State = RequestState.Running,
            Inputs = new RequestInputs(),
            CreatedAt = _clock.GetUtcNow(),
            UpdatedAt = _clock.GetUtcNow(),
            WaitTimeout = TimeSpan.FromMinutes(30),
            RunTimeout = TimeSpan.FromHours(3),
        };
        _driver.AllowStarts = true;
        await _slots.StartAsync(running, "am-e2e", new Resources(Resources.Gib(4), 2, Resources.Gib(1)), CancellationToken.None);
        _driver.AllowStarts = false;
    }

    /// <summary>Writes a portable zip shaped like a release's: one top folder holding the <c>.am-version</c> stamp.</summary>
    /// <param name="version">The version to stamp.</param>
    /// <returns>The zip's full path.</returns>
    /// <exception cref="IOException">The zip could not be written.</exception>
    private string PortableZip(string version)
    {
        string zip = Path.Combine(_root, $"Agentmaster_{version}_x64.zip");
        using ZipArchive archive = ZipFile.Open(zip, ZipArchiveMode.Create);
        using var writer = new StreamWriter(archive.CreateEntry($"agentmaster-{version}/.am-version").Open());
        writer.Write(version);
        return zip;
    }

    /// <summary>Waits until a request has ended.</summary>
    /// <param name="id">The request.</param>
    /// <returns>Its final record.</returns>
    /// <exception cref="TimeoutException">It had not ended within 30 s.</exception>
    /// <remarks>Its job runs on the thread pool and does real file work, so the wait is in real time: the fake clock
    /// would never move by itself.</remarks>
    private async Task<RequestRecord> EndedAsync(string id)
    {
        DateTime deadline = DateTime.UtcNow + TimeSpan.FromSeconds(30);
        while (true)
        {
            RequestRecord? record = _book.Get(id);
            if (record is not null && RequestStates.IsFinal(record.State))
            {
                return record;
            }
            if (DateTime.UtcNow > deadline)
            {
                throw new TimeoutException($"{id} had not ended after 30 s (state {record?.State})");
            }
            await Task.Delay(20);
        }
    }

    /// <summary>A slot driver that starts a slot only while a test allows it, counts every attempt, and reports every
    /// slot it started alive (so the slot manager's liveness watch keeps it).</summary>
    private sealed class CountingDriver : ISlotDriver
    {
        /// <summary>The attempted starts.</summary>
        private int _starts;

        /// <summary>Whether a start succeeds (a busy-site slot) or is refused (a slot the scheduler admitted).</summary>
        public volatile bool AllowStarts;

        /// <summary>The attempted starts so far.</summary>
        public int Starts => Volatile.Read(ref _starts);

        /// <inheritdoc/>
        public string Name => "counting";

        /// <inheritdoc/>
        public bool IsVm => true;

        /// <inheritdoc/>
        public string GuestShareRoot(SlotSpec spec) => spec.ExchangeDir;

        /// <inheritdoc/>
        /// <exception cref="InvalidOperationException">Starts are not allowed now: the slot of an admitted request.</exception>
        public Task<string> StartAsync(SlotSpec spec, CancellationToken cancel)
        {
            Interlocked.Increment(ref _starts);
            if (!AllowStarts)
            {
                throw new InvalidOperationException("this test admits the request but starts no slot for it");
            }
            Directory.CreateDirectory(spec.SlotDir);
            return Task.FromResult("counting-" + spec.SlotId);
        }

        /// <inheritdoc/>
        public Task StopAsync(string handle, bool graceful, CancellationToken cancel) => Task.CompletedTask;

        /// <inheritdoc/>
        public Task<bool> IsAliveAsync(string handle, CancellationToken cancel) => Task.FromResult(true);

        /// <inheritdoc/>
        public Task<bool> SetBalloonAsync(string handle, long targetMiB, CancellationToken cancel) => Task.FromResult(false);
    }
}
