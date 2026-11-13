using System.Security.Cryptography;
using System.Text.Json;
using Amb.Coordinator.Jobs;
using Amb.Coordinator.Slots;
using Amb.Coordinator.Sources;
using Amb.Core.Allocation;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Time.Testing;
using Xunit;

namespace Amb.Coordinator.Tests;

/// <summary>
/// The worker machine's site (since 2026-11-12): its settings (empty means off), its own slot sizes, its Linux capacity,
/// the mirror that takes commits by push (against real git repositories in %TEMP%), the e2e suite exported from a commit,
/// and a build pulled into homepc's site under its own id.
/// </summary>
public sealed class WorkerSiteTests : IDisposable
{
    /// <summary>The test's own site root under %TEMP%, deleted after it.</summary>
    private readonly string _root = Path.Combine(Path.GetTempPath(), "amb-worker-" + Guid.NewGuid().ToString("N"));

    /// <summary>The test's clock.</summary>
    private readonly FakeTimeProvider _clock = new(DateTimeOffset.Parse("2026-11-12T10:00:00Z"));

    /// <summary>Makes the root.</summary>
    public WorkerSiteTests() => Directory.CreateDirectory(_root);

    /// <summary>Deletes the root (git makes some files read-only, which Windows refuses to delete as they are).</summary>
    public void Dispose()
    {
        foreach (string file in Directory.EnumerateFiles(_root, "*", SearchOption.AllDirectories))
        {
            File.SetAttributes(file, FileAttributes.Normal);
        }
        Directory.Delete(_root, recursive: true);
    }

    /// <summary>The worker site's environment, as its start script sets it (worker.py's env file).</summary>
    /// <param name="root">The site root.</param>
    /// <returns>The variables; an empty string is "set but empty".</returns>
    private static Dictionary<string, string> WorkerEnvironment(string root) => new(StringComparer.Ordinal)
    {
        ["AMB_ROOT"] = root,
        ["AMB_SOURCE_REPO"] = "",
        ["AMB_E2E_SOURCE"] = "",
        ["AMB_WINDOWS_PWSH"] = "",
        ["AMB_HOSTFACTS"] = "",
        ["AMB_HOST_MEMINFO"] = "/mnt/hostproc/meminfo",
        ["AMB_SITE_NAME"] = "worker",
        ["AMB_BUILD_LETTER"] = "w",
        ["AMB_BUILD_MEMORY_MIB"] = "16384:32768",
        ["AMB_BUILD_VCPUS"] = "8:16",
        ["AMB_E2E_MEMORY_MIB"] = "6144",
        ["AMB_E2E_VCPUS"] = "4:6",
        ["AMB_E2E_GUEST_MEMORY_MIB"] = "0",
        ["AMB_CAPPED_SLOTS"] = "0",
        ["AMB_DOCKUR_BOOT_WAIT"] = "180",
        ["AMB_DOCKUR_DISK_CACHE"] = "writeback",
        ["AMB_GUEST_SILENCE_MINUTES"] = "10",
        ["AMB_GUEST_HELLO_MINUTES"] = "20",
        ["AMB_RESERVE_HOST_MEMORY_MIB"] = "8192",
        ["AMB_USABLE_VCPUS"] = "20",
        ["AMB_RESERVE_DISK_MIB"] = "51200",
    };

    /// <summary>The worker's settings read from its environment: set-but-empty turns the K: source, the default suite and
    /// Windows off, the site is a Linux one with its own letter and sizes. Unset keeps homepc's defaults.</summary>
    [Fact]
    public void WorkerSettingsComeFromTheEnvironment()
    {
        Dictionary<string, string> env = WorkerEnvironment(_root);
        CoordinatorOptions worker = CoordinatorOptions.FromEnvironment(name => env.GetValueOrDefault(name));
        Assert.True(worker.PushesSource);
        Assert.True(worker.IsLinuxSite);
        Assert.Equal("", worker.E2eSourceDir);
        Assert.Equal("", worker.WindowsPwsh);
        Assert.Equal(("worker", "w"), (worker.SiteName, worker.BuildLetter));
        Assert.Equal(new LinuxReserves(8192, 2048, 20, 51200), worker.LinuxReserves);
        Assert.Equal(new Resources(16384, 8, Resources.Gib(35)), worker.Sizing.Build!.Min);
        Assert.Equal(new Resources(32768, 16, Resources.Gib(35)), worker.Sizing.Build!.Preferred);
        // One number is both the least and the most; the disk stays the kind's own.
        Assert.Equal(new Resources(6144, 4, Resources.Gib(1)), worker.Sizing.E2e!.Min);
        Assert.Equal(new Resources(6144, 6, Resources.Gib(3)), worker.Sizing.E2e!.Preferred);
        Assert.Equal(0, worker.Sizing.E2eGuestMemoryMiB);
        Assert.Equal(180, worker.DockurBootWaitSeconds);
        Assert.Equal("writeback", worker.DockurDiskCache);
        Assert.Equal(TimeSpan.FromMinutes(10), worker.GuestSilence);
        Assert.Equal(TimeSpan.FromMinutes(20), worker.GuestHello);
        Assert.Equal(Path.Combine(_root, "tmp"), worker.TmpDir);

        CoordinatorOptions homepc = CoordinatorOptions.FromEnvironment(name => name == "AMB_ROOT" ? _root : null);
        Assert.False(homepc.PushesSource);
        Assert.False(homepc.IsLinuxSite);
        Assert.Equal(("homepc", "b"), (homepc.SiteName, homepc.BuildLetter));
        Assert.Equal("/mnt/k/source/Agentmaster", homepc.SourceRepo);
        Assert.Equal("/mnt/k/source/Agentmaster/e2e", homepc.E2eSourceDir);
        Assert.Null(homepc.Sizing.Build);
        Assert.Null(homepc.Sizing.E2e);
        Assert.Null(homepc.Sizing.E2eGuestMemoryMiB);
        // homepc keeps dockur's own 30-s firmware wait (its slots boot in seconds) and O_DIRECT disks, the 3-minute
        // silence limit and the 10-minute boot limit.
        Assert.Null(homepc.DockurBootWaitSeconds);
        Assert.Null(homepc.DockurDiskCache);
        Assert.Equal(TimeSpan.FromMinutes(3), homepc.GuestSilence);
        Assert.Equal(TimeSpan.FromMinutes(10), homepc.GuestHello);
    }

