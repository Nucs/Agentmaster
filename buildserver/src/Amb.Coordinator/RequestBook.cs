using System.Collections.Concurrent;
using System.Text.Json;
using Amb.Core.Allocation;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;

namespace Amb.Coordinator;

/// <summary>A request the coordinator refuses to accept (an unknown kind, a missing input): the API answers 400.</summary>
/// <param name="message">What is wrong, for the caller.</param>
public sealed class RequestRejectedException(string message) : Exception(message);

/// <summary>The defaults of each kind (DESIGN.md §2 timeouts, §3.1 sizes).</summary>
public static class RequestDefaults
{
    /// <summary>The default least and most resources of a kind, or null when it needs no slot.</summary>
    /// <param name="kind">The kind.</param>
    /// <returns>The resource request, or null for an import.</returns>
    public static ResourceRequest? ResourcesOf(RequestKind kind) => kind switch
    {
        // An e2e VM: Tiny11 Core uses 1.4-1.9 GB at rest, Agentmaster with a tab ~150 MB more; the e2e runner's own
        // default is 8 GB / 6 CPUs. Its throwaway overlay grows 1-3 GB.
        RequestKind.E2e => new ResourceRequest(
            new Resources(Resources.Gib(3), 2, Resources.Gib(1)),
            new Resources(Resources.Gib(8), 6, Resources.Gib(3))),
        // The smoke check only boots and runs one command.
        RequestKind.Smoke => new ResourceRequest(
            new Resources(Resources.Gib(2), 2, Resources.Gib(1)),
            new Resources(Resources.Gib(4), 2, Resources.Gib(2))),
        RequestKind.Build => new ResourceRequest(
            new Resources(Resources.Gib(8), 4, Resources.Gib(35)),
            new Resources(Resources.Gib(16), 8, Resources.Gib(35))),
        // A base install: 4 GB is what the measurements installed Tiny11 Core with; its 64 GB disk is sparse, ~12 GB
        // written (9.2 GB image + the 3.3 GB ISO while it installs).
        RequestKind.Install => new ResourceRequest(
            new Resources(Resources.Gib(4), 2, Resources.Gib(14)),
            new Resources(Resources.Gib(4), 4, Resources.Gib(16))),
        // A layer: VS Build Tools took 18.5 GB and a vCPU of servicing afterwards; the e2e layer ~2 GB.
        RequestKind.Layer => new ResourceRequest(
            new Resources(Resources.Gib(4), 2, Resources.Gib(4)),
            new Resources(Resources.Gib(6), 4, Resources.Gib(22))),
        _ => null,
    };

    /// <summary>How long a kind may wait for a slot.</summary>
    /// <param name="kind">The kind.</param>
    /// <returns>Build 60 min, test 30, e2e 120, smoke 30, install and layer 60, import 5 (it needs no slot, only the
    /// coordinator).</returns>
    public static TimeSpan WaitOf(RequestKind kind) => TimeSpan.FromMinutes(kind switch
    {
        RequestKind.Build => 60,
        RequestKind.Test => 30,
        RequestKind.E2e => 120,
        RequestKind.Smoke => 30,
        RequestKind.Install or RequestKind.Layer => 60,
        _ => 5,
    });

    /// <summary>How long a kind's work may run.</summary>
    /// <param name="kind">The kind.</param>
    /// <returns>Build 3 h, test 1 h, e2e 2 h, smoke 20 min, install 75 min (its download and install took 16.5 min,
    /// measured), layer 2 h (VS Build Tools took 28 min plus servicing), import 30 min.</returns>
    public static TimeSpan RunOf(RequestKind kind) => TimeSpan.FromMinutes(kind switch
    {
        RequestKind.Build => 180,
        RequestKind.Test => 60,
        RequestKind.E2e => 120,
        RequestKind.Smoke => 20,
        RequestKind.Install => 75,
        RequestKind.Layer => 120,
        _ => 30,
    });
}

/// <summary>
/// Every request the coordinator knows: their records (in the record store, the truth), their event logs, and the
/// signals that wake whoever waits on them. The one place a request's state changes, so every change is checked against
/// the life of DESIGN.md §12 and logged as an event.
/// </summary>
public sealed class RequestBook
{
    private readonly RecordStore _store;
    private readonly TimeProvider _clock;
    private readonly ConcurrentDictionary<string, RequestRecord> _active = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, AsyncSignal> _signals = new(StringComparer.Ordinal);

