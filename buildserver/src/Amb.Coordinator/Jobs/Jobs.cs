using System.Globalization;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using Amb.Coordinator.Slots;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;

namespace Amb.Coordinator.Jobs;

/// <summary>
/// Registers an artifact the service did not build as a build id (DESIGN.md §13 P1: "a hand-built zip or a GitHub
/// release as a build id, so e2e works before builds exist"). No slot: the coordinator copies the file into the store,
/// hashing it on the way, reads its version, and writes the build's manifest.
/// </summary>
public sealed class ImportJob : IJob
{
    /// <inheritdoc/>
    public async Task<JobOutcome> RunAsync(JobContext context, CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(context);
        string source = JobContext.LocalPath(context.Request.Inputs.Source!);
        if (!File.Exists(source))
        {
            return new JobOutcome(false, $"no such file: {context.Request.Inputs.Source}");
        }
        context.MarkRunning($"importing {context.Request.Inputs.Source}");
        string name = Path.GetFileName(source);
        if (!Ids.IsSafe(Path.GetFileNameWithoutExtension(name).Replace('.', '-')))
        {
            return new JobOutcome(false, $"the file's name is not safe for the store: {name}");
        }
        // A build id is taken by writing its record first (compare-and-swap at version 0): two imports at once never
        // share one, the second simply takes the next number.
        for (int attempt = 0; ; attempt++)
        {
            DateTimeOffset now = context.Clock.GetUtcNow();
            string buildId = Ids.NextBuildId(DateOnly.FromDateTime(now.UtcDateTime), context.Store.List("builds"));
            string relative = $"builds/{buildId}/{name}";
            string target = Path.Combine(context.Options.StoreDir, "builds", buildId, name);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            (string sha256, long bytes) = await CopyHashedAsync(source, target, cancel).ConfigureAwait(false);
            string? version = ReadPortableVersion(target);
            var build = new BuildRecord
            {
                Id = buildId,
                Source = BuildSource.Imported,
                CreatedAt = now,
                RequestId = context.Request.Id,
                Commit = context.Request.Inputs.Commit,
                Version = version,
                Origin = context.Request.Inputs.Source,
                Artifacts = [new ArtifactRef(name, "portable-zip", bytes, sha256, relative)],
            };
            try
            {
                context.Store.Write("builds", buildId, build, 0, AmbJson.Default.EnvelopeBuildRecord);
            }
            catch (RecordConflictException) when (attempt < 5)
            {
                // Expected control flow: another import took this id between the listing and the write.
                File.Delete(target);
                continue;
            }
            return new JobOutcome(true, FormattableString.Invariant(
                $"imported as {buildId}: {name}, {bytes / 1048576.0:0.0} MB, version {version ?? "unknown"}, sha256 {sha256[..12]}"),
                BuildId: buildId);
        }
    }

    /// <summary>Copies a file, hashing it on the way.</summary>
    /// <param name="source">The file.</param>
    /// <param name="target">Where it goes (through a temporary name).</param>
    /// <param name="cancel">Cancels the copy.</param>
    /// <returns>Its SHA-256 and size.</returns>
    /// <exception cref="IOException">The copy failed.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    internal static async Task<(string Sha256, long Bytes)> CopyHashedAsync(string source, string target,
        CancellationToken cancel)
    {
        string tmp = target + ".part";
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        long bytes = 0;
        await using (var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, true))
        await using (var output = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20, true))
        {
            byte[] buffer = new byte[1 << 20];
            int read;
            while ((read = await input.ReadAsync(buffer, cancel).ConfigureAwait(false)) > 0)
            {
                sha.AppendData(buffer, 0, read);
                await output.WriteAsync(buffer.AsMemory(0, read), cancel).ConfigureAwait(false);
                bytes += read;
            }
        }
        File.Move(tmp, target, overwrite: true);
        return (Convert.ToHexStringLower(sha.GetHashAndReset()), bytes);
    }

    /// <summary>Reads a portable zip's <c>.am-version</c> stamp (at its root or in its one top folder).</summary>
    /// <param name="zip">The zip.</param>
    /// <returns>The version, or null when the zip has none or is not a zip.</returns>
    internal static string? ReadPortableVersion(string zip)
    {
        try
        {
            using ZipArchive archive = ZipFile.OpenRead(zip);
            ZipArchiveEntry? entry = archive.Entries.FirstOrDefault(e =>
                e.FullName == ".am-version" || e.FullName.EndsWith("/.am-version", StringComparison.Ordinal));
            if (entry is null)
            {
                return null;
            }
            using var reader = new StreamReader(entry.Open());
            string text = reader.ReadToEnd().Trim();
            return text.Length is > 0 and <= 64 ? text : null;
        }
        catch (InvalidDataException)
        {
            // Not a zip: the import still registers the file, without a version.
            return null;
        }
    }
}

