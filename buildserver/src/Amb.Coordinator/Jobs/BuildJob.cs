using System.Globalization;
using System.Text.Json;
using Amb.Coordinator.Slots;
using Amb.Coordinator.Sources;
using Amb.Core.Allocation;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;

namespace Amb.Coordinator.Jobs;

/// <summary>
/// Builds a commit in the kept build VM and records the result as a build id (DESIGN.md §2, §5.1, §11): the build MVP.
/// </summary>
/// <remarks>
/// <para><b>The flow:</b> a build that exists (the same commit, configuration, identity and purge) is returned as it
/// is. Otherwise the site's mirror fetches the source repository, the kept VM boots (its overlay over am-build and its
/// data disk D: survive between builds), the guest gets a bundle of what its worktree lacks, checks the commit out,
/// builds it with that commit's own tools\Build-Agentmaster.ps1 into a layout on D:, and packages the portable zip, the
/// layout zip and the symbols; the coordinator moves them into the store with their SHA-256 and writes the build's
/// record. The VM then shuts down (its disks stay for the next build).</para>
/// <para><b>Nothing touches K::</b> the mirror only reads the source repository; the VM's disks, the share and the store
/// are all in the site, whose disk lives on Q:.</para>
/// <para><b>One at a time:</b> there is one kept VM, so the scheduler admits one build at a time; a second identical
/// request waits, then finds the first one's build here and returns its id.</para>
/// </remarks>
public sealed class BuildJob : VmJob
{
    /// <summary>The am-build image's PowerShell 7: every build step runs in it.</summary>
    private const string Pwsh = @"C:\Program Files\PowerShell\7\pwsh.exe";

    /// <summary>The share's folder the build steps are staged into (from the coordinator's own copy).</summary>
    private const string StepsShare = "steps";

    /// <summary>The checkout script's exit code for "the bundle needs commits the clone lacks".</summary>
    internal const int CheckoutNeedsFullBundle = 3;

