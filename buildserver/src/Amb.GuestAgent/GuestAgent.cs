using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Reflection;
using Amb.Core.Json;
using Amb.Core.Protocol;

namespace Amb.GuestAgent;

/// <summary>
/// The guest agent's life in one slot (DESIGN.md §10): it says hello, heartbeats every 10 s, takes one step at a time
/// from the coordinator by long-polling, and posts each result until the coordinator has it.
/// </summary>
/// <remarks>
/// <para>Everything is a call FROM the guest: nothing listens in the VM, so the slot needs no inbound port and its
/// user-mode network (passt, then slirp4netns) is enough.</para>
/// <para>The agent survives a restarting coordinator: every call is retried, and a result is posted again until it is
/// accepted, because a step whose result is lost would leave its request waiting until the run timeout.</para>
/// </remarks>
internal sealed class GuestAgent : IDisposable
{
    private static readonly TimeSpan HeartbeatEvery = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan RetryEvery = TimeSpan.FromSeconds(2);

    private readonly SlotConfig _config;
    private readonly AgentLog _log;
    private readonly HttpClient _http;
    private readonly StepRunner _runner;
    private readonly object _gate = new();
    private string? _runningStep;
    private CancellationTokenSource? _runningCancel;

    /// <summary>Creates the agent for one slot.</summary>
    /// <param name="config">The slot's id, the coordinator's URL and the slot's token.</param>
    /// <param name="workDir">The agent's folder in the guest (C:\amb): its log and its steps' output live there.</param>
    /// <param name="log">The agent's log.</param>
    /// <exception cref="UriFormatException">The coordinator's URL is not a valid absolute URL.</exception>
    internal GuestAgent(SlotConfig config, string workDir, AgentLog log)
    {
        _config = config;
        _log = log;
        _http = new HttpClient(new SocketsHttpHandler
        {
            ConnectTimeout = TimeSpan.FromSeconds(5),
            // A slot lives for hours; recycled connections pick up a coordinator that moved (a restart of the site).
            PooledConnectionLifetime = TimeSpan.FromMinutes(2),
        })
        {
            BaseAddress = new Uri(config.Coordinator.TrimEnd('/') + "/"),
            // Longer than the 25 s long poll, so a waiting call is never cut off by the client itself.
            Timeout = TimeSpan.FromSeconds(60),
        };
        _http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", config.Token);
        _runner = new StepRunner(config.SlotId, Path.Combine(workDir, "steps"), log);
    }

    /// <summary>The agent's version, as it reports it.</summary>
    internal static string Version =>
        typeof(GuestAgent).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? "0.0.0";

    /// <summary>Runs the agent until a shutdown step, or until <paramref name="stop"/> is cancelled.</summary>
    /// <param name="stop">Cancelled to end the agent (Ctrl+C when run by hand).</param>
    /// <returns>A task that ends with the agent; after a shutdown step Windows is shutting down.</returns>
    internal async Task RunAsync(CancellationToken stop)
    {
        await RetryAsync("hello", () => PostAsync("guest/v1/hello",
            JsonContent.Create(new GuestHello(_config.SlotId, Version, Facts()), AmbJsonLine.Default.GuestHello), stop),
            stop).ConfigureAwait(false);
        _log.Write($"hello accepted by {_config.Coordinator} (agent {Version})");
        using var heartbeatStop = CancellationTokenSource.CreateLinkedTokenSource(stop);
        Task heartbeats = HeartbeatLoopAsync(heartbeatStop.Token);
        try
        {
            while (!stop.IsCancellationRequested)
            {
                GuestStep? step = await NextStepAsync(stop).ConfigureAwait(false);
                if (step is null)
                {
                    continue;
                }
                StepResult result = await RunStepAsync(step, stop).ConfigureAwait(false);
                bool shutdown = step.Kind == StepKinds.Shutdown;
                if (shutdown && !_config.AllowShutdown)
                {
                    // A local slot's agent runs on the PC itself: a shutdown would end the owner's machine. The result
                    // says so, so the coordinator's log shows the step was skipped, not done.
                    result = result with { Outcome = "skipped: the slot does not allow a shutdown (not a VM)" };
                }
                await RetryAsync($"result {step.Id}", () => PostAsync("guest/v1/result",
                    JsonContent.Create(result, AmbJsonLine.Default.StepResult), stop), stop).ConfigureAwait(false);
                if (shutdown)
                {
                    if (_config.AllowShutdown)
                    {
                        _log.Write("shutdown step: Windows shuts down now");
                        StartShutdown();
                    }
                    else
                    {
                        _log.Write("shutdown step: skipped (the slot does not allow it: not a VM)");
                    }
                    return;
                }
            }
        }
        finally
        {
            await heartbeatStop.CancelAsync().ConfigureAwait(false);
            await heartbeats.ConfigureAwait(false);
        }
    }

