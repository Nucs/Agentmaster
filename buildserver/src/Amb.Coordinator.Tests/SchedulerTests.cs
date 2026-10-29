using System.IO.Compression;
using Amb.Coordinator.Slots;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Time.Testing;
using Xunit;

namespace Amb.Coordinator.Tests;

/// <summary>
/// The scheduler's admission order (Scheduler.Admit, DESIGN.md §3.5): a request that has waited past the pass limit for
/// capacity keeps the next free capacity for itself, yet never holds up a request that needs no slot, and a request held
/// behind it still ends at its own wait timeout.
/// </summary>
/// <remarks>Found 2026-10-29: an import of the v0.8.1 release zip waited behind a build that waited hours for memory, past
/// its own 5-minute wait timeout and never looked at, because the admission pass stopped at the build. Every test runs
/// single passes on a fake clock against a PC that has <see cref="FreeMiB"/> free; no slot ever starts.</remarks>
public sealed class SchedulerTests : IDisposable
{
    /// <summary>The memory homepc has free beyond its reserves in every test: a smoke check (2 GB at least) would fit, an
    /// e2e run (3 GB at least) does not.</summary>
    private const long FreeMiB = 2560;

    /// <summary>The test's own site root under %TEMP% (records, store, the import's zip), deleted after it.</summary>
    private readonly string _root = Path.Combine(Path.GetTempPath(), "amb-sched-" + Guid.NewGuid().ToString("N"));

    /// <summary>The clock the requests' ages and wait timeouts run on.</summary>
    private readonly FakeTimeProvider _clock = new(DateTimeOffset.Parse("2026-10-29T06:00:00Z"));

    /// <summary>The driver that refuses every slot and counts the attempts: no test here may start one.</summary>
    private readonly NoSlotDriver _driver = new();

    /// <summary>The request book the scheduler admits from.</summary>
    private readonly RequestBook _book;

    /// <summary>homepc's capacity, fed by <see cref="Report"/> instead of the host-facts helper.</summary>
    private readonly HostCapacity _capacity;

    /// <summary>The scheduler under test; never started as a service, so each test runs its passes itself.</summary>
    private readonly Scheduler _scheduler;

    /// <summary>Makes a site root with a stand-in guest agent file, and a scheduler over it.</summary>
    public SchedulerTests()
    {
        Directory.CreateDirectory(_root);
        string agent = Path.Combine(_root, "amb-guest.exe");
        File.WriteAllText(agent, "a stand-in: no slot starts in these tests");
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
        var slots = new SlotManager(options, store, new GuestBroker(_clock), _driver, _clock,
            NullLogger<SlotManager>.Instance);
        _capacity = new HostCapacity(options, slots, _clock, NullLogger<HostCapacity>.Instance);
        _scheduler = new Scheduler(_book, _capacity, slots, store, options, _clock, NullLoggerFactory.Instance);
    }

    /// <summary>Disposes the scheduler and deletes the site root (every job a test started has ended by then).</summary>
    public void Dispose()
    {
        _scheduler.Dispose();
        Directory.Delete(_root, recursive: true);
    }

    /// <summary>An import that comes while an older e2e run has waited past the pass limit starts at once and imports its
    /// zip, while a younger smoke check that would fit is held behind the e2e run, which keeps waiting.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task ImportStartsBehindARequestThatWaitedPastThePassLimit()
    {
        RequestRecord e2e = _book.Create(new CreateRequest(RequestKind.E2e, new RequestInputs { BuildId = "b20261029-1" }));
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
            Commit = "89cc2766ee2c3c096013b996a6beb9a454185220",
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
        Assert.Equal(0, _driver.Starts);
    }

    /// <summary>A request held behind an older one that waited past the pass limit still ends no-capacity at its own wait
    /// timeout, naming what it waited behind; the older one, whose wait timeout is longer, waits on.</summary>
    [Fact]
    public void RequestHeldBehindStillEndsAtItsOwnWaitTimeout()
    {
        RequestRecord e2e = _book.Create(new CreateRequest(RequestKind.E2e, new RequestInputs { BuildId = "b20261029-1" }));
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
        Assert.Equal(0, _driver.Starts);
    }

    /// <summary>Gives homepc a fresh host-facts report (one older than HostCapacity.Stale holds every slot) in which
    /// Windows' available memory and free commit are each <see cref="FreeMiB"/> above their reserves.</summary>
    private void Report() => _capacity.Override(new HostFactsReport(_clock.GetUtcNow(),
        AvailableMiB: (6 * 1024) + FreeMiB, FreeCommitMiB: (16 * 1024) + FreeMiB, QFreeMiB: 200 * 1024,
        FullScreenGame: false, UserState: 5));

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

    /// <summary>A slot driver whose every start fails: these tests must start no slot, and count any attempt.</summary>
    private sealed class NoSlotDriver : ISlotDriver
    {
        /// <summary>The attempted starts.</summary>
        private int _starts;

        /// <summary>The attempted starts so far.</summary>
        public int Starts => Volatile.Read(ref _starts);

        /// <inheritdoc/>
        public string Name => "none";

        /// <inheritdoc/>
        public bool IsVm => true;

        /// <inheritdoc/>
        public string GuestShareRoot(SlotSpec spec) => spec.ExchangeDir;

        /// <inheritdoc/>
        /// <exception cref="InvalidOperationException">Always: no slot may start in these tests.</exception>
        public Task<string> StartAsync(SlotSpec spec, CancellationToken cancel)
        {
            Interlocked.Increment(ref _starts);
            throw new InvalidOperationException("no slot may start in the scheduler's tests");
        }

        /// <inheritdoc/>
        public Task StopAsync(string handle, bool graceful, CancellationToken cancel) => Task.CompletedTask;

        /// <inheritdoc/>
        public Task<bool> IsAliveAsync(string handle, CancellationToken cancel) => Task.FromResult(false);

        /// <inheritdoc/>
        public Task<bool> SetBalloonAsync(string handle, long targetMiB, CancellationToken cancel) => Task.FromResult(false);
    }
}
