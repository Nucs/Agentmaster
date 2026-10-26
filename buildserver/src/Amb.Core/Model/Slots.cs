using System.Text.Json.Serialization;
using Amb.Core.Allocation;

namespace Amb.Core.Model;

/// <summary>Where a slot is in its life (DESIGN.md §5.1, §12).</summary>
[JsonConverter(typeof(JsonStringEnumConverter<SlotState>))]
public enum SlotState
{
    /// <summary>Its disks are being made and its VM is booting; the guest agent has not called yet.</summary>
    Starting,

    /// <summary>Its guest agent called in and takes steps.</summary>
    Ready,

    /// <summary>It is being shut down (graceful first: Windows shuts down in 8-15 s), then its files are removed.</summary>
    Stopping,

    /// <summary>It is gone: its VM ended and its throwaway files were deleted.</summary>
    Gone,
}

/// <summary>
/// One slot as the coordinator records it (DESIGN.md §6: <c>slots/&lt;slot id&gt;.json</c>): what was started for which
/// request, so a coordinator that restarts finds every slot it owns and can stop the ones whose request ended.
/// </summary>
public sealed record SlotRecord
{
    /// <summary>The slot's id (<see cref="Ids.NewSlotId"/>): its folder, container and record share it.</summary>
    public required string Id { get; init; }

    /// <summary>The request the slot serves.</summary>
    public required string RequestId { get; init; }

    /// <summary>The site it runs on.</summary>
    public required string Site { get; init; }

    /// <summary>The image it boots ("am-e2e", "am-build", "am-install").</summary>
    public required string Image { get; init; }

    /// <summary>What it was granted.</summary>
    public required Resources Grant { get; init; }

    /// <summary>Where it is in its life.</summary>
    public required SlotState State { get; init; }

    /// <summary>The driver that started it ("podman" for the stand-in, "fastrun" later) and its handle there (the
    /// container's name, the fastrun run id).</summary>
    public required string Driver { get; init; }

    /// <summary>The driver's handle of the slot.</summary>
    public required string Handle { get; init; }

    /// <summary>The SHA-256 of the slot's one-time token, which its guest agent presents on every call. Only the hash
    /// is recorded: the token itself is written once into the slot's own share.</summary>
    public required string TokenSha256 { get; init; }

    /// <summary>When it was started (UTC).</summary>
    public required DateTimeOffset CreatedAt { get; init; }

    /// <summary>A kept VM's slot (a build VM): its disks live in the VM's own folder and are reused, so even a leftover
    /// of an earlier run is stopped gracefully, never killed mid-write.</summary>
    public bool Kept { get; init; }

    /// <summary>When its guest agent last called (UTC); a slot silent for 3 minutes is stopped and its request marked
    /// lost (DESIGN.md §12).</summary>
    public DateTimeOffset? LastHeartbeatAt { get; init; }

    /// <summary>The guest's committed memory at its last report, MiB (the balloon's input, DESIGN.md §3.4).</summary>
    public long? GuestCommitMiB { get; init; }

    /// <summary>When it reached <see cref="SlotState.Gone"/> (UTC).</summary>
    public DateTimeOffset? EndedAt { get; init; }
}
