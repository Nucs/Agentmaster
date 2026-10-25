using System.Globalization;
using System.Security.Cryptography;

namespace Amb.Core.Model;

/// <summary>How the service names requests and builds.</summary>
/// <remarks>
/// Both kinds of id sort by time as plain strings (their date comes first, zero-padded), so a folder listing is also a
/// chronological one, and both stay short enough to type: a person reads a build id off the CLI and passes it to
/// <c>amb e2e</c>.
/// </remarks>
public static class Ids
{
    /// <summary>Makes a new request id: <c>r&lt;yyyyMMdd-HHmmss&gt;-&lt;6 hex&gt;</c>, e.g. "r20261025-081200-3fa9c1".</summary>
    /// <param name="now">The current time; its UTC date and time go into the id.</param>
    /// <returns>A new id; the random suffix keeps two requests of the same second apart.</returns>
    public static string NewRequestId(DateTimeOffset now) =>
        "r" + now.UtcDateTime.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture) + "-"
        + Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(3));

    /// <summary>Makes a new slot id: <c>s&lt;yyyyMMdd-HHmmss&gt;-&lt;6 hex&gt;</c>.</summary>
    /// <param name="now">The current time.</param>
    /// <returns>A new id, used for the slot's folder, its container and its record.</returns>
    public static string NewSlotId(DateTimeOffset now) =>
        "s" + now.UtcDateTime.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture) + "-"
        + Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(3));

    /// <summary>Picks the next build id of a day: <c>b&lt;yyyyMMdd&gt;-&lt;n&gt;</c> (DESIGN.md §2), n counting from 1.</summary>
    /// <param name="day">The UTC day of the new build.</param>
    /// <param name="existing">Every build id that exists (other days' ids and malformed names are ignored).</param>
    /// <returns>The day's prefix with one more than the highest n of that day, e.g. "b20261025-3".</returns>
    /// <exception cref="ArgumentNullException"><paramref name="existing"/> is null.</exception>
    public static string NextBuildId(DateOnly day, IEnumerable<string> existing)
    {
        ArgumentNullException.ThrowIfNull(existing);
        string prefix = "b" + day.ToString("yyyyMMdd", CultureInfo.InvariantCulture) + "-";
        int highest = 0;
        foreach (string id in existing)
        {
            if (id.StartsWith(prefix, StringComparison.Ordinal)
                && int.TryParse(id.AsSpan(prefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out int n))
            {
                highest = Math.Max(highest, n);
            }
        }
        return prefix + (highest + 1).ToString(CultureInfo.InvariantCulture);
    }

    /// <summary>Checks that a string is safe to use as a record's file name.</summary>
    /// <param name="id">The id to check.</param>
    /// <returns>True when it holds only ASCII letters, digits, '-' and '_' and is 1-80 characters long.</returns>
    /// <remarks>Ids arrive from callers (a build id on the CLI) and become file names under the records folder: this
    /// check is what keeps "../x" or "a/b" from ever reaching a path.</remarks>
    public static bool IsSafe(string? id) =>
        !string.IsNullOrEmpty(id) && id.Length <= 80 && id.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_');
}
