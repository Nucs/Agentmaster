namespace Amb.Coordinator;

/// <summary>
/// A wake-up for waiters: <see cref="Pulse"/> releases everyone waiting now, and later waiters wait for the next pulse.
/// Used wherever the coordinator long-polls (a guest waiting for its next step, the CLI waiting for events, the
/// scheduler waiting for a change) so that nothing spins and nothing sleeps longer than it must.
/// </summary>
/// <remarks>A waiter always re-checks its condition after waking: a pulse says "something changed", never what.</remarks>
public sealed class AsyncSignal
{
    private TaskCompletionSource _next = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly object _gate = new();

    /// <summary>Wakes every current waiter.</summary>
    public void Pulse()
    {
        TaskCompletionSource released;
        lock (_gate)
        {
            released = _next;
            _next = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        }
        released.TrySetResult();
    }

    /// <summary>Waits for the next pulse, at most for a while.</summary>
    /// <param name="timeout">The longest wait.</param>
    /// <param name="cancel">Cancels the wait.</param>
    /// <returns>True when a pulse came, false when the time ran out.</returns>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    public async Task<bool> WaitAsync(TimeSpan timeout, CancellationToken cancel)
    {
        Task pulse;
        lock (_gate)
        {
            pulse = _next.Task;
        }
        try
        {
            await pulse.WaitAsync(timeout, cancel).ConfigureAwait(false);
            return true;
        }
        catch (TimeoutException)
        {
            // Expected control flow: the wait's time ran out with no pulse.
            return false;
        }
    }
}
