using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using Amb.Core.Protocol;

namespace Amb.Coordinator;

/// <summary>A slot's guest agent has gone silent for too long (DESIGN.md §12: 3 minutes): its request is lost.</summary>
/// <param name="message">What was waited for.</param>
public sealed class SlotLostException(string message) : Exception(message);

/// <summary>
/// The coordinator's side of one slot's guest agent: its token, its hello, its heartbeats, and the ONE step it is given
/// at a time.
/// </summary>
/// <remarks>
/// <para>One outstanding step per slot: a job runs its steps in order, so the session holds the current step until its
/// result arrives. An agent asks for the next step only when it is idle, so a <c>next</c> call while a step is
/// outstanding means the agent never received it (a poll cut off mid-answer, an agent restarted by its bootstrap): the
/// same step is handed out again. A step must therefore be safe to run twice, which every step kind is
/// (copies go through temporary names; a re-run suite starts over).</para>
/// <para>A result for a step that is not the current one (a duplicate post after a retry) is ignored.</para>
/// </remarks>
public sealed class SlotSession
{
    private readonly byte[] _tokenHash;
    private readonly object _gate = new();
    private readonly AsyncSignal _stepReady = new();
    private readonly TaskCompletionSource<GuestHello> _hello = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private GuestStep? _current;
    private TaskCompletionSource<StepResult>? _pending;
    private string? _cancelRequested;

    /// <summary>Why the slot is lost (its VM stopped by itself), or null while it lives; set once by
    /// <see cref="Abandon"/>.</summary>
    private volatile string? _lost;

    /// <summary>Opens a session for a slot.</summary>
    /// <param name="slotId">The slot's id.</param>
    /// <param name="token">The slot's one-time token; only its SHA-256 is kept.</param>
    /// <param name="clock">The clock heartbeats are stamped with.</param>
    internal SlotSession(string slotId, string token, TimeProvider clock)
    {
        SlotId = slotId;
        _tokenHash = SHA256.HashData(Encoding.UTF8.GetBytes(token));
        Clock = clock;
        LastSeen = clock.GetUtcNow();
    }

    /// <summary>The slot's id.</summary>
    public string SlotId { get; }

    /// <summary>The clock.</summary>
    private TimeProvider Clock { get; }

    /// <summary>When the agent last called (any call), or when the session opened.</summary>
    public DateTimeOffset LastSeen { get; private set; }

    /// <summary>The agent's last heartbeat, or null before its first.</summary>
    public GuestHeartbeat? LastHeartbeat { get; private set; }

    /// <summary>Receives the running step's progress lines (set by the job; null drops them).</summary>
    public Action<StepLog>? LogSink { get; set; }

    /// <summary>Whether a bearer token is this slot's.</summary>
    /// <param name="token">The presented token.</param>
    /// <returns>True when its SHA-256 equals the slot's (compared in constant time).</returns>
    public bool Accepts(string? token) => token is not null
        && CryptographicOperations.FixedTimeEquals(SHA256.HashData(Encoding.UTF8.GetBytes(token)), _tokenHash);

    /// <summary>Records the agent's hello (the first one wins; later ones only refresh LastSeen).</summary>
    /// <param name="hello">The hello.</param>
    public void Hello(GuestHello hello)
    {
        Touch();
        _hello.TrySetResult(hello);
    }

    /// <summary>Waits for the agent's hello: the slot is ready.</summary>
    /// <param name="timeout">The longest wait (a VM's boot to ready took 30-75 s, measured).</param>
    /// <param name="cancel">Cancels the wait.</param>
    /// <returns>The hello.</returns>
    /// <exception cref="SlotLostException">No hello within the timeout, or the slot's VM stopped (<see cref="Abandon"/>).</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    public async Task<GuestHello> WaitHelloAsync(TimeSpan timeout, CancellationToken cancel)
    {
        try
        {
            return await _hello.Task.WaitAsync(timeout, cancel).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            throw new SlotLostException($"slot {SlotId}: no hello from its guest agent within {timeout.TotalSeconds:0} s");
        }
    }

