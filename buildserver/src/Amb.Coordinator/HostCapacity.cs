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
/// What the site has free right now (DESIGN.md §3.2). homepc's: Windows' side from the host-facts helper (run every 10 s
/// through WSL's interop), the WSL VM's MemAvailable from /proc/meminfo, and the site's own slots from the slot manager.
/// A Linux site's (the worker machine's CT 803, <see cref="CoordinatorOptions.IsLinuxSite"/>): the host's and the
/// container's meminfo and the site disk's free space, read every 10 s (<see cref="LinuxFree"/>).
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
    private LinuxHostFacts? _lastLinux;
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

    /// <summary>A Linux site's last facts (the worker machine's), or null before the first read or on homepc's site.</summary>
    public LinuxHostFacts? LastLinux => _lastLinux;

    /// <summary>Linux facts to use instead of the files' (tests; the next 10-s read replaces them on a real site).</summary>
    /// <param name="facts">The facts.</param>
    public void OverrideLinux(LinuxHostFacts facts)
    {
        _lastLinux = facts;
        Changed.Pulse();
    }

    /// <summary>The site's free amounts for one request, whichever site this coordinator serves: homepc's
    /// (<see cref="HomepcFree"/>) or a Linux one (<see cref="LinuxFree"/>, when <see cref="CoordinatorOptions.IsLinuxSite"/>).</summary>
    /// <param name="tolerant">homepc only: admit on commit alone (DESIGN.md §3.2); a Linux site has no such term.</param>
    /// <param name="capped">homepc only: the capped commit margin; a Linux site has no commit term.</param>
    /// <param name="idle">Answer what the site would have free with none of its slots running (the scheduler's
    /// first-in-line check, <see cref="HomepcFree"/>).</param>
    /// <returns>The site's free amounts and holds.</returns>
    public SiteFree SiteFree(bool tolerant = false, bool capped = false, bool idle = false) =>
        _options.IsLinuxSite ? LinuxFree(idle) : HomepcFree(tolerant, capped, idle);

    /// <summary>A Linux site's free amounts for one request (<see cref="SiteCapacity.LinuxFree"/>).</summary>
    /// <param name="idle">Answer what it would have free if none of its slots ran.</param>
    /// <returns>The site's free amounts; "no host facts" holds it while there is no fresh read.</returns>
    /// <remarks>The same warm-up rule as homepc's: a VM's memory shows in the host's and the container's numbers only once
    /// its guest has touched it (a Windows guest zeroes its RAM within 1-2 minutes of boot), so a slot younger than
    /// <see cref="WarmUp"/> is subtracted in full, or two requests a few seconds apart would be granted the same memory.</remarks>
    public SiteFree LinuxFree(bool idle = false)
    {
        LinuxHostFacts? facts = _lastLinux;
        DateTimeOffset now = _clock.GetUtcNow();
        if (facts is null || now - facts.At > Stale)
        {
            return new SiteFree(_options.SiteName, _options.HomepcPriority, default,
                [$"no host facts{(_lastError is null ? "" : ": " + _lastError)}"]);
        }
        IReadOnlyList<SlotRecord> active = _slots.Active;
        long warming = active.Where(s => now - s.CreatedAt < WarmUp).Sum(s => s.Grant.MemoryMiB);
        long settled = active.Where(s => now - s.CreatedAt >= WarmUp).Sum(s => s.Grant.MemoryMiB);
        // Idle gives back what the settled slots hold (the numbers show it as used); otherwise the young slots, which
        // the numbers do not show yet, are taken off.
        long adjust = idle ? settled : -warming;
        LinuxHostFacts adjusted = facts with
        {
            HostMemAvailableMiB = facts.HostMemAvailableMiB + adjust,
            SiteMemAvailableMiB = facts.SiteMemAvailableMiB + adjust,
        };
        return SiteCapacity.LinuxFree(adjusted, _options.LinuxReserves, idle ? 0 : active.Sum(s => s.Grant.Vcpus),
            _options.SiteName, _options.HomepcPriority);
    }

    /// <summary>homepc's free amounts for one request.</summary>
    /// <param name="tolerant">Whether the request may be admitted on commit alone (DESIGN.md §3.2).</param>
    /// <param name="capped">Whether the request is a capped slot: the commit term keeps the smaller capped margin
    /// (<see cref="HomepcReserves.CappedCommitMiB"/>, DESIGN.md §16.14).</param>
    /// <param name="idle">Answer what homepc WOULD have free if none of the site's slots ran: every slot whose memory
    /// Windows already shows is given back, and a young one is not taken. The scheduler asks it before it holds younger
    /// requests behind an older one: when even an idle site could not fit the older one, the owner's own programs hold
    /// what it lacks, and holding the younger ones would only leave the site idle.</param>
    /// <returns>The site's free amounts and holds; "no host facts" holds it while there is no fresh report.</returns>
    public SiteFree HomepcFree(bool tolerant = false, bool capped = false, bool idle = false)
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
        // Idle: the slots Windows' numbers already include are added back (their memory would be free); the young ones
        // are simply not subtracted (Windows does not show them yet).
        long settled = active.Where(s => now - s.CreatedAt >= WarmUp).Sum(s => s.Grant.MemoryMiB);
        long adjust = idle ? settled : -warming;
        long wslAvailable = WslMemAvailableMiB() ?? report.AvailableMiB;
        var facts = new HomepcFacts(
            WindowsAvailableMiB: report.AvailableMiB + adjust,
            WslMemAvailableMiB: wslAvailable + adjust,
            FreeCommitMiB: report.FreeCommitMiB + adjust,
            ReclaimableBalloonMiB: 0,
            RunningSlotVcpus: idle ? 0 : active.Sum(s => s.Grant.Vcpus),
            QFreeMiB: report.QFreeMiB,
            FullScreenGame: report.FullScreenGame);
        return SiteCapacity.HomepcFree(facts, _options.Reserves, _options.HomepcPriority, tolerant, capped);
    }

    /// <summary>Runs the helper every 10 s for the coordinator's whole life (a Linux site reads its files instead,
    /// <see cref="ReadLinuxLoopAsync"/>).</summary>
    /// <param name="stoppingToken">Cancelled at shutdown.</param>
    /// <returns>A task that ends at shutdown.</returns>
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        if (_options.IsLinuxSite)
        {
            await ReadLinuxLoopAsync(stoppingToken).ConfigureAwait(false);
            return;
        }
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

    /// <summary>Reads a Linux site's facts every 10 s until shutdown: the host's meminfo, the container's own (lxcfs), and
    /// the free space where the site lives.</summary>
    /// <param name="stoppingToken">Cancelled at shutdown.</param>
    /// <returns>A task that ends at shutdown.</returns>
    /// <remarks>A failed read leaves the last facts in place; after a minute they are stale and the site holds new
    /// slots ("no host facts: …"), so a broken bind mount stops admissions instead of admitting blind.</remarks>
    private async Task ReadLinuxLoopAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                _lastLinux = ReadLinuxFacts(_options.HostMemInfoPath, "/proc/meminfo", _options.Root, _clock.GetUtcNow());
                _lastError = null;
                Changed.Pulse();
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or FormatException
                or InvalidOperationException or ArgumentException)
            {
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

    /// <summary>Reads a Linux site's facts once.</summary>
    /// <param name="hostMemInfo">The host's /proc/meminfo as bind-mounted into the container.</param>
    /// <param name="siteMemInfo">The container's own /proc/meminfo (lxcfs: its memory ceiling and use).</param>
    /// <param name="root">A folder on the site's disk (its root): its file system's free space is the site's.</param>
    /// <param name="now">The time to stamp them with.</param>
    /// <returns>The facts.</returns>
    /// <exception cref="IOException">A file could not be read.</exception>
    /// <exception cref="UnauthorizedAccessException">A file may not be read.</exception>
    /// <exception cref="FormatException">A file has no MemAvailable or MemTotal line, or a value is not a number.</exception>
    /// <exception cref="ArgumentException"><paramref name="root"/> names no file system.</exception>
    internal static LinuxHostFacts ReadLinuxFacts(string hostMemInfo, string siteMemInfo, string root, DateTimeOffset now)
    {
        IReadOnlyDictionary<string, long> host = ReadMemInfoMiB(hostMemInfo);
        IReadOnlyDictionary<string, long> site = ReadMemInfoMiB(siteMemInfo);
        long Need(IReadOnlyDictionary<string, long> values, string key, string file) =>
            values.TryGetValue(key, out long v) ? v : throw new FormatException($"{file} has no {key} line");
        // On Linux any path names its file system (statvfs); Windows (the tests) takes only a drive's root.
        string full = Path.GetFullPath(root);
        var drive = new DriveInfo(OperatingSystem.IsWindows() ? Path.GetPathRoot(full)! : full);
        return new LinuxHostFacts(now,
            Need(host, "MemAvailable", hostMemInfo),
            Need(host, "MemTotal", hostMemInfo),
            Need(site, "MemAvailable", siteMemInfo),
            drive.AvailableFreeSpace / (1024 * 1024));
    }

    /// <summary>Reads a meminfo file's "Key: N kB" lines as MiB.</summary>
    /// <param name="path">The file.</param>
    /// <returns>Each key with its value in MiB (lines that are not "N kB" are skipped).</returns>
    /// <exception cref="IOException">The file could not be read.</exception>
    /// <exception cref="UnauthorizedAccessException">It may not be read.</exception>
    internal static IReadOnlyDictionary<string, long> ReadMemInfoMiB(string path)
    {
        var values = new Dictionary<string, long>(StringComparer.Ordinal);
        foreach (string line in File.ReadLines(path))
        {
            int colon = line.IndexOf(':', StringComparison.Ordinal);
            if (colon <= 0)
            {
                continue;
            }
            string[] parts = line[(colon + 1)..].Split(' ', StringSplitOptions.RemoveEmptyEntries);
            // Only "N kB" lines are amounts (HugePages_Total and its kin are page counts, without a unit).
            if (parts.Length == 2 && parts[1] == "kB"
                && long.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out long kb))
            {
                values[line[..colon]] = kb / 1024;
            }
        }
        return values;
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