    /// <summary>A malformed size, a zero, a least above the most, or a letter that request or slot ids use is refused at
    /// start, never turned into a default silently.</summary>
    [Fact]
    public void MalformedWorkerSettingsAreRefused()
    {
        foreach ((string name, string value) in new[]
        {
            ("AMB_BUILD_MEMORY_MIB", "16G"),
            ("AMB_BUILD_MEMORY_MIB", "32768:16384"),
            ("AMB_BUILD_VCPUS", "0:16"),
            ("AMB_E2E_MEMORY_MIB", "1:2:3"),
            ("AMB_BUILD_LETTER", "r"),
            ("AMB_BUILD_LETTER", "W"),
            ("AMB_BUILD_LETTER", "wk"),
            ("AMB_USABLE_VCPUS", "-1"),
            ("AMB_DOCKUR_BOOT_WAIT", "29"),
            ("AMB_DOCKUR_BOOT_WAIT", "1801"),
            ("AMB_DOCKUR_BOOT_WAIT", "3m"),
            ("AMB_DOCKUR_DISK_CACHE", "unsafe"),
            ("AMB_DOCKUR_DISK_CACHE", "WriteBack"),
            ("AMB_DOCKUR_DISK_CACHE", "fast"),
            ("AMB_GUEST_SILENCE_MINUTES", "0"),
            ("AMB_GUEST_SILENCE_MINUTES", "61"),
            ("AMB_GUEST_SILENCE_MINUTES", "5m"),
            ("AMB_GUEST_HELLO_MINUTES", "4"),
            ("AMB_GUEST_HELLO_MINUTES", "61"),
            ("AMB_GUEST_HELLO_MINUTES", "20m"),
        })
        {
            Dictionary<string, string> env = WorkerEnvironment(_root);
            env[name] = value;
            Assert.Throws<FormatException>(() => CoordinatorOptions.FromEnvironment(key => env.GetValueOrDefault(key)));
        }
    }

    /// <summary>The kept build VM's data disk follows the site's cache mode: O_DIRECT with native AIO when the site sets
    /// none (homepc), the host's page cache with threaded AIO for writeback (QEMU refuses native AIO on a cached file),
    /// and native AIO again for directsync, which is direct I/O too.</summary>
    [Fact]
    public void DataDiskFollowsTheSiteCacheMode()
    {
        Assert.Contains(",cache=none,aio=native,", PodmanSlotDriver.DataDiskArguments(null), StringComparison.Ordinal);
        Assert.Contains(",cache=writeback,aio=threads,", PodmanSlotDriver.DataDiskArguments("writeback"),
            StringComparison.Ordinal);
        Assert.Contains(",cache=writethrough,aio=threads,", PodmanSlotDriver.DataDiskArguments("writethrough"),
            StringComparison.Ordinal);
        Assert.Contains(",cache=directsync,aio=native,", PodmanSlotDriver.DataDiskArguments("directsync"),
            StringComparison.Ordinal);
        // Everything else of the attachment stays as measured: its own controller, the raw file, discard.
        Assert.StartsWith(" -device virtio-scsi-pci,id=ambdata -drive file=/storage/data-disk.raw",
            PodmanSlotDriver.DataDiskArguments("writeback"), StringComparison.Ordinal);
        Assert.EndsWith(",discard=unmap,if=none -device scsi-hd,drive=ambdata0,bus=ambdata.0,rotation_rate=1",
            PodmanSlotDriver.DataDiskArguments("writeback"), StringComparison.Ordinal);
    }

