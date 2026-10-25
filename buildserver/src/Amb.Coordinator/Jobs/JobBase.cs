using Amb.Coordinator.Slots;
using Amb.Core.Allocation;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;

namespace Amb.Coordinator.Jobs;

/// <summary>How a job ended: the request's final state follows from it.</summary>
/// <param name="Succeeded">Whether the work succeeded.</param>
/// <param name="Outcome">The one-line summary recorded on the request.</param>
/// <param name="ExitCode">The work's exit code, when it ran a program that has one.</param>
/// <param name="BuildId">The build id made or used.</param>
/// <param name="ResultsPath">The results' folder in the store, relative to the store's root.</param>
public sealed record JobOutcome(bool Succeeded, string Outcome, int? ExitCode = null, string? BuildId = null,
    string? ResultsPath = null);

/// <summary>A step a guest agent ran and that failed; the job ends Failed with the step's own words.</summary>
/// <param name="result">The step's result.</param>
public sealed class StepFailedException(StepResult result)
    : Exception($"step {result.StepId} failed: {result.Outcome}" + (string.IsNullOrWhiteSpace(result.StderrTail)
        ? ""
        : " | stderr: " + result.StderrTail.Trim()[^Math.Min(400, result.StderrTail.Trim().Length)..]))
{
    /// <summary>The step's result.</summary>
    public StepResult Result { get; } = result;
}

/// <summary>Everything a job may use: its request, the book, the slots, the store, the settings.</summary>
public sealed class JobContext
{
    /// <summary>Creates a context.</summary>
    /// <param name="request">The request as it was granted.</param>
    /// <param name="book">The request book.</param>
    /// <param name="slots">The slot manager.</param>
    /// <param name="store">The record store.</param>
    /// <param name="options">The settings.</param>
    /// <param name="clock">The clock.</param>
    /// <param name="log">The log.</param>
    public JobContext(RequestRecord request, RequestBook book, SlotManager slots, RecordStore store,
        CoordinatorOptions options, TimeProvider clock, ILogger log)
    {
        Request = request;
        Book = book;
        Slots = slots;
        Store = store;
        Options = options;
        Clock = clock;
        Log = log;
    }

    /// <summary>The request as it was granted (its grant and site are set).</summary>
    public RequestRecord Request { get; }

    /// <summary>The request book.</summary>
    public RequestBook Book { get; }

    /// <summary>The slot manager.</summary>
    public SlotManager Slots { get; }

    /// <summary>The record store.</summary>
    public RecordStore Store { get; }

    /// <summary>The settings.</summary>
    public CoordinatorOptions Options { get; }

    /// <summary>The clock.</summary>
    public TimeProvider Clock { get; }

    /// <summary>The log.</summary>
    public ILogger Log { get; }

    /// <summary>The slot's grant (a slot job is always granted one).</summary>
    /// <exception cref="InvalidOperationException">The request was not granted resources.</exception>
    public Resources Grant => Request.Grant ?? throw new InvalidOperationException($"request {Request.Id} has no grant");

    /// <summary>Appends an event to the request's log.</summary>
    /// <param name="kind">"log", "progress", "slot", "step".</param>
    /// <param name="message">The line.</param>
    public void Event(string kind, string message) => Book.Event(Request.Id, kind, message);

    /// <summary>Moves the request to Running (its work began: the guest said hello, or an import started).</summary>
    /// <param name="message">The event's line.</param>
    public void MarkRunning(string message) => Book.Move(Request.Id, RequestState.Running, message);

    /// <summary>Moves the request to Collecting (its work ended; its results are being kept).</summary>
    /// <param name="message">The event's line.</param>
    public void MarkCollecting(string message) => Book.Move(Request.Id, RequestState.Collecting, message);

    /// <summary>Turns a path a caller gave into one the coordinator can open: on Linux (the site distro), a Windows path
    /// <c>X:\a\b</c> becomes <c>/mnt/x/a/b</c> (WSL's automount); anything else is used as it is.</summary>
    /// <param name="path">The caller's path.</param>
    /// <returns>The local path.</returns>
    public static string LocalPath(string path)
    {
        if (OperatingSystem.IsLinux() && path.Length >= 3 && char.IsAsciiLetter(path[0]) && path[1] == ':'
            && path[2] is '\\' or '/')
        {
            return "/mnt/" + char.ToLowerInvariant(path[0]) + "/" + path[3..].Replace('\\', '/');
        }
        return path;
    }
}

