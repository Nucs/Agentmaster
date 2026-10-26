using System.Text.Json.Serialization;
using Amb.Core.Allocation;

namespace Amb.Core.Model;

/// <summary>What a request asks the service to do (DESIGN.md §2).</summary>
[JsonConverter(typeof(JsonStringEnumConverter<RequestKind>))]
public enum RequestKind
{
    /// <summary>Registers an artifact the service did not build (a hand-built portable zip, a GitHub release asset) as a
    /// build id, so e2e runs work before the service builds (DESIGN.md §13 P1). Runs in the coordinator; no slot.</summary>
    Import,

    /// <summary>Builds a commit as Debug or Release into a new build id (a build VM; Phase 3).</summary>
    Build,

    /// <summary>Runs a test kind on a build (m5, syntax, installer) in the build VM that holds the build's tree.</summary>
    Test,

    /// <summary>Runs e2e suites on a build id's artifacts in fresh <c>am-e2e</c> VMs.</summary>
    E2e,

    /// <summary>The slot chain's own check: boots one VM of an image, runs one command in it through the guest agent,
    /// and shuts it down. Proves an image and the network path from guest to coordinator (DESIGN.md §13 P0.2); its
    /// outcome carries the boot-to-ready time.</summary>
    Smoke,

    /// <summary>Makes a base image (DESIGN.md §4.3): dockur's unattended install of a Tiny11 build with the image's OEM
    /// folder (which registers the guest agent's bootstrap); the install ends when the new guest's agent says hello, and
    /// the VM's folder becomes the read-only image.</summary>
    Install,

    /// <summary>Makes a layer image (DESIGN.md §4.3, D2): boots an image through a new overlay, runs the request's steps
    /// in it, shuts it down, and keeps the overlay as a new read-only image over the first.</summary>
    Layer,
}

/// <summary>Where a request is in its life (DESIGN.md §12); every change is also an event line.</summary>
[JsonConverter(typeof(JsonStringEnumConverter<RequestState>))]
public enum RequestState
{
    /// <summary>Accepted, waiting for a slot that fits (scored again whenever capacity changes).</summary>
    Pending,

    /// <summary>A slot was granted and is being made: overlay, VM boot, the guest agent's first call.</summary>
    Starting,

    /// <summary>The work runs (the guest agent's steps, or the coordinator's own for an import).</summary>
    Running,

    /// <summary>The work ended; its results are being copied into the store.</summary>
    Collecting,

    /// <summary>Done, and the work reported success.</summary>
    Succeeded,

    /// <summary>Done, and the work (or the service) reported a failure; the outcome says which.</summary>
    Failed,

    /// <summary>Ended by a cancel; its slot was released.</summary>
    Cancelled,

    /// <summary>Never fitted anywhere within its wait timeout; the outcome names what was short.</summary>
    NoCapacity,

    /// <summary>Its slot went silent for 3 minutes (no heartbeat) and was stopped; a build is retried once.</summary>
    Lost,
}

/// <summary>Rules about <see cref="RequestState"/>: which states end a request and which moves are allowed.</summary>
public static class RequestStates
{
    /// <summary>Whether a state ends the request (no move leaves it).</summary>
    /// <param name="state">The state.</param>
    /// <returns>True for succeeded, failed, cancelled, no-capacity and lost.</returns>
    public static bool IsFinal(RequestState state) => state is RequestState.Succeeded or RequestState.Failed
        or RequestState.Cancelled or RequestState.NoCapacity or RequestState.Lost;

    /// <summary>Whether a request may move from one state to another.</summary>
    /// <param name="from">The current state.</param>
    /// <param name="to">The proposed state.</param>
    /// <returns>True when the move follows the life of DESIGN.md §12. A final state never moves; a pending request may
    /// go straight to running when it needs no slot (an import).</returns>
    /// <remarks>The coordinator refuses any other move, so a late event (a heartbeat after a cancel) can never revive a
    /// request that ended.</remarks>
    public static bool CanMove(RequestState from, RequestState to) => (from, to) switch
    {
        (RequestState.Pending, RequestState.Starting or RequestState.Running or RequestState.Cancelled
            or RequestState.NoCapacity or RequestState.Failed) => true,
        (RequestState.Starting, RequestState.Running or RequestState.Failed or RequestState.Cancelled
            or RequestState.Lost) => true,
        (RequestState.Running, RequestState.Collecting or RequestState.Succeeded or RequestState.Failed
            or RequestState.Cancelled or RequestState.Lost) => true,
        (RequestState.Collecting, RequestState.Succeeded or RequestState.Failed or RequestState.Cancelled
            or RequestState.Lost) => true,
        _ => false,
    };
}

/// <summary>
/// What a request was asked with. One flat record for every kind, each field used by the kinds its summary names, so
/// the JSON stays plain and the source-generated serializer needs no polymorphism.
/// </summary>
public sealed record RequestInputs
{
    /// <summary>Import: the artifact to register: a path this PC can read (a portable zip) or an https URL.</summary>
    public string? Source { get; init; }

    /// <summary>Import: the commit the artifact was built from, when the caller knows it (recorded, not checked).</summary>
    public string? Commit { get; init; }

    /// <summary>E2e and test: the build id whose artifacts are used.</summary>
    public string? BuildId { get; init; }

    /// <summary>E2e: the arguments for the suite runner (pytest's), e.g. ["-v", "suites/test_tab_menu.py"].</summary>
    public IReadOnlyList<string>? SuiteArgs { get; init; }

