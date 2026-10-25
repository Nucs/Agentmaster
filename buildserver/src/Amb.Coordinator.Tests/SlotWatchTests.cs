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
/// The slot manager's liveness watch (SlotManager.StartWatch) and a session's abandon: a slot whose VM stops by itself
/// is given up within two checks, one failed check never ends a healthy slot, and a stop on purpose ends the watch.
/// </summary>
public sealed class SlotWatchTests : IDisposable
{
    /// <summary>The grant every test slot gets.</summary>
    private static readonly Resources Grant = new(Resources.Gib(4), 2, Resources.Gib(2));

    /// <summary>The test's own site root under %TEMP%, deleted after it.</summary>
    private readonly string _root = Path.Combine(Path.GetTempPath(), "amb-watch-" + Guid.NewGuid().ToString("N"));

    /// <summary>The clock the watch's waits run on.</summary>
    private readonly FakeTimeProvider _clock = new(DateTimeOffset.Parse("2026-10-25T10:00:00Z"));

    /// <summary>The driver whose slots live or die on the test's word.</summary>
    private readonly FakeDriver _driver = new();

    /// <summary>The slot manager under test.</summary>
    private readonly SlotManager _slots;

    /// <summary>Makes a site root with a stand-in guest agent file, and the manager over it.</summary>
    public SlotWatchTests()
    {
        Directory.CreateDirectory(_root);
        string agent = Path.Combine(_root, "amb-guest.exe");
        File.WriteAllText(agent, "a stand-in: the fake driver never runs it");
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
        _slots = new SlotManager(options, new RecordStore(options.RecordsDir), new GuestBroker(_clock), _driver, _clock,
            NullLogger<SlotManager>.Instance);
    }

    /// <summary>Deletes the site root.</summary>
    public void Dispose() => Directory.Delete(_root, recursive: true);

    /// <summary>A VM that stops by itself fails the hello wait as lost after two "not running" answers.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task VmThatStopsIsGivenUpWithinTwoChecks()
    {
        SlotLease lease = await _slots.StartAsync(Request(), "am-e2e", Grant, CancellationToken.None);
        Task<GuestHello> hello = lease.Session.WaitHelloAsync(TimeSpan.FromMinutes(10), CancellationToken.None);
        Assert.True(await AdvanceUntilAsync(() => _driver.Checks >= 2));
        Assert.False(hello.IsCompleted);
        _driver.Alive = false;
        Assert.True(await AdvanceUntilAsync(() => hello.IsCompleted));
        SlotLostException lost = await Assert.ThrowsAsync<SlotLostException>(() => hello);
        Assert.Contains("stopped by itself", lost.Message, StringComparison.Ordinal);
        // A later step fails at once too.
        await Assert.ThrowsAsync<SlotLostException>(() => lease.Session.RunStepAsync(
            new GuestStep("1", StepKinds.Run, Command: "cmd.exe"), TimeSpan.FromMinutes(3), CancellationToken.None));
        await _slots.StopAsync(lease, graceful: false);
        Assert.Empty(_slots.Active);
    }

    /// <summary>An outstanding step fails as lost as soon as the slot is abandoned, not after the silence limit.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task OutstandingStepFailsWhenTheVmStops()
    {
        SlotLease lease = await _slots.StartAsync(Request(), "am-e2e", Grant, CancellationToken.None);
        Task<StepResult> step = lease.Session.RunStepAsync(new GuestStep("1", StepKinds.Run, Command: "cmd.exe"),
            TimeSpan.FromHours(1), CancellationToken.None);
        _driver.Alive = false;
        Assert.True(await AdvanceUntilAsync(() => step.IsCompleted));
        await Assert.ThrowsAsync<SlotLostException>(() => step);
        await _slots.StopAsync(lease, graceful: false);
    }

    /// <summary>"Not running" answers that never come twice in a row (a failed podman inspect) never end a slot.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task SingleDeadAnswersAreIgnored()
    {
        lock (_driver.Answers)
        {
            foreach (bool answer in new[] { false, true, false, true, false, true })
            {
                _driver.Answers.Enqueue(answer);
            }
        }
        SlotLease lease = await _slots.StartAsync(Request(), "am-e2e", Grant, CancellationToken.None);
        Task<GuestHello> hello = lease.Session.WaitHelloAsync(TimeSpan.FromMinutes(10), CancellationToken.None);
        Assert.True(await AdvanceUntilAsync(() => _driver.Checks >= 8));
        Assert.False(hello.IsCompleted);
        await _slots.StopAsync(lease, graceful: false);
    }

    /// <summary>A stop on purpose ends the watch: the driver is not asked again, so a graceful shutdown never reads as
    /// a death.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task StopOnPurposeEndsTheWatch()
    {
        SlotLease lease = await _slots.StartAsync(Request(), "am-e2e", Grant, CancellationToken.None);
        Assert.True(await AdvanceUntilAsync(() => _driver.Checks >= 1));
        // The guest's shutdown step ended the VM: the stop finds it stopped and removes it.
        _driver.Alive = false;
        await _slots.StopAsync(lease, graceful: true);
        int checks = _driver.Checks;
        for (int i = 0; i < 10; i++)
        {
            _clock.Advance(SlotManager.WatchInterval);
            await Task.Delay(10);
        }
        Assert.Equal(checks, _driver.Checks);
        Assert.Equal([false], _driver.StopsGraceful);
    }

