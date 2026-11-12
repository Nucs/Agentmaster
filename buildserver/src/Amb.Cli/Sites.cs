using Amb.Core.Model;

namespace Amb.Cli;

/// <summary>
/// The build server's sites as the CLI knows them (since 2026-11-12): <see cref="Home"/>, the WSL distro on this PC
/// (build ids "b…"), and <see cref="Worker"/>, the container on the worker machine in Stuttgart, reached through the link
/// home (build ids "w…"). Builds and e2e runs go to the worker by default; installs always run through home, which pulls a
/// worker build first (Program.cs, <c>amb install</c>).
/// </summary>
/// <remarks>
/// A request id names no site (both sites mint "r…" ids), so `amb logs`/`show`/`cancel` ask the chosen site first and the
/// other one when it answers "no such request". A build id names its site by its letter (<see cref="OfBuild"/>).
/// </remarks>
internal static class Sites
{
    /// <summary>The site on this PC: the WSL distro "buildserver", lazy (it stops after 10 idle minutes and amb wakes it).</summary>
    public const string Home = "home";

    /// <summary>The site on the worker machine: CT 803 at 192.168.50.21, always up, its drive 2 holding the store.</summary>
    public const string Worker = "worker";

    /// <summary>The site a command goes to when it names none: AMB_SITE, else <see cref="Worker"/> (the owner's choice of
    /// 2026-11-12: builds and e2e runs on the worker machine, homepc's site as the fallback).</summary>
    /// <returns><see cref="Home"/> or <see cref="Worker"/>.</returns>
    /// <remarks>An AMB_SITE that names neither falls back to the worker; `--site` with an unknown name is refused by
    /// <see cref="TryParse"/> instead, before anything is sent.</remarks>
    public static string Default() =>
        TryParse(Environment.GetEnvironmentVariable("AMB_SITE"), out string? site) ? site! : Worker;

    /// <summary>Reads a site name ("home", or "worker"; "homepc" is home's name in the coordinator's own records).</summary>
    /// <param name="text">The name as given.</param>
    /// <param name="site">The site, when the name is known.</param>
    /// <returns>False for null, empty or an unknown name.</returns>
    public static bool TryParse(string? text, out string? site)
    {
        site = text?.Trim().ToLowerInvariant() switch
        {
            "home" or "homepc" => Home,
            "worker" => Worker,
            _ => null,
        };
        return site is not null;
    }

    /// <summary>The other site.</summary>
    /// <param name="site">A site.</param>
    /// <returns>Worker for home, home for the worker.</returns>
    public static string Other(string site) => site == Home ? Worker : Home;

    /// <summary>The site that made a build, by its id's letter.</summary>
    /// <param name="buildId">A build id.</param>
    /// <returns><see cref="Home"/> for "b…", <see cref="Worker"/> for "w…", null for anything else (the caller uses its
    /// chosen site).</returns>
    public static string? OfBuild(string? buildId) => Ids.BuildLetterOf(buildId) switch
    {
        "b" => Home,
        "w" => Worker,
        _ => null,
    };

    /// <summary>A site's coordinator URL: AMB_URL / AMB_WORKER_URL, else the address it listens on.</summary>
    /// <param name="site">The site.</param>
    /// <returns>Home: http://127.0.0.1:2207 (WSL's localhost forwarding). Worker: http://192.168.50.21:2207 (CT 803 on the
    /// home LAN through the link; DNS buildserver.local names it too).</returns>
    public static string UrlOf(string site) => site == Home
        ? Environment.GetEnvironmentVariable("AMB_URL") is { Length: > 0 } u ? u : "http://127.0.0.1:2207"
        : Environment.GetEnvironmentVariable("AMB_WORKER_URL") is { Length: > 0 } w ? w : "http://192.168.50.21:2207";
}
