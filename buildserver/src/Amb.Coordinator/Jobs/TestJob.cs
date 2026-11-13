using System.Globalization;
using System.Text.RegularExpressions;
using Amb.Coordinator.Slots;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;

namespace Amb.Coordinator.Jobs;

/// <summary>
/// Runs a build's test artifact in a fresh VM (`amb test &lt;build id&gt; m5`, DESIGN.md §13): the engine's test harness
/// that the build compiled in its own tree (BuildJob's tests step: m5-tests.zip) is unpacked on the VM's C: and run there.
/// Nothing is compiled at test time, so a run tests exactly the bytes its build made, and costs one VM's boot plus the
/// harness's own time; the build VM is never touched (a test runs while the next build builds).
/// </summary>
/// <remarks>
/// <para><b>The run:</b> m5_tests.exe from the zip's folder, its output written to a file on C: that the guest agent
/// streams as progress events (its section lines, its failures, its summary), then copied into the store's request folder
/// (<c>requests/&lt;id&gt;/m5-output.txt</c>). Its profile is a fresh folder on the VM's C: (AGENTMASTER_PROFILE): the
/// harness's engine traces and window records never meet a real profile.</para>
/// <para><b>The outcome:</b> the harness's own summary ("12946 checks, 3 failures"); the request succeeds only when the
/// harness exits 0 (every check passed). A failing check is a result, never an error of the job: the step runs directly,
/// like the build step.</para>
/// <para><b>Older builds:</b> a build made before its build had a tests step (2026-11-13), or whose tests step did not
/// compile, has no "m5-tests" artifact: the request fails at once, with no slot started, and says which.</para>
/// </remarks>
public sealed class TestJob : VmJob
{
    /// <summary>The name of the m5 harness's zip in a build's artifacts (written by steps\build\tests.ps1).</summary>
    internal const string M5Zip = "m5-tests.zip";

    /// <summary>The artifact kind of a build's m5 harness (<see cref="BuildJob.ArtifactKind"/> of <see cref="M5Zip"/>).</summary>
    internal const string M5ArtifactKind = "m5-tests";

    /// <summary>The test kinds served: "m5", the engine's harness (src\cascadia\TerminalApp\AgentMaster\tests). The
    /// request book refuses any other (DESIGN.md §2 also names "syntax" and "installer", not served yet).</summary>
    internal static readonly string[] Kinds = ["m5"];

    /// <summary>The image a test VM boots when the request names none: the plain base image (the harness needs no
    /// Build Tools, Python or Claude Code, only Windows' own DLLs).</summary>
    internal const string DefaultImage = "am-base";

    /// <summary>Where the harness is unpacked in the guest (on the VM's own disk: a run from the share would load
    /// its exe over SMB).</summary>
    private const string GuestFolder = @"C:\amb\test\m5";

    /// <summary>The harness's output in the guest: written there by cmd.exe's redirection, tailed as progress, then put
    /// on the share.</summary>
    private const string GuestOutput = @"C:\amb\test\out\m5-output.txt";

    /// <summary>The harness's profile in the guest (AGENTMASTER_PROFILE): a folder nothing else uses.</summary>
    private const string GuestProfile = @"C:\amb\test\profile";

    /// <summary>The most "[FAIL]" lines of a run that become events (the whole output is kept in the store).</summary>
    private const int FailureEvents = 40;