    /// <summary>Hands a step to the agent and waits for its result.</summary>
    /// <param name="step">The step.</param>
    /// <param name="silence">How long the agent may go without any call before the slot counts as lost (3 minutes).</param>
    /// <param name="cancel">Cancels the step: the agent is asked (through its heartbeat) to stop it, and the result is
    /// waited for up to a minute more so its process tree is gone before the slot is reused or stopped.</param>
    /// <returns>The step's result.</returns>
    /// <exception cref="InvalidOperationException">A step is already outstanding.</exception>
    /// <exception cref="SlotLostException">The agent went silent, or the slot's VM stopped (<see cref="Abandon"/>).</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    public async Task<StepResult> RunStepAsync(GuestStep step, TimeSpan silence, CancellationToken cancel)
    {
        TaskCompletionSource<StepResult> pending;
        lock (_gate)
        {
            if (_lost is { } lost)
            {
                throw new SlotLostException(lost);
            }
            if (_current is not null)
            {
                throw new InvalidOperationException($"slot {SlotId}: step {_current.Id} is still outstanding");
            }
            _current = step;
            _cancelRequested = null;
            pending = _pending = new TaskCompletionSource<StepResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        }
        _stepReady.Pulse();
        try
        {
            while (true)
            {
                // The silence check runs on the session's clock, and so do its waits: a check every 15 s of the SAME
                // time the agent's calls are stamped with (a test's fake clock advances both at once).
                Task finished = await Task.WhenAny(pending.Task, Task.Delay(TimeSpan.FromSeconds(15), Clock, cancel))
                    .ConfigureAwait(false);
                if (finished == pending.Task)
                {
                    return await pending.Task.ConfigureAwait(false);
                }
                cancel.ThrowIfCancellationRequested();
                if (Clock.GetUtcNow() - LastSeen > silence)
                {
                    throw new SlotLostException($"slot {SlotId}: no call from its guest agent for {silence.TotalMinutes:0} min "
                        + $"(during step {step.Id}, {step.Kind})");
                }
            }
        }
        catch (OperationCanceledException)
        {
            lock (_gate)
            {
                _cancelRequested = step.Id;
            }
            // Cleanup-on-cancel: give the agent its next heartbeat (10 s) to stop the step's process tree, and its
            // result a minute to arrive, so nothing the step started outlives the cancel.
            await Task.WhenAny(pending.Task, Task.Delay(TimeSpan.FromMinutes(1), Clock, CancellationToken.None))
                .ConfigureAwait(false);
            throw;
        }
        finally
        {
            lock (_gate)
            {
                if (ReferenceEquals(_pending, pending))
                {
                    _current = null;
                    _pending = null;
                }
            }
        }
    }

    /// <summary>The agent's long poll: the outstanding step, as soon as there is one.</summary>
    /// <param name="wait">The longest wait (the agent asks for 25 s).</param>
    /// <param name="cancel">Cancelled when the HTTP call ends.</param>
    /// <returns>The step, or null when none came within the wait.</returns>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    public async Task<GuestStep?> NextAsync(TimeSpan wait, CancellationToken cancel)
    {
        Touch();
        DateTimeOffset deadline = Clock.GetUtcNow() + wait;
        while (true)
        {
            lock (_gate)
            {
                if (_current is not null && _pending is { Task.IsCompleted: false })
                {
                    return _current;
                }
            }
            TimeSpan left = deadline - Clock.GetUtcNow();
            if (left <= TimeSpan.Zero || !await _stepReady.WaitAsync(left, cancel).ConfigureAwait(false))
            {
                return null;
            }
        }
    }

