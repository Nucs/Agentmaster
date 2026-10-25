using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using Amb.Coordinator.Slots;
using Amb.Core.Allocation;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Protocol;

namespace Amb.Coordinator;

/// <summary>
/// What homepc has free right now (DESIGN.md §3.2): Windows' side from the host-facts helper (run every 10 s through
/// WSL's interop), the WSL VM's MemAvailable from /proc/meminfo, and the site's own slots from the slot manager.
/// </summary>
/// <remarks>
/// A slot just started has not taken its memory yet (a Windows guest touches all of RAM_SIZE within 1-2 minutes of
/// boot, measured), so Windows' available memory still counts it as free: the full grant of every slot younger than
/// <see cref="WarmUp"/> is subtracted, or two requests admitted a few seconds apart would both be granted the same
/// memory.
/// </remarks>
public sealed class HostCapacity : BackgroundService
{
    /// <summary>How long a new slot's memory is counted as taken before Windows' own numbers show it.</summary>
    public static readonly TimeSpan WarmUp = TimeSpan.FromSeconds(150);

    /// <summary>How old the last host-facts report may be before homepc holds new slots (the helper runs every 10 s).</summary>
    public static readonly TimeSpan Stale = TimeSpan.FromSeconds(60);

    private readonly CoordinatorOptions _options;
    private readonly SlotManager _slots;
    private readonly TimeProvider _clock;
    private readonly ILogger<HostCapacity> _log;
    private HostFactsReport? _last;
    private string? _lastError;

    /// <summary>Creates the capacity source.</summary>
    /// <param name="options">The settings (the helper's path, the reserves).</param>
    /// <param name="slots">The slot manager (the running grants).</param>
    /// <param name="clock">The clock.</param>
    /// <param name="log">The log.</param>
    public HostCapacity(CoordinatorOptions options, SlotManager slots, TimeProvider clock, ILogger<HostCapacity> log)
    {
        _options = options;
        _slots = slots;
        _clock = clock;
        _log = log;
    }

    /// <summary>The last host-facts report, or null before the first.</summary>
    public HostFactsReport? Last => _last;

    /// <summary>Pulsed after every new report: the scheduler re-scores waiting requests.</summary>
    public AsyncSignal Changed { get; } = new();

    /// <summary>A report to use instead of the helper's (tests; a site without the helper).</summary>
    /// <param name="report">The report.</param>
    public void Override(HostFactsReport report)
    {
        _last = report;
        Changed.Pulse();
    }

    /// <summary>homepc's free amounts for one request.</summary>
    /// <param name="tolerant">Whether the request may be admitted on commit alone (DESIGN.md §3.2).</param>
    /// <returns>The site's free amounts and holds; "no host facts" holds it while there is no fresh report.</returns>
    public SiteFree HomepcFree(bool tolerant = false)
    {
        HostFactsReport? report = _last;
        DateTimeOffset now = _clock.GetUtcNow();
        if (report is null || now - report.At > Stale)
        {
            return new SiteFree(SiteCapacity.Homepc, _options.HomepcPriority, default,
                [$"no host facts{(_lastError is null ? "" : ": " + _lastError)}"]);
        }
        IReadOnlyList<SlotRecord> active = _slots.Active;
        long warming = active.Where(s => now - s.CreatedAt < WarmUp).Sum(s => s.Grant.MemoryMiB);
        long wslAvailable = WslMemAvailableMiB() ?? report.AvailableMiB;
        var facts = new HomepcFacts(
            WindowsAvailableMiB: report.AvailableMiB - warming,
            WslMemAvailableMiB: wslAvailable - warming,
            FreeCommitMiB: report.FreeCommitMiB - warming,
            ReclaimableBalloonMiB: 0,
            RunningSlotVcpus: active.Sum(s => s.Grant.Vcpus),
            QFreeMiB: report.QFreeMiB,
            FullScreenGame: report.FullScreenGame);
        return SiteCapacity.HomepcFree(facts, _options.Reserves, _options.HomepcPriority, tolerant);
    }

