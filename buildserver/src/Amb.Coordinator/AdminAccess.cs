using System.Security.Cryptography;
using System.Text;
using Amb.Coordinator.Slots;

namespace Amb.Coordinator;

/// <summary>
/// The admin token every API call must carry (DESIGN.md §2: "anyone with a token"), and the filter that checks it.
/// The token lives in the site (<c>/srv/buildserver/coordinator/admin.token</c>, root only); the CLI reads it through
/// wsl.exe. It is made at the coordinator's first start and kept, so callers need not learn a new one at every start.
/// </summary>
public sealed class AdminAccess : IEndpointFilter
{
    private readonly byte[] _hash;

    /// <summary>Creates the filter for one token.</summary>
    /// <param name="token">The admin token.</param>
    public AdminAccess(string token) => _hash = SHA256.HashData(Encoding.UTF8.GetBytes(token));

    /// <summary>Reads the admin token, or makes one (64 hex characters, written with mode 0600) when there is none.</summary>
    /// <param name="path">The token file.</param>
    /// <returns>The token.</returns>
    /// <exception cref="IOException">The file could not be read or written.</exception>
    /// <exception cref="UnauthorizedAccessException">The process may not read or write it.</exception>
    public static string LoadOrCreate(string path)
    {
        if (File.Exists(path))
        {
            string existing = File.ReadAllText(path).Trim();
            if (existing.Length >= 32)
            {
                return existing;
            }
        }
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string token = Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(32));
        var options = new FileStreamOptions { Mode = FileMode.Create, Access = FileAccess.Write };
        if (!OperatingSystem.IsWindows())
        {
            // Root only from its first byte: the file is never readable by others, not even for an instant.
            options.UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite;
        }
        using (var stream = new FileStream(path, options))
        using (var writer = new StreamWriter(stream))
        {
            writer.Write(token);
        }
        return token;
    }

    /// <summary>Lets a call through when it carries the admin token, else answers 401.</summary>
    /// <param name="context">The call.</param>
    /// <param name="next">The rest of the pipeline.</param>
    /// <returns>The endpoint's result, or 401.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="context"/> or <paramref name="next"/> is null.</exception>
    public ValueTask<object?> InvokeAsync(EndpointFilterInvocationContext context, EndpointFilterDelegate next)
    {
        ArgumentNullException.ThrowIfNull(context);
        ArgumentNullException.ThrowIfNull(next);
        string? header = context.HttpContext.Request.Headers.Authorization;
        const string Prefix = "Bearer ";
        if (header is null || !header.StartsWith(Prefix, StringComparison.Ordinal)
            || !CryptographicOperations.FixedTimeEquals(SHA256.HashData(Encoding.UTF8.GetBytes(header[Prefix.Length..].Trim())), _hash))
        {
            return ValueTask.FromResult<object?>(Results.Text("missing or wrong admin token", statusCode: 401));
        }
        return next(context);
    }
}

/// <summary>
/// Ends the coordinator when it has had nothing to do for a while (DESIGN.md §5.2: the site is lazy): no request that
/// has not ended, no slot, and no API call for <see cref="CoordinatorOptions.IdleExitMinutes"/>. The distro then stops by
/// itself once its last process is gone, and the CLI wakes it again at the next request.
/// </summary>
public sealed class IdleWatch(CoordinatorOptions options, RequestBook book, SlotManager slots, TimeProvider clock,
    IHostApplicationLifetime lifetime, ILogger<IdleWatch> log) : BackgroundService
{
    private DateTimeOffset _lastCall = clock.GetUtcNow();

    /// <summary>Records an API call (a middleware calls this for every request to /api/).</summary>
    public void Touch() => _lastCall = clock.GetUtcNow();

    /// <summary>Checks every 30 s and stops the application once it has idled long enough.</summary>
    /// <param name="stoppingToken">Cancelled at shutdown.</param>
    /// <returns>A task that ends at shutdown.</returns>
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        if (options.IdleExitMinutes <= 0)
        {
            return;
        }
        var limit = TimeSpan.FromMinutes(options.IdleExitMinutes);
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(30), clock, stoppingToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }
            if (book.Active.Count == 0 && slots.Active.Count == 0 && clock.GetUtcNow() - _lastCall > limit)
            {
                log.LogInformation("idle for {Minutes} min with nothing to do: exiting", options.IdleExitMinutes);
                lifetime.StopApplication();
                return;
            }
        }
    }
}
