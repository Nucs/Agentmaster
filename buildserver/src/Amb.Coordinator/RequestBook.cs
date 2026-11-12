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
        // An e2e VM, tuned 2026-10-29 (DESIGN.md §16.15): a 2 GB cap on a 3 GB guest (E2eGuestMemoryMiB: the rest of its
        // RAM in the site's swap file on Q:) and 4 vCPUs ran the tuning tests in 1.15x the time of an uncapped 8 GB /
        // 6 vCPU VM; a 1.5 GB cap timed Claude Code's start out, 2 vCPUs made a UI test 2.7x slower. `amb e2e --ram 8`
        // asks for the old uncapped VM. Its throwaway overlay grows 1-3 GB.
        RequestKind.E2e => new ResourceRequest(
            new Resources(Resources.Gib(2), 4, Resources.Gib(1)),
            new Resources(Resources.Gib(2), 4, Resources.Gib(3))),
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

    /// <summary>The RAM an e2e VM's guest sees when the request names no size (3 GB over the default 2 GB cap: a capped
    /// slot, DESIGN.md §16.15). A request that names its memory (`--ram`) and no guest RAM gets an uncapped VM.</summary>
    public const long E2eGuestMemoryMiB = 3 * 1024;

    /// <summary>A request's inputs with its kind's default guest RAM filled in: an e2e request that names neither its
    /// memory nor its guest's RAM becomes the tuned capped VM.</summary>
    /// <param name="kind">The request's kind.</param>
    /// <param name="inputs">The inputs as asked.</param>
    /// <param name="guestMemoryMiB">The guest RAM to fill in (<see cref="E2eGuestMemoryMiB"/> unless a site sizes its
    /// e2e VMs otherwise, <see cref="RequestSizing.E2eGuestMemoryMiB"/>).</param>
    /// <returns>The inputs, with <see cref="RequestInputs.GuestMemoryMiB"/> set to <paramref name="guestMemoryMiB"/> for
    /// such an e2e request; unchanged otherwise.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="inputs"/> is null.</exception>
    public static RequestInputs WithDefaultGuestMemory(RequestKind kind, RequestInputs inputs,
        long guestMemoryMiB = E2eGuestMemoryMiB)
    {
        ArgumentNullException.ThrowIfNull(inputs);
        return kind == RequestKind.E2e && inputs.MemoryMiB is null && inputs.GuestMemoryMiB is null
            ? inputs with { GuestMemoryMiB = guestMemoryMiB }
            : inputs;
    }

    /// <summary>How long a kind may wait for a slot.</summary>
    /// <param name="kind">The kind.</param>
    /// <returns>Build 60 min, test 30, e2e 120, smoke 30, install and layer 60, deploy 30 (it needs no slot, but waits
    /// behind another deploy), import 5 (it needs no slot, only the coordinator).</returns>
    public static TimeSpan WaitOf(RequestKind kind) => TimeSpan.FromMinutes(kind switch
    {
        RequestKind.Build => 60,
        RequestKind.Test => 30,
        RequestKind.E2e => 120,
        RequestKind.Smoke => 30,
        RequestKind.Install or RequestKind.Layer => 60,
        RequestKind.Deploy => 30,
        _ => 5,
    });

    /// <summary>How long a kind's work may run.</summary>
    /// <param name="kind">The kind.</param>
    /// <returns>Build 3 h, test 1 h, e2e 2 h, smoke 20 min, install 150 min (its download and install took 16.5 min on
    /// homepc and over 43 min on the worker machine, measured; InstallJob waits up to 120 min for the new guest's hello),
    /// layer 2 h (VS Build Tools took 28 min plus servicing), deploy 75 min (up to 45 for the requesting
    /// session's turn to end, 10 for the build lock, then the swap: 5-17 s down, measured, and the watch), import
    /// 30 min.</returns>
    public static TimeSpan RunOf(RequestKind kind) => TimeSpan.FromMinutes(kind switch
    {
        RequestKind.Build => 180,
        RequestKind.Test => 60,
        RequestKind.E2e => 120,
        RequestKind.Smoke => 20,
        RequestKind.Install => 150,
        RequestKind.Layer => 120,
        RequestKind.Deploy => 75,
        _ => 30,
    });
}