    /// <inheritdoc/>
    public override async Task<JobOutcome> RunAsync(JobContext context, CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(context);
        RequestRecord request = context.Request;
        string buildId = request.Inputs.BuildId!;
        BuildRecord? build = context.Store.Read("builds", buildId, AmbJson.Default.EnvelopeBuildRecord)?.Value;
        if (build is null)
        {
            return new JobOutcome(false, $"no build {buildId} on this site");
        }
        ArtifactRef? zip = build.Artifacts.FirstOrDefault(a => a.Kind == M5ArtifactKind);
        if (zip is null)
        {
            return new JobOutcome(false, $"build {build.Id} has no m5 test artifact ({M5Zip}): it was built before builds compiled one (2026-11-13), was pulled without it, or its tests step did not compile (its m5-compile.log)",
                BuildId: build.Id);
        }
        SlotLease lease = await StartAndWaitAsync(context, request.Inputs.Image ?? DefaultImage, cancel).ConfigureAwait(false);
        bool graceful = false;
        try
        {
            // The coordinator's own copy into the share (fast, on the site's disk), checked against the build's record:
            // the bytes run are the bytes the build made.
            string zipInShare = ShareFile(lease, $"test/{zip.Name}");
            Directory.CreateDirectory(Path.GetDirectoryName(zipInShare)!);
            (string sha256, long bytes) = await ImportJob.CopyHashedAsync(Path.Combine(context.Options.StoreDir, zip.StorePath),
                zipInShare, cancel).ConfigureAwait(false);
            if (sha256 != zip.Sha256)
            {
                return new JobOutcome(false, $"build {build.Id}'s {zip.Name} changed in the store: {sha256} is not {zip.Sha256}",
                    BuildId: build.Id);
            }
            Directory.CreateDirectory(ShareFile(lease, "test/out"));
            context.Event("slot", FormattableString.Invariant(
                $"staged build {build.Id}'s {zip.Name} ({bytes / 1048576.0:0.0} MB, {build.Commit?[..9] ?? "no commit"})"));

            await RunAsync(context, lease, new GuestStep("expand", StepKinds.Expand,
                From: GuestPathOnShare($"test/{zip.Name}"), To: GuestFolder), cancel).ConfigureAwait(false);
            StepResult run = await RunHarnessAsync(context, lease, request, cancel).ConfigureAwait(false);

            context.MarkCollecting("tests ended: keeping their output");
            await RunAsync(context, lease, new GuestStep("keep-output", StepKinds.Put, From: GuestOutput,
                To: GuestPathOnShare("test/out/m5-output.txt"), TimeoutS: 300), cancel).ConfigureAwait(false);
            string outDir = Path.Combine(context.Options.StoreDir, "requests", request.Id);
            Directory.CreateDirectory(outDir);
            string output = Path.Combine(outDir, "m5-output.txt");
            File.Copy(ShareFile(lease, "test/out/m5-output.txt"), output, overwrite: true);
            string[] lines = await File.ReadAllLinesAsync(output, cancel).ConfigureAwait(false);
            foreach (string failure in lines.Where(l => l.Contains("[FAIL]", StringComparison.Ordinal)).Take(FailureEvents))
            {
                context.Event("output", "m5: " + failure.Trim());
            }
            (bool passed, string summary) = Summarize(lines, run.ExitCode, run.DurationSeconds);
            await RunAsync(context, lease, new GuestStep("shutdown", StepKinds.Shutdown), cancel).ConfigureAwait(false);
            graceful = true;
            return new JobOutcome(passed, $"{summary}; build {build.Id}", run.ExitCode, build.Id, $"requests/{request.Id}");
        }
        finally
        {
            // The slot's memory figures go into the request's events first (DESIGN.md §16.14).
            await StopSlotAsync(context, lease, graceful).ConfigureAwait(false);
        }
    }

