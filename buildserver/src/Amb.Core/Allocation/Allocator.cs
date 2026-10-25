namespace Amb.Core.Allocation;

/// <summary>
/// What one site can give new slots right now, after its reserves (DESIGN.md §3.2), and how much it is preferred.
/// </summary>
/// <param name="Site">The site's name ("homepc", "dock").</param>
/// <param name="Priority">The site's priority (settings; dock 2.0, homepc 1.0: E1 "the proxmox machine is higher
/// priority"). Multiplies the availability, so a site twice as preferred wins while it can give at least half as
/// much.</param>
/// <param name="Free">What is free for new slots after the reserves; a field may be negative when a reserve exceeds what
/// the site has.</param>
/// <param name="Holds">Reasons the site takes no new slot at all, whatever is free (a full-screen game on homepc, a site
/// that is down); empty when it takes slots.</param>
public sealed record SiteFree(string Site, double Priority, Resources Free, IReadOnlyList<string> Holds);

/// <summary>Where a request would go and with how much, as scored against one site (DESIGN.md §3.3).</summary>
/// <param name="Site">The site scored.</param>
/// <param name="Score">priority × availability; 0 when the request does not fit there.</param>
/// <param name="Availability">The smallest per-resource fit, 0..1.</param>
/// <param name="Grant">What a slot started there now would get: per resource the preferred amount, or what is free when
/// less is; meaningful only when <paramref name="Score"/> is above 0.</param>
/// <param name="Shortage">Why the request does not fit there (the first resource below its minimum, or a hold), or null
/// when it fits.</param>
public sealed record Placement(string Site, double Score, double Availability, Resources Grant, string? Shortage);

/// <summary>
/// The admission rule of DESIGN.md §3.3: a request goes to the site with the highest priority × availability, where
/// availability is how much of its preferred amount the site can give, resource by resource, and nothing below the
/// minimum counts.
/// </summary>
/// <remarks>
/// Pure functions of their inputs, so the coordinator can re-score every waiting request whenever anything changes (a
/// slot ends, a balloon shrinks, Windows frees memory, every 15 s) without side effects. Why the product: E1 asks for
/// "availability x priority" with dock first; with dock at 2.0, dock wins while it can give at least half of what homepc
/// can give in full (0.5 × 2 = 1 × 1).
/// </remarks>
public static class Allocator
{
    /// <summary>How well a free amount serves one resource of a request.</summary>
    /// <param name="free">What the site has free of the resource.</param>
    /// <param name="min">The request's minimum of it.</param>
    /// <param name="preferred">The request's preferred amount of it.</param>
    /// <returns>0 when less than the minimum is free; else free / preferred, capped at 1. A resource the request does
    /// not need at all (preferred 0) always fits fully.</returns>
    public static double Fit(long free, long min, long preferred)
    {
        if (preferred <= 0)
        {
            return 1.0;
        }
        if (free < min)
        {
            return 0.0;
        }
        return Math.Min(1.0, (double)free / preferred);
    }

    /// <summary>Scores a request against one site.</summary>
    /// <param name="request">What the request asks for.</param>
    /// <param name="site">What the site has free and its priority.</param>
    /// <returns>The placement: score, availability, the grant a slot would get, and what was short when it does not
    /// fit.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="request"/> or <paramref name="site"/> is null.</exception>
    public static Placement Score(ResourceRequest request, SiteFree site)
    {
        ArgumentNullException.ThrowIfNull(request);
        ArgumentNullException.ThrowIfNull(site);
        if (site.Holds.Count > 0)
        {
            // A hold (a full-screen game, a site that is down) refuses new slots whatever is free.
            return new Placement(site.Site, 0, 0, default, "hold: " + string.Join(", ", site.Holds));
        }
        Resources min = request.Min, pref = request.Preferred, free = site.Free;
        double memory = Fit(free.MemoryMiB, min.MemoryMiB, pref.MemoryMiB);
        double cpus = Fit(free.Vcpus, min.Vcpus, pref.Vcpus);
        double disk = Fit(free.DiskMiB, min.DiskMiB, pref.DiskMiB);
        double availability = Math.Min(memory, Math.Min(cpus, disk));
        string? shortage = memory == 0 ? "memory" : cpus == 0 ? "vcpus" : disk == 0 ? "disk" : null;
        // The grant: the preferred amount, or what is free when less is (never below the minimum, since a grant is
        // only used when every resource fits).
        Resources grant = Resources.Min(pref, new Resources(
            Math.Max(free.MemoryMiB, 0), Math.Max(free.Vcpus, 0), Math.Max(free.DiskMiB, 0)));
        return new Placement(site.Site, site.Priority * availability, availability, grant, shortage);
    }

    /// <summary>Picks the best site for a request, or none when it fits nowhere now (it waits).</summary>
    /// <param name="request">What the request asks for.</param>
    /// <param name="sites">Every site the request may run on, each with what it has free.</param>
    /// <returns>The placement with the highest score, ties going to the higher priority and then to the site listed
    /// first; null when every score is 0.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="request"/>, <paramref name="sites"/> or one of the sites
    /// is null.</exception>
    public static Placement? Choose(ResourceRequest request, IEnumerable<SiteFree> sites)
    {
        ArgumentNullException.ThrowIfNull(sites);
        Placement? best = null;
        double bestPriority = double.NegativeInfinity;
        foreach (SiteFree site in sites)
        {
            Placement placement = Score(request, site);
            if (placement.Score <= 0)
            {
                continue;
            }
            bool better = best is null || placement.Score > best.Score
                || (placement.Score == best.Score && site.Priority > bestPriority);
            if (better)
            {
                best = placement;
                bestPriority = site.Priority;
            }
        }
        return best;
    }

    /// <summary>Explains why a request fits nowhere, for the <c>no-capacity</c> outcome and the CLI.</summary>
    /// <param name="request">What the request asks for.</param>
    /// <param name="sites">The sites it may run on.</param>
    /// <returns>One "site: shortage" phrase per site, joined with "; " (e.g. "homepc: memory; dock: memory").</returns>
    /// <exception cref="ArgumentNullException"><paramref name="request"/>, <paramref name="sites"/> or one of the sites
    /// is null.</exception>
    public static string ExplainShortage(ResourceRequest request, IEnumerable<SiteFree> sites)
    {
        ArgumentNullException.ThrowIfNull(sites);
        IEnumerable<string> parts = sites
            .Select(site => Score(request, site))
            .Select(p => $"{p.Site}: {p.Shortage ?? "fits"}");
        return string.Join("; ", parts);
    }
}
