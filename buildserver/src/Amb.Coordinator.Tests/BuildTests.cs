using Amb.Coordinator.Jobs;
using Amb.Coordinator.Slots;
using Amb.Coordinator.Sources;
using Amb.Core.Allocation;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Records;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Time.Testing;
using Xunit;

namespace Amb.Coordinator.Tests;

/// <summary>The build MVP's rules that need no VM: what a build request may ask, when a build exists already, how much a
/// bundle carries, how parallel a build runs, what each file is, and that a kept VM's disks outlive its slot.</summary>
public sealed class BuildTests : IDisposable
{
    /// <summary>A full commit SHA for the tests.</summary>
    private const string Commit = "5f5d53fa98edbe586f9998ec576ae8fd7d252c57";

    /// <summary>The test's own site root under %TEMP%, deleted after it.</summary>
    private readonly string _root = Path.Combine(Path.GetTempPath(), "amb-build-" + Guid.NewGuid().ToString("N"));

    /// <summary>Makes the root.</summary>
    public BuildTests() => Directory.CreateDirectory(_root);

    /// <summary>Deletes the root.</summary>
    public void Dispose() => Directory.Delete(_root, recursive: true);

    /// <summary>A build request gets its defaults and canonical spellings; a ref, a short SHA or an unknown value is
    /// refused.</summary>
    [Fact]
    public void BuildInputsAreCheckedAndCompleted()
    {
        RequestInputs filled = RequestBook.NormalizeBuild(new RequestInputs { Commit = Commit.ToUpperInvariant() });
        Assert.Equal((Commit, "Release", "dev", "none"), (filled.Commit, filled.Configuration, filled.Identity, filled.Purge));
        RequestInputs debug = RequestBook.NormalizeBuild(new RequestInputs
        {
            Commit = Commit, Configuration = "debug", Identity = "RELEASE", Purge = "Force",
        });
        Assert.Equal(("Debug", "release", "force"), (debug.Configuration, debug.Identity, debug.Purge));
        foreach (RequestInputs bad in new[]
        {
            new RequestInputs(),
            new RequestInputs { Commit = "HEAD" },
            new RequestInputs { Commit = Commit[..9] },
            new RequestInputs { Commit = Commit[..39] + "g" },
            new RequestInputs { Commit = Commit, Configuration = "Fast" },
            new RequestInputs { Commit = Commit, Identity = "prod" },
            new RequestInputs { Commit = Commit, Purge = "auto" },
        })
        {
            Assert.Throws<RequestRejectedException>(() => RequestBook.NormalizeBuild(bad));
        }
    }

    /// <summary>A build exists only for the same commit, configuration, identity and purge, and only when the service
    /// built it (an import of the same commit is no build of it); the newest one wins.</summary>
    [Fact]
    public void ExistingBuildIsFoundByItsFourKeys()
    {
        var store = new RecordStore(Path.Combine(_root, "records"));
        BuildRecord Build(string id, string commit, string configuration, string identity, string purge, BuildSource source,
            int minute) => new()
        {
            Id = id, Source = source, CreatedAt = DateTimeOffset.Parse("2026-10-26T10:00:00Z").AddMinutes(minute),
            RequestId = "r1", Commit = commit, Configuration = configuration, Identity = identity, Purge = purge,
            Artifacts = [],
        };
        foreach (BuildRecord b in new[]
        {
            Build("b20261026-1", Commit, "Release", "dev", "none", BuildSource.Imported, 0),
            Build("b20261026-2", Commit, "Release", "dev", "none", BuildSource.Built, 1),
            Build("b20261026-3", Commit, "Release", "dev", "none", BuildSource.Built, 2),
            Build("b20261026-4", Commit, "Debug", "dev", "none", BuildSource.Built, 3),
        })
        {
            store.Write("builds", b.Id, b, 0, AmbJson.Default.EnvelopeBuildRecord);
        }
        Assert.Equal("b20261026-3", BuildJob.FindExisting(store, Commit, "Release", "dev", "none")?.Id);
        Assert.Equal("b20261026-4", BuildJob.FindExisting(store, Commit, "Debug", "dev", "none")?.Id);
        Assert.Null(BuildJob.FindExisting(store, Commit, "Release", "release", "none"));
        Assert.Null(BuildJob.FindExisting(store, Commit, "Release", "dev", "force"));
        Assert.Null(BuildJob.FindExisting(store, new string('a', 40), "Release", "dev", "none"));
    }