    /// <summary>Takes a step's result from the agent.</summary>
    /// <param name="result">The result.</param>
    /// <returns>True when it was the outstanding step's; false for a duplicate or a stale one.</returns>
    public bool Complete(StepResult result)
    {
        Touch();
        lock (_gate)
        {
            if (_current?.Id != result.StepId || _pending is null)
            {
                return false;
            }
            return _pending.TrySetResult(result);
        }
    }

    /// <summary>Takes a heartbeat and answers whether the running step must stop.</summary>
    /// <param name="heartbeat">The heartbeat.</param>
    /// <returns>The reply: the step to cancel, if the job cancelled it.</returns>
    public HeartbeatReply Heartbeat(GuestHeartbeat heartbeat)
    {
        Touch();
        LastHeartbeat = heartbeat;
        lock (_gate)
        {
            return new HeartbeatReply(_cancelRequested is not null && _cancelRequested == heartbeat.RunningStep
                ? _cancelRequested
                : null);
        }
    }

    /// <summary>Gives the slot up because its VM stopped by itself: the hello wait and the outstanding step fail at once
    /// with <see cref="SlotLostException"/>, and every later step too.</summary>
    /// <param name="reason">What was seen (it becomes the request's outcome).</param>
    /// <remarks>Called by the slot manager's liveness watch. Without it a dead VM is noticed only by silence: 3 minutes
    /// during a step, the whole hello timeout during a boot (10 minutes; 45 during an install). The first reason
    /// wins; a later call changes nothing.</remarks>
    public void Abandon(string reason)
    {
        lock (_gate)
        {
            if (_lost is not null)
            {
                return;
            }
            _lost = reason;
            // Faulting the step's completion wakes RunStepAsync, whose finally then clears the step.
            _pending?.TrySetException(new SlotLostException(reason));
        }
        _hello.TrySetException(new SlotLostException(reason));
        _stepReady.Pulse();
    }

    /// <summary>Takes a batch of progress lines and hands them to the job's sink.</summary>
    /// <param name="log">The lines.</param>
    public void Log(StepLog log)
    {
        Touch();
        LogSink?.Invoke(log);
    }

    /// <summary>Stamps the agent's last call.</summary>
    private void Touch() => LastSeen = Clock.GetUtcNow();
}

/// <summary>Every open slot session, by slot id; the guest API's handlers find their session here.</summary>
public sealed class GuestBroker(TimeProvider clock)
{
    private readonly ConcurrentDictionary<string, SlotSession> _sessions = new(StringComparer.Ordinal);

    /// <summary>Opens the session of a new slot.</summary>
    /// <param name="slotId">The slot's id.</param>
    /// <param name="token">Its one-time token.</param>
    /// <returns>The session.</returns>
    /// <exception cref="InvalidOperationException">A session with that id is open already.</exception>
    public SlotSession Open(string slotId, string token)
    {
        var session = new SlotSession(slotId, token, clock);
        if (!_sessions.TryAdd(slotId, session))
        {
            throw new InvalidOperationException($"slot {slotId} has a session already");
        }
        return session;
    }

    /// <summary>Closes a slot's session (its agent's calls are refused from then on).</summary>
    /// <param name="slotId">The slot's id.</param>
    public void Close(string slotId) => _sessions.TryRemove(slotId, out _);

    /// <summary>Finds the session a guest call belongs to, if its token is the slot's.</summary>
    /// <param name="slotId">The slot id the call names.</param>
    /// <param name="authorization">The call's Authorization header ("Bearer &lt;token&gt;").</param>
    /// <returns>The session, or null when the slot is unknown or the token is wrong.</returns>
    public SlotSession? Authenticate(string? slotId, string? authorization)
    {
        if (slotId is null || !_sessions.TryGetValue(slotId, out SlotSession? session))
        {
            return null;
        }
        const string Prefix = "Bearer ";
        string? token = authorization is not null && authorization.StartsWith(Prefix, StringComparison.Ordinal)
            ? authorization[Prefix.Length..].Trim()
            : null;
        return session.Accepts(token) ? session : null;
    }
}
