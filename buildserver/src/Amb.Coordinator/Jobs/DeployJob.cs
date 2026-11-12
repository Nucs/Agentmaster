using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using Amb.Coordinator.Sources;
using Amb.Core.Json;
using Amb.Core.Model;

namespace Amb.Coordinator.Jobs;

/// <summary>
/// Installs a build on one of this PC's Agentmaster instances, dev or production (<see cref="RequestKind.Deploy"/>,
/// `amb install`; DESIGN.md §2's <c>install</c>, §13 P4): what session 14fdd99b did by hand for dev 0.0.1.36-0.0.1.39
/// and production 0.8.0.1, as one request.
/// </summary>
/// <remarks>
/// <para><b>The flow:</b> the build must be the instance's identity (dev builds for dev, release-identity builds for
/// production) and have a layout zip. The coordinator stages, in a folder of its own on Q:, the zip (copied from the store
/// and hashed on the way), the Windows-side script (buildserver/steps/install/install-build.ps1, deployed with the
/// coordinator) and the COMMITTED tools\Swap-AgentmasterLayout.ps1 and tools\am-lock.sh of
/// <see cref="CoordinatorOptions.InstallScriptsRef"/>; then it runs the script with Windows' PowerShell 7 through WSL's
/// interop and streams its lines as the request's progress. The script does the rest (stage into the free slot, check,
/// wait for the requesting session, the build-launch mutex, the swap, the watch) and ends with an "INSTALL {json}" line
/// and a result file.</para>
/// <para><b>The same build twice is a no-op:</b> when the instance's running TerminalApp.dll is byte-identical to the
/// build's, the script stops before staging anything and reports success with nothing stopped, unless the request asks
/// for a <see cref="RequestInputs.Reinstall"/>. A swap onto the build that already runs only restarts the instance and
/// cuts off every running turn; on 2026-11-04 a session that re-ran its install after each restart did that 6 times.</para>
/// <para><b>Outside every instance:</b> a Windows program started through the distro's interop descends from
/// wslhost.exe and wslservice.exe (measured 2026-10-29), never from an Agentmaster GUI, so the swap's self-hosting guard
/// passes and the stop cannot kill the swap, even when the session that asked lives in the instance being swapped. It
/// runs in the user's session, so the swap's launch reaches the desktop.</para>
/// <para><b>A cancel never interrupts a swap:</b> the job writes the stage's cancel file, which the script reads at every
/// point before the swap (it then puts the slot back and stops); once the script printed <see cref="SwapStartMarker"/>,
/// the job waits for it to finish. Killing it there could leave the instance stopped and unregistered.</para>
/// </remarks>
public sealed class DeployJob : IJob
{
    /// <summary>The line the Windows side prints just before it stops anything (the swap's start).</summary>
    internal const string SwapStartMarker = "SWAP START:";

    /// <summary>The prefix of the Windows side's last line, its result as JSON.</summary>
    internal const string ResultPrefix = "INSTALL ";

    /// <summary>How long a cancelled install may take to stop by itself: the build lock's wait (10 min) is the longest
    /// stretch in which the script does not read its cancel file, then the swap and its rollback.</summary>
    private static readonly TimeSpan CancelGrace = TimeSpan.FromMinutes(15);

    /// <summary>Serializes the event writes of the two output readers (the request book appends lines to one file).</summary>
    private readonly Lock _events = new();

