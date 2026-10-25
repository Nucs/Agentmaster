namespace Amb.Core.Allocation;

/// <summary>
/// An amount of the three resources a slot is admitted by (DESIGN.md §3.1): guest memory, vCPUs and disk growth.
/// </summary>
/// <remarks>
/// Memory and disk are in MiB so that every amount is an integer the records can carry exactly; a gigabyte figure in
/// the design converts with <see cref="Gib"/>. Negative amounts are legal in arithmetic (a site whose reserve exceeds
/// what it has free computes a negative free amount) and are clamped by whoever turns them into a grant.
/// </remarks>
/// <param name="MemoryMiB">Guest memory, MiB.</param>
/// <param name="Vcpus">Virtual CPUs.</param>
/// <param name="DiskMiB">Disk the slot may add (overlays, data disks), MiB.</param>
public readonly record struct Resources(long MemoryMiB, int Vcpus, long DiskMiB)
{
    /// <summary>Converts gigabytes (GiB, as DESIGN.md writes them) into MiB.</summary>
    /// <param name="gib">An amount in GiB; fractions are rounded to the nearest MiB.</param>
    /// <returns>The amount in MiB.</returns>
    public static long Gib(double gib) => (long)Math.Round(gib * 1024.0);

    /// <summary>The per-resource minimum of two amounts: what both allow at once.</summary>
    /// <param name="a">One amount.</param>
    /// <param name="b">The other amount.</param>
    /// <returns>Each field the smaller of the two.</returns>
    public static Resources Min(Resources a, Resources b) =>
        new(Math.Min(a.MemoryMiB, b.MemoryMiB), Math.Min(a.Vcpus, b.Vcpus), Math.Min(a.DiskMiB, b.DiskMiB));

    /// <summary>True when every field of this amount is at least the other's.</summary>
    /// <param name="other">The amount to compare with.</param>
    /// <returns>Whether this amount covers <paramref name="other"/> in every resource.</returns>
    public bool Covers(Resources other) =>
        MemoryMiB >= other.MemoryMiB && Vcpus >= other.Vcpus && DiskMiB >= other.DiskMiB;

    /// <summary>Adds two amounts field by field (the running slots' grants added up).</summary>
    /// <param name="a">One amount.</param>
    /// <param name="b">The other amount.</param>
    /// <returns>The sum.</returns>
    public static Resources operator +(Resources a, Resources b) =>
        new(a.MemoryMiB + b.MemoryMiB, a.Vcpus + b.Vcpus, a.DiskMiB + b.DiskMiB);

    /// <summary>Subtracts one amount from another field by field (a reserve taken from what a site has free).</summary>
    /// <param name="a">The amount to subtract from.</param>
    /// <param name="b">The amount to subtract.</param>
    /// <returns>The difference; it may be negative.</returns>
    public static Resources operator -(Resources a, Resources b) =>
        new(a.MemoryMiB - b.MemoryMiB, a.Vcpus - b.Vcpus, a.DiskMiB - b.DiskMiB);

    /// <summary>Formats the amount the way logs and the CLI print it, e.g. "3.6 GB, 6 vCPU, 3.0 GB disk".</summary>
    /// <returns>The amount in GiB with one decimal.</returns>
    public override string ToString() =>
        FormattableString.Invariant($"{MemoryMiB / 1024.0:0.0} GB, {Vcpus} vCPU, {DiskMiB / 1024.0:0.0} GB disk");
}

/// <summary>
/// What a request asks of each resource: at least <see cref="Min"/>, at most <see cref="Preferred"/> (DESIGN.md §3.1).
/// </summary>
/// <remarks>
/// A slot gets, per resource, whatever is free between the two: a VM that can run in 3 GB but prefers 8 GB starts on a
/// busy PC instead of waiting. A request whose minimum is never free waits until its wait timeout and then ends
/// <c>no-capacity</c>, naming the resource that was short.
/// </remarks>
public sealed record ResourceRequest
{
    /// <summary>Creates a request after checking that the minimum fits under the preferred amount.</summary>
    /// <param name="min">The least the slot can run with; a zero field means the slot needs none of it.</param>
    /// <param name="preferred">The most the slot can use; never less than <paramref name="min"/>. A zero field (with a
    /// zero minimum) means the resource does not matter to this request, and it never limits the fit.</param>
    /// <exception cref="ArgumentException">A field is negative, or a field of <paramref name="min"/> exceeds the same
    /// field of <paramref name="preferred"/>.</exception>
    public ResourceRequest(Resources min, Resources preferred)
    {
        if (min.MemoryMiB < 0 || min.Vcpus < 0 || min.DiskMiB < 0)
        {
            throw new ArgumentException($"a minimum amount is negative ({min})", nameof(min));
        }
        if (!preferred.Covers(min))
        {
            throw new ArgumentException($"the minimum ({min}) exceeds the preferred amount ({preferred})", nameof(min));
        }
        Min = min;
        Preferred = preferred;
    }

    /// <summary>The least the slot can run with, per resource.</summary>
    public Resources Min { get; }

    /// <summary>The most the slot can use, per resource.</summary>
    public Resources Preferred { get; }
}
