using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization.Metadata;
using Amb.Core.Model;

namespace Amb.Core.Records;

/// <summary>A record as stored: its value and the version a compare-and-swap write must name.</summary>
/// <typeparam name="T">The record's type.</typeparam>
/// <param name="Version">1 for the first write, one more at every write after it.</param>
/// <param name="Value">The record.</param>
public sealed record Versioned<T>(long Version, T Value);

/// <summary>
/// A write named a version that is no longer the record's current one: someone else wrote it in between. The caller
/// reads the record again and decides anew; nothing was written.
/// </summary>
public sealed class RecordConflictException : Exception
{
    /// <summary>Creates the exception for one record.</summary>
    /// <param name="path">The record's file.</param>
    /// <param name="expected">The version the write expected.</param>
    /// <param name="actual">The version the record has (0 when it does not exist).</param>
    public RecordConflictException(string path, long expected, long actual)
        : base($"{path}: expected version {expected}, found {actual}")
    {
        Path = path;
        Expected = expected;
        Actual = actual;
    }

    /// <summary>The record's file.</summary>
    public string Path { get; }

    /// <summary>The version the write expected.</summary>
    public long Expected { get; }

    /// <summary>The version the record has.</summary>
    public long Actual { get; }
}

/// <summary>
/// The coordinator's records: small JSON files on the site's own disk, each written atomically (DESIGN.md §6, the
/// owner's D1: "filestore has to be on disk … make sure read-write atomically to the file").
/// </summary>
/// <remarks>
/// <para><b>The write</b>: serialize → write <c>&lt;name&gt;.json.tmp-&lt;pid&gt;-&lt;n&gt;</c> in the same folder → fsync the
/// file → rename it over <c>&lt;name&gt;.json</c> → fsync the folder (Linux). A reader reads whole files and skips
/// <c>.tmp-</c> files, so it sees the old version or the new one, never half of one; a crash between two steps leaves at
/// most a temporary file, which the next start deletes.</para>
/// <para><b>One writer</b>: the coordinator runs one instance, and this class serializes writes to each record with a
/// lock of its own. Each record carries a version, and a write replaces only the version it read (compare-and-swap), so
/// two code paths that both read, decide and write one record cannot silently overwrite each other.</para>
/// <para><b>Lazy</b>: nothing is cached; every read goes to the file, so the coordinator can stop between any two writes
/// and lose nothing.</para>
/// </remarks>
public sealed class RecordStore
{
    /// <summary>The collections DESIGN.md §6 names; each is a folder of the store.</summary>
    public static readonly IReadOnlyList<string> Collections = ["requests", "builds", "slots", "vms"];

    private readonly string _root;
    private readonly ConcurrentDictionary<string, object> _locks = new(StringComparer.Ordinal);
    private long _tmpCounter;

    /// <summary>Opens (and creates when missing) a store, and deletes the temporary files a crash left behind.</summary>
    /// <param name="root">The store's folder (<c>/srv/buildserver/records</c> in the site).</param>
    /// <exception cref="ArgumentException"><paramref name="root"/> is empty.</exception>
    /// <exception cref="IOException">A folder could not be created or a leftover file could not be deleted.</exception>
    /// <exception cref="UnauthorizedAccessException">The process may not write the folder.</exception>
    public RecordStore(string root)
    {
        ArgumentException.ThrowIfNullOrEmpty(root);
        _root = Path.GetFullPath(root);
        foreach (string collection in Collections)
        {
            string folder = Path.Combine(_root, collection);
            Directory.CreateDirectory(folder);
            foreach (string tmp in Directory.EnumerateFiles(folder, "*.tmp-*"))
            {
                // A temporary file is never a record: either its rename happened (and this copy is stale) or the
                // write crashed before it (and the record still holds its old version).
                File.Delete(tmp);
            }
        }
    }

    /// <summary>The store's root folder.</summary>
    public string Root => _root;