    /// <inheritdoc/>
    public async Task<JobOutcome> RunAsync(JobContext context, CancellationToken cancel)
    {
        ArgumentNullException.ThrowIfNull(context);
        RequestInputs inputs = context.Request.Inputs;
        string buildId = inputs.BuildId!;
        if (string.IsNullOrEmpty(context.Options.WindowsPwsh))
        {
            // A site with no Windows beside it (the worker machine's): nothing here can reach an instance. `amb install`
            // sends an install to homepc's site, which pulls the build from here first.
            return new JobOutcome(false,
                $"site {context.Options.SiteName} cannot install builds (no Windows here): install through the home site, `amb install {buildId} --instance …`");
        }
        BuildRecord? build = context.Store.Read("builds", buildId, AmbJson.Default.EnvelopeBuildRecord)?.Value;
        if (build is null)
        {
            return new JobOutcome(false, $"no build {buildId}");
        }
        InstanceTarget target = TargetOf(inputs.Instance!, context.Options);
        string? refusal = Refusal(build, target);
        if (refusal is not null)
        {
            return new JobOutcome(false, refusal, BuildId: buildId);
        }
        ArtifactRef layout = build.Artifacts.First(a => a.Kind == "layout-zip");
        context.MarkRunning($"installing {buildId} ({build.Commit![..9]}, {build.Configuration}) on {inputs.Instance} ({target.Package})");

        string stageWindows = context.Options.InstallStage.TrimEnd('\\') + "\\" + context.Request.Id;
        string stage = JobContext.LocalPath(stageWindows);
        Directory.CreateDirectory(stage);
        var lines = new List<string>();
        try
        {
            // The scripts: the Windows side from the coordinator's own deployed copy; the swap and the lock as committed on
            // the branch (never a working tree's half-done edit, never an old build's copy without today's fixes).
            var mirror = new SourceMirror(context.Options, context.Log);
            await mirror.RefreshAsync(cancel).ConfigureAwait(false);
            string scriptsCommit = await mirror.ExportFileAsync(context.Options.InstallScriptsRef, "tools/Swap-AgentmasterLayout.ps1",
                Path.Combine(stage, "Swap-AgentmasterLayout.ps1"), cancel).ConfigureAwait(false);
            await mirror.ExportFileAsync(context.Options.InstallScriptsRef, "tools/am-lock.sh", Path.Combine(stage, "am-lock.sh"),
                cancel).ConfigureAwait(false);
            File.Copy(Path.Combine(context.Options.InstallStepsDir, "install-build.ps1"), Path.Combine(stage, "install-build.ps1"),
                overwrite: true);
            context.Event("source", $"the swap and lock scripts of {context.Options.InstallScriptsRef} ({scriptsCommit[..9]})");
            (string sha256, long bytes) = await ImportJob.CopyHashedAsync(Path.Combine(context.Options.StoreDir, layout.StorePath),
                Path.Combine(stage, layout.Name), cancel).ConfigureAwait(false);
            if (sha256 != layout.Sha256)
            {
                return new JobOutcome(false, $"the store's {layout.Name} hashes to {sha256[..12]}, not its manifest's {layout.Sha256[..12]}",
                    BuildId: buildId);
            }
            context.Event("source", FormattableString.Invariant($"staged {layout.Name} ({bytes / 1048576.0:0.0} MB) in {stageWindows}"));

            IReadOnlyList<string> args = ScriptArguments(inputs, target, build, layout, stageWindows, context.Request.Id);
            (int exitCode, bool cancelled) = await RunScriptAsync(context, stage, args, lines, cancel).ConfigureAwait(false);
            string resultsPath = KeepLog(context, stage, lines);
            string? json = ReadResultJson(Path.Combine(stage, "install.json"), lines);
            (bool ok, string message) = Summarize(json, exitCode);
            if (cancelled && !ok)
            {
                // The script stopped at a safe point (or was given up on): the scheduler records the cancel or timeout.
                cancel.ThrowIfCancellationRequested();
            }
            return new JobOutcome(ok, cancelled ? message + " (a cancel came during the swap, which is never interrupted)" : message,
                exitCode, buildId, resultsPath);
        }
        finally
        {
            // Cleanup in every outcome: the staged zip is 120-210 MB on Q:, and the log was kept in the store.
            try
            {
                Directory.Delete(stage, recursive: true);
            }
            catch (IOException ex)
            {
                context.Log.LogWarning("could not remove {Stage}: {Error}", stage, ex.Message);
            }
            catch (UnauthorizedAccessException ex)
            {
                context.Log.LogWarning("could not remove {Stage}: {Error}", stage, ex.Message);
            }
        }
    }