/// <summary>
/// The slot chain's check (DESIGN.md §13 P0.2): boot one VM of an image, run one command through its guest agent (or
/// the request's own steps), shut it down. Its outcome carries the boot-to-ready time and the command's output.
/// </summary>
public sealed class SmokeJob : VmJob
{
    /// <inheritdoc/>
    public override async Task<JobOutcome> RunAsync(JobContext context, CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(context);
        string image = context.Request.Inputs.Image ?? "am-base";
        DateTimeOffset started = context.Clock.GetUtcNow();
        SlotLease lease = await StartAndWaitAsync(context, image, cancel).ConfigureAwait(false);
        bool graceful = false;
        try
        {
            double ready = (context.Clock.GetUtcNow() - started).TotalSeconds;
            IReadOnlyList<GuestStep> steps = context.Request.Inputs.Steps
                ?? [new GuestStep("1", StepKinds.Run, Command: "cmd.exe", Args: ["/c", "ver"], TimeoutS: 60)];
            string last = "";
            foreach (GuestStep step in steps)
            {
                StepResult result = await RunAsync(context, lease, step, cancel).ConfigureAwait(false);
                last = (result.StdoutTail ?? "").Trim();
            }
            context.MarkCollecting("shutting the guest down");
            await RunAsync(context, lease, new GuestStep("shutdown", StepKinds.Shutdown), cancel).ConfigureAwait(false);
            graceful = true;
            return new JobOutcome(true, FormattableString.Invariant($"ready in {ready:0} s; {last.ReplaceLineEndings(" ")}"));
        }
        finally
        {
            await context.Slots.StopAsync(lease, graceful).ConfigureAwait(false);
        }
    }
}

/// <summary>
/// Makes a base image (DESIGN.md §4.3): dockur's unattended install of a Tiny11 build, with an OEM folder that
/// registers the guest agent's bootstrap at logon. The install has ended when the new guest's agent says hello; the
/// guest then shuts down and its folder becomes the image.
/// </summary>
public sealed class InstallJob : VmJob
{
    /// <summary>The ISO download (3.3 GB in 4 min) plus the install (12.5 min) plus the first logon, measured
    /// 2026-10-25 at 16.5 min in all; twice that for a slow network or a loaded PC.</summary>
    protected override TimeSpan HelloTimeout => TimeSpan.FromMinutes(45);

    /// <inheritdoc/>
    public override async Task<JobOutcome> RunAsync(JobContext context, CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(context);
        RequestInputs inputs = context.Request.Inputs;
        string image = inputs.NewImage!;
        if (Directory.Exists(Path.Combine(context.Options.ImagesDir, image)))
        {
            return new JobOutcome(false, $"image {image} exists already");
        }
        string oem = JobContext.LocalPath(inputs.OemDir!);
        DateTimeOffset started = context.Clock.GetUtcNow();
        SlotLease lease = await StartAndWaitAsync(context, image, cancel, inputs.Version, oem).ConfigureAwait(false);
        bool committed = false;
        try
        {
            foreach (GuestStep step in inputs.Steps ?? [])
            {
                await RunAsync(context, lease, step, cancel).ConfigureAwait(false);
            }
            context.MarkCollecting("installed: shutting down to keep it as an image");
            await RunAsync(context, lease, new GuestStep("shutdown", StepKinds.Shutdown), cancel).ConfigureAwait(false);
            await context.Slots.StopAsync(lease, graceful: true, commitAs: image).ConfigureAwait(false);
            committed = true;
            double minutes = (context.Clock.GetUtcNow() - started).TotalMinutes;
            return new JobOutcome(true, FormattableString.Invariant(
                $"image {image} made from {inputs.Version} in {minutes:0.0} min ({ImageSize(context, image)})"));
        }
        finally
        {
            if (!committed)
            {
                await context.Slots.StopAsync(lease, graceful: false).ConfigureAwait(false);
            }
        }
    }

    /// <summary>The size of an image's files, for the outcome.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="image">The image.</param>
    /// <returns>E.g. "9.2 GB on disk".</returns>
    internal static string ImageSize(JobContext context, string image)
    {
        string dir = Path.Combine(context.Options.ImagesDir, image);
        long bytes = Directory.Exists(dir) ? Directory.EnumerateFiles(dir).Sum(f => new FileInfo(f).Length) : 0;
        return FormattableString.Invariant($"{bytes / 1e9:0.0} GB in files");
    }
}