    /// <summary>Releases the HTTP client.</summary>
    public void Dispose() => _http.Dispose();

    /// <summary>Long-polls the coordinator for the slot's next step.</summary>
    /// <param name="stop">Cancelled when the agent ends.</param>
    /// <returns>The step, or null when none came within the poll (or the call failed, after a short pause).</returns>
    /// <exception cref="OperationCanceledException"><paramref name="stop"/> was cancelled.</exception>
    private async Task<GuestStep?> NextStepAsync(CancellationToken stop)
    {
        try
        {
            using HttpResponseMessage response = await _http.GetAsync(
                $"guest/v1/next?slot={Uri.EscapeDataString(_config.SlotId)}&wait=25", stop).ConfigureAwait(false);
            if (response.StatusCode == HttpStatusCode.NoContent)
            {
                return null;
            }
            if (!response.IsSuccessStatusCode)
            {
                // 401/404: the slot is not (or no longer) known; the coordinator stops the VM itself. Wait, do not spin.
                _log.Write($"next: HTTP {(int)response.StatusCode}");
                await Task.Delay(TimeSpan.FromSeconds(5), stop).ConfigureAwait(false);
                return null;
            }
            return await response.Content.ReadFromJsonAsync(AmbJsonLine.Default.GuestStep, stop).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException && !stop.IsCancellationRequested)
        {
            // Expected while the coordinator restarts or the network comes up: try again shortly.
            _log.Write($"next: {ex.GetType().Name}: {ex.Message}");
            await Task.Delay(RetryEvery, stop).ConfigureAwait(false);
            return null;
        }
    }

    /// <summary>Runs a step with a cancellation source the heartbeat's reply can trigger, and sends its progress.</summary>
    /// <param name="step">The step.</param>
    /// <param name="stop">Cancelled when the agent ends.</param>
    /// <returns>The step's result.</returns>
    private async Task<StepResult> RunStepAsync(GuestStep step, CancellationToken stop)
    {
        using var cancel = CancellationTokenSource.CreateLinkedTokenSource(stop);
        lock (_gate)
        {
            _runningStep = step.Id;
            _runningCancel = cancel;
        }
        try
        {
            _log.Write($"step {step.Id}: {step.Kind}");
            return await _runner.RunAsync(step, lines => PostAsync("guest/v1/log",
                JsonContent.Create(new StepLog(_config.SlotId, step.Id, lines), AmbJsonLine.Default.StepLog), stop),
                cancel.Token).ConfigureAwait(false);
        }
        finally
        {
            lock (_gate)
            {
                _runningStep = null;
                _runningCancel = null;
            }
        }
    }