    /// <summary>What an instance is on this PC.</summary>
    /// <param name="Package">Its package (the swap's -PackageName).</param>
    /// <param name="CliInstance">Its name for agentmaster-cli's --instance.</param>
    /// <param name="Identity">The build identity it runs ("dev" or "release").</param>
    /// <param name="Slots">Its two layout folders.</param>
    internal sealed record InstanceTarget(string Package, string CliInstance, string Identity, IReadOnlyList<string> Slots);

    /// <summary>The package, CLI name, build identity and layout folders of an instance.</summary>
    /// <param name="instance">"dev" or "production" (as the request book normalized it).</param>
    /// <param name="options">The settings (the folders).</param>
    /// <returns>The instance's target.</returns>
    /// <exception cref="ArgumentException">Another instance name (the book refuses those already).</exception>
    internal static InstanceTarget TargetOf(string instance, CoordinatorOptions options) => instance switch
    {
        "dev" => new InstanceTarget("AgentmasterDev", "dev", "dev", options.DevSlots),
        "production" => new InstanceTarget("Agentmaster", "release", "release", options.ProductionSlots),
        _ => throw new ArgumentException($"unknown instance {instance}", nameof(instance)),
    };

    /// <summary>Why a build cannot be installed on an instance, or null when it can.</summary>
    /// <param name="build">The build.</param>
    /// <param name="target">The instance.</param>
    /// <returns>The refusal's text, or null.</returns>
    /// <remarks>The swap refuses a layout of the other identity too, but only after the layout was staged; refusing here
    /// costs nothing and says which build to make.</remarks>
    internal static string? Refusal(BuildRecord build, InstanceTarget target)
    {
        if (!build.Artifacts.Any(a => a.Kind == "layout-zip"))
        {
            return $"build {build.Id} has no layout zip (an import holds only a portable zip): build the commit with `amb build`";
        }
        if (build.Commit is not { Length: 40 })
        {
            return $"build {build.Id} names no commit, so its layout cannot be checked against one";
        }
        if (build.Identity != target.Identity)
        {
            return $"build {build.Id} is a {build.Identity ?? "unknown"}-identity build; {target.Package} needs `amb build --identity {target.Identity}`";
        }
        return null;
    }

    /// <summary>The Windows-side script's arguments (after pwsh's own): -DryRun, -Reinstall and the injected failure only
    /// when the request asks for them.</summary>
    /// <param name="inputs">The request's inputs.</param>
    /// <param name="target">The instance.</param>
    /// <param name="build">The build.</param>
    /// <param name="layout">The build's layout zip.</param>
    /// <param name="stageWindows">The stage folder, as a Windows path.</param>
    /// <param name="requestId">The request.</param>
    /// <returns>pwsh's arguments, the script's included.</returns>
    internal static IReadOnlyList<string> ScriptArguments(RequestInputs inputs, InstanceTarget target, BuildRecord build,
        ArtifactRef layout, string stageWindows, string requestId)
    {
        var args = new List<string>
        {
            "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", stageWindows + "\\install-build.ps1",
            "-Instance", inputs.Instance!, "-Package", target.Package, "-CliInstance", target.CliInstance,
            "-Slots", string.Join(';', target.Slots),
            "-Zip", stageWindows + "\\" + layout.Name, "-Sha256", layout.Sha256,
            "-BuildId", build.Id, "-Commit", build.Commit!,
            "-SwapScript", stageWindows + "\\Swap-AgentmasterLayout.ps1", "-LockScript", stageWindows + "\\am-lock.sh",
            "-RequestId", requestId,
            "-RequestedByPid", (inputs.RequestedByPid ?? 0).ToString(CultureInfo.InvariantCulture),
            "-WaitOthersMinutes", (inputs.WaitOthersMinutes ?? 0).ToString(CultureInfo.InvariantCulture),
            "-Result", stageWindows + "\\install.json", "-CancelFile", stageWindows + "\\cancel",
        };
        if (inputs.DryRun == true)
        {
            args.Add("-DryRun");
        }
        if (inputs.Reinstall == true)
        {
            // Only when asked: without it the Windows side leaves an instance that already runs this build alone.
            args.Add("-Reinstall");
        }
        if (inputs.InjectFailure == "after-stop")
        {
            args.Add("-InjectFailureAfterStop");
        }
        return args;
    }