/// <summary>
/// Makes a layer image (DESIGN.md §4.3, D2): boots an image through a new overlay, runs the request's steps, shuts the
/// guest down, and keeps the overlay as a new read-only image whose backing file is the first image.
/// </summary>
public sealed class LayerJob : VmJob
{
    /// <inheritdoc/>
    public override async Task<JobOutcome> RunAsync(JobContext context, CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(context);
        RequestInputs inputs = context.Request.Inputs;
        string image = inputs.NewImage!;
        if (Directory.Exists(Path.Combine(context.Options.ImagesDir, image)))
        {
            return new JobOutcome(false, $"image {image} exists already");
        }
        DateTimeOffset started = context.Clock.GetUtcNow();
        SlotLease lease = await StartAndWaitAsync(context, inputs.Image!, cancel).ConfigureAwait(false);
        bool committed = false;
        try
        {
            foreach (GuestStep step in inputs.Steps!)
            {
                await RunAsync(context, lease, step, cancel).ConfigureAwait(false);
            }
            context.MarkCollecting($"steps done: shutting down to keep the overlay as {image}");
            await RunAsync(context, lease, new GuestStep("shutdown", StepKinds.Shutdown), cancel).ConfigureAwait(false);
            await context.Slots.StopAsync(lease, graceful: true, commitAs: image).ConfigureAwait(false);
            committed = true;
            double minutes = (context.Clock.GetUtcNow() - started).TotalMinutes;
            return new JobOutcome(true, FormattableString.Invariant(
                $"image {image} made over {inputs.Image} in {minutes:0.0} min ({InstallJob.ImageSize(context, image)})"));
        }
        finally
        {
            if (!committed)
            {
                await context.Slots.StopAsync(lease, graceful: false).ConfigureAwait(false);
            }
        }
    }
}