/// <summary>
/// A site's own slot sizes for the kinds whose size depends on the machine (DESIGN.md §3.1): a build VM and an e2e VM.
/// homepc's are <see cref="RequestDefaults"/> (measured on a 64 GB PC that the owner works on); the worker machine has
/// 128 GB and nobody at its screen, so its builds get twice the memory and vCPUs and its e2e VMs are uncapped.
/// </summary>
/// <remarks>Only the defaults change: a request that names its own size (`--ram`, `--vcpus`) still gets exactly that, and
/// the kinds not named here keep <see cref="RequestDefaults.ResourcesOf"/>.</remarks>
public sealed record RequestSizing
{
    /// <summary>A build VM's least and most, or null for <see cref="RequestDefaults.ResourcesOf"/>'s (8-16 GB, 4-8 vCPUs).
    /// BuildJob.Parallelism picks msbuild's parallelism from the memory granted.</summary>
    public ResourceRequest? Build { get; init; }

    /// <summary>An e2e VM's least and most, or null for the default (a 2 GB cap, 4 vCPUs).</summary>
    public ResourceRequest? E2e { get; init; }

    /// <summary>The RAM an e2e VM's guest sees when the request names no size: null for
    /// <see cref="RequestDefaults.E2eGuestMemoryMiB"/> (3 GB over a 2 GB cap: a capped slot); 0 for none, so the VM is
    /// uncapped and its guest sees its whole grant (the worker machine's, which has memory to spare and no swap file).</summary>
    public long? E2eGuestMemoryMiB { get; init; }

    /// <summary>Whether the site can run a capped slot (a guest RAM above the slot's memory, the rest in a swap file):
    /// homepc's can (its site's swap file on Q:, DESIGN.md §16.14); the worker machine's cannot (CT 803 has no swap, so
    /// the guest would be killed when it touched more than its cap), and refuses such a request when it is made.</summary>
    public bool CappedSlots { get; init; } = true;

    /// <summary>The least and most resources of a kind on this site.</summary>
    /// <param name="kind">The kind.</param>
    /// <returns>This site's size for a build or an e2e run when it sets one, else the kind's default (null for a kind
    /// with no slot).</returns>
    public ResourceRequest? ResourcesOf(RequestKind kind) => kind switch
    {
        RequestKind.Build when Build is not null => Build,
        RequestKind.E2e when E2e is not null => E2e,
        _ => RequestDefaults.ResourcesOf(kind),
    };

    /// <summary>A request's inputs with this site's default guest RAM filled in (see
    /// <see cref="RequestDefaults.WithDefaultGuestMemory"/>).</summary>
    /// <param name="kind">The request's kind.</param>
    /// <param name="inputs">The inputs as asked.</param>
    /// <returns>The inputs, unchanged when this site's e2e VMs are uncapped (<see cref="E2eGuestMemoryMiB"/> 0).</returns>
    /// <exception cref="ArgumentNullException"><paramref name="inputs"/> is null.</exception>
    public RequestInputs WithDefaultGuestMemory(RequestKind kind, RequestInputs inputs)
    {
        ArgumentNullException.ThrowIfNull(inputs);
        long guest = E2eGuestMemoryMiB ?? RequestDefaults.E2eGuestMemoryMiB;
        return guest > 0 ? RequestDefaults.WithDefaultGuestMemory(kind, inputs, guest) : inputs;
    }
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
    private readonly RequestSizing _sizing;
    private readonly ConcurrentDictionary<string, RequestRecord> _active = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, AsyncSignal> _signals = new(StringComparer.Ordinal);