    /// <summary>A bundle carries nothing when the VM holds the commit (or a descendant of it: an older commit built after a
    /// newer one), the difference when it holds a basis the mirror knows, and the whole history otherwise.</summary>
    [Fact]
    public void BundleCarriesOnlyWhatTheVmLacks()
    {
        string basis = new('b', 40);
        Assert.Equal(BundleMode.None, SourceMirror.PlanBundle(Commit, Commit, basisInMirror: true, commitReachableFromBasis: true));
        Assert.Equal(BundleMode.None, SourceMirror.PlanBundle(Commit, basis, basisInMirror: true, commitReachableFromBasis: true));
        Assert.Equal(BundleMode.Incremental, SourceMirror.PlanBundle(Commit, basis, basisInMirror: true, commitReachableFromBasis: false));
        Assert.Equal(BundleMode.Full, SourceMirror.PlanBundle(Commit, basis, basisInMirror: false, commitReachableFromBasis: false));
        Assert.Equal(BundleMode.Full, SourceMirror.PlanBundle(Commit, null, basisInMirror: false, commitReachableFromBasis: false));
    }

    /// <summary>The VM's record keeps each worktree's submodule commit apart: a new worktree finds no basis for its own
    /// submodule clone, however many other worktrees hold it (the first Debug build's checkout failed when one VM-wide
    /// entry said its empty clone held the commit), and an older record's VM-wide entry goes at the next checkout.</summary>
    [Fact]
    public void SubmoduleBasisIsPerWorktree()
    {
        string sdk = new('5', 40);
        var legacy = new Dictionary<string, string>(StringComparer.Ordinal) { ["release-dev"] = Commit, [BuildJob.LegacySdkKey] = sdk };
        Dictionary<string, string> afterRelease = BuildJob.RecordCommits(legacy, "release-dev", Commit, sdk);
        Assert.False(afterRelease.ContainsKey(BuildJob.LegacySdkKey));
        Assert.Equal(sdk, afterRelease[BuildJob.SdkKey("release-dev")]);
        Assert.False(afterRelease.ContainsKey(BuildJob.SdkKey("debug-dev")));
        Assert.NotEqual(BuildJob.SdkKey(BuildJob.WorktreeKey("Release", "dev")), BuildJob.SdkKey(BuildJob.WorktreeKey("Debug", "dev")));
        Dictionary<string, string> afterDebug = BuildJob.RecordCommits(afterRelease, "debug-dev", Commit, sdk);
        Assert.Equal(sdk, afterDebug[BuildJob.SdkKey("debug-dev")]);
        Assert.Equal(sdk, afterDebug[BuildJob.SdkKey("release-dev")]);
        Assert.False(BuildJob.RecordCommits(afterDebug, "debug-dev", Commit, sdkCommit: null).ContainsKey(BuildJob.SdkKey("debug-dev")));
    }

    /// <summary>The build's parallelism grows with the VM's memory and never exceeds its vCPUs.</summary>
    [Fact]
    public void ParallelityFollowsTheMemory()
    {
        Assert.Equal((2, 2), BuildJob.Parallelism(new Resources(Resources.Gib(8), 8, 0)));
        Assert.Equal((2, 3), BuildJob.Parallelism(new Resources(Resources.Gib(12), 8, 0)));
        Assert.Equal((3, 3), BuildJob.Parallelism(new Resources(Resources.Gib(16), 8, 0)));
        Assert.Equal((1, 3), BuildJob.Parallelism(new Resources(Resources.Gib(16), 1, 0)));
    }

    /// <summary>Each file of a build is named by its role (what `amb fetch --kind` picks).</summary>
    [Fact]
    public void ArtifactKindsFollowTheNames()
    {
        const string Portable = "AgentmasterDev_0.0.1.0_x64.zip";
        Assert.Equal("portable-zip", BuildJob.ArtifactKind(Portable, Portable));
        Assert.Equal("layout-zip", BuildJob.ArtifactKind("AgentmasterDev_0.0.1.0_x64_layout.zip", Portable));
        Assert.Equal("symbols-zip", BuildJob.ArtifactKind("AgentmasterDev_0.0.1.0_x64_symbols.zip", Portable));
        Assert.Equal("build-log", BuildJob.ArtifactKind("build.log", Portable));
        Assert.Equal("build-log", BuildJob.ArtifactKind("build.log.err", Portable));
        Assert.Equal("build-json", BuildJob.ArtifactKind("build.json", Portable));
        Assert.Equal("file", BuildJob.ArtifactKind("other.txt", Portable));
        Assert.Equal("release-dev", BuildJob.WorktreeKey("Release", "dev"));
    }