/// <summary>One kind of work. A job does its work and returns how it ended; the scheduler moves the request.</summary>
public interface IJob
{
    /// <summary>Does the work.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="cancel">Cancelled by a cancel request or the run timeout.</param>
    /// <returns>How it ended.</returns>
    /// <exception cref="OperationCanceledException">The job was cancelled.</exception>
    /// <exception cref="SlotLostException">The slot's guest went silent.</exception>
    Task<JobOutcome> RunAsync(JobContext context, CancellationToken cancel);
}

/// <summary>What VM jobs share: starting the slot, waiting for its guest, running steps, staging files.</summary>
public abstract class VmJob : IJob
{
    /// <summary>How long a guest may take from VM start to hello (a boot to ready took 30-75 s, measured; an install
    /// takes 12-17 minutes, see <see cref="InstallJob"/>).</summary>
    protected virtual TimeSpan HelloTimeout => TimeSpan.FromMinutes(10);

    /// <summary>How long a guest may go without any call before the slot counts as lost (DESIGN.md §12).</summary>
    protected static readonly TimeSpan Silence = TimeSpan.FromMinutes(3);

    /// <inheritdoc/>
    public abstract Task<JobOutcome> RunAsync(JobContext context, CancellationToken cancel);

    /// <summary>Starts the slot, stages its files, and waits for its guest's hello; the request moves to Running.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="image">The image to boot.</param>
    /// <param name="cancel">Cancels the start.</param>
    /// <param name="installVersion">For an install: the dockur VERSION.</param>
    /// <param name="oemDir">For an install: the OEM folder.</param>
    /// <returns>The lease; the caller stops it in a finally block.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="context"/> is null.</exception>
    /// <exception cref="InvalidOperationException">The slot could not be started.</exception>
    /// <exception cref="SlotLostException">No hello within <see cref="HelloTimeout"/>.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    protected async Task<SlotLease> StartAndWaitAsync(JobContext context, string image, CancellationToken cancel,
        string? installVersion = null, string? oemDir = null)
    {
        ArgumentNullException.ThrowIfNull(context);
        SlotLease lease = await context.Slots.StartAsync(context.Request, image, context.Grant, cancel, installVersion,
            oemDir).ConfigureAwait(false);
        try
        {
            context.Book.Patch(context.Request.Id, r => r with { SlotId = lease.Spec.SlotId });
            context.Event("slot", $"slot {lease.Spec.SlotId} started ({image}, {context.Grant})");
            Stage(context, lease);
            DateTimeOffset started = context.Clock.GetUtcNow();
            GuestHello hello = await lease.Session.WaitHelloAsync(HelloTimeout, cancel).ConfigureAwait(false);
            context.Slots.MarkReady(lease.Spec.SlotId);
            double seconds = (context.Clock.GetUtcNow() - started).TotalSeconds;
            context.MarkRunning(FormattableString.Invariant(
                $"guest ready after {seconds:0} s: {hello.Facts.Os}, {hello.Facts.Vcpus} vCPU, {hello.Facts.MemoryMiB} MiB, agent {hello.AgentVersion}"));
            lease.Session.LogSink = log =>
            {
                foreach (string line in log.Lines)
                {
                    context.Event("progress", line);
                }
            };
            return lease;
        }
        catch
        {
            // Cleanup-on-failure: a slot whose guest never came up is stopped here; the caller only stops leases it got.
            await context.Slots.StopAsync(lease, graceful: false).ConfigureAwait(false);
            throw;
        }
    }

    /// <summary>Runs one step and fails the job unless it succeeded.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="lease">The slot.</param>
    /// <param name="step">The step.</param>
    /// <param name="cancel">Cancels the step.</param>
    /// <returns>Its result.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="context"/> or <paramref name="lease"/> is null.</exception>
    /// <exception cref="StepFailedException">The step's exit code was not 0.</exception>
    /// <exception cref="SlotLostException">The guest went silent.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    protected static async Task<StepResult> RunAsync(JobContext context, SlotLease lease, GuestStep step,
        CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(context);
        ArgumentNullException.ThrowIfNull(lease);
        context.Event("step", $"step {step.Id}: {step.Kind} {Describe(step)}");
        StepResult result = await lease.Session.RunStepAsync(step, Silence, cancel).ConfigureAwait(false);
        context.Event("step", FormattableString.Invariant(
            $"step {step.Id}: {result.Outcome} ({result.DurationSeconds:0.0} s)"));
        if (result.ExitCode != 0)
        {
            throw new StepFailedException(result);
        }
        return result;
    }

    /// <summary>Copies the request's stage items into the slot's share before its guest uses them.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="lease">The slot.</param>
    /// <exception cref="ArgumentNullException"><paramref name="context"/> or <paramref name="lease"/> is null.</exception>
    /// <exception cref="InvalidOperationException">A source does not exist or a target leaves the share.</exception>
    /// <exception cref="IOException">A copy failed.</exception>
    protected static void Stage(JobContext context, SlotLease lease)
    {
        ArgumentNullException.ThrowIfNull(context);
        ArgumentNullException.ThrowIfNull(lease);
        foreach (StageItem item in context.Request.Inputs.Stage ?? [])
        {
            string source = JobContext.LocalPath(item.Source);
            string target = ShareFile(lease, item.Target);
            if (File.Exists(source))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.Copy(source, target, overwrite: true);
            }
            else if (Directory.Exists(source))
            {
                CopyTree(source, target);
            }
            else
            {
                throw new InvalidOperationException($"stage: {item.Source} does not exist");
            }
            context.Event("slot", $"staged {item.Source} -> {item.Target}");
        }
    }

    /// <summary>The local path of a file in a slot's share.</summary>
    /// <param name="lease">The slot.</param>
    /// <param name="relative">The path in the share, '/'-separated.</param>
    /// <returns>The absolute local path.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="lease"/> is null.</exception>
    /// <exception cref="InvalidOperationException">The path climbs out of the share.</exception>
    protected static string ShareFile(SlotLease lease, string relative)
    {
        ArgumentNullException.ThrowIfNull(lease);
        string root = Path.GetFullPath(lease.Spec.ExchangeDir);
        string full = Path.GetFullPath(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
        if (!full.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.Ordinal))
        {
            throw new InvalidOperationException($"{relative} leaves the slot's share");
        }
        return full;
    }

    /// <summary>The path of a file in the share as the guest sees it.</summary>
    /// <param name="lease">The slot.</param>
    /// <param name="relative">The path in the share, '/'-separated.</param>
    /// <returns>E.g. "\\host.lan\Data\e2e\out\r1.zip" in a VM.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="lease"/> is null.</exception>
    protected static string GuestPath(SlotLease lease, string relative)
    {
        ArgumentNullException.ThrowIfNull(lease);
        string separator = lease.GuestShare.Contains('\\', StringComparison.Ordinal) ? "\\" : "/";
        return lease.GuestShare.TrimEnd('\\', '/') + separator + relative.Replace("/", separator, StringComparison.Ordinal);
    }

    /// <summary>Copies a folder tree, leaving out caches no guest needs (__pycache__, .pytest_cache).</summary>
    /// <param name="source">The folder.</param>
    /// <param name="target">Where it goes.</param>
    /// <exception cref="IOException">A copy failed.</exception>
    protected static void CopyTree(string source, string target)
    {
        Directory.CreateDirectory(target);
        foreach (string dir in Directory.EnumerateDirectories(source))
        {
            string name = Path.GetFileName(dir);
            if (name is "__pycache__" or ".pytest_cache" or ".venv" or "venv")
            {
                continue;
            }
            CopyTree(dir, Path.Combine(target, name));
        }
        foreach (string file in Directory.EnumerateFiles(source))
        {
            File.Copy(file, Path.Combine(target, Path.GetFileName(file)), overwrite: true);
        }
    }

    /// <summary>A step's short description for the event log.</summary>
    /// <param name="step">The step.</param>
    /// <returns>Its command line, or its paths.</returns>
    private static string Describe(GuestStep step) => step.Kind switch
    {
        StepKinds.Run => $"{step.Command} {string.Join(' ', step.Args ?? [])}",
        StepKinds.Fetch or StepKinds.Put or StepKinds.Expand => $"{step.From} -> {step.To}",
        _ => "",
    };
}
