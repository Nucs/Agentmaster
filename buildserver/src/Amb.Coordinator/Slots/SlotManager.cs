using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text.Json;
using Amb.Core.Allocation;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;

namespace Amb.Coordinator.Slots;

/// <summary>One slot a job holds: its record, its guest session, and how its guest sees its share.</summary>
/// <param name="Spec">What it was started with.</param>
/// <param name="Session">Its guest agent's session.</param>
/// <param name="Handle">The driver's handle.</param>
/// <param name="GuestShare">The share's root as the guest sees it ("\\host.lan\Data" in a VM).</param>
public sealed record SlotLease(SlotSpec Spec, SlotSession Session, string Handle, string GuestShare);

/// <summary>
/// Makes, tracks and removes slots: their folders, their shares (the guest agent and its <c>slot.json</c>), their records
/// (DESIGN.md §6: <c>slots/&lt;id&gt;.json</c>) and their guest sessions, through one <see cref="ISlotDriver"/>.
/// </summary>
/// <remarks>
/// <para>A coordinator that starts finds the slots an earlier run left (a crash, a restart) in the records and removes
/// them: their requests are marked lost by the scheduler, so nothing would ever use them again, and a VM left running
/// holds its whole RAM_SIZE until someone notices.</para>
/// <para>Every started slot has a liveness watch until it is stopped (<see cref="StartWatch"/>): a VM that stops by
/// itself is given up within ~30 s instead of at the silence limit or the hello timeout.</para>
/// </remarks>
public sealed class SlotManager
{
    private readonly CoordinatorOptions _options;
    private readonly RecordStore _store;
    private readonly GuestBroker _broker;
    private readonly ISlotDriver _driver;
    private readonly TimeProvider _clock;
    private readonly ILogger<SlotManager> _log;
    private readonly ConcurrentDictionary<string, SlotRecord> _active = new(StringComparer.Ordinal);

    /// <summary>The liveness watch of every started slot, by slot id (see <see cref="StartWatch"/>).</summary>
    private readonly ConcurrentDictionary<string, CancellationTokenSource> _watches = new(StringComparer.Ordinal);

    /// <summary>How often the liveness watch asks the driver whether a slot still runs.</summary>
    public static readonly TimeSpan WatchInterval = TimeSpan.FromSeconds(15);

    /// <summary>Creates the manager.</summary>
    /// <param name="options">The settings (folders, the guest agent, the guest URL).</param>
    /// <param name="store">The record store.</param>
    /// <param name="broker">The guest broker.</param>
    /// <param name="driver">The slot driver.</param>
    /// <param name="clock">The clock.</param>
    /// <param name="log">The log.</param>
    public SlotManager(CoordinatorOptions options, RecordStore store, GuestBroker broker, ISlotDriver driver,
        TimeProvider clock, ILogger<SlotManager> log)
    {
        _options = options;
        _store = store;
        _broker = broker;
        _driver = driver;
        _clock = clock;
        _log = log;
    }

    /// <summary>The slots that are not gone.</summary>
    public IReadOnlyList<SlotRecord> Active => [.. _active.Values.OrderBy(s => s.CreatedAt)];

    /// <summary>Pulsed when a slot is removed: freed capacity, so the scheduler re-scores.</summary>
    public AsyncSignal Released { get; } = new();

    /// <summary>Removes every slot an earlier run of the coordinator left behind.</summary>
    /// <param name="cancel">Cancels the cleanup.</param>
    /// <returns>How many were removed.</returns>
    /// <exception cref="IOException">The records could not be read.</exception>
    /// <exception cref="JsonException">A slot record is damaged.</exception>
    public async Task<int> RemoveLeftoversAsync(CancellationToken cancel)
    {
        int removed = 0;
        foreach (string id in _store.List("slots"))
        {
            SlotRecord? slot = _store.Read("slots", id, AmbJson.Default.EnvelopeSlotRecord)?.Value;
            if (slot is null || slot.State == SlotState.Gone)
            {
                continue;
            }
            _log.LogWarning("slot {Slot} ({Request}) was left by an earlier run: removing it", slot.Id, slot.RequestId);
            await _driver.StopAsync(slot.Handle, graceful: false, cancel).ConfigureAwait(false);
            DeleteFolders(slot.Id);
            MarkGone(slot.Id);
            removed++;
        }
        return removed;
    }