    /// <summary>A rebuilt image gets another stamp (its kept VMs are then discarded).</summary>
    [Fact]
    public void ImageStampChangesWithTheImage()
    {
        string disk = Path.Combine(_root, "data.qcow2");
        File.WriteAllText(disk, "one");
        File.SetLastWriteTimeUtc(disk, DateTime.Parse("2026-10-26T10:00:00Z").ToUniversalTime());
        string first = BuildJob.ImageStamp(disk);
        File.WriteAllText(disk, "a longer one");
        Assert.NotEqual(first, BuildJob.ImageStamp(disk));
    }

    /// <summary>A kept VM's slot uses the VM's own folder, which outlives the slot (its share does not), and is always
    /// stopped gracefully, even when the job gave up on its guest.</summary>
    /// <returns>A task.</returns>
    [Fact]
    public async Task KeptVmOutlivesItsSlot()
    {
        var clock = new FakeTimeProvider(DateTimeOffset.Parse("2026-10-26T10:00:00Z"));
        string agent = Path.Combine(_root, "amb-guest.exe");
        File.WriteAllText(agent, "a stand-in");
        var options = new CoordinatorOptions
        {
            Root = _root, Listen = "http://127.0.0.1:0", GuestUrl = "http://127.0.0.1:0", SlotDriver = "fake",
            HostFactsCommand = "", GuestAgentPath = agent, DockurImage = "none", IdleExitMinutes = 0, E2eSourceDir = _root,
        };
        var driver = new RecordingDriver();
        var slots = new SlotManager(options, new RecordStore(options.RecordsDir), new GuestBroker(clock), driver, clock,
            NullLogger<SlotManager>.Instance);
        var request = new RequestRecord
        {
            Id = "r1", Kind = RequestKind.Build, State = RequestState.Starting, Inputs = new RequestInputs(),
            CreatedAt = clock.GetUtcNow(), UpdatedAt = clock.GetUtcNow(), WaitTimeout = TimeSpan.FromMinutes(60),
            RunTimeout = TimeSpan.FromMinutes(180),
        };
        SlotLease lease = await slots.StartAsync(request, "am-build", new Resources(Resources.Gib(8), 4, 0),
            CancellationToken.None, keptVm: "build-1", dataDiskGiB: 128);
        Assert.Equal(Path.Combine(options.VmsDir, "build-1"), lease.Spec.SlotDir);
        Assert.True(lease.Spec.Kept);
        Assert.Equal(128, lease.Spec.DataDiskGiB);
        File.WriteAllText(Path.Combine(lease.Spec.SlotDir, "data.qcow2"), "the VM's overlay");
        await slots.StopAsync(lease, graceful: false);
        Assert.Equal([true], driver.StopsGraceful);
        Assert.True(File.Exists(Path.Combine(options.VmsDir, "build-1", "data.qcow2")));
        Assert.False(Directory.Exists(lease.Spec.ExchangeDir));
        Assert.Empty(slots.Active);
        await Assert.ThrowsAsync<ArgumentException>(() => slots.StartAsync(request, "am-build",
            new Resources(Resources.Gib(8), 4, 0), CancellationToken.None, keptVm: "../evil"));
    }

    /// <summary>A slot driver that starts nothing and records how each slot was stopped.</summary>
    private sealed class RecordingDriver : ISlotDriver
    {
        /// <summary>The <c>graceful</c> argument of every stop, in order.</summary>
        public List<bool> StopsGraceful { get; } = [];

        /// <inheritdoc/>
        public string Name => "recording";

        /// <inheritdoc/>
        public bool IsVm => true;

        /// <inheritdoc/>
        public string GuestShareRoot(SlotSpec spec) => spec.ExchangeDir;

        /// <inheritdoc/>
        public Task<string> StartAsync(SlotSpec spec, CancellationToken cancel)
        {
            Directory.CreateDirectory(spec.SlotDir);
            return Task.FromResult("recording-" + spec.SlotId);
        }

        /// <inheritdoc/>
        public Task StopAsync(string handle, bool graceful, CancellationToken cancel)
        {
            lock (StopsGraceful)
            {
                StopsGraceful.Add(graceful);
            }
            return Task.CompletedTask;
        }

        /// <inheritdoc/>
        public Task<bool> IsAliveAsync(string handle, CancellationToken cancel) => Task.FromResult(true);

        /// <inheritdoc/>
        public Task<bool> SetBalloonAsync(string handle, long targetMiB, CancellationToken cancel) => Task.FromResult(false);
    }
}
