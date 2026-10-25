using System.Collections.Concurrent;
using Amb.Coordinator.Jobs;
using Amb.Coordinator.Slots;
using Amb.Core.Allocation;
using Amb.Core.Model;
using Amb.Core.Records;

namespace Amb.Coordinator;

/// <summary>
/// Admits waiting requests and runs their jobs (DESIGN.md §3.3, §3.5, §12): every pending request is scored against
/// what the sites have free, whenever something changes (a request comes or ends, a slot is released, Windows' numbers
/// move) and at least every 15 s; a request that fits gets its grant and its job; one that waited past its wait timeout
/// ends no-capacity, naming what was short.
/// </summary>
/// <remarks>
/// <para><b>Order:</b> first come, first served; a younger request may pass an older one that does not fit, until the
/// older one has waited 10 minutes: from then on nothing younger is admitted, so a large request is not starved by a
/// stream of small ones.</para>
/// <para><b>Restarts:</b> a request that was starting or running when the coordinator stopped is marked lost (its slot is
/// removed by <see cref="SlotManager.RemoveLeftoversAsync"/>); a pending one simply waits on.</para>
/// </remarks>
public sealed class Scheduler : BackgroundService
{
    /// <summary>How long an older request that does not fit may be passed by younger ones (DESIGN.md §3.5).</summary>
    public static readonly TimeSpan PassLimit = TimeSpan.FromMinutes(10);

    private readonly RequestBook _book;
    private readonly HostCapacity _capacity;
    private readonly SlotManager _slots;
    private readonly RecordStore _store;
    private readonly CoordinatorOptions _options;
    private readonly TimeProvider _clock;
    private readonly ILogger<Scheduler> _log;
    private readonly ILoggerFactory _logs;
    private readonly ConcurrentDictionary<string, RunningJob> _running = new(StringComparer.Ordinal);

    /// <summary>A job that runs: its task, its cancellation, and whether a cancel (not a timeout) stopped it.</summary>
    private sealed class RunningJob(CancellationTokenSource cancel)
    {
        /// <summary>Cancelled by a cancel request or the run timeout.</summary>
        public CancellationTokenSource Cancel { get; } = cancel;

        /// <summary>Set when a caller cancelled the request (a timeout leaves it false).</summary>
        public volatile bool CancelRequested;

        /// <summary>The job's task.</summary>
        public Task? Task { get; set; }
    }

    /// <summary>Creates the scheduler.</summary>
    /// <param name="book">The request book.</param>
    /// <param name="capacity">homepc's capacity.</param>
    /// <param name="slots">The slot manager.</param>
    /// <param name="store">The record store.</param>
    /// <param name="options">The settings.</param>
    /// <param name="clock">The clock.</param>
    /// <param name="logs">The log factory (each job logs under its own category).</param>
    public Scheduler(RequestBook book, HostCapacity capacity, SlotManager slots, RecordStore store,
        CoordinatorOptions options, TimeProvider clock, ILoggerFactory logs)
    {
        _book = book;
        _capacity = capacity;
        _slots = slots;
        _store = store;
        _options = options;
        _clock = clock;
        _logs = logs;
        _log = logs.CreateLogger<Scheduler>();
    }

    /// <summary>How many jobs run now.</summary>
    public int Running => _running.Count;

    /// <summary>Cancels a request: a pending one ends at once; a running one's job is cancelled and ends Cancelled once
    /// its slot is stopped.</summary>
    /// <param name="id">The request's id.</param>
    /// <returns>True when the request existed and had not ended.</returns>
    /// <exception cref="IOException">The record could not be written.</exception>
    public bool Cancel(string id)
    {
        if (_running.TryGetValue(id, out RunningJob? job))
        {
            job.CancelRequested = true;
            job.Cancel.Cancel();
            _book.Event(id, "log", "cancel requested: stopping the job and its slot");
            return true;
        }
        return _book.Move(id, RequestState.Cancelled, "cancelled while it waited") is not null;
    }