    /// <summary>Runs the Windows side and streams its lines as events, honouring a cancel without ever interrupting a
    /// swap.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="stage">The stage folder (local path): the process's working folder, and where the cancel file goes.</param>
    /// <param name="args">pwsh's arguments.</param>
    /// <param name="lines">Receives every line (standard output and error) for the kept log.</param>
    /// <param name="cancel">The job's cancel (a cancel request or the run timeout).</param>
    /// <returns>The script's exit code, and whether a cancel came while it ran.</returns>
    /// <exception cref="InvalidOperationException">pwsh could not be started.</exception>
    /// <exception cref="System.ComponentModel.Win32Exception">pwsh was not found (WSL's interop is off?).</exception>
    private async Task<(int ExitCode, bool Cancelled)> RunScriptAsync(JobContext context, string stage, IReadOnlyList<string> args,
        List<string> lines, CancellationToken cancel)
    {
        var info = new ProcessStartInfo(context.Options.WindowsPwsh)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            // A drive-letter folder: interop gives the Windows process this folder as Q:\..., not a \\wsl.localhost path.
            WorkingDirectory = stage,
        };
        foreach (string arg in args)
        {
            info.ArgumentList.Add(arg);
        }
        if (OperatingSystem.IsLinux() && File.Exists("/run/WSL/1_interop"))
        {
            // The distro init's own interop socket: the coordinator outlives the wsl.exe call that started it (see
            // HostCapacity, measured 2026-10-25).
            info.Environment["WSL_INTEROP"] = "/run/WSL/1_interop";
        }
        using Process process = Process.Start(info) ?? throw new InvalidOperationException($"{context.Options.WindowsPwsh} did not start");
        bool swapStarted = false;
        Task readOut = PumpAsync(process.StandardOutput, "progress");
        Task readErr = PumpAsync(process.StandardError, "stderr");
        bool cancelled = false;
        try
        {
            await process.WaitForExitAsync(cancel).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            cancelled = true;
            await File.WriteAllTextAsync(Path.Combine(stage, "cancel"), "cancel", CancellationToken.None).ConfigureAwait(false);
            Event(context, "state", Volatile.Read(ref swapStarted)
                ? "cancel asked during the swap: waiting for it to finish (a swap is never interrupted)"
                : "cancel asked: the install stops at its next safe point and puts the slot back");
            using var grace = new CancellationTokenSource(CancelGrace);
            try
            {
                await process.WaitForExitAsync(grace.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                // Given up on after the grace: only before the swap is a kill safe (the swap itself may still run on in
                // Windows; the next request finds the instance as it left it, and the lock's TTL frees the lock).
                if (!Volatile.Read(ref swapStarted))
                {
                    try
                    {
                        process.Kill(entireProcessTree: true);
                    }
                    catch (InvalidOperationException)
                    {
                        // Expected control flow: it ended between the wait and the kill.
                    }
                }
                Event(context, "state", $"the install did not stop within {CancelGrace.TotalMinutes:0} min of the cancel");
            }
        }
        await Task.WhenAll(readOut, readErr).WaitAsync(TimeSpan.FromSeconds(30), CancellationToken.None).ConfigureAwait(false);
        return (process.HasExited ? process.ExitCode : -1, cancelled);

        // Reads one stream line by line into events (the result line stays out of them: it is the outcome).
        async Task PumpAsync(StreamReader reader, string kind)
        {
            while (await reader.ReadLineAsync(CancellationToken.None).ConfigureAwait(false) is { } line)
            {
                lock (lines)
                {
                    lines.Add(line);
                }
                if (line.Contains(SwapStartMarker, StringComparison.Ordinal))
                {
                    Volatile.Write(ref swapStarted, true);
                }
                if (!line.StartsWith(ResultPrefix, StringComparison.Ordinal))
                {
                    Event(context, kind, line);
                }
            }
        }
    }

