using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using Amb.Core.Protocol;

namespace Amb.GuestAgent;

/// <summary>
/// Runs one <see cref="GuestStep"/> in the guest and turns whatever happens into a <see cref="StepResult"/>: a step never
/// throws out of here, because the coordinator must always get an answer for the step it handed out.
/// </summary>
internal sealed class StepRunner
{
    /// <summary>How much of each output stream a result carries (the whole output stays in the guest's step folder).</summary>
    private const int TailBytes = 4096;

    private readonly string _slotId;
    private readonly string _stepsDir;
    private readonly AgentLog _log;

    /// <summary>Creates a runner for one slot.</summary>
    /// <param name="slotId">The slot's id, copied into every result.</param>
    /// <param name="stepsDir">Where each run step's full output goes (C:\amb\steps\&lt;id&gt;.out / .err).</param>
    /// <param name="log">The agent's log.</param>
    internal StepRunner(string slotId, string stepsDir, AgentLog log)
    {
        _slotId = slotId;
        _stepsDir = stepsDir;
        _log = log;
        Directory.CreateDirectory(stepsDir);
    }

    /// <summary>Runs a step to its end, its timeout, or its cancellation.</summary>
    /// <param name="step">The step.</param>
    /// <param name="progress">Called with each batch of new lines of the step's progress file while a run step runs
    /// (at most once a second); exceptions it throws are logged and ignored.</param>
    /// <param name="cancel">Cancelled when the coordinator asks for the step to stop (or the agent ends).</param>
    /// <returns>The result; for an exception the outcome names it and the exit code is 1.</returns>
    internal async Task<StepResult> RunAsync(GuestStep step, Func<IReadOnlyList<string>, Task> progress,
        CancellationToken cancel)
    {
        var watch = Stopwatch.StartNew();
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
            timeout.CancelAfter(TimeSpan.FromSeconds(Math.Max(1, step.TimeoutS)));
            StepResult result = step.Kind switch
            {
                StepKinds.Run => await RunProgramAsync(step, progress, timeout.Token).ConfigureAwait(false),
                StepKinds.Fetch => await CopyAsync(step, verify: true, timeout.Token).ConfigureAwait(false),
                StepKinds.Put => await CopyAsync(step, verify: false, timeout.Token).ConfigureAwait(false),
                StepKinds.Expand => Expand(step),
                StepKinds.Shutdown => Done(step, 0, "ok"),
                _ => Done(step, 1, $"failed: unknown step kind '{step.Kind}'"),
            };
            return result with { DurationSeconds = watch.Elapsed.TotalSeconds };
        }
        catch (OperationCanceledException)
        {
            // Expected control flow: a cancel from the coordinator, or the step's own timeout.
            return Done(step, -1, cancel.IsCancellationRequested ? "cancelled" : "timeout") with
            {
                DurationSeconds = watch.Elapsed.TotalSeconds,
            };
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException
            or System.ComponentModel.Win32Exception or ArgumentException or NotSupportedException)
        {
            _log.Write($"step {step.Id} ({step.Kind}) failed: {ex}");
            return Done(step, 1, $"failed: {ex.GetType().Name}: {ex.Message}") with
            {
                DurationSeconds = watch.Elapsed.TotalSeconds,
            };
        }
    }

    /// <summary>Runs a program, capturing its output to files, tailing its progress file, and stopping its whole process
    /// tree when the step is cancelled or times out.</summary>
    /// <param name="step">The run step.</param>
    /// <param name="progress">Receives the progress file's new lines.</param>
    /// <param name="stop">The step's timeout and the coordinator's cancellation together (RunAsync tells the two apart
    /// from its own token afterwards).</param>
    /// <returns>The result with the exit code and both output tails.</returns>
    /// <exception cref="ArgumentException">The step has no command.</exception>
    /// <exception cref="System.ComponentModel.Win32Exception">The program could not be started.</exception>
    /// <exception cref="OperationCanceledException">The step was stopped (after its process tree was killed).</exception>
    private async Task<StepResult> RunProgramAsync(GuestStep step, Func<IReadOnlyList<string>, Task> progress,
        CancellationToken stop)
    {
        if (string.IsNullOrEmpty(step.Command))
        {
            throw new ArgumentException("a run step needs a command");
        }
        string outPath = Path.Combine(_stepsDir, step.Id + ".out");
        string errPath = Path.Combine(_stepsDir, step.Id + ".err");
        var info = new ProcessStartInfo(step.Command)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            CreateNoWindow = true,
            WorkingDirectory = string.IsNullOrEmpty(step.Cwd) ? _stepsDir : step.Cwd,
        };
        foreach (string arg in step.Args ?? [])
        {
            info.ArgumentList.Add(arg);
        }
        foreach ((string name, string value) in step.Env ?? new Dictionary<string, string>())
        {
            info.Environment[name] = value;
        }
        using var process = new Process { StartInfo = info };
        await using var outFile = new FileStream(outPath, FileMode.Create, FileAccess.Write, FileShare.Read);
        await using var errFile = new FileStream(errPath, FileMode.Create, FileAccess.Write, FileShare.Read);
        process.Start();
        process.StandardInput.Close();
        _log.Write($"step {step.Id}: started pid {process.Id}: {step.Command} {string.Join(' ', step.Args ?? [])}");
        Task copyOut = process.StandardOutput.BaseStream.CopyToAsync(outFile, CancellationToken.None);
        Task copyErr = process.StandardError.BaseStream.CopyToAsync(errFile, CancellationToken.None);
        using var tailStop = new CancellationTokenSource();
        Task tail = step.ProgressFile is null
            ? Task.CompletedTask
            : TailAsync(step.ProgressFile, progress, tailStop.Token);
        try
        {
            await process.WaitForExitAsync(stop).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Cleanup-on-cancel: the whole tree goes (a pytest run starts Agentmaster, which starts claude.exe and
            // its shells), or a cancelled step would leave processes running into the next one.
            KillTree(process, step.Id);
            await Task.WhenAll(copyOut, copyErr).WaitAsync(TimeSpan.FromSeconds(10), CancellationToken.None)
                .ConfigureAwait(false);
            await tailStop.CancelAsync().ConfigureAwait(false);
            throw;
        }
        await Task.WhenAll(copyOut, copyErr).ConfigureAwait(false);
        await tailStop.CancelAsync().ConfigureAwait(false);
        await tail.ConfigureAwait(false);
        await outFile.FlushAsync(CancellationToken.None).ConfigureAwait(false);
        await errFile.FlushAsync(CancellationToken.None).ConfigureAwait(false);
        int code = process.ExitCode;
        _log.Write($"step {step.Id}: exit {code}");
        return Done(step, code, code == 0 ? "ok" : $"failed: exit code {code}") with
        {
            StdoutTail = Tail(outPath),
            StderrTail = Tail(errPath),
        };
    }

    /// <summary>Kills a process and all its descendants; failures are logged (the process may have ended already).</summary>
    /// <param name="process">The step's process.</param>
    /// <param name="stepId">The step's id, for the log.</param>
    private void KillTree(Process process, string stepId)
    {
        try
        {
            process.Kill(entireProcessTree: true);
            _log.Write($"step {stepId}: killed its process tree");
        }
        catch (InvalidOperationException)
        {
            // Expected: the process ended between the cancel and the kill.
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            _log.Write($"step {stepId}: kill failed: {ex.Message}");
        }
    }

    /// <summary>Follows a progress file the program appends to and hands each batch of complete new lines to a callback.</summary>
    /// <param name="path">The file (it may not exist yet).</param>
    /// <param name="progress">The callback.</param>
    /// <param name="stop">Cancelled when the program ended; one last read follows, so no final line is lost.</param>
    /// <returns>A task that ends after the last read.</returns>
    private async Task TailAsync(string path, Func<IReadOnlyList<string>, Task> progress, CancellationToken stop)
    {
        long offset = 0;
        var pending = new StringBuilder();
        bool last = false;
        while (true)
        {
            try
            {
                if (File.Exists(path))
                {
                    await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
                        FileShare.ReadWrite | FileShare.Delete);
                    if (stream.Length > offset)
                    {
                        stream.Position = offset;
                        using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: false);
                        pending.Append(await reader.ReadToEndAsync(CancellationToken.None).ConfigureAwait(false));
                        offset = stream.Length;
                        string text = pending.ToString();
                        int end = text.LastIndexOf('\n');
                        if (end >= 0)
                        {
                            // Only complete lines go out: a line the program is still writing waits for the next read.
                            string[] lines = text[..end].Replace("\r", "", StringComparison.Ordinal).Split('\n');
                            pending.Clear().Append(text[(end + 1)..]);
                            await progress(lines).ConfigureAwait(false);
                        }
                    }
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or HttpRequestException
                or TaskCanceledException)
            {
                // Progress is a convenience: a failed read or send is logged and the tail goes on; the result still
                // carries the step's outcome.
                _log.Write($"progress {path}: {ex.Message}");
            }
            if (last)
            {
                return;
            }
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(1), stop).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                // Expected: the program ended; read once more for its final lines.
                last = true;
            }
        }
    }

    /// <summary>Copies a file through a temporary name and reports its SHA-256; a fetch also checks it.</summary>
    /// <param name="step">The fetch or put step.</param>
    /// <param name="verify">Whether to compare with the step's SHA-256 (fetch).</param>
    /// <param name="stop">The step's timeout and cancellation.</param>
    /// <returns>The result with the file's size and SHA-256; "sha256 mismatch" (exit 1, the copy deleted) when the
    /// expected hash differs.</returns>
    /// <exception cref="ArgumentException">The step has no source or destination.</exception>
    /// <exception cref="IOException">The copy failed.</exception>
    /// <exception cref="OperationCanceledException">The step was stopped.</exception>
    private async Task<StepResult> CopyAsync(GuestStep step, bool verify, CancellationToken stop)
    {
        if (string.IsNullOrEmpty(step.From) || string.IsNullOrEmpty(step.To))
        {
            throw new ArgumentException("a copy step needs a source and a destination");
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(step.To))!);
        string tmp = step.To + ".part";
        string hash;
        long bytes;
        await using (var source = new FileStream(step.From, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20,
            useAsync: true))
        await using (var target = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20,
            useAsync: true))
        using (var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
        {
            byte[] buffer = new byte[1 << 20];
            int read;
            while ((read = await source.ReadAsync(buffer, stop).ConfigureAwait(false)) > 0)
            {
                sha.AppendData(buffer, 0, read);
                await target.WriteAsync(buffer.AsMemory(0, read), stop).ConfigureAwait(false);
            }
            bytes = target.Length;
            hash = Convert.ToHexStringLower(sha.GetHashAndReset());
        }
        if (verify && step.Sha256 is not null && !string.Equals(step.Sha256, hash, StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(tmp);
            return Done(step, 1, $"sha256 mismatch: expected {step.Sha256}, got {hash}") with { Sha256 = hash, Bytes = bytes };
        }
        File.Move(tmp, step.To, overwrite: true);
        return Done(step, 0, "ok") with { Sha256 = hash, Bytes = bytes };
    }

    /// <summary>Extracts a zip into a folder, replacing files that exist.</summary>
    /// <param name="step">The expand step.</param>
    /// <returns>The result.</returns>
    /// <exception cref="ArgumentException">The step has no source or destination.</exception>
    /// <exception cref="InvalidDataException">The file is not a zip.</exception>
    /// <exception cref="IOException">Extracting failed.</exception>
    private StepResult Expand(GuestStep step)
    {
        if (string.IsNullOrEmpty(step.From) || string.IsNullOrEmpty(step.To))
        {
            throw new ArgumentException("an expand step needs a source and a destination");
        }
        Directory.CreateDirectory(step.To);
        ZipFile.ExtractToDirectory(step.From, step.To, overwriteFiles: true);
        return Done(step, 0, "ok");
    }

    /// <summary>A result with the common fields filled.</summary>
    /// <param name="step">The step.</param>
    /// <param name="exitCode">Its exit code.</param>
    /// <param name="outcome">Its outcome.</param>
    /// <returns>The result (duration 0; the caller sets it).</returns>
    private StepResult Done(GuestStep step, int exitCode, string outcome) =>
        new(_slotId, step.Id, exitCode, outcome, 0);

    /// <summary>The last <see cref="TailBytes"/> of a file as text, in whole lines.</summary>
    /// <param name="path">The file.</param>
    /// <returns>Its tail, or null when it is empty or cannot be read.</returns>
    /// <remarks>The coordinator logs each line of a tail as an event, so a tail never starts with the cut-off end of a
    /// line (or of a UTF-8 character): when the file is longer than the tail, everything up to the first line break is
    /// dropped.</remarks>
    private static string? Tail(string path)
    {
        try
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            if (stream.Length == 0)
            {
                return null;
            }
            bool cut = stream.Length > TailBytes;
            stream.Position = Math.Max(0, stream.Length - TailBytes);
            using var reader = new StreamReader(stream, Encoding.UTF8);
            string text = reader.ReadToEnd();
            if (cut && text.IndexOf('\n') is int newline and >= 0)
            {
                text = text[(newline + 1)..];
            }
            return text.Length == 0 ? null : text;
        }
        catch (IOException)
        {
            // The tail is a convenience; the full output stays in the step folder.
            return null;
        }
    }
}