    /// <summary>Reads one record.</summary>
    /// <typeparam name="T">The record's type.</typeparam>
    /// <param name="collection">One of <see cref="Collections"/>.</param>
    /// <param name="id">The record's id (<see cref="Ids.IsSafe"/>).</param>
    /// <param name="type">The source-generated metadata of <see cref="Envelope{T}"/> for T.</param>
    /// <returns>The record and its version, or null when it does not exist.</returns>
    /// <exception cref="ArgumentException">The collection is unknown or the id is not safe.</exception>
    /// <exception cref="JsonException">The file is not a valid envelope of T (a record damaged outside this store).</exception>
    /// <exception cref="IOException">The file could not be read.</exception>
    public Versioned<T>? Read<T>(string collection, string id, JsonTypeInfo<Envelope<T>> type)
    {
        string path = PathOf(collection, id);
        Envelope<T>? envelope = ReadEnvelope(path, type);
        return envelope is null ? null : new Versioned<T>(envelope.Version, envelope.Record);
    }

    /// <summary>Writes one record if its current version is the one named, atomically.</summary>
    /// <typeparam name="T">The record's type.</typeparam>
    /// <param name="collection">One of <see cref="Collections"/>.</param>
    /// <param name="id">The record's id (<see cref="Ids.IsSafe"/>).</param>
    /// <param name="value">The new value.</param>
    /// <param name="expectedVersion">The version the caller read (0 for "the record must not exist yet").</param>
    /// <param name="type">The source-generated metadata of <see cref="Envelope{T}"/> for T.</param>
    /// <returns>The new version (<paramref name="expectedVersion"/> + 1).</returns>
    /// <exception cref="ArgumentException">The collection is unknown or the id is not safe.</exception>
    /// <exception cref="RecordConflictException">The record's version is not <paramref name="expectedVersion"/>.</exception>
    /// <exception cref="JsonException">The current file is not a valid envelope of T.</exception>
    /// <exception cref="IOException">Writing, syncing or renaming failed; the record keeps its old version.</exception>
    public long Write<T>(string collection, string id, T value, long expectedVersion, JsonTypeInfo<Envelope<T>> type)
    {
        string path = PathOf(collection, id);
        lock (_locks.GetOrAdd(path, _ => new object()))
        {
            long actual = ReadEnvelope(path, type)?.Version ?? 0;
            if (actual != expectedVersion)
            {
                throw new RecordConflictException(path, expectedVersion, actual);
            }
            long version = expectedVersion + 1;
            byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(new Envelope<T>(version, value), type);
            string tmp = $"{path}.tmp-{Environment.ProcessId}-{Interlocked.Increment(ref _tmpCounter)}";
            try
            {
                using (var stream = new FileStream(tmp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    stream.Write(bytes);
                    // The data must be on disk before the rename publishes it, or a power loss could leave a renamed but
                    // empty record.
                    stream.Flush(flushToDisk: true);
                }
                File.Move(tmp, path, overwrite: true);
                SyncFolder(Path.GetDirectoryName(path)!);
            }
            catch
            {
                // Cleanup-on-failure: the temporary file must never outlive a failed write (a reader skips it, but a
                // leftover would sit until the next start).
                TryDelete(tmp);
                throw;
            }
            return version;
        }
    }

    /// <summary>Reads a record, changes it with a function and writes it back, retrying when another write intervened.</summary>
    /// <typeparam name="T">The record's type.</typeparam>
    /// <param name="collection">One of <see cref="Collections"/>.</param>
    /// <param name="id">The record's id.</param>
    /// <param name="change">Computes the new value from the current one (null when it does not exist yet); returning
    /// null leaves the record unchanged and ends the update.</param>
    /// <param name="type">The source-generated metadata of <see cref="Envelope{T}"/> for T.</param>
    /// <param name="attempts">How often to retry after a conflict (in-process writers serialize on the lock, so a
    /// conflict means a writer outside this process: a bug, reported by the final exception).</param>
    /// <returns>The value written with its version, or the current one when <paramref name="change"/> returned null.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="change"/> is null.</exception>
    /// <exception cref="ArgumentException">The collection is unknown or the id is not safe.</exception>
    /// <exception cref="RecordConflictException">Every attempt met another write.</exception>
    /// <exception cref="JsonException">The current file is not a valid envelope of T.</exception>
    /// <exception cref="IOException">Reading or writing failed.</exception>
    public Versioned<T>? Update<T>(string collection, string id, Func<T?, T?> change, JsonTypeInfo<Envelope<T>> type,
        int attempts = 3) where T : class
    {
        ArgumentNullException.ThrowIfNull(change);
        string path = PathOf(collection, id);
        for (int attempt = 1; ; attempt++)
        {
            lock (_locks.GetOrAdd(path, _ => new object()))
            {
                Versioned<T>? current = Read(collection, id, type);
                T? next = change(current?.Value);
                if (next is null)
                {
                    return current;
                }
                try
                {
                    long version = Write(collection, id, next, current?.Version ?? 0, type);
                    return new Versioned<T>(version, next);
                }
                catch (RecordConflictException) when (attempt < attempts)
                {
                    // Expected control flow: another writer won the race; read again and decide anew.
                }
            }
        }
    }

    /// <summary>Lists the ids of a collection's records, oldest name first (ids sort by time).</summary>
    /// <param name="collection">One of <see cref="Collections"/>.</param>
    /// <returns>The ids, without temporary files and event logs.</returns>
    /// <exception cref="ArgumentException">The collection is unknown.</exception>
    /// <exception cref="IOException">The folder could not be listed.</exception>
    public IReadOnlyList<string> List(string collection)
    {
        string folder = FolderOf(collection);
        return [.. Directory.EnumerateFiles(folder, "*.json")
            .Select(Path.GetFileNameWithoutExtension)
            .Where(id => id is not null && Ids.IsSafe(id))
            .Select(id => id!)
            .Order(StringComparer.Ordinal)];
    }

    /// <summary>Appends one event to a request's event log (<c>requests/&lt;id&gt;.events.jsonl</c>), flushed to disk.</summary>
    /// <param name="requestId">The request's id.</param>
    /// <param name="line">The event, already serialized to one line of JSON (no line breaks).</param>
    /// <exception cref="ArgumentException">The id is not safe, or the line holds a line break.</exception>
    /// <exception cref="IOException">The append failed.</exception>
    /// <remarks>Appends are not atomic like record writes: a crash mid-append can leave a partial last line, which
    /// <see cref="ReadEventLines"/> drops. Events are a log, not the truth; the request's record is.</remarks>
    public void AppendEventLine(string requestId, string line)
    {
        if (line.Contains('\n') || line.Contains('\r'))
        {
            throw new ArgumentException("an event line must not contain a line break", nameof(line));
        }
        string path = EventsPathOf(requestId);
        lock (_locks.GetOrAdd(path, _ => new object()))
        {
            using var stream = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.Read);
            stream.Write(Encoding.UTF8.GetBytes(line + "\n"));
            stream.Flush(flushToDisk: true);
        }
    }

