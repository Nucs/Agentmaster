using System.Text.Json.Serialization;

namespace Amb.Core.Model;

/// <summary>Where a build id's artifacts came from.</summary>
[JsonConverter(typeof(JsonStringEnumConverter<BuildSource>))]
public enum BuildSource
{
    /// <summary>Built by the service from a commit (Phase 3).</summary>
    Built,

    /// <summary>Registered by an import: built by hand or downloaded (DESIGN.md §13 P1).</summary>
    Imported,
}

/// <summary>One file a build id holds, kept in the store and named by its SHA-256 in the manifest.</summary>
/// <param name="Name">The file's name in the build (e.g. "Agentmaster_0.8.1.1_x64.zip").</param>
/// <param name="Kind">What it is: "portable-zip", "layout-zip", "symbols-zip", "build-log", "build-json".</param>
/// <param name="Bytes">Its size.</param>
/// <param name="Sha256">Its SHA-256, lowercase hex: every consumer checks it before use.</param>
/// <param name="StorePath">Its path relative to the store's root (<c>builds/&lt;build id&gt;/&lt;name&gt;</c>).</param>
public sealed record ArtifactRef(string Name, string Kind, long Bytes, string Sha256, string StorePath);

/// <summary>
/// A build id's manifest (DESIGN.md §2, §6: <c>builds/&lt;build id&gt;.json</c>): an immutable result; once written it is
/// never changed, so an e2e run names exactly the bytes it tested.
/// </summary>
public sealed record BuildRecord
{
    /// <summary>The build id, <c>b&lt;yyyyMMdd&gt;-&lt;n&gt;</c> (<see cref="Ids.NextBuildId"/>).</summary>
    public required string Id { get; init; }

    /// <summary>Built by the service, or imported.</summary>
    public required BuildSource Source { get; init; }

    /// <summary>When the id was made (UTC).</summary>
    public required DateTimeOffset CreatedAt { get; init; }

    /// <summary>The request that made it.</summary>
    public required string RequestId { get; init; }

    /// <summary>The commit it was built from: checked for a build, as given by the caller for an import.</summary>
    public string? Commit { get; init; }

    /// <summary>"Debug" or "Release", when known.</summary>
    public string? Configuration { get; init; }

    /// <summary>"dev" or "release", when known.</summary>
    public string? Identity { get; init; }

    /// <summary>A build: "none" or "force" (see <see cref="RequestInputs.Purge"/>); null for an import.</summary>
    public string? Purge { get; init; }

    /// <summary>A build: how long the build itself took (the build step alone, not the boot or the packaging), in
    /// seconds; null for an import.</summary>
    public double? BuildSeconds { get; init; }

    /// <summary>The version the artifact carries (a portable zip's <c>.am-version</c>), when it has one.</summary>
    public string? Version { get; init; }

    /// <summary>Import: where the artifact came from (the path or URL it was read from).</summary>
    public string? Origin { get; init; }

    /// <summary>The build's files.</summary>
    public required IReadOnlyList<ArtifactRef> Artifacts { get; init; }
}