    /// <summary>Sends a heartbeat every 10 s and cancels the running step when the coordinator asks.</summary>
    /// <param name="stop">Cancelled when the agent ends.</param>
    /// <returns>A task that ends with the agent.</returns>
    private async Task HeartbeatLoopAsync(CancellationToken stop)
    {
        while (!stop.IsCancellationRequested)
        {
            try
            {
                GuestMemory.Snapshot memory = GuestMemory.Read();
                string? running;
                lock (_gate)
                {
                    running = _runningStep;
                }
                using HttpResponseMessage response = await _http.PostAsync("guest/v1/heartbeat",
                    JsonContent.Create(new GuestHeartbeat(_config.SlotId, memory.CommitMiB, memory.AvailableMiB, running),
                        AmbJsonLine.Default.GuestHeartbeat), stop).ConfigureAwait(false);
                if (response.IsSuccessStatusCode)
                {
                    HeartbeatReply? reply = await response.Content
                        .ReadFromJsonAsync(AmbJsonLine.Default.HeartbeatReply, stop).ConfigureAwait(false);
                    CancelIfAsked(reply?.CancelStep);
                }
            }
            catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or InvalidOperationException
                && !stop.IsCancellationRequested)
            {
                // A missed heartbeat is not fatal: the coordinator gives a slot 3 minutes of silence.
                _log.Write($"heartbeat: {ex.GetType().Name}: {ex.Message}");
            }
            try
            {
                await Task.Delay(HeartbeatEvery, stop).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    /// <summary>Cancels the running step when its id is the one the coordinator named.</summary>
    /// <param name="stepId">The step to cancel, or null.</param>
    private void CancelIfAsked(string? stepId)
    {
        if (stepId is null)
        {
            return;
        }
        lock (_gate)
        {
            if (_runningStep == stepId && _runningCancel is { } cancel)
            {
                _log.Write($"step {stepId}: cancelled by the coordinator");
                cancel.Cancel();
            }
        }
    }

    /// <summary>POSTs a body and throws unless the coordinator accepted it.</summary>
    /// <param name="path">The path under the coordinator's URL.</param>
    /// <param name="body">The JSON body.</param>
    /// <param name="stop">Cancelled when the agent ends.</param>
    /// <returns>A task that ends when the call succeeded.</returns>
    /// <exception cref="HttpRequestException">The call failed or was refused.</exception>
    private async Task PostAsync(string path, HttpContent body, CancellationToken stop)
    {
        using HttpResponseMessage response = await _http.PostAsync(path, body, stop).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
    }

    /// <summary>Runs a call until it succeeds, pausing between attempts.</summary>
    /// <param name="what">What is retried, for the log.</param>
    /// <param name="call">The call.</param>
    /// <param name="stop">Cancelled when the agent ends.</param>
    /// <returns>A task that ends when the call succeeded.</returns>
    /// <exception cref="OperationCanceledException"><paramref name="stop"/> was cancelled.</exception>
    private async Task RetryAsync(string what, Func<Task> call, CancellationToken stop)
    {
        for (int attempt = 1; ; attempt++)
        {
            try
            {
                await call().ConfigureAwait(false);
                return;
            }
            catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException && !stop.IsCancellationRequested)
            {
                if (attempt == 1 || attempt % 30 == 0)
                {
                    _log.Write($"{what}: attempt {attempt} failed: {ex.GetType().Name}: {ex.Message}");
                }
                await Task.Delay(RetryEvery, stop).ConfigureAwait(false);
            }
        }
    }

    /// <summary>The guest's facts for the hello.</summary>
    /// <returns>Windows' version, the machine, the user, CPUs, memory and uptime.</returns>
    private static GuestFacts Facts()
    {
        GuestMemory.Snapshot memory = GuestMemory.Read();
        return new GuestFacts(Environment.OSVersion.VersionString, Environment.MachineName, Environment.UserName,
            Environment.ProcessorCount, memory.TotalMiB, memory.AvailableMiB, memory.CommitMiB,
            Environment.TickCount64 / 1000.0);
    }

    /// <summary>Starts Windows' shutdown (shutdown.exe /s /t 0): the slot's graceful end.</summary>
    private void StartShutdown()
    {
        try
        {
            using Process? shutdown = Process.Start(new ProcessStartInfo("shutdown.exe", "/s /t 0 /f")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
            });
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            // The coordinator stops the VM itself after its grace period; the log says why the guest did not.
            _log.Write($"shutdown failed: {ex.Message}");
        }
    }
}