    /// <summary>Reads a request's event lines from one line on.</summary>
    /// <param name="requestId">The request's id.</param>
    /// <param name="skip">How many lines the caller has seen already.</param>
    /// <returns>The lines after the first <paramref name="skip"/>; a partial last line (no newline yet) is left for the
    /// next read.</returns>
    /// <exception cref="ArgumentException">The id is not safe.</exception>
    /// <exception cref="IOException">The file could not be read.</exception>
    public IReadOnlyList<string> ReadEventLines(string requestId, int skip = 0)
    {
        string path = EventsPathOf(requestId);
        if (!File.Exists(path))
        {
            return [];
        }
        string text;
        using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
        using (var reader = new StreamReader(stream, Encoding.UTF8))
        {
            text = reader.ReadToEnd();
        }
        int complete = text.LastIndexOf('\n');
        if (complete < 0)
        {
            return [];
        }
        return [.. text[..complete].Split('\n').Skip(skip)];
    }

    /// <summary>The file of one record.</summary>
    /// <param name="collection">One of <see cref="Collections"/>.</param>
    /// <param name="id">The record's id.</param>
    /// <returns>The absolute path of <c>&lt;collection&gt;/&lt;id&gt;.json</c>.</returns>
    /// <exception cref="ArgumentException">The collection is unknown or the id is not safe.</exception>
    public string PathOf(string collection, string id)
    {
        if (!Ids.IsSafe(id))
        {
            throw new ArgumentException($"not a safe record id: {id}", nameof(id));
        }
        return Path.Combine(FolderOf(collection), id + ".json");
    }

    /// <summary>The folder of a collection.</summary>
    /// <param name="collection">One of <see cref="Collections"/>.</param>
    /// <returns>Its absolute path.</returns>
    /// <exception cref="ArgumentException">The collection is unknown.</exception>
    private string FolderOf(string collection)
    {
        if (!Collections.Contains(collection, StringComparer.Ordinal))
        {
            throw new ArgumentException($"unknown collection: {collection}", nameof(collection));
        }
        return Path.Combine(_root, collection);
    }