    /// <summary>Recovers from an earlier run, then admits requests until shutdown.</summary>
    /// <param name="stoppingToken">Cancelled at shutdown.</param>
    /// <returns>A task that ends at shutdown, after every running job was cancelled and has stopped its slot.</returns>
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        foreach (RequestRecord request in _book.Active.Where(r => r.State != RequestState.Pending))
        {
            _book.Move(request.Id, RequestState.Lost, "the coordinator restarted while this request ran");
        }
        int left = await _slots.RemoveLeftoversAsync(stoppingToken).ConfigureAwait(false);
        if (left > 0)
        {
            _log.LogWarning("removed {Count} slot(s) an earlier run left", left);
        }
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                Admit();
            }
            catch (Exception ex) when (ex is IOException or System.Text.Json.JsonException)
            {
                // A record that cannot be read or written stops this pass, not the scheduler: the next pass retries.
                _log.LogError(ex, "admission pass failed");
            }
            try
            {
                await Task.WhenAny(
                    _book.QueueChanged.WaitAsync(TimeSpan.FromSeconds(15), stoppingToken),
                    _capacity.Changed.WaitAsync(TimeSpan.FromSeconds(15), stoppingToken),
                    _slots.Released.WaitAsync(TimeSpan.FromSeconds(15), stoppingToken)).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }
        }
        foreach (RunningJob job in _running.Values)
        {
            job.Cancel.Cancel();
        }
        await Task.WhenAll(_running.Values.Select(j => j.Task ?? Task.CompletedTask)).ConfigureAwait(false);
    }

    /// <summary>One admission pass over every pending request, oldest first.</summary>
    /// <exception cref="IOException">A record could not be read or written.</exception>
    private void Admit()
    {
        DateTimeOffset now = _clock.GetUtcNow();
        foreach (RequestRecord request in _book.Active.Where(r => r.State == RequestState.Pending))
        {
            if (request.Resources is null)
            {
                // No slot (an import): it only needs the coordinator.
                Start(_book.Move(request.Id, RequestState.Starting, "started (no slot needed)"));
                continue;
            }
            SiteFree homepc = _capacity.HomepcFree();
            Placement? placement = Allocator.Choose(request.Resources, [homepc]);
            if (placement is null)
            {
                string shortage = Allocator.ExplainShortage(request.Resources, [homepc]);
                if (now - request.CreatedAt > request.WaitTimeout)
                {
                    _book.Move(request.Id, RequestState.NoCapacity, $"no-capacity after {request.WaitTimeout.TotalMinutes:0} min: {shortage}",
                        r => r with { Outcome = "no-capacity: " + shortage });
                    continue;
                }
                string waiting = "waiting: " + shortage;
                if (request.Outcome != waiting)
                {
                    _book.Patch(request.Id, r => r with { Outcome = waiting });
                    _book.Event(request.Id, "placement", waiting);
                }
                if (now - request.CreatedAt > PassLimit)
                {
                    // It has waited long enough: nothing younger passes it, so the next free capacity is its own.
                    break;
                }
                continue;
            }
            Start(_book.Move(request.Id, RequestState.Starting,
                FormattableString.Invariant($"placed on {placement.Site} (score {placement.Score:0.00}): {placement.Grant}"),
                r => r with { Site = placement.Site, Grant = placement.Grant, Outcome = null }));
        }
    }

    /// <summary>Starts a request's job on the thread pool, with its run timeout, and records how it ended.</summary>
    /// <param name="request">The request, as moved to Starting (null when the move lost a race: nothing is started).</param>
    private void Start(RequestRecord? request)
    {
        if (request is null)
        {
            return;
        }
        var cancel = new CancellationTokenSource(request.RunTimeout, _clock);
        var running = new RunningJob(cancel);
        _running[request.Id] = running;
        IJob job = JobFor(request.Kind);
        var context = new JobContext(request, _book, _slots, _store, _options, _clock,
            _logs.CreateLogger("Amb.Coordinator.Jobs." + request.Kind));
        running.Task = Task.Run(async () =>
        {
            try
            {
                JobOutcome outcome = await job.RunAsync(context, cancel.Token).ConfigureAwait(false);
                _book.Move(request.Id, outcome.Succeeded ? RequestState.Succeeded : RequestState.Failed, outcome.Outcome,
                    r => r with
                    {
                        Outcome = outcome.Outcome,
                        ExitCode = outcome.ExitCode,
                        BuildId = outcome.BuildId ?? r.BuildId,
                        ResultsPath = outcome.ResultsPath ?? r.ResultsPath,
                    });
            }
            catch (OperationCanceledException) when (running.CancelRequested)
            {
                _book.Move(request.Id, RequestState.Cancelled, "cancelled", r => r with { Outcome = "cancelled" });
            }
            catch (OperationCanceledException)
            {
                string outcome = $"run timeout after {request.RunTimeout.TotalMinutes:0} min";
                _book.Move(request.Id, RequestState.Failed, outcome, r => r with { Outcome = outcome });
            }
            catch (SlotLostException ex)
            {
                _book.Move(request.Id, RequestState.Lost, ex.Message, r => r with { Outcome = ex.Message });
            }
            catch (Exception ex)
            {
                // Any other failure ends the request Failed with what happened; the scheduler itself carries on.
                _log.LogError(ex, "request {Id} failed", request.Id);
                string outcome = $"{ex.GetType().Name}: {ex.Message}";
                _book.Move(request.Id, RequestState.Failed, outcome, r => r with { Outcome = outcome });
            }
            finally
            {
                _running.TryRemove(request.Id, out _);
                cancel.Dispose();
                _book.QueueChanged.Pulse();
            }
        });
    }

    /// <summary>The job that does a kind's work.</summary>
    /// <param name="kind">The kind.</param>
    /// <returns>Its job.</returns>
    /// <exception cref="NotSupportedException">The kind is not served (the book refuses such requests already).</exception>
    private static IJob JobFor(RequestKind kind) => kind switch
    {
        RequestKind.Import => new ImportJob(),
        RequestKind.Smoke => new SmokeJob(),
        RequestKind.Install => new InstallJob(),
        RequestKind.Layer => new LayerJob(),
        RequestKind.E2e => new E2eJob(),
        _ => throw new NotSupportedException($"{kind} is not served yet"),
    };
}