    /// <summary>Starts a slot for a request: folders, share, record, guest session, then the driver.</summary>
    /// <param name="request">The request it serves.</param>
    /// <param name="image">The image it boots (for an install: the image it will become).</param>
    /// <param name="grant">What it was granted.</param>
    /// <param name="cancel">Cancels the start (the half-made slot is removed).</param>
    /// <param name="installVersion">For an install slot: the dockur VERSION to install; null to boot an image.</param>
    /// <param name="oemDir">For an install slot: the OEM folder dockur runs at the end of the install.</param>
    /// <returns>The lease; the caller must <see cref="StopAsync"/> it in every outcome.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="request"/> is null.</exception>
    /// <exception cref="InvalidOperationException">The driver could not start it, or the guest agent is missing.</exception>
    /// <exception cref="IOException">A folder or file could not be made.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    public async Task<SlotLease> StartAsync(RequestRecord request, string image, Resources grant, CancellationToken cancel,
        string? installVersion = null, string? oemDir = null)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (!File.Exists(_options.GuestAgentPath))
        {
            throw new InvalidOperationException($"no guest agent at {_options.GuestAgentPath}");
        }
        string slotId = Ids.NewSlotId(_clock.GetUtcNow());
        var spec = new SlotSpec(slotId, image, grant, Path.Combine(_options.SlotsDir, slotId),
            Path.Combine(_options.ExchangeDir, slotId), Path.Combine(_options.ImagesDir, image), installVersion, oemDir);
        string token = Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(32));
        // The share first: the guest agent and its slot.json must be there before the VM's logon task looks for them.
        string amb = Path.Combine(spec.ExchangeDir, "amb");
        Directory.CreateDirectory(amb);
        File.Copy(_options.GuestAgentPath, Path.Combine(amb, "amb-guest.exe"), overwrite: true);
        // A shutdown step may shut Windows down only in a VM: a local slot's agent runs on the PC itself.
        await File.WriteAllTextAsync(Path.Combine(amb, "slot.json"),
            JsonSerializer.Serialize(new SlotConfig(slotId, _options.GuestUrl, token, _driver.IsVm),
                AmbJsonLine.Default.SlotConfig),
            cancel).ConfigureAwait(false);
        var record = new SlotRecord
        {
            Id = slotId,
            RequestId = request.Id,
            Site = SiteCapacity.Homepc,
            Image = image,
            Grant = grant,
            State = SlotState.Starting,
            Driver = _driver.Name,
            Handle = "",
            TokenSha256 = Convert.ToHexStringLower(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(token))),
            CreatedAt = _clock.GetUtcNow(),
        };
        _store.Write("slots", slotId, record, 0, AmbJson.Default.EnvelopeSlotRecord);
        _active[slotId] = record;
        SlotSession session = _broker.Open(slotId, token);
        try
        {
            string handle = await _driver.StartAsync(spec, cancel).ConfigureAwait(false);
            Update(slotId, s => s with { Handle = handle });
            var lease = new SlotLease(spec, session, handle, _driver.GuestShareRoot(spec));
            StartWatch(lease);
            return lease;
        }
        catch
        {
            // Cleanup-on-failure: a slot that never started still has a share, a record and a session.
            _broker.Close(slotId);
            DeleteFolders(slotId);
            MarkGone(slotId);
            throw;
        }
    }

    /// <summary>Marks a slot ready once its guest agent said hello.</summary>
    /// <param name="slotId">The slot's id.</param>
    /// <exception cref="IOException">The record could not be written.</exception>
    public void MarkReady(string slotId) => Update(slotId, s => s with { State = SlotState.Ready, LastHeartbeatAt = _clock.GetUtcNow() });

    /// <summary>Stops a slot and removes everything it had: the driver's container, its folders, its session.</summary>
    /// <param name="lease">The slot.</param>
    /// <param name="graceful">Shut Windows down first.</param>
    /// <param name="keepShare">Keep the share's folder (a failed run's evidence that was not copied yet).</param>
    /// <param name="commitAs">Keep the slot's disks as a new read-only image of this name instead of deleting them (an
    /// install or a layer, after its guest shut down cleanly); null deletes them.</param>
    /// <returns>A task that ends when the slot is gone (and, with <paramref name="commitAs"/>, the image is in
    /// place).</returns>
    /// <exception cref="ArgumentNullException"><paramref name="lease"/> is null.</exception>
    /// <exception cref="IOException">The slot's folder could not be moved into the images folder (only with
    /// <paramref name="commitAs"/>; the slot is gone either way).</exception>
    public async Task StopAsync(SlotLease lease, bool graceful, bool keepShare = false, string? commitAs = null)
    {
        ArgumentNullException.ThrowIfNull(lease);
        // The watch ends first: a stop on purpose (a graceful shutdown included) must never read as the VM dying.
        if (_watches.TryRemove(lease.Spec.SlotId, out CancellationTokenSource? watch))
        {
            await watch.CancelAsync().ConfigureAwait(false);
        }
        Update(lease.Spec.SlotId, s => s with { State = SlotState.Stopping });
        try
        {
            await _driver.StopAsync(lease.Handle, graceful, CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception or IOException)
        {
            _log.LogError(ex, "slot {Slot}: the driver could not stop it", lease.Spec.SlotId);
        }
        _broker.Close(lease.Spec.SlotId);
        try
        {
            if (commitAs is not null)
            {
                Commit(lease.Spec, commitAs);
            }
        }
        finally
        {
            DeleteFolders(lease.Spec.SlotId, keepShare);
            MarkGone(lease.Spec.SlotId);
        }
    }

    /// <summary>Starts a slot's liveness watch, so a slot whose VM stops by itself is given up at once.</summary>
    /// <param name="lease">The slot.</param>
    /// <remarks>
    /// <para>Every <see cref="WatchInterval"/> the driver is asked whether the slot still runs. Two "not running"
    /// answers in a row (so one failed <c>podman inspect</c> never ends a healthy slot) abandon its session
    /// (<see cref="SlotSession.Abandon"/>): the job's hello wait or step then fails as lost within ~30 s, instead of
    /// after the silence limit (3 min) or the whole hello timeout (10 min; 45 for an install). The job's own finally
    /// block stops the slot as in every other outcome, which removes the dead container and the folders.</para>
    /// <para>The token source is never disposed: it has no timer and no wait handle, and disposing it would race the
    /// watch's last use of it.</para>
    /// </remarks>
    private void StartWatch(SlotLease lease)
    {
        var stop = new CancellationTokenSource();
        _watches[lease.Spec.SlotId] = stop;
        _ = WatchAsync(lease, stop);
    }

    /// <summary>The liveness watch's loop (<see cref="StartWatch"/>); it never throws.</summary>
    /// <param name="lease">The slot.</param>
    /// <param name="stop">Cancelled when the slot is stopped on purpose.</param>
    /// <returns>A task that ends when the slot is stopped or given up.</returns>
    private async Task WatchAsync(SlotLease lease, CancellationTokenSource stop)
    {
        string slotId = lease.Spec.SlotId;
        int deadAnswers = 0;
        try
        {
            while (true)
            {
                await Task.Delay(WatchInterval, _clock, stop.Token).ConfigureAwait(false);
                bool alive;
                try
                {
                    alive = await _driver.IsAliveAsync(lease.Handle, stop.Token).ConfigureAwait(false);
                }
                catch (Exception ex) when (ex is TimeoutException or InvalidOperationException
                    or System.ComponentModel.Win32Exception or IOException)
                {
                    // A check that could not be made says nothing about the slot: it counts as neither answer.
                    _log.LogWarning("slot {Slot}: its liveness check failed: {Error}", slotId, ex.Message);
                    continue;
                }
                deadAnswers = alive ? 0 : deadAnswers + 1;
                if (deadAnswers >= 2)
                {
                    string reason = $"slot {slotId}: its VM stopped by itself (the {_driver.Name} slot is not running)";
                    _log.LogWarning("{Reason}", reason);
                    lease.Session.Abandon(reason);
                    return;
                }
            }
        }
        catch (OperationCanceledException)
        {
            // Expected control flow: the slot was stopped on purpose.
        }
        catch (Exception ex)
        {
            // Nobody awaits the watch, so nothing may escape it; a watch that died leaves the silence limit in charge.
            _log.LogError(ex, "slot {Slot}: its liveness watch failed", slotId);
        }
        finally
        {
            // Only this watch's own entry: a stop removed it already, and nothing else can reuse the slot's id.
            _watches.TryRemove(KeyValuePair.Create(slotId, stop));
        }
    }

    /// <summary>Turns a stopped slot's folder into a read-only image: one rename on the site's disk.</summary>
    /// <param name="spec">The slot.</param>
    /// <param name="image">The new image's name.</param>
    /// <exception cref="IOException">The image exists already, the slot's disk is missing, or the move failed.</exception>
    /// <remarks>An install's ISO is deleted first (dockur keeps nothing else the image needs). ⚠ The image must hold
    /// <c>windows.boot</c>: without it dockur would take the image for an unfinished install and delete its disks; a slot
    /// whose install never finished therefore never becomes an image.</remarks>
    private void Commit(SlotSpec spec, string image)
    {
        string target = Path.Combine(_options.ImagesDir, image);
        if (Directory.Exists(target))
        {
            throw new IOException($"image {image} exists already");
        }
        if (!File.Exists(Path.Combine(spec.SlotDir, "data.qcow2")) || !File.Exists(Path.Combine(spec.SlotDir, "windows.boot")))
        {
            throw new IOException($"slot {spec.SlotId} has no finished disk (data.qcow2 and windows.boot) to keep as {image}");
        }
        foreach (string iso in Directory.EnumerateFiles(spec.SlotDir, "*.iso"))
        {
            File.Delete(iso);
        }
        Directory.CreateDirectory(_options.ImagesDir);
        Directory.Move(spec.SlotDir, target);
        _log.LogInformation("slot {Slot}: kept as image {Image}", spec.SlotId, image);
    }

    /// <summary>Changes a slot's record (in memory and on disk).</summary>
    /// <param name="slotId">The slot's id.</param>
    /// <param name="change">The change.</param>
    /// <exception cref="IOException">The record could not be written.</exception>
    private void Update(string slotId, Func<SlotRecord, SlotRecord> change)
    {
        Versioned<SlotRecord>? updated = _store.Update<SlotRecord>("slots", slotId,
            current => current is null ? null : change(current), AmbJson.Default.EnvelopeSlotRecord);
        if (updated is not null && updated.Value.State != SlotState.Gone)
        {
            _active[slotId] = updated.Value;
        }
    }

    /// <summary>Marks a slot gone and drops it from the active list.</summary>
    /// <param name="slotId">The slot's id.</param>
    private void MarkGone(string slotId)
    {
        try
        {
            _store.Update<SlotRecord>("slots", slotId,
                current => current is null ? null : current with { State = SlotState.Gone, EndedAt = _clock.GetUtcNow() },
                AmbJson.Default.EnvelopeSlotRecord);
        }
        catch (IOException ex)
        {
            // The slot is gone either way; a record that still says otherwise is removed again at the next start.
            _log.LogError(ex, "slot {Slot}: its record could not be marked gone", slotId);
        }
        _active.TryRemove(slotId, out _);
        Released.Pulse();
    }

    /// <summary>Deletes a slot's own folder (overlay, UEFI variables) and, unless kept, its share.</summary>
    /// <param name="slotId">The slot's id.</param>
    /// <param name="keepShare">Keep the share.</param>
    private void DeleteFolders(string slotId, bool keepShare = false)
    {
        foreach (string folder in keepShare
                     ? new[] { Path.Combine(_options.SlotsDir, slotId) }
                     : new[] { Path.Combine(_options.SlotsDir, slotId), Path.Combine(_options.ExchangeDir, slotId) })
        {
            try
            {
                if (Directory.Exists(folder))
                {
                    Directory.Delete(folder, recursive: true);
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                // A file still held (an overlay the container has not released yet) is removed at the next start.
                _log.LogWarning("slot {Slot}: {Folder} could not be deleted: {Error}", slotId, folder, ex.Message);
            }
        }
    }
}
