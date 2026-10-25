using Amb.Core.Allocation;
using Amb.Core.Model;

namespace Amb.Core.Protocol;

/// <summary>
/// The body of <c>POST /api/v1/requests</c>: what to do and with how much (DESIGN.md §2). Every field but
/// <see cref="Kind"/> is optional: the coordinator fills the per-kind defaults of DESIGN.md §3.1 and §2.
/// </summary>
/// <param name="Kind">What the request asks for.</param>
/// <param name="Inputs">Its inputs.</param>
/// <param name="Resources">The least and the most of each resource, or null for the kind's default.</param>
/// <param name="WaitTimeoutMinutes">How long it may wait for a slot, or null for the kind's default.</param>
/// <param name="RunTimeoutMinutes">How long its work may run, or null for the kind's default.</param>
public sealed record CreateRequest(
    RequestKind Kind,
    RequestInputs? Inputs = null,
    ResourceRequest? Resources = null,
    int? WaitTimeoutMinutes = null,
    int? RunTimeoutMinutes = null);

/// <summary>A page of a request's events (<c>GET /api/v1/requests/{id}/events?skip=N&amp;wait=S</c>).</summary>
/// <param name="Next">The skip value for the next call (the number of events seen so far).</param>
/// <param name="Events">The events after the first skip, oldest first.</param>
/// <param name="Request">The request as it is now (its state tells whether to wait for more).</param>
public sealed record EventPage(int Next, IReadOnlyList<RequestEvent> Events, RequestRecord Request);

/// <summary>What the coordinator reports of itself (<c>GET /api/v1/status</c>).</summary>
/// <param name="Version">The coordinator's version.</param>
/// <param name="StartedAt">When it started (UTC).</param>
/// <param name="Sites">Each site with what it has free now for a default e2e request.</param>
/// <param name="Active">The requests that are not final, oldest first.</param>
/// <param name="Slots">The slots that are not gone.</param>
/// <param name="HostFacts">homepc's Windows facts as last read, or null when none were read yet.</param>
public sealed record CoordinatorStatus(
    string Version,
    DateTimeOffset StartedAt,
    IReadOnlyList<SiteFree> Sites,
    IReadOnlyList<RequestRecord> Active,
    IReadOnlyList<SlotRecord> Slots,
    HostFactsReport? HostFacts);

/// <summary>
/// What Windows says of itself, printed as JSON by the helper <c>amb-hostfacts.exe</c>, which the coordinator runs every
/// 10 s through WSL's interop (DESIGN.md §3.2: these numbers bind homepc's memory).
/// </summary>
/// <param name="At">When they were read (UTC).</param>
/// <param name="AvailableMiB">Windows' available physical memory.</param>
/// <param name="FreeCommitMiB">Windows' free commit.</param>
/// <param name="QFreeMiB">Free space of the drive that holds the site (Q:).</param>
/// <param name="FullScreenGame">Whether a full-screen Direct3D program runs (QUNS_RUNNING_D3D_FULL_SCREEN).</param>
/// <param name="UserState">SHQueryUserNotificationState's value, for the log (1 not present … 7 quiet time).</param>
public sealed record HostFactsReport(
    DateTimeOffset At,
    long AvailableMiB,
    long FreeCommitMiB,
    long QFreeMiB,
    bool FullScreenGame,
    int UserState);