    /// <summary>The firmware-wait patch, run by a real POSIX shell over a copy of dockur 6.05's line: only that exact line
    /// changes (a comment that mentions it stays), the script says what it set, and then starts what it was given.</summary>
    [Fact]
    public void BootWaitScriptRewritesDockursWaitAndStartsDockur()
    {
        string entry = Path.Combine(_root, "entry.sh");
        const string Comment = "# waitForBoot \"$pid\" 30 & gives the firmware 30 s";
        File.WriteAllText(entry, "pid=$!\n" + Comment + "\n" + PodmanSlotDriver.DockurBootWaitLine + "\n\nrc=0\n");
        // The test's "dockur" prints the patched file: what the real entrypoint would run.
        string script = PodmanSlotDriver.BootWaitScript(180, ShellPath(entry), "cat \"$f\"");
        (int exit, string output, string error) = RunPosixShell(script);
        Assert.True(exit == 0, $"exit {exit}: {error}");
        Assert.Contains("amb: dockur's firmware wait set to 180 s", output, StringComparison.Ordinal);
        Assert.Equal("pid=$!\n" + Comment + "\nwaitForBoot \"$pid\" 180 &\n\nrc=0\n",
            File.ReadAllText(entry).Replace("\r\n", "\n", StringComparison.Ordinal));
    }

    /// <summary>A dockur whose entry script lacks the exact line (another version) is refused before anything starts:
    /// exit 64, an "amb:" line naming the setting, and the file untouched.</summary>
    [Fact]
    public void BootWaitScriptRefusesAnotherDockur()
    {
        string entry = Path.Combine(_root, "entry.sh");
        const string Other = "pid=$!\nwaitForBoot \"$pid\" 45 &\n";
        File.WriteAllText(entry, Other);
        (int exit, string output, string error) = RunPosixShell(
            PodmanSlotDriver.BootWaitScript(180, ShellPath(entry), "echo dockur-started"));
        Assert.Equal(PodmanSlotDriver.BootWaitRefused, exit);
        Assert.Contains("is not in", error, StringComparison.Ordinal);
        Assert.Contains("AMB_DOCKUR_BOOT_WAIT", error, StringComparison.Ordinal);
        Assert.DoesNotContain("dockur-started", output, StringComparison.Ordinal);
        Assert.Equal(Other, File.ReadAllText(entry));
    }