    /// <summary>Opens the book over a record store, loading every request that has not ended.</summary>
    /// <param name="store">The record store.</param>
    /// <param name="clock">The clock (TimeProvider.System in production).</param>
    /// <param name="sizing">The site's own slot sizes (the coordinator's <see cref="CoordinatorOptions.Sizing"/>), or null
    /// for homepc's defaults (the tests').</param>
    /// <exception cref="ArgumentNullException"><paramref name="store"/> or <paramref name="clock"/> is null.</exception>
    /// <exception cref="IOException">The records could not be read.</exception>
    /// <exception cref="JsonException">A record is damaged.</exception>
    public RequestBook(RecordStore store, TimeProvider clock, RequestSizing? sizing = null)
    {
        ArgumentNullException.ThrowIfNull(store);
        ArgumentNullException.ThrowIfNull(clock);
        _store = store;
        _clock = clock;
        _sizing = sizing ?? new RequestSizing();
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
    /// <exception cref="RequestRejectedException">The kind is not served yet, an input is missing or unsafe, or the request
    /// asks for a capped slot on a site that has none (<see cref="RequestSizing.CappedSlots"/>).</exception>
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
            case RequestKind.E2e when inputs.SuiteCommit is not null:
                // A full SHA only, like a build's commit: the job exports the suite of exactly that commit from the mirror.
                string suiteCommit = inputs.SuiteCommit.Trim().ToLowerInvariant();
                if (suiteCommit.Length != 40 || !suiteCommit.All(char.IsAsciiHexDigitLower))
                {
                    throw new RequestRejectedException("inputs.suiteCommit must be a full 40-character commit SHA");
                }
                if (inputs.SuiteSource is not null)
                {
                    throw new RequestRejectedException("inputs.suiteSource and inputs.suiteCommit name two suites: give one");
                }
                inputs = inputs with { SuiteCommit = suiteCommit };
                break;
            case RequestKind.Test:
                throw new RequestRejectedException("Test is not served yet (DESIGN.md §13: `amb test` comes after the build MVP)");
            case RequestKind.Build:
                inputs = NormalizeBuild(inputs);
                break;
            case RequestKind.Deploy:
                inputs = NormalizeDeploy(inputs);
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
        // An e2e request that names no size gets the site's default VM (homepc's tuned capped one; the worker machine's
        // uncapped one); the record says so (its guest RAM, when capped).
        inputs = _sizing.WithDefaultGuestMemory(ask.Kind, inputs);
        ResourceRequest? resources = SizeSlot(ask.Kind, ask.Resources ?? _sizing.ResourcesOf(ask.Kind), inputs);
        if (!_sizing.CappedSlots && inputs.GuestMemoryMiB is long guest && resources is not null
            && guest > resources.Preferred.MemoryMiB)
        {
            throw new RequestRejectedException(
                "this site has no swap for a capped slot: give the VM as much memory as its guest RAM (--ram, without a larger --guest-ram)");
        }
        DateTimeOffset now = _clock.GetUtcNow();
        var record = new RequestRecord
        {
            Id = Ids.NewRequestId(now),
            Kind = ask.Kind,
            State = RequestState.Pending,
            Inputs = inputs,
            Resources = resources,
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

    /// <summary>Applies an e2e or smoke request's own slot size (its memory, its vCPUs, its guest's RAM) to the resources
    /// it is admitted by.</summary>
    /// <param name="kind">The request's kind.</param>
    /// <param name="resources">The resources the caller asked for, or the kind's defaults (null for a kind with no slot).</param>
    /// <param name="inputs">The inputs: <see cref="RequestInputs.MemoryMiB"/>, <see cref="RequestInputs.Vcpus"/> and
    /// <see cref="RequestInputs.GuestMemoryMiB"/> when given.</param>
    /// <returns>The resources, unchanged when no size was given.</returns>
    /// <exception cref="RequestRejectedException">A size was given for a kind other than e2e or smoke, or one is out of
    /// range (memory and guest RAM 1-64 GiB, vCPUs 1-16), or the guest's RAM is less than the slot's memory.</exception>
    /// <remarks>A given memory or vCPU count becomes both the minimum and the preferred amount: a tuned slot gets exactly
    /// that or waits, so a measurement is never run on a bigger slot by chance. A guest RAM alone only lowers the memory
    /// range to it: a slot is never granted more memory than its guest sees.</remarks>
    internal static ResourceRequest? SizeSlot(RequestKind kind, ResourceRequest? resources, RequestInputs inputs)
    {
        if (inputs.MemoryMiB is null && inputs.Vcpus is null && inputs.GuestMemoryMiB is null)
        {
            return resources;
        }
        if (kind is not (RequestKind.E2e or RequestKind.Smoke) || resources is null)
        {
            throw new RequestRejectedException("inputs.memoryMiB, vcpus and guestMemoryMiB size an e2e or smoke slot only");
        }
        const long LeastMiB = 1024, MostMiB = 64 * 1024;
        if (inputs.MemoryMiB is < LeastMiB or > MostMiB || inputs.GuestMemoryMiB is < LeastMiB or > MostMiB)
        {
            throw new RequestRejectedException("inputs.memoryMiB and inputs.guestMemoryMiB must be 1024 to 65536 MiB");
        }
        if (inputs.Vcpus is < 1 or > 16)
        {
            throw new RequestRejectedException("inputs.vcpus must be 1 to 16");
        }
        if (inputs.GuestMemoryMiB is long asked && inputs.MemoryMiB is long cap && asked < cap)
        {
            throw new RequestRejectedException("inputs.guestMemoryMiB must be at least inputs.memoryMiB (the slot's cap)");
        }
        Resources min = resources.Min, preferred = resources.Preferred;
        if (inputs.MemoryMiB is long memory)
        {
            min = min with { MemoryMiB = memory };
            preferred = preferred with { MemoryMiB = memory };
        }
        else if (inputs.GuestMemoryMiB is long guest)
        {
            min = min with { MemoryMiB = Math.Min(min.MemoryMiB, guest) };
            preferred = preferred with { MemoryMiB = Math.Min(preferred.MemoryMiB, guest) };
        }
        if (inputs.Vcpus is int vcpus)
        {
            min = min with { Vcpus = vcpus };
            preferred = preferred with { Vcpus = vcpus };
        }
        return new ResourceRequest(min, preferred);
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
        // A snapshot names the commit it was made on by its full SHA too: the build job checks the snapshot's parent
        // against it, and the build's record keeps it.
        string? snapshotOf = inputs.SnapshotOf?.Trim().ToLowerInvariant();
        if (snapshotOf is not null && (snapshotOf.Length != 40 || !snapshotOf.All(char.IsAsciiHexDigitLower)))
        {
            throw new RequestRejectedException("inputs.snapshotOf must be a full 40-character commit SHA");
        }
        return inputs with { Commit = commit, Configuration = configuration, Identity = identity, Purge = purge, SnapshotOf = snapshotOf };
    }

    /// <summary>Checks a deploy request's inputs and fills its defaults.</summary>
    /// <param name="inputs">The inputs as asked.</param>
    /// <returns>The inputs with the instance in its canonical spelling ("dev" / "production"), the wait for other sessions
    /// set (default 0, the owner's "cut off at once"), and a dry run and a reinstall spelled out (default false: a build
    /// the instance already runs is then left alone by the Windows side).</returns>
    /// <exception cref="RequestRejectedException">The build id is missing or unsafe, the instance unknown, a number out
    /// of range, or the injected failure unknown.</exception>
    /// <remarks>No default instance: an install stops the instance it targets and ends every claude it hosts, so the
    /// caller names it every time ("release" is accepted for production, the CLI's own name for it).</remarks>
    internal static RequestInputs NormalizeDeploy(RequestInputs inputs)
    {
        if (!Ids.IsSafe(inputs.BuildId))
        {
            throw new RequestRejectedException("a deploy needs inputs.buildId (an existing build id)");
        }
        string instance = (inputs.Instance ?? "").Trim().ToLowerInvariant() switch
        {
            "dev" => "dev",
            "production" or "release" => "production",
            _ => throw new RequestRejectedException($"inputs.instance must be dev or production, not {inputs.Instance ?? "(none)"}"),
        };
        int waitOthers = inputs.WaitOthersMinutes ?? 0;
        if (waitOthers is < 0 or > 240)
        {
            throw new RequestRejectedException("inputs.waitOthersMinutes must be 0 to 240");
        }
        if (inputs.RequestedByPid is <= 0)
        {
            throw new RequestRejectedException("inputs.requestedByPid must be a process id");
        }
        string? inject = inputs.InjectFailure?.Trim().ToLowerInvariant();
        if (inject is not null and not "after-stop")
        {
            throw new RequestRejectedException($"inputs.injectFailure must be after-stop, not {inputs.InjectFailure}");
        }
        // Reinstall is spelled out like DryRun, so the record says which way the install was asked to go.
        return inputs with
        {
            Instance = instance, WaitOthersMinutes = waitOthers, DryRun = inputs.DryRun ?? false, Reinstall = inputs.Reinstall ?? false,
            InjectFailure = inject,
        };
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
    /// <param name="message">The line; NUL characters are dropped and line breaks folded.</param>
    /// <param name="state">The state after the event, for state events.</param>
    /// <exception cref="IOException">The event could not be appended.</exception>
    public void Event(string id, string kind, string message, RequestState? state = null)
    {
        // An event line is one line: a multi-line message (an exception's text) is folded. NULs go: a native tool that
        // writes UTF-16 into a redirected stream (MakePri's warnings in a build's package step) reaches the guest's
        // UTF-8 reader with a NUL beside every character, which `amb logs` and the JSON show as noise.
        string folded = message.Replace("\0", "", StringComparison.Ordinal).Replace("\r", "", StringComparison.Ordinal)
            .Replace("\n", " | ", StringComparison.Ordinal);
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