    /// <inheritdoc/>
    public override async Task<JobOutcome> RunAsync(JobContext context, CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(context);
        RequestRecord request = context.Request;
        RequestInputs inputs = request.Inputs;
        string commit = inputs.Commit!, configuration = inputs.Configuration!, identity = inputs.Identity!, purge = inputs.Purge!;
        BuildRecord? existing = FindExisting(context.Store, commit, configuration, identity, purge);
        if (existing is not null)
        {
            return new JobOutcome(true, $"build {existing.Id} exists: {commit[..9]}, {configuration}, {identity}, purge {purge}",
                BuildId: existing.Id);
        }

        DateTimeOffset fetchStart = context.Clock.GetUtcNow();
        context.MarkRunning($"fetching {context.Options.SourceRepo} into the site's mirror");
        var mirror = new SourceMirror(context.Options, context.Log);
        string? sdkCommit = await mirror.FetchAsync(commit, cancel).ConfigureAwait(false);
        context.Event("source", FormattableString.Invariant(
            $"mirror has {commit[..9]} (submodule {(sdkCommit is null ? "none" : sdkCommit[..9])}) after {(context.Clock.GetUtcNow() - fetchStart).TotalSeconds:0} s"));

        VmRecord vm = PrepareVm(context);
        string key = WorktreeKey(configuration, identity);
        SlotLease lease = await StartAndWaitAsync(context, vm.Image, cancel, keptVm: vm.Name, dataDiskGiB: vm.DataDiskGiB)
            .ConfigureAwait(false);
        bool graceful = false;
        try
        {
            CopyTree(context.Options.BuildStepsDir, ShareFile(lease, StepsShare));
            await RunAsync(context, lease, Script("disk", "prepare-disk.ps1", 600, []), cancel).ConfigureAwait(false);

            string worktree = $@"D:\src\{key}";
            string layout = $@"D:\out\{key}\layout";
            string log = $@"D:\out\{key}\build.log";
            await CheckoutAsync(context, lease, mirror, vm, key, commit, sdkCommit, worktree, cancel).ConfigureAwait(false);
            // From here the worktree holds the commit, built or not: the next bundle starts from it.
            vm = SaveVm(context, vm, key, commit, sdkCommit);

            (int maxCpu, int clMpCount) = Parallelism(context.Grant);
            int buildTimeout = (int)Math.Max(600, request.RunTimeout.TotalSeconds - 1800);
            GuestStep buildStep = Script("build", "build.ps1", buildTimeout,
                ["-Worktree", worktree, "-Configuration", configuration, "-Identity", identity, "-Purge", purge,
                 "-MaxCpu", maxCpu.ToString(CultureInfo.InvariantCulture),
                 "-ClMpCount", clMpCount.ToString(CultureInfo.InvariantCulture), "-Layout", layout, "-Log", log],
                progressFile: log);
            context.Event("step", FormattableString.Invariant(
                $"step build: {configuration}, {identity}, purge {purge}, /m:{maxCpu}, CL_MPCount {clMpCount}, in {context.Grant}"));
            // A failing build is a result (its log is kept), not an error of the job: the step is run directly.
            StepResult built = await lease.Session.RunStepAsync(buildStep, Silence, cancel).ConfigureAwait(false);
            context.Event("step", FormattableString.Invariant(
                $"step build: {built.Outcome} ({built.DurationSeconds / 60:0.0} min)"));
            LogTails(context, built);
            string outShare = $@"Z:\out\{request.Id}";
            if (built.ExitCode != 0)
            {
                await RunAsync(context, lease, new GuestStep("keep-log", StepKinds.Run, Command: Pwsh,
                    Args: ["-NoProfile", "-Command",
                           $"New-Item -ItemType Directory -Force '{outShare}' | Out-Null; Copy-Item '{log}*' '{outShare}' -Force"],
                    TimeoutS: 300), cancel).ConfigureAwait(false);
                string kept = KeepFailedLogs(context, lease, request.Id);
                await RunAsync(context, lease, new GuestStep("shutdown", StepKinds.Shutdown), cancel).ConfigureAwait(false);
                graceful = true;
                return new JobOutcome(false, FormattableString.Invariant(
                    $"build failed (exit {built.ExitCode}) after {built.DurationSeconds / 60:0.0} min; its log: {kept}"),
                    built.ExitCode, ResultsPath: $"requests/{request.Id}");
            }

            await RunAsync(context, lease, Script("package", "package.ps1", 1800,
                ["-Worktree", worktree, "-Configuration", configuration, "-Identity", identity, "-Layout", layout,
                 "-Log", log, "-Out", outShare]), cancel).ConfigureAwait(false);
            context.MarkCollecting("built: keeping its artifacts");
            BuildRecord record = await CollectAsync(context, lease, commit, configuration, identity, purge,
                built.DurationSeconds, cancel).ConfigureAwait(false);
            await RunAsync(context, lease, new GuestStep("shutdown", StepKinds.Shutdown), cancel).ConfigureAwait(false);
            graceful = true;
            ArtifactRef? zip = record.Artifacts.FirstOrDefault(a => a.Kind == "portable-zip");
            return new JobOutcome(true, FormattableString.Invariant(
                $"built {record.Id}: {zip?.Name} ({(zip?.Bytes ?? 0) / 1048576.0:0} MB), {configuration} {identity}, {commit[..9]}, build {built.DurationSeconds / 60:0.0} min"),
                BuildId: record.Id);
        }
        finally
        {
            // A kept VM is always stopped gracefully (SlotManager.StopAsync): its disks are the next build's.
            await context.Slots.StopAsync(lease, graceful).ConfigureAwait(false);
        }
    }

    /// <summary>The worktree a configuration and identity build in (one per pair: switching between them rebuilds
    /// nothing).</summary>
    /// <param name="configuration">"Release" or "Debug".</param>
    /// <param name="identity">"dev" or "release".</param>
    /// <returns>E.g. "release-dev".</returns>
    internal static string WorktreeKey(string configuration, string identity) =>
        configuration.ToLowerInvariant() + "-" + identity;

    /// <summary>The build that a request names, when it exists already.</summary>
    /// <param name="store">The record store.</param>
    /// <param name="commit">The commit.</param>
    /// <param name="configuration">The configuration.</param>
    /// <param name="identity">The identity.</param>
    /// <param name="purge">The purge.</param>
    /// <returns>The newest build of those four (by id), or null.</returns>
    /// <exception cref="IOException">The records could not be read.</exception>
    /// <exception cref="JsonException">A build record is damaged.</exception>
    internal static BuildRecord? FindExisting(RecordStore store, string commit, string configuration, string identity,
        string purge) =>
        store.List("builds")
            .Select(id => store.Read("builds", id, AmbJson.Default.EnvelopeBuildRecord)?.Value)
            .OfType<BuildRecord>()
            .Where(b => b.Source == BuildSource.Built && b.Commit == commit && b.Configuration == configuration
                && b.Identity == identity && b.Purge == purge)
            .OrderByDescending(b => b.CreatedAt)
            .FirstOrDefault();