    /// <summary>Appends an event, one writer at a time.</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="kind">The event's kind.</param>
    /// <param name="message">Its line.</param>
    private void Event(JobContext context, string kind, string message)
    {
        lock (_events)
        {
            context.Event(kind, message);
        }
    }

    /// <summary>Keeps the run's lines and result in the store (requests/&lt;id&gt;/install.log, install.json).</summary>
    /// <param name="context">The job's context.</param>
    /// <param name="stage">The stage folder (its install.json is kept too).</param>
    /// <param name="lines">Every line the script printed.</param>
    /// <returns>The results' folder, relative to the store's root.</returns>
    /// <exception cref="IOException">The log could not be written.</exception>
    private static string KeepLog(JobContext context, string stage, List<string> lines)
    {
        string folder = Path.Combine(context.Options.StoreDir, "requests", context.Request.Id);
        Directory.CreateDirectory(folder);
        lock (lines)
        {
            File.WriteAllLines(Path.Combine(folder, "install.log"), lines);
        }
        string result = Path.Combine(stage, "install.json");
        if (File.Exists(result))
        {
            File.Copy(result, Path.Combine(folder, "install.json"), overwrite: true);
        }
        return $"requests/{context.Request.Id}";
    }

    /// <summary>The script's result: its result file, else its last "INSTALL {json}" line.</summary>
    /// <param name="file">The result file.</param>
    /// <param name="lines">The lines it printed.</param>
    /// <returns>The JSON, or null when it wrote neither (it died early).</returns>
    internal static string? ReadResultJson(string file, List<string> lines)
    {
        if (File.Exists(file))
        {
            string text = File.ReadAllText(file).Trim();
            if (text.Length > 0)
            {
                return text;
            }
        }
        lock (lines)
        {
            return lines.LastOrDefault(l => l.StartsWith(ResultPrefix, StringComparison.Ordinal))?[ResultPrefix.Length..];
        }
    }

    /// <summary>The request's outcome from the script's result.</summary>
    /// <param name="json">The result JSON, or null.</param>
    /// <param name="exitCode">The script's exit code.</param>
    /// <returns>Whether it succeeded, and the outcome's line.</returns>
    /// <remarks>Exit 2 means the old version came back after a failed swap ("rolled back"), 3 that someone must look at
    /// the instance now: both are said first, so `amb list` shows them at a glance.</remarks>
    internal static (bool Ok, string Message) Summarize(string? json, int exitCode)
    {
        string message = $"the install script exited {exitCode} without a result";
        bool ok = false;
        if (json is not null)
        {
            try
            {
                using JsonDocument doc = JsonDocument.Parse(json);
                JsonElement root = doc.RootElement;
                if (root.TryGetProperty("message", out JsonElement m) && m.GetString() is { Length: > 0 } text)
                {
                    message = text;
                }
                ok = root.TryGetProperty("ok", out JsonElement o) && o.ValueKind == JsonValueKind.True && exitCode == 0;
            }
            catch (JsonException)
            {
                message = $"the install script exited {exitCode} with an unreadable result";
            }
        }
        string prefix = exitCode switch
        {
            2 => "ROLLED BACK: ",
            3 => "LOOK AT THE INSTANCE NOW: ",
            4 => "cancelled: ",
            _ => "",
        };
        return (ok, prefix + message);
    }
}