    /// <summary>Runs the helper every 10 s for the coordinator's whole life.</summary>
    /// <param name="stoppingToken">Cancelled at shutdown.</param>
    /// <returns>A task that ends at shutdown.</returns>
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        if (string.IsNullOrEmpty(_options.HostFactsCommand))
        {
            return;
        }
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                _last = await ReadAsync(stoppingToken).ConfigureAwait(false);
                _lastError = null;
                Changed.Pulse();
            }
            catch (Exception ex) when (ex is InvalidOperationException or JsonException or System.ComponentModel.Win32Exception
                or IOException or TimeoutException)
            {
                // A failed read leaves the last report in place; after a minute it is stale and homepc holds new slots.
                if (_lastError != ex.Message)
                {
                    _log.LogWarning("host facts: {Error}", ex.Message);
                }
                _lastError = ex.Message;
            }
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(10), _clock, stoppingToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    /// <summary>Runs the helper once and parses its one line of JSON.</summary>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>The report.</returns>
    /// <exception cref="InvalidOperationException">The helper failed or printed nothing.</exception>
    /// <exception cref="JsonException">It printed something that is not a report.</exception>
    /// <exception cref="System.ComponentModel.Win32Exception">It could not be started.</exception>
    /// <exception cref="TimeoutException">It did not finish within 15 s.</exception>
    private async Task<HostFactsReport> ReadAsync(CancellationToken cancel)
    {
        var info = new ProcessStartInfo(_options.HostFactsCommand)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        if (OperatingSystem.IsLinux() && File.Exists("/run/WSL/1_interop"))
        {
            // The distro init's own interop socket: a daemon outlives the wsl.exe call that started it, and with it the
            // socket that call's WSL_INTEROP named (measured 2026-10-25: 1_interop serves a daemon's calls).
            info.Environment["WSL_INTEROP"] = "/run/WSL/1_interop";
        }
        using Process process = Process.Start(info)
            ?? throw new InvalidOperationException($"{_options.HostFactsCommand} did not start");
        Task<string> output = process.StandardOutput.ReadToEndAsync(cancel);
        Task<string> error = process.StandardError.ReadToEndAsync(cancel);
        try
        {
            await process.WaitForExitAsync(cancel).WaitAsync(TimeSpan.FromSeconds(15), cancel).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            process.Kill(entireProcessTree: true);
            throw;
        }
        string text = (await output.ConfigureAwait(false)).Trim();
        if (process.ExitCode != 0 || text.Length == 0)
        {
            throw new InvalidOperationException(
                $"{_options.HostFactsCommand} exited {process.ExitCode}: {(await error.ConfigureAwait(false)).Trim()}");
        }
        return JsonSerializer.Deserialize(text, AmbJsonLine.Default.HostFactsReport)
            ?? throw new InvalidOperationException($"{_options.HostFactsCommand} printed null");
    }

    /// <summary>MemAvailable of the Linux the coordinator runs in (the WSL VM), MiB.</summary>
    /// <returns>The value, or null when not on Linux or /proc/meminfo has no such line.</returns>
    private static long? WslMemAvailableMiB()
    {
        if (!OperatingSystem.IsLinux())
        {
            return null;
        }
        try
        {
            foreach (string line in File.ReadLines("/proc/meminfo"))
            {
                if (line.StartsWith("MemAvailable:", StringComparison.Ordinal))
                {
                    string kb = line["MemAvailable:".Length..].Trim().Split(' ')[0];
                    return long.Parse(kb, NumberStyles.None, CultureInfo.InvariantCulture) / 1024;
                }
            }
        }
        catch (IOException)
        {
            // Without it, Windows' available memory stands in (the stricter term in practice, DESIGN.md §16).
        }
        return null;
    }
}