    /// <summary>Picks msbuild's parallelism for the VM's memory.</summary>
    /// <param name="grant">The slot's grant.</param>
    /// <returns>/m (projects at once) and CL_MPCount (compilers per project).</returns>
    /// <remarks>Native, a Debug build at 3 compilers took 8.7 GB of commit, a Release 7.8 GB, and each compiler that loads
    /// TerminalApp's precompiled header takes 1.8-2.3 GB (DESIGN.md §3.1); Windows itself keeps ~1.5 GB. Up to 4
    /// compilers (2 × 2) below 10 GB, 6 below 14 GB, 9 above; the e2e hand-builds ran /m:2 CL_MPCount 2 on a busy PC.
    /// P0.4 measures how long that takes.</remarks>
    internal static (int MaxCpu, int ClMpCount) Parallelism(Resources grant)
    {
        (int maxCpu, int clMp) = grant.MemoryMiB switch
        {
            < 10 * 1024 => (2, 2),
            < 14 * 1024 => (2, 3),
            _ => (3, 3),
        };
        return (Math.Max(1, Math.Min(maxCpu, grant.Vcpus)), clMp);
    }

    /// <summary>One build step: a staged script run with the image's PowerShell 7.</summary>
    /// <param name="id">The step's id.</param>
    /// <param name="script">The script's name in the share's steps folder.</param>
    /// <param name="timeoutS">The step's timeout.</param>
    /// <param name="args">The script's arguments.</param>
    /// <param name="progressFile">A file whose new lines the agent streams as progress, or null.</param>
    /// <returns>The step.</returns>
    private static GuestStep Script(string id, string script, int timeoutS, IReadOnlyList<string> args,
        string? progressFile = null) =>
        new(id, StepKinds.Run, Command: Pwsh,
            Args: ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $@"Z:\{StepsShare}\{script}", .. args],
            TimeoutS: timeoutS, ProgressFile: progressFile);

    /// <summary>Gets the commit into the VM's worktree: an incremental bundle when the VM's record names a basis, and
    /// once more with the whole history when the guest says its clone lacks that basis.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="lease">The slot.</param>
    /// <param name="mirror">The site's mirror.</param>
    /// <param name="vm">The VM's record (its worktrees' commits).</param>
    /// <param name="key">The worktree's key.</param>
    /// <param name="commit">The commit.</param>
    /// <param name="sdkCommit">The submodule's pinned commit, or null.</param>
    /// <param name="worktree">The worktree's folder in the guest.</param>
    /// <param name="cancel">Cancels the job.</param>
    /// <returns>A task that ends when the worktree holds the commit.</returns>
    /// <exception cref="StepFailedException">The checkout failed (even with the whole history).</exception>
    /// <exception cref="SlotLostException">The guest went silent.</exception>
    /// <exception cref="InvalidOperationException">A bundle could not be made.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    private static async Task CheckoutAsync(JobContext context, SlotLease lease, SourceMirror mirror, VmRecord vm, string key,
        string commit, string? sdkCommit, string worktree, CancellationToken cancel)
    {
        for (int attempt = 0; ; attempt++)
        {
            bool full = attempt > 0;
            string srcFolder = ShareFile(lease, "src");
            if (Directory.Exists(srcFolder))
            {
                Directory.Delete(srcFolder, recursive: true);
            }
            SourceBundle bundle = await mirror.BundleAsync(context.Request.Id, commit, vm.Commits.GetValueOrDefault(key),
                sdkCommit, vm.Commits.GetValueOrDefault("sdk"), srcFolder, full, cancel).ConfigureAwait(false);
            context.Event("source", FormattableString.Invariant(
                $"bundle: {bundle.Mode}, {bundle.Bytes / 1048576.0:0.0} MB{(bundle.SdkBundleFile is null ? "" : " with the submodule")}"));
            GuestStep step = Script("checkout", "checkout.ps1", 1800,
                ["-Worktree", worktree, "-Commit", commit,
                 "-Bundle", bundle.BundleFile is null ? "" : $@"Z:\src\{bundle.BundleFile}", "-BundleRef", bundle.Ref,
                 "-SdkCommit", sdkCommit ?? "",
                 "-SdkBundle", bundle.SdkBundleFile is null ? "" : $@"Z:\src\{bundle.SdkBundleFile}", "-SdkRef", bundle.SdkRef]);
            try
            {
                await RunAsync(context, lease, step, cancel).ConfigureAwait(false);
                return;
            }
            catch (StepFailedException ex) when (ex.Result.ExitCode == CheckoutNeedsFullBundle && !full)
            {
                // Expected once: the VM's data disk was made anew since its record's last build; the whole history fixes it.
                context.Event("source", "the VM's clone lacks the bundle's basis: sending the whole history");
            }
        }
    }

    /// <summary>Finds or makes the kept build VM's record; a VM made over another image than today's is discarded.</summary>
    /// <param name="context">The job's context.</param>
    /// <returns>The record to boot.</returns>
    /// <exception cref="InvalidOperationException">The build image does not exist.</exception>
    /// <exception cref="IOException">A record could not be written, or a discarded VM's folder deleted.</exception>
    /// <exception cref="JsonException">The VM's record is damaged.</exception>
    private static VmRecord PrepareVm(JobContext context)
    {
        string image = context.Options.BuildImage;
        string imageDisk = Path.Combine(context.Options.ImagesDir, image, "data.qcow2");
        if (!File.Exists(imageDisk))
        {
            throw new InvalidOperationException($"no build image {image}: make it with `images.py build-image`");
        }
        string stamp = ImageStamp(imageDisk);
        string name = context.Options.BuildVmName;
        string folder = Path.Combine(context.Options.VmsDir, name);
        VmRecord? vm = context.Store.Read("vms", name, AmbJson.Default.EnvelopeVmRecord)?.Value;
        bool usable = vm is not null && vm.Image == image && vm.ImageStamp == stamp
            && File.Exists(Path.Combine(folder, "data.qcow2"));
        if (!usable)
        {
            if (Directory.Exists(folder))
            {
                // Cleanup: an overlay over a changed base reads garbage, and a record without disks names nothing.
                context.Event("slot", $"the build VM {name} was made over another image (or lost its disks): making it anew");
                Directory.Delete(folder, recursive: true);
            }
            var fresh = new VmRecord
            {
                Name = name,
                Image = image,
                ImageStamp = stamp,
                DataDiskGiB = context.Options.BuildDataDiskGiB,
                CreatedAt = context.Clock.GetUtcNow(),
            };
            context.Store.Update<VmRecord>("vms", name, _ => fresh, AmbJson.Default.EnvelopeVmRecord);
            return fresh;
        }
        return vm!;
    }

    /// <summary>Records what a worktree holds after a checkout.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="vm">The VM's record.</param>
    /// <param name="key">The worktree's key.</param>
    /// <param name="commit">Its commit.</param>
    /// <param name="sdkCommit">The submodule's commit, or null.</param>
    /// <returns>The updated record.</returns>
    /// <exception cref="IOException">The record could not be written.</exception>
    private static VmRecord SaveVm(JobContext context, VmRecord vm, string key, string commit, string? sdkCommit)
    {
        var commits = new Dictionary<string, string>(vm.Commits, StringComparer.Ordinal) { [key] = commit };
        if (sdkCommit is not null)
        {
            commits["sdk"] = sdkCommit;
        }
        VmRecord next = vm with { Commits = commits, LastUsedAt = context.Clock.GetUtcNow() };
        context.Store.Update<VmRecord>("vms", vm.Name, _ => next, AmbJson.Default.EnvelopeVmRecord);
        return next;
    }

    /// <summary>An image disk's identity: its size and modification time.</summary>
    /// <param name="imageDisk">The image's data.qcow2.</param>
    /// <returns>"&lt;bytes&gt;:&lt;ticks&gt;".</returns>
    internal static string ImageStamp(string imageDisk)
    {
        var info = new FileInfo(imageDisk);
        return string.Create(CultureInfo.InvariantCulture, $"{info.Length}:{info.LastWriteTimeUtc.Ticks}");
    }

    /// <summary>Moves a failed build's logs from the share into the store.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="lease">The slot.</param>
    /// <param name="requestId">The request.</param>
    /// <returns>Where they are (relative to the store's root).</returns>
    /// <exception cref="IOException">A copy failed.</exception>
    private static string KeepFailedLogs(JobContext context, SlotLease lease, string requestId)
    {
        string from = ShareFile(lease, $"out/{requestId}");
        string to = Path.Combine(context.Options.StoreDir, "requests", requestId);
        Directory.CreateDirectory(to);
        if (Directory.Exists(from))
        {
            foreach (string file in Directory.EnumerateFiles(from))
            {
                File.Copy(file, Path.Combine(to, Path.GetFileName(file)), overwrite: true);
            }
        }
        return $"requests/{requestId}/build.log";
    }

    /// <summary>Moves a build's artifacts from the share into the store and writes its record (taking the next build id
    /// by compare-and-swap, as an import does).</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="lease">The slot.</param>
    /// <param name="commit">The commit.</param>
    /// <param name="configuration">The configuration.</param>
    /// <param name="identity">The identity.</param>
    /// <param name="purge">The purge.</param>
    /// <param name="buildSeconds">The build step's time.</param>
    /// <param name="cancel">Cancels the copies.</param>
    /// <returns>The build's record.</returns>
    /// <exception cref="InvalidOperationException">The package step left no build.json or portable zip.</exception>
    /// <exception cref="IOException">A copy failed.</exception>
    /// <exception cref="RecordConflictException">Five other builds took the next ids meanwhile.</exception>
    private static async Task<BuildRecord> CollectAsync(JobContext context, SlotLease lease, string commit,
        string configuration, string identity, string purge, double buildSeconds, CancellationToken cancel)
    {
        string outDir = ShareFile(lease, $"out/{context.Request.Id}");
        string manifestFile = Path.Combine(outDir, "build.json");
        if (!File.Exists(manifestFile))
        {
            throw new InvalidOperationException("the package step left no build.json");
        }
        string? version = null;
        string? portable = null;
        using (JsonDocument facts = JsonDocument.Parse(await File.ReadAllBytesAsync(manifestFile, cancel).ConfigureAwait(false)))
        {
            if (facts.RootElement.TryGetProperty("version", out JsonElement v))
            {
                version = v.GetString();
            }
            if (facts.RootElement.TryGetProperty("portable", out JsonElement p))
            {
                portable = p.GetString();
            }
        }
        if (portable is null || !File.Exists(Path.Combine(outDir, portable)))
        {
            throw new InvalidOperationException("the package step left no portable zip");
        }
        for (int attempt = 0; ; attempt++)
        {
            DateTimeOffset now = context.Clock.GetUtcNow();
            string buildId = Ids.NextBuildId(DateOnly.FromDateTime(now.UtcDateTime), context.Store.List("builds"));
            string target = Path.Combine(context.Options.StoreDir, "builds", buildId);
            Directory.CreateDirectory(target);
            var artifacts = new List<ArtifactRef>();
            foreach (string file in Directory.EnumerateFiles(outDir).Order(StringComparer.Ordinal))
            {
                string name = Path.GetFileName(file);
                (string sha256, long bytes) = await ImportJob.CopyHashedAsync(file, Path.Combine(target, name), cancel)
                    .ConfigureAwait(false);
                artifacts.Add(new ArtifactRef(name, ArtifactKind(name, portable), bytes, sha256, $"builds/{buildId}/{name}"));
            }
            var record = new BuildRecord
            {
                Id = buildId,
                Source = BuildSource.Built,
                CreatedAt = now,
                RequestId = context.Request.Id,
                Commit = commit,
                Configuration = configuration,
                Identity = identity,
                Purge = purge,
                Version = version,
                BuildSeconds = Math.Round(buildSeconds, 1),
                Origin = context.Options.SourceRepo,
                Artifacts = artifacts,
            };
            try
            {
                context.Store.Write("builds", buildId, record, 0, AmbJson.Default.EnvelopeBuildRecord);
                return record;
            }
            catch (RecordConflictException) when (attempt < 5)
            {
                // Expected control flow: an import took this id between the listing and the write; the copies go and
                // the next id is tried.
                Directory.Delete(target, recursive: true);
            }
        }
    }

    /// <summary>What a file of a build is, by its name.</summary>
    /// <param name="name">The file's name.</param>
    /// <param name="portable">The portable zip's name (from build.json).</param>
    /// <returns>"portable-zip", "layout-zip", "symbols-zip", "build-log", "build-json" or "file".</returns>
    internal static string ArtifactKind(string name, string portable) => name switch
    {
        _ when name == portable => "portable-zip",
        _ when name.EndsWith("_layout.zip", StringComparison.Ordinal) => "layout-zip",
        _ when name.EndsWith("_symbols.zip", StringComparison.Ordinal) => "symbols-zip",
        "build.log" or "build.log.err" => "build-log",
        "build.json" => "build-json",
        _ => "file",
    };
}