/// <summary>
/// Runs e2e suites on a build's portable zip in a fresh VM (DESIGN.md §13 P1). The guest half of the e2e engine runs
/// unchanged: the bootstrap maps the slot's share to Z:, so <c>Z:\e2e\src</c>, <c>Z:\e2e\out</c> and
/// <c>Z:\e2e\builds</c> are where e2e/guest/run_suite.py and provision.ps1 expect them.
/// </summary>
public sealed class E2eJob : VmJob
{
    /// <inheritdoc/>
    public override async Task<JobOutcome> RunAsync(JobContext context, CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(context);
        RequestRecord request = context.Request;
        BuildRecord? build = context.Store.Read("builds", request.Inputs.BuildId!, AmbJson.Default.EnvelopeBuildRecord)?.Value;
        ArtifactRef? zip = build?.Artifacts.FirstOrDefault(a => a.Kind == "portable-zip");
        if (build is null || zip is null)
        {
            return new JobOutcome(false, $"build {request.Inputs.BuildId} has no portable zip");
        }
        string suite = JobContext.LocalPath(request.Inputs.SuiteSource ?? context.Options.E2eSourceDir);
        if (!File.Exists(Path.Combine(suite, "guest", "run_suite.py")))
        {
            return new JobOutcome(false, $"no e2e suite at {suite} (guest/run_suite.py is missing)");
        }
        string run = request.Id;
        SlotLease lease = await StartAndWaitAsync(context, request.Inputs.Image ?? "am-e2e", cancel).ConfigureAwait(false);
        bool graceful = false;
        string results = $"requests/{request.Id}";
        try
        {
            // The build and the suite go into the share by the coordinator's own copies (fast, on the site's disk), then
            // the guest reads them over SMB by their exact names.
            string zipInShare = ShareFile(lease, $"e2e/builds/{zip.Name}");
            Directory.CreateDirectory(Path.GetDirectoryName(zipInShare)!);
            (string sha256, _) = await ImportJob.CopyHashedAsync(Path.Combine(context.Options.StoreDir, zip.StorePath),
                zipInShare, cancel).ConfigureAwait(false);
            if (sha256 != zip.Sha256)
            {
                return new JobOutcome(false, $"build {build.Id}'s zip changed in the store: {sha256} is not {zip.Sha256}",
                    BuildId: build.Id);
            }
            CopyTree(suite, ShareFile(lease, "e2e/src"));
            Directory.CreateDirectory(ShareFile(lease, "e2e/out"));
            context.Event("slot", $"staged build {build.Id} ({zip.Name}) and the suite from {suite}");

            // The am-e2e layer ran provision.ps1 once with its cache on C: (Claude Code, Git, PowerShell 7, the venv's
            // wheels): with the same cache here every step but the build under test finds itself in place and skips.
            await RunAsync(context, lease, new GuestStep("provision", StepKinds.Run, Command: "powershell.exe",
                Args: ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", @"Z:\e2e\src\guest\provision.ps1",
                    "-AgentmasterZip", $@"Z:\e2e\builds\{zip.Name}", "-Pwsh", "-Cache", @"C:\e2e\cache"],
                TimeoutS: 1800), cancel).ConfigureAwait(false);

            List<string> args = [@"Z:\e2e\src\guest\run_suite.py", run, .. request.Inputs.SuiteArgs ?? ["-v"]];
            int timeout = (int)Math.Max(60, request.RunTimeout.TotalSeconds - 300);
            StepResult suiteRun = await RunSuiteAsync(context, lease, args, run, timeout, cancel).ConfigureAwait(false);

            context.MarkCollecting("suite ended: keeping its results");
            string outDir = Path.Combine(context.Options.StoreDir, "requests", request.Id);
            Directory.CreateDirectory(outDir);
            foreach (string file in Directory.EnumerateFiles(ShareFile(lease, "e2e/out"), run + ".*"))
            {
                File.Copy(file, Path.Combine(outDir, Path.GetFileName(file)), overwrite: true);
            }
            (bool passed, string summary) = Summarize(Path.Combine(outDir, run + ".status.json"), suiteRun.ExitCode);
            await RunAsync(context, lease, new GuestStep("shutdown", StepKinds.Shutdown), cancel).ConfigureAwait(false);
            graceful = true;
            return new JobOutcome(passed, summary, suiteRun.ExitCode, build.Id, results);
        }
        finally
        {
            await context.Slots.StopAsync(lease, graceful).ConfigureAwait(false);
        }
    }

    /// <summary>Runs run_suite.py with its progress file streamed as events; a failing suite is a result, not an error.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="lease">The slot.</param>
    /// <param name="args">run_suite.py's arguments.</param>
    /// <param name="run">The run's name.</param>
    /// <param name="timeoutS">The step's timeout.</param>
    /// <param name="cancel">Cancels the run.</param>
    /// <returns>The step's result (pytest's exit code).</returns>
    /// <exception cref="SlotLostException">The guest went silent.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    private static async Task<StepResult> RunSuiteAsync(JobContext context, SlotLease lease, List<string> args, string run,
        int timeoutS, CancellationToken cancel)
    {
        var step = new GuestStep("suite", StepKinds.Run, Command: @"C:\e2e\venv\Scripts\python.exe", Args: args,
            Cwd: @"C:\e2e", TimeoutS: timeoutS, ProgressFile: $@"Z:\e2e\out\{run}.progress.jsonl");
        context.Event("step", $"step suite: {step.Command} {string.Join(' ', args)}");
        StepResult result = await lease.Session.RunStepAsync(step, Silence, cancel).ConfigureAwait(false);
        context.Event("step", FormattableString.Invariant($"step suite: {result.Outcome} ({result.DurationSeconds / 60:0.0} min)"));
        return result;
    }

    /// <summary>Turns run_suite.py's status file into the request's outcome.</summary>
    /// <param name="statusFile">The status file in the store.</param>
    /// <param name="exitCode">pytest's exit code.</param>
    /// <returns>Whether every test passed, and a one-line summary.</returns>
    internal static (bool Passed, string Summary) Summarize(string statusFile, int exitCode)
    {
        try
        {
            using JsonDocument status = JsonDocument.Parse(File.ReadAllBytes(statusFile));
            JsonElement root = status.RootElement;
            int Get(string name) => root.TryGetProperty(name, out JsonElement value) && value.TryGetInt32(out int n) ? n : 0;
            int tests = Get("tests"), failures = Get("failures"), errors = Get("errors"), skipped = Get("skipped");
            int passed = tests - failures - errors - skipped;
            return (exitCode == 0, string.Create(CultureInfo.InvariantCulture,
                $"{tests} tests: {passed} passed, {failures} failed, {errors} errors, {skipped} skipped (exit {exitCode})"));
        }
        catch (Exception ex) when (ex is IOException or JsonException)
        {
            return (false, $"exit {exitCode}; no status file ({ex.Message})");
        }
    }
}