    /// <summary>Runs m5_tests.exe with its output in a file the agent streams as progress.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="lease">The slot.</param>
    /// <param name="request">The request (its run timeout bounds the step).</param>
    /// <param name="cancel">Cancels the run.</param>
    /// <returns>The step's result: the harness's exit code (0 all passed, 1 a check failed), -1 when it was stopped.</returns>
    /// <exception cref="SlotLostException">The guest went silent.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    /// <remarks>cmd.exe does the redirection: the harness's whole output goes to the file (the agent's own capture keeps
    /// only a step's last 4 KB). Its exit code is the harness's (cmd /c returns its last command's). The step leaves
    /// 5 minutes of the run timeout for keeping the output and the shutdown.</remarks>
    private static async Task<StepResult> RunHarnessAsync(JobContext context, SlotLease lease, RequestRecord request,
        CancellationToken cancel)
    {
        int timeout = (int)Math.Max(60, request.RunTimeout.TotalSeconds - 300);
        var step = new GuestStep("m5", StepKinds.Run, Command: "cmd.exe",
            Args: ["/d", "/c", $@"{GuestFolder}\m5_tests.exe", ">", GuestOutput, "2>&1"],
            Cwd: GuestFolder, Env: new Dictionary<string, string>(StringComparer.Ordinal) { ["AGENTMASTER_PROFILE"] = GuestProfile },
            TimeoutS: timeout, ProgressFile: GuestOutput);
        // The output's folder must exist before cmd.exe opens the file (the profile's is the harness's own business);
        // "if not exist" keeps the step from failing on a folder that is there already.
        string outFolder = GuestOutput[..GuestOutput.LastIndexOf('\\')];
        await RunAsync(context, lease, new GuestStep("prepare", StepKinds.Run, Command: "cmd.exe",
            Args: ["/d", "/c", "if", "not", "exist", outFolder, "mkdir", outFolder], TimeoutS: 60), cancel).ConfigureAwait(false);
        context.Event("step", $"step m5: {GuestFolder}\\m5_tests.exe (AGENTMASTER_PROFILE {GuestProfile})");
        StepResult result = await lease.Session.RunStepAsync(step, SilenceOf(context), cancel).ConfigureAwait(false);
        context.Event("step", FormattableString.Invariant($"step m5: {result.Outcome} ({result.DurationSeconds:0.0} s)"));
        LogTails(context, result);
        return result;
    }

    /// <summary>The path a guest sees for a file in its slot's share: the bootstrap maps the share to Z:.</summary>
    /// <param name="relative">The path in the share, '/'-separated.</param>
    /// <returns>E.g. "Z:\test\m5-tests.zip".</returns>
    internal static string GuestPathOnShare(string relative) => @"Z:\" + relative.Replace('/', '\\');

    /// <summary>The harness's summary line: "12946 checks, 3 failures - FAILURES" (m5_tests.cpp's last line).</summary>
    private static readonly Regex SummaryLine = new(@"^(\d+) checks, (\d+) failures\b", RegexOptions.CultureInvariant);

    /// <summary>Turns the harness's output and exit code into the request's outcome.</summary>
    /// <param name="lines">The harness's output, line by line.</param>
    /// <param name="exitCode">Its exit code (0 every check passed; -1 the step was stopped).</param>
    /// <param name="seconds">The run's time.</param>
    /// <returns>Whether it passed (exit 0), and e.g. "m5: 12946 checks, 3 failures (exit 1), 99 s".</returns>
    /// <exception cref="ArgumentNullException"><paramref name="lines"/> is null.</exception>
    /// <remarks>The LAST summary line counts (the harness prints one, at its end). No summary line means the harness
    /// never got there (a crash, a stop): the outcome says so with the exit code, so a run that died early never reads as
    /// a pass of nothing.</remarks>
    internal static (bool Passed, string Summary) Summarize(IReadOnlyList<string> lines, int exitCode, double seconds)
    {
        ArgumentNullException.ThrowIfNull(lines);
        Match? last = lines.Select(l => SummaryLine.Match(l.Trim())).LastOrDefault(m => m.Success);
        string time = seconds.ToString("0", CultureInfo.InvariantCulture);
        if (last is null)
        {
            return (false, $"m5: no summary line (exit {exitCode}{(exitCode == -1 ? ": stopped" : ": the harness ended early")}), {time} s");
        }
        long checks = long.Parse(last.Groups[1].Value, CultureInfo.InvariantCulture);
        long failures = long.Parse(last.Groups[2].Value, CultureInfo.InvariantCulture);
        return (exitCode == 0 && failures == 0, string.Create(CultureInfo.InvariantCulture,
            $"m5: {checks} checks, {failures} failures (exit {exitCode}), {time} s"));
    }
}
