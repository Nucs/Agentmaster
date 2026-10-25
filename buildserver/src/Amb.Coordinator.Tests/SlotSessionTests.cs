using Amb.Coordinator;
using Amb.Core.Protocol;
using Microsoft.Extensions.Time.Testing;
using Xunit;

namespace Amb.Coordinator.Tests;

/// <summary>A slot's step hand-out (GuestBroker / SlotSession): the promises of its remarks, without a VM.</summary>
public sealed class SlotSessionTests
{
    /// <summary>A run step for the tests.</summary>
    private static readonly GuestStep Step = new("1", StepKinds.Run, Command: "cmd.exe", Args: ["/c", "ver"]);

    /// <summary>The broker authenticates only the slot's own token.</summary>
    [Fact]
    public void OnlyTheSlotsTokenIsAccepted()
    {
        var broker = new GuestBroker(TimeProvider.System);
        broker.Open("s1", "secret-token-of-slot-one-0123456789");
        Assert.NotNull(broker.Authenticate("s1", "Bearer secret-token-of-slot-one-0123456789"));
        Assert.Null(broker.Authenticate("s1", "Bearer wrong"));
        Assert.Null(broker.Authenticate("s1", null));
        Assert.Null(broker.Authenticate("s2", "Bearer secret-token-of-slot-one-0123456789"));
        broker.Close("s1");
        Assert.Null(broker.Authenticate("s1", "Bearer secret-token-of-slot-one-0123456789"));
    }

    /// <summary>A step handed out but never answered is handed out again at the agent's next poll (a lost delivery heals).</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task UnansweredStepIsDeliveredAgain()
    {
        SlotSession session = new GuestBroker(TimeProvider.System).Open("s1", "token-0123456789-0123456789-01234");
        Task<StepResult> running = session.RunStepAsync(Step, TimeSpan.FromMinutes(3), CancellationToken.None);
        Assert.Equal("1", (await session.NextAsync(TimeSpan.FromSeconds(1), CancellationToken.None))?.Id);
        Assert.Equal("1", (await session.NextAsync(TimeSpan.FromSeconds(1), CancellationToken.None))?.Id);
        Assert.True(session.Complete(new StepResult("s1", "1", 0, "ok", 0.1)));
        Assert.Equal("ok", (await running).Outcome);
        // Answered: nothing is outstanding, so a poll returns nothing.
        Assert.Null(await session.NextAsync(TimeSpan.FromMilliseconds(100), CancellationToken.None));
    }

    /// <summary>A duplicate or stale result is ignored; only one step may be outstanding.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task DuplicatesAndStaleResultsAreIgnored()
    {
        SlotSession session = new GuestBroker(TimeProvider.System).Open("s1", "token-0123456789-0123456789-01234");
        Task<StepResult> running = session.RunStepAsync(Step, TimeSpan.FromMinutes(3), CancellationToken.None);
        Assert.False(session.Complete(new StepResult("s1", "other", 0, "ok", 0)));
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            session.RunStepAsync(Step with { Id = "2" }, TimeSpan.FromMinutes(3), CancellationToken.None));
        Assert.True(session.Complete(new StepResult("s1", "1", 0, "ok", 0)));
        Assert.False(session.Complete(new StepResult("s1", "1", 0, "ok again", 0)));
        Assert.Equal("ok", (await running).Outcome);
    }

    /// <summary>A poll waits for a step that arrives during the wait (no spinning, no lost wake-up).</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task PollWakesWhenAStepArrives()
    {
        SlotSession session = new GuestBroker(TimeProvider.System).Open("s1", "token-0123456789-0123456789-01234");
        Task<GuestStep?> poll = session.NextAsync(TimeSpan.FromSeconds(10), CancellationToken.None);
        await Task.Delay(100);
        Task<StepResult> running = session.RunStepAsync(Step, TimeSpan.FromMinutes(3), CancellationToken.None);
        GuestStep? delivered = await poll.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("1", delivered?.Id);
        session.Complete(new StepResult("s1", "1", 0, "ok", 0));
        await running;
    }

    /// <summary>A cancelled step is announced to the agent through its heartbeat reply, and the job gets the cancel.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task CancelIsAnnouncedThroughTheHeartbeat()
    {
        SlotSession session = new GuestBroker(TimeProvider.System).Open("s1", "token-0123456789-0123456789-01234");
        using var cancel = new CancellationTokenSource();
        Task<StepResult> running = session.RunStepAsync(Step, TimeSpan.FromMinutes(3), cancel.Token);
        await session.NextAsync(TimeSpan.FromSeconds(1), CancellationToken.None);
        Assert.Null(session.Heartbeat(new GuestHeartbeat("s1", 1000, 2000, "1")).CancelStep);
        await cancel.CancelAsync();
        // The job waits (up to a minute) for the agent to stop the step; the agent learns it at its next heartbeat.
        await Task.Delay(200);
        Assert.Equal("1", session.Heartbeat(new GuestHeartbeat("s1", 1000, 2000, "1")).CancelStep);
        session.Complete(new StepResult("s1", "1", -1, "cancelled", 0.5));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => running);
    }

    /// <summary>A guest silent for longer than the limit makes the step fail as lost.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task SilenceMeansTheSlotIsLost()
    {
        var clock = new FakeTimeProvider(DateTimeOffset.Parse("2026-10-25T09:00:00Z"));
        SlotSession session = new GuestBroker(clock).Open("s1", "token-0123456789-0123456789-01234");
        Task<StepResult> running = session.RunStepAsync(Step, TimeSpan.FromMinutes(3), CancellationToken.None);
        // The session's 15 s check runs on the same clock, so advancing it fires the check with 4 minutes of silence.
        clock.Advance(TimeSpan.FromMinutes(4));
        await Assert.ThrowsAsync<SlotLostException>(() => running.WaitAsync(TimeSpan.FromSeconds(30)));
    }

    /// <summary>The hello wait ends with the hello, or as lost after its timeout.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task HelloOrLost()
    {
        SlotSession session = new GuestBroker(TimeProvider.System).Open("s1", "token-0123456789-0123456789-01234");
        await Assert.ThrowsAsync<SlotLostException>(() =>
            session.WaitHelloAsync(TimeSpan.FromMilliseconds(100), CancellationToken.None));
        var hello = new GuestHello("s1", "0.1.0", new GuestFacts("Windows", "VM", "Docker", 4, 4096, 2000, 1500, 30));
        session.Hello(hello);
        Assert.Same(hello, await session.WaitHelloAsync(TimeSpan.FromSeconds(1), CancellationToken.None));
    }
}