    /// <summary>The event log of a request.</summary>
    /// <param name="requestId">The request's id.</param>
    /// <returns>The absolute path of <c>requests/&lt;id&gt;.events.jsonl</c>.</returns>
    /// <exception cref="ArgumentException">The id is not safe.</exception>
    private string EventsPathOf(string requestId)
    {
        if (!Ids.IsSafe(requestId))
        {
            throw new ArgumentException($"not a safe request id: {requestId}", nameof(requestId));
        }
        return Path.Combine(_root, "requests", requestId + ".events.jsonl");
    }

    /// <summary>Reads and parses an envelope file.</summary>
    /// <typeparam name="T">The record's type.</typeparam>
    /// <param name="path">The record's file.</param>
    /// <param name="type">The envelope's metadata.</param>
    /// <returns>The envelope, or null when the file does not exist.</returns>
    /// <exception cref="JsonException">The file is not a valid envelope.</exception>
    /// <exception cref="IOException">The file could not be read.</exception>
    private static Envelope<T>? ReadEnvelope<T>(string path, JsonTypeInfo<Envelope<T>> type)
    {
        byte[] bytes;
        try
        {
            bytes = File.ReadAllBytes(path);
        }
        catch (FileNotFoundException)
        {
            // Expected control flow: a record that does not exist yet reads as null.
            return null;
        }
        return JsonSerializer.Deserialize(bytes, type) ?? throw new JsonException($"{path}: the file holds null");
    }

    /// <summary>Makes a rename in a folder durable: fsync of the folder on Linux; nothing to do on Windows, where the
    /// rename is a metadata update NTFS journals itself.</summary>
    /// <param name="folder">The folder that holds the renamed file.</param>
    /// <exception cref="IOException">The folder could not be opened or synced on Linux.</exception>
    private static void SyncFolder(string folder)
    {
        if (!OperatingSystem.IsLinux())
        {
            return;
        }
        int fd = LibC.Open(folder, LibC.ORdOnly | LibC.ODirectory);
        if (fd < 0)
        {
            throw new IOException($"open({folder}) failed: errno {Marshal.GetLastPInvokeError()}");
        }
        try
        {
            if (LibC.Fsync(fd) != 0)
            {
                throw new IOException($"fsync({folder}) failed: errno {Marshal.GetLastPInvokeError()}");
            }
        }
        finally
        {
            _ = LibC.Close(fd);
        }
    }

    /// <summary>Deletes a file and ignores every failure (cleanup that must not hide the error being reported).</summary>
    /// <param name="path">The file.</param>
    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
            // Deliberately ignored: this runs while another exception propagates, and the next start deletes the file.
        }
        catch (UnauthorizedAccessException)
        {
            // Deliberately ignored, as above.
        }
    }
}

/// <summary>The stored form of a record: its version and its value (the JSON <c>{"version": n, "record": …}</c>).</summary>
/// <typeparam name="T">The record's type.</typeparam>
/// <param name="Version">The record's version.</param>
/// <param name="Record">The record.</param>
public sealed record Envelope<T>(long Version, T Record);

/// <summary>The three libc calls the record store needs to fsync a folder on Linux.</summary>
internal static partial class LibC
{
    /// <summary>open(2)'s O_RDONLY.</summary>
    internal const int ORdOnly = 0;

    /// <summary>open(2)'s O_DIRECTORY on x86-64 Linux.</summary>
    internal const int ODirectory = 0x10000;

    /// <summary>open(2).</summary>
    /// <param name="path">The path.</param>
    /// <param name="flags">The flags.</param>
    /// <returns>A file descriptor, or -1 with errno set.</returns>
    [LibraryImport("libc", EntryPoint = "open", StringMarshalling = StringMarshalling.Utf8, SetLastError = true)]
    internal static partial int Open(string path, int flags);

    /// <summary>fsync(2).</summary>
    /// <param name="fd">The descriptor.</param>
    /// <returns>0, or -1 with errno set.</returns>
    [LibraryImport("libc", EntryPoint = "fsync", SetLastError = true)]
    internal static partial int Fsync(int fd);

    /// <summary>close(2).</summary>
    /// <param name="fd">The descriptor.</param>
    /// <returns>0, or -1 with errno set.</returns>
    [LibraryImport("libc", EntryPoint = "close", SetLastError = true)]
    internal static partial int Close(int fd);
}