    /// <summary>E2e: the image the VM boots ("am-e2e" by default; "am-install" for the install lane).</summary>
    public string? Image { get; init; }

    /// <summary>Build: "Debug" or "Release".</summary>
    public string? Configuration { get; init; }

    /// <summary>Build: "dev" or "release" (the package identity).</summary>
    public string? Identity { get; init; }

    /// <summary>Build: "none" (an incremental build in the build VM's kept tree) or "force" (the tree's build outputs
    /// are deleted first: a clean build). Part of a build's identity: a request for a commit already built with the
    /// same configuration, identity and purge returns that build id.</summary>
    public string? Purge { get; init; }

    /// <summary>Test: the test kind ("m5", "syntax", "installer").</summary>
    public string? TestKind { get; init; }

    /// <summary>Install, layer: the new image's name (it must not exist yet).</summary>
    public string? NewImage { get; init; }

    /// <summary>Install: the dockur VERSION to install ("core11" for Tiny11 Core, "tiny11" for Tiny11).</summary>
    public string? Version { get; init; }

    /// <summary>Install: the OEM folder dockur copies to C:\OEM and runs (a path the coordinator can read).</summary>
    public string? OemDir { get; init; }

    /// <summary>Layer (and smoke, when given): the steps to run in the VM, in order; a failed step fails the request.</summary>
    public IReadOnlyList<Protocol.GuestStep>? Steps { get; init; }

    /// <summary>Files or folders to copy into the slot's share before the VM starts, so the steps can use them.</summary>
    public IReadOnlyList<StageItem>? Stage { get; init; }

    /// <summary>E2e: the e2e suite's folder to run (a path the coordinator can read); default: the coordinator's
    /// configured copy of the repository's e2e folder.</summary>
    public string? SuiteSource { get; init; }
}

/// <summary>One file or folder copied into a slot's share before its VM starts.</summary>
/// <param name="Source">A path the coordinator can read: a Windows path (Q:\…, mapped to /mnt/q/… in the site) or a
/// path in the site.</param>
/// <param name="Target">Where it goes, relative to the share's root, with '/' as separator (e.g. "layer/python.exe").</param>
public sealed record StageItem(string Source, string Target);

/// <summary>
/// One request as the coordinator records it (DESIGN.md §6: <c>requests/&lt;id&gt;.json</c>, at most 64 KB). Logs,
/// binlogs and evidence go to the store, never into this record.
/// </summary>
public sealed record RequestRecord
{
    /// <summary>The request's id (<see cref="Ids.NewRequestId"/>); returned at once to the caller.</summary>
    public required string Id { get; init; }

    /// <summary>What the request asks for.</summary>
    public required RequestKind Kind { get; init; }

    /// <summary>Where it is in its life.</summary>
    public required RequestState State { get; init; }

    /// <summary>The inputs it was asked with.</summary>
    public required RequestInputs Inputs { get; init; }

    /// <summary>The least and the most of each resource it can use; null for a request that needs no slot.</summary>
    public ResourceRequest? Resources { get; init; }

    /// <summary>When it was accepted (UTC).</summary>
    public required DateTimeOffset CreatedAt { get; init; }

    /// <summary>When its record last changed (UTC).</summary>
    public required DateTimeOffset UpdatedAt { get; init; }

    /// <summary>How long it may wait for a slot before it ends no-capacity (build 60 min, test 30, e2e 120).</summary>
    public required TimeSpan WaitTimeout { get; init; }

    /// <summary>How long its work may run once started (build 3 h, test 1 h, e2e 2 h).</summary>
    public required TimeSpan RunTimeout { get; init; }

    /// <summary>When its work started (the move to Starting or Running), or null while it waits.</summary>
    public DateTimeOffset? StartedAt { get; init; }

    /// <summary>When it ended (a final state), or null while it lives.</summary>
    public DateTimeOffset? EndedAt { get; init; }

    /// <summary>The site it was placed on, once granted.</summary>
    public string? Site { get; init; }

    /// <summary>The slot that runs it, once granted.</summary>
    public string? SlotId { get; init; }

    /// <summary>What the slot was granted (between the minimum and the preferred amount).</summary>
    public Resources? Grant { get; init; }

    /// <summary>The build id it made (a build, an import) or used (test, e2e).</summary>
    public string? BuildId { get; init; }

    /// <summary>A one-line summary of how it ended or why it waits: "48 passed, 1 failed", "no-capacity: homepc:
    /// memory", the exception that ended it.</summary>
    public string? Outcome { get; init; }

    /// <summary>The store folder of its results (logs, JUnit XML, evidence), relative to the store's root.</summary>
    public string? ResultsPath { get; init; }

    /// <summary>The exit code its work reported (pytest's, the build's), when it ran.</summary>
    public int? ExitCode { get; init; }
}

/// <summary>
/// One event of a request (DESIGN.md §6: <c>requests/&lt;id&gt;.events.jsonl</c>, one line per event, flushed per line).
/// The CLI's <c>--wait</c> streams these.
/// </summary>
/// <param name="At">When it happened (UTC).</param>
/// <param name="Kind">What happened: "state", "progress", "log", "placement", "error".</param>
/// <param name="Message">The human line, e.g. "13/49 FAIL test_x 40 s".</param>
/// <param name="State">The request's state after the event, for "state" events.</param>
public sealed record RequestEvent(DateTimeOffset At, string Kind, string Message, RequestState? State = null);
