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
/// <param name="HostFacts">homepc's Windows facts as last read, or null when none were read yet (or the site is not on
/// Windows' WSL: the worker machine's reports <paramref name="LinuxFacts"/> instead).</param>
/// <param name="Site">The site's name ("homepc", "worker"); null from a coordinator older than 2026-11-12.</param>
/// <param name="PushesSource">True when the site's mirror takes commits only by push (<c>POST /api/v1/source/bundle</c>,
/// which `amb` does before a build or an e2e run): it has no repository to fetch from. False: it fetches K: by path.</param>
/// <param name="LinuxFacts">A Linux site's host facts as last read (the worker machine), or null.</param>
/// <param name="BuildLetter">The letter this site's build ids start with ("b" homepc, "w" the worker), or null from an
/// older coordinator (then "b").</param>
public sealed record CoordinatorStatus(
    string Version,
    DateTimeOffset StartedAt,
    IReadOnlyList<SiteFree> Sites,
    IReadOnlyList<RequestRecord> Active,
    IReadOnlyList<SlotRecord> Slots,
    HostFactsReport? HostFacts,
    string? Site = null,
    bool PushesSource = false,
    LinuxHostFacts? LinuxFacts = null,
    string? BuildLetter = null);

/// <summary>
/// What a Linux site measures of itself every 10 s (the worker machine's site, a container on a Proxmox host): the
/// numbers its admission binds on (DESIGN.md §3.2, the worker site's form of it).
/// </summary>
/// <param name="At">When they were read (UTC).</param>
/// <param name="HostMemAvailableMiB">The HOST's available memory (its own /proc/meminfo, bind-mounted into the site's
/// container): what every guest of the machine shares (TraderLab's worker, the planned Windows Server VM, the site).</param>
/// <param name="HostMemTotalMiB">The host's physical memory.</param>
/// <param name="SiteMemAvailableMiB">What the site's own container may still take: lxcfs' /proc/meminfo inside it, its
/// memory ceiling minus what it uses (the container's limit binds even when the host has more).</param>
/// <param name="DiskFreeMiB">Free space where the site keeps its disks and store (drive 2 on the worker).</param>
public sealed record LinuxHostFacts(
    DateTimeOffset At,
    long HostMemAvailableMiB,
    long HostMemTotalMiB,
    long SiteMemAvailableMiB,
    long DiskFreeMiB);

/// <summary>The commits a site's mirror holds as tips (<c>GET /api/v1/source/tips?repo=…&amp;want=…</c>): `amb` leaves
/// them out of the bundle it pushes, so only what the mirror lacks travels. Also the answer of a push
/// (<c>POST /api/v1/source/bundle</c>): then <paramref name="Commits"/> are the commits it took.</summary>
/// <param name="Repo">"agentmaster" or "sdk" (the refs/claude-agent-sdk-cpp submodule).</param>
/// <param name="Commits">Full SHAs: the newest tips of the mirror's pushed refs and branches (at most 100).</param>
/// <param name="Has">Whether the mirror holds the commit the call asked about (<c>want</c>): true means nothing needs to
/// be pushed for it; null when no commit was asked about.</param>
public sealed record SourceTips(string Repo, IReadOnlyList<string> Commits, bool? Has = null);

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