    /// <summary>The script's wait stays in the setting's range, and quoting survives a single quote in a path.</summary>
    [Fact]
    public void BootWaitScriptChecksItsArguments()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => PodmanSlotDriver.BootWaitScript(29));
        Assert.Throws<ArgumentOutOfRangeException>(() => PodmanSlotDriver.BootWaitScript(1801));
        Assert.Throws<ArgumentException>(() => PodmanSlotDriver.BootWaitScript(180, " "));
        Assert.Equal("'it'\\''s'", PodmanSlotDriver.ShQuote("it's"));
        string script = PodmanSlotDriver.BootWaitScript(180);
        Assert.StartsWith("f='/run/entry.sh'\n", script, StringComparison.Ordinal);
        Assert.EndsWith("\nexec /usr/bin/tini -s /run/entry.sh", script, StringComparison.Ordinal);
    }

    /// <summary>A slot's output as dockur prints it (colors, the firmware's screen codes, carriage returns) comes out as
    /// plain lines, the last ones kept.</summary>
    [Fact]
    public void SlotOutputIsCleaned()
    {
        const string Raw = "\u001b[1;34m❯ \u001b[1;36mBooting Windows using QEMU v11.1.0...\u001b[0m\n\n"
            + "\u001b[2J\u001b[001;001H\u001b[=3h\u001b[8;042;160t\r\n"
            + "\u001b[1;31m❯ ERROR: Timeout while waiting for QEMU to boot the machine, terminating...\u001b[0m\n"
            + "qemu-system-x86_64: terminating on signal 15 from pid 694 (bash)\r❯ Shutdown completed!\n";
        Assert.Equal(new[]
        {
            "❯ ERROR: Timeout while waiting for QEMU to boot the machine, terminating...",
            "qemu-system-x86_64: terminating on signal 15 from pid 694 (bash)",
            "❯ Shutdown completed!",
        }, PodmanSlotDriver.CleanOutputLines(Raw, 3));
        Assert.Equal(4, PodmanSlotDriver.CleanOutputLines(Raw, 10).Count);
        Assert.Empty(PodmanSlotDriver.CleanOutputLines("\u001b[2J\r\n\n", 4));
    }

    /// <summary>A path as the POSIX shell of <see cref="RunPosixShell"/> reads it: forward slashes (Git for Windows' sh
    /// takes C:/... as it is).</summary>
    /// <param name="path">The path.</param>
    /// <returns>The path with forward slashes.</returns>
    private static string ShellPath(string path) => path.Replace('\\', '/');

    /// <summary>Runs a script with a POSIX shell: /bin/sh on Linux; on Windows the sh of Git for Windows (found beside the
    /// git these tests already need), its folder first on PATH so that its own grep and sed run.</summary>
    /// <param name="script">The script.</param>
    /// <returns>The exit code and both outputs.</returns>
    /// <exception cref="Xunit.Sdk.XunitException">No POSIX shell was found: the script cannot be proved without one.</exception>
    private static (int Exit, string Output, string Error) RunPosixShell(string script)
    {
        string shell = "/bin/sh";
        string path = Environment.GetEnvironmentVariable("PATH") ?? "";
        if (OperatingSystem.IsWindows())
        {
            // git --exec-path is <git>/mingw64/libexec/git-core; the shell is <git>/usr/bin/sh.exe.
            var probe = new System.Diagnostics.ProcessStartInfo("git", "--exec-path")
            {
                RedirectStandardOutput = true,
                UseShellExecute = false,
            };
            using (System.Diagnostics.Process git = System.Diagnostics.Process.Start(probe)!)
            {
                string exec = git.StandardOutput.ReadToEnd().Trim();
                git.WaitForExit();
                string bin = Path.GetFullPath(Path.Combine(exec, "..", "..", "..", "usr", "bin"));
                shell = Path.Combine(bin, "sh.exe");
                path = bin + Path.PathSeparator + path;
            }
            Assert.True(File.Exists(shell), $"no POSIX sh at {shell}: install Git for Windows to run this test");
        }
        var info = new System.Diagnostics.ProcessStartInfo(shell)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        info.ArgumentList.Add("-c");
        info.ArgumentList.Add(script);
        info.Environment["PATH"] = path;
        using System.Diagnostics.Process process = System.Diagnostics.Process.Start(info)!;
        // Both streams drained at once: a full pipe would block the shell while the test waits on the other one.
        Task<string> error = process.StandardError.ReadToEndAsync();
        string output = process.StandardOutput.ReadToEnd();
        process.WaitForExit();
        return (process.ExitCode, output, error.GetAwaiter().GetResult());
    }

    /// <summary>The worker's request book gives builds its own sizes and e2e VMs uncapped; homepc's keeps the tuned
    /// capped e2e VM. A request that names its own size still gets exactly that.</summary>
    [Fact]
    public void WorkerSizesTheSlots()
    {
        Dictionary<string, string> env = WorkerEnvironment(_root);
        RequestSizing sizing = CoordinatorOptions.FromEnvironment(name => env.GetValueOrDefault(name)).Sizing;
        var worker = new RequestBook(new RecordStore(Path.Combine(_root, "worker")), _clock, sizing);
        RequestRecord e2e = worker.Create(new CreateRequest(RequestKind.E2e, new RequestInputs { BuildId = "w20261112-1" }));
        Assert.Null(e2e.Inputs.GuestMemoryMiB);
        Assert.Equal(6144, e2e.Resources!.Min.MemoryMiB);
        Assert.False(Scheduler.IsCapped(e2e));
        RequestRecord build = worker.Create(new CreateRequest(RequestKind.Build,
            new RequestInputs { Commit = "6482adbde46949a8ebbd873966516356e9b3e0be" }));
        Assert.Equal((16384, 32768, 8, 16), (build.Resources!.Min.MemoryMiB, build.Resources.Preferred.MemoryMiB,
            build.Resources.Min.Vcpus, build.Resources.Preferred.Vcpus));
        RequestRecord sized = worker.Create(new CreateRequest(RequestKind.E2e,
            new RequestInputs { BuildId = "w20261112-1", MemoryMiB = 8192, Vcpus = 8 }));
        Assert.Equal((8192, 8192, 8, 8), (sized.Resources!.Min.MemoryMiB, sized.Resources.Preferred.MemoryMiB,
            sized.Resources.Min.Vcpus, sized.Resources.Preferred.Vcpus));
        // No swap on the worker: a capped VM is refused when asked, never killed later by its cap.
        Assert.False(sizing.CappedSlots);
        Assert.Throws<RequestRejectedException>(() => worker.Create(new CreateRequest(RequestKind.E2e,
            new RequestInputs { BuildId = "w20261112-1", MemoryMiB = 2048, GuestMemoryMiB = 4096 })));

        var homepc = new RequestBook(new RecordStore(Path.Combine(_root, "homepc")), _clock);
        RequestRecord capped = homepc.Create(new CreateRequest(RequestKind.E2e, new RequestInputs { BuildId = "b20261112-1" }));
        Assert.Equal(RequestDefaults.E2eGuestMemoryMiB, capped.Inputs.GuestMemoryMiB);
        Assert.True(Scheduler.IsCapped(capped));
        Assert.True(CoordinatorOptions.FromEnvironment(name => name == "AMB_ROOT" ? _root : null).Sizing.CappedSlots);
    }

    /// <summary>An e2e request's suite commit is a full SHA (kept in lower case), and names the only suite.</summary>
    [Fact]
    public void SuiteCommitIsChecked()
    {
        const string Commit = "6482adbde46949a8ebbd873966516356e9b3e0be";
        var book = new RequestBook(new RecordStore(_root), _clock);
        RequestRecord ok = book.Create(new CreateRequest(RequestKind.E2e,
            new RequestInputs { BuildId = "w1", SuiteCommit = Commit.ToUpperInvariant() }));
        Assert.Equal(Commit, ok.Inputs.SuiteCommit);
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.E2e,
            new RequestInputs { BuildId = "w1", SuiteCommit = "HEAD" })));
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.E2e,
            new RequestInputs { BuildId = "w1", SuiteCommit = Commit[..12] })));
        Assert.Throws<RequestRejectedException>(() => book.Create(new CreateRequest(RequestKind.E2e,
            new RequestInputs { BuildId = "w1", SuiteCommit = Commit, SuiteSource = "/srv/e2e" })));
    }

    /// <summary>The worker's bigger build VMs build more in parallel; homepc's sizes keep what they had.</summary>
    [Fact]
    public void BiggerBuildVmsBuildMoreAtOnce()
    {
        Assert.Equal((3, 3), BuildJob.Parallelism(new Resources(Resources.Gib(16), 8, 0)));
        Assert.Equal((3, 3), BuildJob.Parallelism(new Resources(Resources.Gib(23), 16, 0)));
        Assert.Equal((4, 3), BuildJob.Parallelism(new Resources(Resources.Gib(32), 16, 0)));
        Assert.Equal((5, 3), BuildJob.Parallelism(new Resources(Resources.Gib(48), 16, 0)));
        Assert.Equal((2, 3), BuildJob.Parallelism(new Resources(Resources.Gib(48), 2, 0)));
    }

    /// <summary>A meminfo file is read as MiB by its "N kB" lines; the facts take the host's and the container's
    /// MemAvailable and the root's free space, and a file without MemAvailable is refused.</summary>
    [Fact]
    public void LinuxFactsAreReadFromMeminfo()
    {
        string host = Path.Combine(_root, "host-meminfo");
        string site = Path.Combine(_root, "site-meminfo");
        File.WriteAllText(host, "MemTotal:       131889380 kB\nMemFree:         2000000 kB\nMemAvailable:   104857600 kB\nHugePages_Total:       0\n");
        File.WriteAllText(site, "MemTotal:       104857600 kB\nMemAvailable:    94371840 kB\n");
        IReadOnlyDictionary<string, long> values = HostCapacity.ReadMemInfoMiB(host);
        Assert.Equal(128798, values["MemTotal"]);
        Assert.False(values.ContainsKey("HugePages_Total"));
        LinuxHostFacts facts = HostCapacity.ReadLinuxFacts(host, site, _root, _clock.GetUtcNow());
        Assert.Equal((102400, 128798, 92160), (facts.HostMemAvailableMiB, facts.HostMemTotalMiB, facts.SiteMemAvailableMiB));
        Assert.True(facts.DiskFreeMiB > 0);
        File.WriteAllText(site, "MemTotal:       104857600 kB\n");
        Assert.Throws<FormatException>(() => HostCapacity.ReadLinuxFacts(host, site, _root, _clock.GetUtcNow()));
    }

    /// <summary>A Linux site's free memory is the smaller of the host's and the container's, each minus its reserve; a
    /// slot younger than the warm-up is taken off, an older one is in the numbers already, and the idle answer gives back
    /// what the settled slots hold. Stale facts hold the site.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task LinuxCapacityCountsYoungSlots()
    {
        Dictionary<string, string> env = WorkerEnvironment(_root);
        env["AMB_GUEST_AGENT"] = Path.Combine(_root, "amb-guest.exe");
        File.WriteAllText(env["AMB_GUEST_AGENT"], "a stand-in");
        CoordinatorOptions options = CoordinatorOptions.FromEnvironment(name => env.GetValueOrDefault(name));
        var slots = new SlotManager(options, new RecordStore(options.RecordsDir), new GuestBroker(_clock), new NoDriver(),
            _clock, NullLogger<SlotManager>.Instance);
        var capacity = new HostCapacity(options, slots, _clock, NullLogger<HostCapacity>.Instance);
        Assert.Contains(capacity.SiteFree().Holds, h => h.StartsWith("no host facts", StringComparison.Ordinal));

        capacity.OverrideLinux(new LinuxHostFacts(_clock.GetUtcNow(), 60000, 128000, 90000, 800_000));
        SiteFree free = capacity.SiteFree();
        Assert.Equal("worker", free.Site);
        // min(60000 - 8192, 90000 - 2048); 20 vCPUs; 800000 - 51200 MiB of disk.
        Assert.Equal(new Resources(51808, 20, 748800), free.Free);

        var request = new RequestRecord
        {
            Id = "r1", Kind = RequestKind.Build, State = RequestState.Starting, Inputs = new RequestInputs(),
            CreatedAt = _clock.GetUtcNow(), UpdatedAt = _clock.GetUtcNow(), WaitTimeout = TimeSpan.FromMinutes(60),
            RunTimeout = TimeSpan.FromMinutes(180),
        };
        SlotLease lease = await slots.StartAsync(request, "am-build", new Resources(32768, 16, 0), CancellationToken.None);
        Assert.Equal("worker", slots.Active.Single().Site);
        Assert.Equal(new Resources(51808 - 32768, 4, 748800), capacity.SiteFree().Free);

        // Settled: the facts show the VM's memory as used, so nothing is taken off twice; idle gives it back.
        _clock.Advance(HostCapacity.WarmUp + TimeSpan.FromSeconds(1));
        capacity.OverrideLinux(new LinuxHostFacts(_clock.GetUtcNow(), 60000 - 32768, 128000, 90000 - 32768, 800_000));
        Assert.Equal(51808 - 32768, capacity.SiteFree().Free.MemoryMiB);
        Assert.Equal(new Resources(51808, 20, 748800), capacity.SiteFree(idle: true).Free);

        _clock.Advance(HostCapacity.Stale + TimeSpan.FromSeconds(1));
        Assert.Contains(capacity.SiteFree().Holds, h => h.StartsWith("no host facts", StringComparison.Ordinal));
        await slots.StopAsync(lease, graceful: false);
    }

    /// <summary>The push-mode mirror: it fetches nothing, takes a pushed bundle as refs/amb/pushed/&lt;sha&gt;, names its
    /// tips and whether it holds a commit, takes an incremental bundle on top, and refuses a bundle that names another
    /// ref or builds on what it lacks. A commit never pushed fails a build with "push it first"; the suite of a pushed
    /// commit is written out of the mirror.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task PushedCommitsReachTheMirror()
    {
        string source = Path.Combine(_root, "source");
        Directory.CreateDirectory(source);
        await Git(source, "init", "-q");
        await Git(source, "config", "user.email", "test@example.invalid");
        await Git(source, "config", "user.name", "test");
        await Git(source, "config", "core.autocrlf", "false");
        Directory.CreateDirectory(Path.Combine(source, "e2e", "guest"));
        File.WriteAllText(Path.Combine(source, "e2e", "guest", "run_suite.py"), "print('suite one')\n");
        File.WriteAllText(Path.Combine(source, "README.md"), "outside the suite\n");
        await Git(source, "add", "-A");
        await Git(source, "commit", "-q", "-m", "one");
        string first = (await Git(source, "rev-parse", "HEAD")).Trim();
        File.WriteAllText(Path.Combine(source, "e2e", "guest", "run_suite.py"), "print('suite two')\n");
        await Git(source, "commit", "-q", "-am", "two");
        string second = (await Git(source, "rev-parse", "HEAD")).Trim();
        string firstBundle = await MakeBundle(source, "refs/push/" + first, first, null);
        string secondBundle = await MakeBundle(source, "refs/push/" + second, second, first);

        Dictionary<string, string> env = WorkerEnvironment(Path.Combine(_root, "site"));
        var mirror = new SourceMirror(CoordinatorOptions.FromEnvironment(name => env.GetValueOrDefault(name)),
            NullLogger.Instance);
        SourceTips empty = await mirror.TipsAsync(SourceMirror.RepoName, first, CancellationToken.None);
        Assert.Equal((false, 0), (empty.Has, empty.Commits.Count));
        // A build of a commit never pushed: no fetch from anywhere, the job says to push.
        InvalidOperationException missing = await Assert.ThrowsAsync<InvalidOperationException>(
            () => mirror.EnsureCommitAsync(first, CancellationToken.None));
        Assert.Contains("push", missing.Message, StringComparison.Ordinal);
        // An incremental bundle cannot go first: its basis is not there.
        InvalidOperationException early = await Assert.ThrowsAsync<InvalidOperationException>(
            () => mirror.ReceiveBundleAsync(SourceMirror.RepoName, secondBundle, CancellationToken.None));
        Assert.Contains("does not apply", early.Message, StringComparison.Ordinal);

        Assert.Equal([first], await mirror.ReceiveBundleAsync(SourceMirror.RepoName, firstBundle, CancellationToken.None));
        Assert.Equal([second], await mirror.ReceiveBundleAsync(SourceMirror.RepoName, secondBundle, CancellationToken.None));
        SourceTips tips = await mirror.TipsAsync(SourceMirror.RepoName, second.ToUpperInvariant(), CancellationToken.None);
        Assert.True(tips.Has);
        // Both commits are tips (their order is by commit date, which two commits of the same second share).
        Assert.Equal(new[] { first, second }.Order(StringComparer.Ordinal), tips.Commits.Order(StringComparer.Ordinal));
        await mirror.EnsureCommitAsync(second, CancellationToken.None);
        Assert.Equal(first, await mirror.ParentAsync(second, CancellationToken.None));

        string suite = Path.Combine(_root, "suite");
        Assert.Equal(1, await mirror.ExportTreeAsync(first, "e2e", suite, CancellationToken.None));
        Assert.Equal("print('suite one')\n", File.ReadAllText(Path.Combine(suite, "guest", "run_suite.py")));
        Assert.False(File.Exists(Path.Combine(suite, "README.md")));
        await Assert.ThrowsAsync<InvalidOperationException>(
            () => mirror.ExportTreeAsync(first, "no-such-folder", Path.Combine(_root, "none"), CancellationToken.None));

        // A bundle that would move a branch is refused before anything is fetched.
        await Git(source, "branch", "-f", "evil", second);
        string branchBundle = await MakeBundle(source, "refs/heads/evil", second, null);
        InvalidOperationException branch = await Assert.ThrowsAsync<InvalidOperationException>(
            () => mirror.ReceiveBundleAsync(SourceMirror.RepoName, branchBundle, CancellationToken.None));
        Assert.Contains("refs/push/", branch.Message, StringComparison.Ordinal);
        await Assert.ThrowsAsync<ArgumentException>(() => mirror.TipsAsync("other", null, CancellationToken.None));
        await Assert.ThrowsAsync<ArgumentException>(() => mirror.TipsAsync(SourceMirror.RepoName, "HEAD", CancellationToken.None));
    }

    /// <summary>A worker build pulled into homepc's site keeps its id and its files' SHA-256, lists only the files that
    /// came, and is answered "here already" the second time; a changed file or homepc's own letter is refused, with no
    /// store folder left behind.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task PulledBuildKeepsItsId()
    {
        CoordinatorOptions options = CoordinatorOptions.FromEnvironment(name => name == "AMB_ROOT" ? _root : null);
        var store = new RecordStore(options.RecordsDir);
        var book = new RequestBook(store, _clock);
        var slots = new SlotManager(options, store, new GuestBroker(_clock), new NoDriver(), _clock,
            NullLogger<SlotManager>.Instance);
        string folder = Path.Combine(_root, "pulled");
        Directory.CreateDirectory(folder);
        byte[] layout = "a layout zip"u8.ToArray();
        byte[] manifest = "{\"version\":\"0.0.1.0\"}"u8.ToArray();
        File.WriteAllBytes(Path.Combine(folder, "AgentmasterDev_0.0.1.0_x64_layout.zip"), layout);
        File.WriteAllBytes(Path.Combine(folder, "build.json"), manifest);
        var worker = new BuildRecord
        {
            Id = "w20261112-1", Source = BuildSource.Built, CreatedAt = _clock.GetUtcNow(), RequestId = "r20261112-100000-abcdef",
            Commit = "6482adbde46949a8ebbd873966516356e9b3e0be", Configuration = "Release", Identity = "dev", Purge = "none",
            Origin = "worker",
            Artifacts =
            [
                new ArtifactRef("AgentmasterDev_0.0.1.0_x64_layout.zip", "layout-zip", layout.Length, Sha(layout), "builds/w20261112-1/x"),
                new ArtifactRef("build.json", "build-json", manifest.Length, Sha(manifest), "builds/w20261112-1/build.json"),
                new ArtifactRef("AgentmasterDev_0.0.1.0_x64_symbols.zip", "symbols-zip", 99, new string('0', 64), "builds/w20261112-1/s"),
            ],
        };
        await WriteManifest(folder, worker);

        JobOutcome pulled = await RunImport(book, store, slots, options, folder);
        Assert.True(pulled.Succeeded, pulled.Outcome);
        Assert.Equal("w20261112-1", pulled.BuildId);
        BuildRecord here = store.Read("builds", "w20261112-1", AmbJson.Default.EnvelopeBuildRecord)!.Value;
        Assert.Equal(BuildSource.Imported, here.Source);
        Assert.Equal(["layout-zip", "build-json"], here.Artifacts.Select(a => a.Kind));
        Assert.All(here.Artifacts, a => Assert.Equal($"builds/w20261112-1/{a.Name}", a.StorePath));
        Assert.Equal(layout, File.ReadAllBytes(Path.Combine(options.StoreDir, "builds", "w20261112-1", "AgentmasterDev_0.0.1.0_x64_layout.zip")));
        Assert.StartsWith("pulled from worker", here.Origin, StringComparison.Ordinal);
        Assert.Equal(worker.Commit, here.Commit);
        Assert.Null(DeployJob.Refusal(here, DeployJob.TargetOf("dev", options)));

        JobOutcome again = await RunImport(book, store, slots, options, folder);
        Assert.True(again.Succeeded);
        Assert.Contains("already", again.Outcome, StringComparison.Ordinal);

        // A file that is not the bytes the worker built: refused, nothing kept.
        await WriteManifest(folder, worker with { Id = "w20261112-2" });
        File.WriteAllBytes(Path.Combine(folder, "build.json"), "changed"u8.ToArray());
        JobOutcome changed = await RunImport(book, store, slots, options, folder);
        Assert.False(changed.Succeeded);
        Assert.Contains("hashes to", changed.Outcome, StringComparison.Ordinal);
        Assert.False(Directory.Exists(Path.Combine(options.StoreDir, "builds", "w20261112-2")));

        // homepc's own letter: an id of this site is never taken from another.
        await WriteManifest(folder, worker with { Id = "b20261112-9" });
        Assert.False((await RunImport(book, store, slots, options, folder)).Succeeded);
    }

    /// <summary>On cgroup v2 a slot's figures come from memory.peak, the swap files and memory.events' "max"; without the
    /// swap files the swap figures are 0.</summary>
    [Fact]
    public void CgroupV2MemoryFigures()
    {
        string dir = Path.Combine(_root, "cgroup");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "memory.peak"), (6L << 30).ToString(System.Globalization.CultureInfo.InvariantCulture) + "\n");
        File.WriteAllText(Path.Combine(dir, "memory.events"), "low 0\nhigh 0\nmax 17\noom 0\noom_kill 0\n");
        Assert.Equal(new SlotMemoryStats(6144, 6144, 0, 17), PodmanSlotDriver.ReadMemoryV2(dir));
        File.WriteAllText(Path.Combine(dir, "memory.swap.peak"), (1L << 30).ToString(System.Globalization.CultureInfo.InvariantCulture));
        File.WriteAllText(Path.Combine(dir, "memory.swap.current"), (512L << 20).ToString(System.Globalization.CultureInfo.InvariantCulture));
        Assert.Equal(new SlotMemoryStats(6144, 7168, 512, 17), PodmanSlotDriver.ReadMemoryV2(dir));
    }

    /// <summary>A site with no Windows beside it refuses an install at once, pointing at the home site.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task WorkerRefusesInstalls()
    {
        Dictionary<string, string> env = WorkerEnvironment(_root);
        CoordinatorOptions options = CoordinatorOptions.FromEnvironment(name => env.GetValueOrDefault(name));
        var store = new RecordStore(options.RecordsDir);
        var book = new RequestBook(store, _clock);
        var slots = new SlotManager(options, store, new GuestBroker(_clock), new NoDriver(), _clock,
            NullLogger<SlotManager>.Instance);
        RequestRecord request = book.Create(new CreateRequest(RequestKind.Deploy,
            new RequestInputs { BuildId = "w20261112-1", Instance = "dev" }));
        var context = new JobContext(book.Move(request.Id, RequestState.Starting, "test")!, book, slots, store, options,
            _clock, NullLogger.Instance);
        JobOutcome outcome = await new DeployJob().RunAsync(context, CancellationToken.None);
        Assert.False(outcome.Succeeded);
        Assert.Contains("home site", outcome.Outcome, StringComparison.Ordinal);
    }

    /// <summary>Runs an import of a folder through the job, as the scheduler would (the request moved to Starting first).</summary>
    /// <param name="book">The request book.</param>
    /// <param name="store">The record store.</param>
    /// <param name="slots">The slot manager.</param>
    /// <param name="options">The site's settings.</param>
    /// <param name="folder">The folder.</param>
    /// <returns>The job's outcome.</returns>
    private async Task<JobOutcome> RunImport(RequestBook book, RecordStore store, SlotManager slots,
        CoordinatorOptions options, string folder)
    {
        RequestRecord request = book.Create(new CreateRequest(RequestKind.Import, new RequestInputs { Source = folder }));
        var context = new JobContext(book.Move(request.Id, RequestState.Starting, "test")!, book, slots, store, options,
            _clock, NullLogger.Instance);
        return await new ImportJob().RunAsync(context, CancellationToken.None);
    }

    /// <summary>Writes a build record as `amb` saves it beside the files it pulled.</summary>
    /// <param name="folder">The folder.</param>
    /// <param name="build">The record.</param>
    /// <returns>A task.</returns>
    private static Task WriteManifest(string folder, BuildRecord build) =>
        File.WriteAllTextAsync(Path.Combine(folder, "amb-build.json"), JsonSerializer.Serialize(build, AmbJson.Default.BuildRecord));

    /// <summary>A byte array's SHA-256, lowercase hex (as manifests carry it).</summary>
    /// <param name="bytes">The bytes.</param>
    /// <returns>The hash.</returns>
    private static string Sha(byte[] bytes) => Convert.ToHexStringLower(SHA256.HashData(bytes));

    /// <summary>Makes a bundle of one ref in a repository (as `amb` does in its scratch repository).</summary>
    /// <param name="repo">The repository.</param>
    /// <param name="refName">The ref the bundle carries (made at <paramref name="commit"/>).</param>
    /// <param name="commit">The commit.</param>
    /// <param name="exclude">A commit whose history is left out, or null for the whole history.</param>
    /// <returns>The bundle's path.</returns>
    private async Task<string> MakeBundle(string repo, string refName, string commit, string? exclude)
    {
        await Git(repo, "update-ref", refName, commit);
        string file = Path.Combine(_root, Guid.NewGuid().ToString("N") + ".bundle");
        if (exclude is null)
        {
            await Git(repo, "bundle", "create", "-q", file, refName);
        }
        else
        {
            await Git(repo, "bundle", "create", "-q", file, refName, "^" + exclude);
        }
        return file;
    }

    /// <summary>Runs git in a folder and fails the test when it fails.</summary>
    /// <param name="dir">The folder.</param>
    /// <param name="args">git's arguments.</param>
    /// <returns>Its standard output.</returns>
    private static async Task<string> Git(string dir, params string[] args)
    {
        CommandResult result = await Commands.RunAsync("git", ["-C", dir, .. args], TimeSpan.FromMinutes(1),
            CancellationToken.None);
        Assert.True(result.ExitCode == 0, $"git {string.Join(' ', args)}: {result.Error}");
        return result.Output;
    }

    /// <summary>A slot driver that starts nothing (the capacity and import tests only need slot records).</summary>
    private sealed class NoDriver : ISlotDriver
    {
        /// <inheritdoc/>
        public string Name => "none";

        /// <inheritdoc/>
        public bool IsVm => true;

        /// <inheritdoc/>
        public string GuestShareRoot(SlotSpec spec) => spec.ExchangeDir;

        /// <inheritdoc/>
        public Task<string> StartAsync(SlotSpec spec, CancellationToken cancel) => Task.FromResult("none-" + spec.SlotId);

        /// <inheritdoc/>
        public Task StopAsync(string handle, bool graceful, CancellationToken cancel) => Task.CompletedTask;

        /// <inheritdoc/>
        public Task<bool> IsAliveAsync(string handle, CancellationToken cancel) => Task.FromResult(true);

        /// <inheritdoc/>
        public Task<bool> SetBalloonAsync(string handle, long targetMiB, CancellationToken cancel) => Task.FromResult(false);
    }
}