    /// <summary>Opens the book over a record store, loading every request that has not ended.</summary>
    /// <param name="store">The record store.</param>
    /// <param name="clock">The clock (TimeProvider.System in production).</param>
    /// <exception cref="ArgumentNullException">An argument is null.</exception>
    /// <exception cref="IOException">The records could not be read.</exception>
    /// <exception cref="JsonException">A record is damaged.</exception>
    public RequestBook(RecordStore store, TimeProvider clock)
    {
        ArgumentNullException.ThrowIfNull(store);
        ArgumentNullException.ThrowIfNull(clock);
        _store = store;
        _clock = clock;
        foreach (string id in store.List("requests"))
        {
            RequestRecord? record = store.Read("requests", id, AmbJson.Default.EnvelopeRequestRecord)?.Value;
            if (record is not null && !RequestStates.IsFinal(record.State))
            {
                _active[id] = record;
            }
        }
    }

    /// <summary>Pulsed whenever a request is added, ends, or changes state: the scheduler waits on it.</summary>
    public AsyncSignal QueueChanged { get; } = new();

    /// <summary>The requests that have not ended, oldest first.</summary>
    public IReadOnlyList<RequestRecord> Active => [.. _active.Values.OrderBy(r => r.CreatedAt).ThenBy(r => r.Id, StringComparer.Ordinal)];

    /// <summary>Accepts a request: fills its defaults, checks its inputs, records it as pending.</summary>
    /// <param name="ask">What the caller asked for.</param>
    /// <returns>The new record.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="ask"/> is null.</exception>
    /// <exception cref="RequestRejectedException">The kind is not served yet, or an input is missing or unsafe.</exception>
    /// <exception cref="IOException">The record could not be written.</exception>
    public RequestRecord Create(CreateRequest ask)
    {
        ArgumentNullException.ThrowIfNull(ask);
        RequestInputs inputs = ask.Inputs ?? new RequestInputs();
        switch (ask.Kind)
        {
            case RequestKind.Import when string.IsNullOrWhiteSpace(inputs.Source):
                throw new RequestRejectedException("an import needs inputs.source (a path this PC can read)");
            case RequestKind.E2e when !Ids.IsSafe(inputs.BuildId):
                throw new RequestRejectedException("an e2e request needs inputs.buildId (an existing build id)");
            case RequestKind.Test:
                throw new RequestRejectedException("Test is not served yet (DESIGN.md §13: `amb test` comes after the build MVP)");
            case RequestKind.Build:
                inputs = NormalizeBuild(inputs);
                break;
            case RequestKind.Install when !Ids.IsSafe(inputs.NewImage) || string.IsNullOrEmpty(inputs.Version)
                || string.IsNullOrEmpty(inputs.OemDir):
                throw new RequestRejectedException("an install needs inputs.newImage, inputs.version and inputs.oemDir");
            case RequestKind.Layer when !Ids.IsSafe(inputs.Image) || !Ids.IsSafe(inputs.NewImage)
                || inputs.Steps is not { Count: > 0 }:
                throw new RequestRejectedException("a layer needs inputs.image, inputs.newImage and inputs.steps");
        }
        foreach (StageItem item in inputs.Stage ?? [])
        {
            if (string.IsNullOrEmpty(item.Source) || string.IsNullOrEmpty(item.Target) || item.Target.Contains("..")
                || item.Target.StartsWith('/') || item.Target.Contains(':'))
            {
                throw new RequestRejectedException($"a stage item needs a source and a relative target: {item.Target}");
            }
        }
        if (inputs.Image is not null && !Ids.IsSafe(inputs.Image))
        {
            throw new RequestRejectedException($"not a safe image name: {inputs.Image}");
        }
        DateTimeOffset now = _clock.GetUtcNow();
        var record = new RequestRecord
        {
            Id = Ids.NewRequestId(now),
            Kind = ask.Kind,
            State = RequestState.Pending,
            Inputs = inputs,
            Resources = ask.Resources ?? RequestDefaults.ResourcesOf(ask.Kind),
            CreatedAt = now,
            UpdatedAt = now,
            WaitTimeout = ask.WaitTimeoutMinutes is int w ? TimeSpan.FromMinutes(w) : RequestDefaults.WaitOf(ask.Kind),
            RunTimeout = ask.RunTimeoutMinutes is int r ? TimeSpan.FromMinutes(r) : RequestDefaults.RunOf(ask.Kind),
        };
        _store.Write("requests", record.Id, record, 0, AmbJson.Default.EnvelopeRequestRecord);
        _active[record.Id] = record;
        Event(record.Id, "state", $"accepted: {record.Kind}", RequestState.Pending);
        QueueChanged.Pulse();
        return record;
    }

