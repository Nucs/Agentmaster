namespace Amb.Core.Model;

/// <summary>
/// A kept build VM (DESIGN.md §5.1 <c>win-build</c>, §6: <c>vms/&lt;name&gt;.json</c>): its disks outlive each build, so the
/// next build of the same worktree is incremental. The record says which image its overlay was made over and which
/// commit each worktree on its data disk holds, so the next bundle carries only what the VM lacks.
/// </summary>
/// <remarks>
/// ⚠ The overlay's backing file is the image's own disk: a VM whose image was rebuilt since (another
/// <see cref="ImageStamp"/>) is discarded and made anew, because an overlay over a changed base reads garbage.
/// </remarks>
public sealed record VmRecord
{
    /// <summary>The VM's name (its folder under <c>vms/</c>), e.g. "build-1".</summary>
    public required string Name { get; init; }

    /// <summary>The image its overlay is backed by ("am-build").</summary>
    public required string Image { get; init; }

    /// <summary>The image disk's size and modification time when the overlay was made ("&lt;bytes&gt;:&lt;ticks&gt;"): a
    /// rebuilt image changes it.</summary>
    public required string ImageStamp { get; init; }

    /// <summary>The raw data disk's virtual size (GiB; a sparse file, so only what the guest wrote takes space).</summary>
    public required int DataDiskGiB { get; init; }

    /// <summary>When the VM was first made (UTC).</summary>
    public required DateTimeOffset CreatedAt { get; init; }

    /// <summary>When a build last used it (UTC).</summary>
    public DateTimeOffset? LastUsedAt { get; init; }

    /// <summary>The commit each worktree holds after its last build, by worktree key ("release-dev"; "sdk" for the
    /// submodule's repository): the basis of the next incremental bundle.</summary>
    public IReadOnlyDictionary<string, string> Commits { get; init; } = new Dictionary<string, string>(StringComparer.Ordinal);
}
