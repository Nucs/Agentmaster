namespace Amb.Core.Protocol;

/// <summary>
/// What a slot's guest agent needs to find its coordinator: written by the coordinator into the slot's share as
/// <c>\\host.lan\Data\amb\slot.json</c> before the VM boots, and read by the agent by that exact path (a guest never
/// lists the share for instructions: its SMB client serves listings from a lease cache, DESIGN.md §10).
/// </summary>
/// <param name="SlotId">The slot's id; the agent sends it with every call.</param>
/// <param name="Coordinator">The coordinator's base URL as the guest reaches it, e.g. "http://10.0.2.2:2207" (the slot's
/// slirp4netns gateway, which maps to the site distro's loopback).</param>
/// <param name="Token">The slot's one-time bearer token. The coordinator keeps only its SHA-256.</param>
/// <param name="AllowShutdown">Whether a shutdown step may really shut Windows down. True only for a VM slot: a local
/// slot (the tests' agent, running on the PC itself) must never shut the PC down, so its agent skips the step.</param>
public sealed record SlotConfig(string SlotId, string Coordinator, string Token, bool AllowShutdown = false);

/// <summary>The agent's first call: who it is and what the guest looks like.</summary>
/// <param name="SlotId">The slot's id.</param>
/// <param name="AgentVersion">The agent's version (its assembly's informational version).</param>
/// <param name="Facts">The guest's facts at that moment.</param>
public sealed record GuestHello(string SlotId, string AgentVersion, GuestFacts Facts);

/// <summary>What the guest agent measures of its Windows.</summary>
/// <param name="Os">The Windows product line (e.g. "Microsoft Windows NT 10.0.26200.0").</param>
/// <param name="Machine">The guest's computer name.</param>
/// <param name="User">The user the agent runs as (the auto-logon user).</param>
/// <param name="Vcpus">Logical processors the guest sees.</param>
/// <param name="MemoryMiB">Physical memory the guest sees (after the balloon).</param>
/// <param name="AvailableMiB">Available physical memory.</param>
/// <param name="CommitMiB">Committed memory (the commit limit minus what is still available of it).</param>
/// <param name="UptimeSeconds">Seconds since the guest booted (boot-to-ready is measured with it).</param>
public sealed record GuestFacts(string Os, string Machine, string User, int Vcpus, long MemoryMiB, long AvailableMiB,
    long CommitMiB, double UptimeSeconds);

/// <summary>The agent's heartbeat, every 10 s: alive, how much memory the guest commits, what it runs.</summary>
/// <param name="SlotId">The slot's id.</param>
/// <param name="CommitMiB">Committed memory now (the balloon's input, DESIGN.md §3.4).</param>
/// <param name="AvailableMiB">Available physical memory now.</param>
/// <param name="RunningStep">The id of the step it runs, or null when idle.</param>
public sealed record GuestHeartbeat(string SlotId, long CommitMiB, long AvailableMiB, string? RunningStep);

/// <summary>The coordinator's answer to a heartbeat.</summary>
/// <param name="CancelStep">A step the agent must stop now (its process tree is killed), or null.</param>
public sealed record HeartbeatReply(string? CancelStep);

/// <summary>The kinds of step a guest agent runs; the coordinator composes every job from these.</summary>
public static class StepKinds
{
    /// <summary>Runs a program: <see cref="GuestStep.Command"/> with <see cref="GuestStep.Args"/>, in
    /// <see cref="GuestStep.Cwd"/>, with <see cref="GuestStep.Env"/> added, for at most <see cref="GuestStep.TimeoutS"/>;
    /// a <see cref="GuestStep.ProgressFile"/> is tailed while it runs and its new lines are sent as step logs.</summary>
    public const string Run = "run";

    /// <summary>Copies <see cref="GuestStep.From"/> (usually on the share) to <see cref="GuestStep.To"/> and checks its
    /// SHA-256 against <see cref="GuestStep.Sha256"/> when one is given.</summary>
    public const string Fetch = "fetch";

    /// <summary>Extracts the zip <see cref="GuestStep.From"/> into the folder <see cref="GuestStep.To"/> (replacing
    /// what is there).</summary>
    public const string Expand = "expand";

    /// <summary>Copies <see cref="GuestStep.From"/> (in the guest) to <see cref="GuestStep.To"/> (usually on the share),
    /// through a temporary name, and reports its size and SHA-256.</summary>
    public const string Put = "put";

    /// <summary>Shuts Windows down (after the result is sent): the graceful end of a slot.</summary>
    public const string Shutdown = "shutdown";
}

/// <summary>One step for a guest agent (the answer of <c>GET /guest/v1/next</c>). Fields are used by the kinds their
/// summaries name; a flat record keeps the protocol plain for the source-generated serializer.</summary>
/// <param name="Id">The step's id, unique within its slot.</param>
/// <param name="Kind">One of <see cref="StepKinds"/>.</param>
/// <param name="Command">Run: the program.</param>
/// <param name="Args">Run: its arguments, each passed as one argument (no shell parsing).</param>
/// <param name="Cwd">Run: the working folder.</param>
/// <param name="Env">Run: variables added to the agent's environment.</param>
/// <param name="TimeoutS">Run, fetch, put: the most seconds the step may take; then it is stopped and fails.</param>
/// <param name="From">Fetch, expand, put: the source path.</param>
/// <param name="To">Fetch, expand, put: the destination path.</param>
/// <param name="Sha256">Fetch: the expected SHA-256 of the file, lowercase hex.</param>
/// <param name="ProgressFile">Run: a file the program appends lines to; the agent sends each new line.</param>
public sealed record GuestStep(
    string Id,
    string Kind,
    string? Command = null,
    IReadOnlyList<string>? Args = null,
    string? Cwd = null,
    IReadOnlyDictionary<string, string>? Env = null,
    int TimeoutS = 3600,
    string? From = null,
    string? To = null,
    string? Sha256 = null,
    string? ProgressFile = null);

/// <summary>The result of one step (<c>POST /guest/v1/result</c>).</summary>
/// <param name="SlotId">The slot's id.</param>
/// <param name="StepId">The step's id.</param>
/// <param name="ExitCode">The program's exit code for a run; 0 for success or 1 for failure for the other kinds; -1
/// when the step was stopped (timeout, cancel).</param>
/// <param name="Outcome">"ok", "failed: …", "timeout", "cancelled", "sha256 mismatch: …".</param>
/// <param name="DurationSeconds">How long the step took.</param>
/// <param name="StdoutTail">Run: the last 4 KB of standard output.</param>
/// <param name="StderrTail">Run: the last 4 KB of standard error.</param>
/// <param name="Sha256">Fetch, put: the SHA-256 of the file copied.</param>
/// <param name="Bytes">Fetch, put: its size.</param>
public sealed record StepResult(string SlotId, string StepId, int ExitCode, string Outcome, double DurationSeconds,
    string? StdoutTail = null, string? StderrTail = null, string? Sha256 = null, long? Bytes = null);

/// <summary>New lines of a running step's progress file (<c>POST /guest/v1/log</c>).</summary>
/// <param name="SlotId">The slot's id.</param>
/// <param name="StepId">The step's id.</param>
/// <param name="Lines">The new lines, in order.</param>
public sealed record StepLog(string SlotId, string StepId, IReadOnlyList<string> Lines);