    /// <summary>Checks a build request's inputs and fills its defaults, so its record says exactly what is built.</summary>
    /// <param name="inputs">The inputs as asked.</param>
    /// <returns>The inputs with the commit in lower case and the configuration, identity and purge in their canonical
    /// spelling ("Release" / "Debug", "dev" / "release", "none" / "force"; defaults Release, dev, none).</returns>
    /// <exception cref="RequestRejectedException">The commit is not a full 40-hex SHA-1, or a value is unknown.</exception>
    /// <remarks>A full SHA only, never a ref: a branch moves, and a build id names immutable bytes, so `amb build` resolves
    /// HEAD or a branch on the caller's side and sends the commit it means.</remarks>
    internal static RequestInputs NormalizeBuild(RequestInputs inputs)
    {
        string commit = (inputs.Commit ?? "").Trim().ToLowerInvariant();
        if (commit.Length != 40 || !commit.All(char.IsAsciiHexDigitLower))
        {
            throw new RequestRejectedException("a build needs inputs.commit: a full 40-character commit SHA (resolve a ref first)");
        }
        string configuration = (inputs.Configuration ?? "Release").Trim().ToLowerInvariant() switch
        {
            "release" => "Release",
            "debug" => "Debug",
            _ => throw new RequestRejectedException($"inputs.configuration must be Release or Debug, not {inputs.Configuration}"),
        };
        string identity = (inputs.Identity ?? "dev").Trim().ToLowerInvariant() switch
        {
            "dev" => "dev",
            "release" => "release",
            _ => throw new RequestRejectedException($"inputs.identity must be dev or release, not {inputs.Identity}"),
        };
        string purge = (inputs.Purge ?? "none").Trim().ToLowerInvariant() switch
        {
            "none" => "none",
            "force" => "force",
            _ => throw new RequestRejectedException($"inputs.purge must be none or force, not {inputs.Purge}"),
        };
        return inputs with { Commit = commit, Configuration = configuration, Identity = identity, Purge = purge };
    }

    /// <summary>Reads a request's record.</summary>
    /// <param name="id">The request's id.</param>
    /// <returns>The record, or null when there is none (or the id is not safe).</returns>
    /// <exception cref="IOException">The record could not be read.</exception>
    /// <exception cref="JsonException">The record is damaged.</exception>
    public RequestRecord? Get(string id) =>
        Ids.IsSafe(id) ? _store.Read("requests", id, AmbJson.Default.EnvelopeRequestRecord)?.Value : null;

    /// <summary>Lists the newest requests.</summary>
    /// <param name="limit">How many at most.</param>
    /// <returns>The newest first.</returns>
    /// <exception cref="IOException">The records could not be read.</exception>
    /// <exception cref="JsonException">A record is damaged.</exception>
    public IReadOnlyList<RequestRecord> Newest(int limit) =>
        [.. _store.List("requests").Reverse().Take(Math.Clamp(limit, 1, 500)).Select(Get).OfType<RequestRecord>()];