    /// <summary>A thrown-away slot whose Windows does not stop after its shutdown step is removed by force once
    /// <see cref="SlotManager.SelfStopGrace"/> has passed, not after the driver's own 100-s graceful window.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task HungShutdownIsCappedForAThrownAwaySlot()
    {
        SlotLease lease = await _slots.StartAsync(Request(), "am-e2e", Grant, CancellationToken.None);
        Task stop = _slots.StopAsync(lease, graceful: true);
        // The fake driver says "running" throughout: only the grace ends the wait.
        Assert.True(await AdvanceUntilAsync(() => stop.IsCompleted, TimeSpan.FromSeconds(1)));
        await stop;
        Assert.Equal([false], _driver.StopsGraceful);
        Assert.Empty(_slots.Active);
    }

    /// <summary>A slot that becomes an image keeps the driver's full graceful stop (its disk must be consistent), and
    /// its folder becomes the image.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task CommittedSlotKeepsTheFullGracefulStop()
    {
        SlotLease lease = await _slots.StartAsync(Request(), "am-new", Grant, CancellationToken.None, installVersion: "core11");
        await _slots.StopAsync(lease, graceful: true, commitAs: "am-new");
        Assert.Equal([true], _driver.StopsGraceful);
        Assert.True(File.Exists(Path.Combine(_root, "images", "am-new", "data.qcow2")));
    }

    /// <summary>Advances the clock one step at a time until a condition holds (the manager's continuations run on the
    /// thread pool, so each step gives them a moment).</summary>
    /// <param name="done">The condition.</param>
    /// <param name="step">How far each step moves the clock; null = one watch interval.</param>
    /// <returns>True when it held within 200 steps.</returns>
    private async Task<bool> AdvanceUntilAsync(Func<bool> done, TimeSpan? step = null)
    {
        for (int i = 0; i < 200; i++)
        {
            if (done())
            {
                return true;
            }
            _clock.Advance(step ?? SlotManager.WatchInterval);
            await Task.Delay(10);
        }
        return done();
    }

    /// <summary>A request for a test slot.</summary>
    /// <returns>The record.</returns>
    private RequestRecord Request() => new()
    {
        Id = Ids.NewRequestId(_clock.GetUtcNow()),
        Kind = RequestKind.Smoke,
        State = RequestState.Starting,
        Inputs = new RequestInputs(),
        CreatedAt = _clock.GetUtcNow(),
        UpdatedAt = _clock.GetUtcNow(),
        WaitTimeout = TimeSpan.FromMinutes(30),
        RunTimeout = TimeSpan.FromMinutes(20),
    };

    /// <summary>A slot driver whose slots live or die on the test's word; it starts and stops nothing.</summary>
    private sealed class FakeDriver : ISlotDriver
    {
        /// <summary>The liveness checks made so far.</summary>
        private int _checks;

        /// <summary>Answers to give first, in order (guarded by locking the queue); then <see cref="Alive"/>.</summary>
        public Queue<bool> Answers { get; } = new();

        /// <summary>The answer once <see cref="Answers"/> is empty.</summary>
        public bool Alive { get; set; } = true;

        /// <summary>The liveness checks made so far.</summary>
        public int Checks => Volatile.Read(ref _checks);

        /// <summary>The <c>graceful</c> argument of every stop, in order (guarded by locking the list).</summary>
        public List<bool> StopsGraceful { get; } = [];

        /// <inheritdoc/>
        public string Name => "fake";

        /// <inheritdoc/>
        public bool IsVm => true;

        /// <inheritdoc/>
        public string GuestShareRoot(SlotSpec spec) => spec.ExchangeDir;

        /// <inheritdoc/>
        /// <remarks>Writes the two files a finished install leaves (dockur's disk and its windows.boot marker), so a
        /// slot can be kept as an image.</remarks>
        public Task<string> StartAsync(SlotSpec spec, CancellationToken cancel)
        {
            Directory.CreateDirectory(spec.SlotDir);
            File.WriteAllText(Path.Combine(spec.SlotDir, "data.qcow2"), "a stand-in disk");
            File.WriteAllText(Path.Combine(spec.SlotDir, "windows.boot"), "");
            return Task.FromResult("fake-" + spec.SlotId);
        }

        /// <inheritdoc/>
        public Task StopAsync(string handle, bool graceful, CancellationToken cancel)
        {
            lock (StopsGraceful)
            {
                StopsGraceful.Add(graceful);
            }
            return Task.CompletedTask;
        }

        /// <inheritdoc/>
        public Task<bool> IsAliveAsync(string handle, CancellationToken cancel)
        {
            Interlocked.Increment(ref _checks);
            lock (Answers)
            {
                return Task.FromResult(Answers.Count > 0 ? Answers.Dequeue() : Alive);
            }
        }

        /// <inheritdoc/>
        public Task<bool> SetBalloonAsync(string handle, long targetMiB, CancellationToken cancel) => Task.FromResult(false);
    }
}