    /// <summary>Moves a request to a new state (when the life of DESIGN.md §12 allows it), changing other fields too.</summary>
    /// <param name="id">The request's id.</param>
    /// <param name="to">The new state.</param>
    /// <param name="message">The event's line.</param>
    /// <param name="change">Further changes to the record (outcome, slot, grant), or null.</param>
    /// <returns>The new record, or null when the move is not allowed (the request ended, or a racing move won).</returns>
    /// <exception cref="IOException">The record could not be written.</exception>
    /// <exception cref="JsonException">The record is damaged.</exception>
    public RequestRecord? Move(string id, RequestState to, string message, Func<RequestRecord, RequestRecord>? change = null)
    {
        bool moved = false;
        Versioned<RequestRecord>? result = _store.Update<RequestRecord>("requests", id, current =>
        {
            if (current is null || !RequestStates.CanMove(current.State, to))
            {
                return null;
            }
            DateTimeOffset now = _clock.GetUtcNow();
            RequestRecord next = (change?.Invoke(current) ?? current) with { State = to, UpdatedAt = now };
            if (to is RequestState.Starting or RequestState.Running && next.StartedAt is null)
            {
                next = next with { StartedAt = now };
            }
            if (RequestStates.IsFinal(to))
            {
                next = next with { EndedAt = now };
            }
            moved = true;
            return next;
        }, AmbJson.Default.EnvelopeRequestRecord);
        if (!moved || result is null)
        {
            return null;
        }
        if (RequestStates.IsFinal(to))
        {
            _active.TryRemove(id, out _);
        }
        else
        {
            _active[id] = result.Value;
        }
        Event(id, "state", message, to);
        QueueChanged.Pulse();
        return result.Value;
    }

    /// <summary>Changes a request's fields without moving its state (the reason it waits, its results folder).</summary>
    /// <param name="id">The request's id.</param>
    /// <param name="change">The change; it must not change the state.</param>
    /// <returns>The new record, or null when the request does not exist.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="change"/> is null.</exception>
    /// <exception cref="IOException">The record could not be written.</exception>
    /// <exception cref="JsonException">The record is damaged.</exception>
    public RequestRecord? Patch(string id, Func<RequestRecord, RequestRecord> change)
    {
        ArgumentNullException.ThrowIfNull(change);
        Versioned<RequestRecord>? result = _store.Update<RequestRecord>("requests", id,
            current => current is null ? null : change(current) with { State = current.State, UpdatedAt = _clock.GetUtcNow() },
            AmbJson.Default.EnvelopeRequestRecord);
        if (result is not null && !RequestStates.IsFinal(result.Value.State))
        {
            _active[id] = result.Value;
        }
        return result?.Value;
    }

    /// <summary>Appends one event to a request's log and wakes whoever waits on the request.</summary>
    /// <param name="id">The request's id.</param>
    /// <param name="kind">"state", "progress", "log", "placement", "error".</param>
    /// <param name="message">The line.</param>
    /// <param name="state">The state after the event, for state events.</param>
    /// <exception cref="IOException">The event could not be appended.</exception>
    public void Event(string id, string kind, string message, RequestState? state = null)
    {
        // An event line is one line: a multi-line message (an exception's text) is folded.
        string folded = message.Replace("\r", "", StringComparison.Ordinal).Replace("\n", " | ", StringComparison.Ordinal);
        string line = JsonSerializer.Serialize(new RequestEvent(_clock.GetUtcNow(), kind, folded, state),
            AmbJsonLine.Default.RequestEvent);
        _store.AppendEventLine(id, line);
        SignalOf(id).Pulse();
    }

    /// <summary>Reads a request's events from one on.</summary>
    /// <param name="id">The request's id.</param>
    /// <param name="skip">How many the caller has seen.</param>
    /// <returns>The events after them (unparsable lines are skipped).</returns>
    /// <exception cref="IOException">The log could not be read.</exception>
    public IReadOnlyList<RequestEvent> Events(string id, int skip)
    {
        var events = new List<RequestEvent>();
        foreach (string line in _store.ReadEventLines(id, skip))
        {
            try
            {
                if (JsonSerializer.Deserialize(line, AmbJsonLine.Default.RequestEvent) is { } e)
                {
                    events.Add(e);
                }
            }
            catch (JsonException)
            {
                // A damaged line (a crash mid-append) is skipped: events are a log, the record is the truth.
            }
        }
        return events;
    }

    /// <summary>Waits until a request changes (a new event) or the time runs out.</summary>
    /// <param name="id">The request's id.</param>
    /// <param name="timeout">The longest wait.</param>
    /// <param name="cancel">Cancels the wait.</param>
    /// <returns>True when something changed.</returns>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    public Task<bool> WaitForChangeAsync(string id, TimeSpan timeout, CancellationToken cancel) =>
        SignalOf(id).WaitAsync(timeout, cancel);

    /// <summary>The signal of one request (made on first use).</summary>
    /// <param name="id">The request's id.</param>
    /// <returns>Its signal.</returns>
    private AsyncSignal SignalOf(string id) => _signals.GetOrAdd(id, _ => new AsyncSignal());
}
