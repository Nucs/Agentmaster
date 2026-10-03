#:property PublishAot=false
// Agentmaster: threadcensus — a READ-ONLY thread census of a running Agentmaster (or any) process.
// The evidence behind doc/agentmaster/RENDERING.md §4b ("One tab on screen, many in the strip").
//
//   dotnet run tools/threadcensus.cs -- <pid> [seconds=10]
//
// What it prints, over a <seconds> sampling window:
//   1. Every thread grouped by its Win32 start module (+ its thread description when it has one):
//      thread count, how many woke at least once, CPU ms per second, wake-ups (context switches) per
//      second. "Rendering Output Thread" rows are the terminals' Renderer threads (one per
//      INITIALIZED terminal); "nvwgf2umx.dll" rows are the NVIDIA user-mode driver's threads (20 per
//      Direct3D 11 device).
//   2. Each render thread that woke: its wake rate (~2 wake-ups per painted frame) and CPU.
//   3. The NVIDIA threads cut into 20-thread device bursts (creation order), each paired with the
//      render thread created closest to it — so an idle terminal's device can be told from a
//      painting one. XAML's own device is the burst at process start with no render thread nearby.
//   4. The XAML / UI threads by exact start offset (Windows.UI.Xaml.dll+0x6F59C0 is XAML's frame
//      thread: it wakes once per display frame while anything in the XAML tree animates).
//
// SAFE ON PRODUCTION: nothing is suspended, injected, or written. It reads
// NtQuerySystemInformation(SystemProcessInformation), NtQueryInformationThread(class 9 =
// ThreadQuerySetWin32StartAddress), GetThreadDescription, GetThreadTimes and the process's module
// list. Unlike tools/livesample.exe it never stops a thread, so it cannot freeze the UI.
//
// Caveats:
//   - Per-thread CPU comes from the kernel's tick-based accounting, so small values are quantized to
//     ~15.6 ms per sampling window (1.56 ms/s over 10 s). Wake-up counts are exact.
//   - Run it from a folder outside the repo if the repo's Directory.Build.props ever breaks a
//     file-based build (copy the file next to the scratch output; nothing else is needed).
using System.Diagnostics;
using System.Runtime.InteropServices;

var options = Census.ParseArguments(args);
var census = Census.Take(options.Pid, options.SampleMs);
Census.Print(census);

/// <summary>
/// The command line of one census run: which process, and how long to sample it.
/// </summary>
/// <param name="Pid">The process id to sample.</param>
/// <param name="SampleMs">The sampling window in milliseconds; longer windows smooth bursty threads.</param>
internal sealed record CensusOptions(int Pid, int SampleMs);

/// <summary>
/// One thread's identity and its activity over the sampling window. Identity is read once (start
/// address, description, creation time); activity is the delta between two kernel snapshots.
/// </summary>
/// <param name="Tid">The thread id.</param>
/// <param name="Start">The Win32 start address as <c>module+0xOFFSET</c>, or <c>?0xADDRESS</c> outside every module.</param>
/// <param name="Description">The thread description (SetThreadDescription), empty when none was set.</param>
/// <param name="Created">The thread's creation time (local), <see cref="DateTime.MinValue"/> when unreadable.</param>
/// <param name="CpuMsPerSecond">User + kernel CPU over the window, in milliseconds per second.</param>
/// <param name="WakesPerSecond">Context switches over the window, per second (each one is a wake-up).</param>
internal sealed record ThreadActivity(int Tid, string Start, string Description, DateTime Created, double CpuMsPerSecond, double WakesPerSecond)
{
    /// <summary>
    /// The start module alone (the part of <see cref="Start"/> before <c>+</c>) — the grouping key,
    /// because offsets differ per thread routine while the module says who owns the thread.
    /// </summary>
    public string Module => Start.Split('+')[0];

    /// <summary>
    /// The row label: the start module, plus the description in brackets when the thread has one
    /// (so the Renderer and ConPTY threads, which share their DLLs with other threads, stay apart).
    /// </summary>
    public string Category => Description.Length > 0 ? $"{Module} [{Description}]" : Module;

    /// <summary>True when the thread woke at least once during the window.</summary>
    public bool Active => WakesPerSecond > 0;
}

/// <summary>
/// The result of one census: every thread that lived through the whole window, and the window's
/// real length (sleep overshoot included, so the per-second rates stay honest).
/// </summary>
/// <param name="Pid">The sampled process id.</param>
/// <param name="Seconds">The measured window length in seconds.</param>
/// <param name="Threads">The threads present in both snapshots.</param>
internal sealed record CensusResult(int Pid, double Seconds, IReadOnlyList<ThreadActivity> Threads);

/// <summary>
/// Takes and prints the census. Every member is read-only with respect to the target process.
/// </summary>
internal static class Census
{
    /// <summary>The thread description AtlasEngine's Renderer gives its thread (renderer.cpp).</summary>
    private const string RenderThreadDescription = "Rendering Output Thread";

    /// <summary>NVIDIA's Direct3D 11 user-mode driver; it starts 20 threads per device.</summary>
    private const string NvidiaDriverModule = "nvwgf2umx.dll";

    /// <summary>How many NVIDIA driver threads one device starts (measured; see RENDERING.md §1).</summary>
    private const int NvidiaThreadsPerDevice = 20;

    /// <summary>
    /// Parses <c>&lt;pid&gt; [seconds]</c>.
    /// </summary>
    /// <param name="args">The raw command-line arguments.</param>
    /// <returns>The parsed options; the window defaults to 10 s and is clamped to 1–300 s.</returns>
    /// <exception cref="ArgumentException">No pid was given, or an argument is not a number.</exception>
    public static CensusOptions ParseArguments(string[] args)
    {
        if (args.Length < 1 || !int.TryParse(args[0], out var pid))
        {
            throw new ArgumentException("usage: dotnet run tools/threadcensus.cs -- <pid> [seconds=10]");
        }
        var seconds = 10;
        if (args.Length > 1 && !int.TryParse(args[1], out seconds))
        {
            throw new ArgumentException($"seconds must be a whole number, got '{args[1]}'");
        }
        return new CensusOptions(pid, Math.Clamp(seconds, 1, 300) * 1000);
    }

    /// <summary>
    /// Samples the process twice, <paramref name="sampleMs"/> apart, and returns per-thread deltas.
    /// The identity reads run between the snapshots, so they cost the window nothing.
    /// </summary>
    /// <param name="pid">The process to sample.</param>
    /// <param name="sampleMs">The window length in milliseconds.</param>
    /// <returns>Every thread alive in both snapshots, with its identity and activity.</returns>
    /// <exception cref="ArgumentException">The process does not exist.</exception>
    /// <exception cref="System.ComponentModel.Win32Exception">The module list cannot be read (another user's or an elevated process).</exception>
    /// <exception cref="InvalidOperationException">NtQuerySystemInformation failed.</exception>
    public static CensusResult Take(int pid, int sampleMs)
    {
        var modules = ReadModuleMap(pid);
        var before = SnapshotThreads(pid);
        var clock = Stopwatch.StartNew();

        // Identity first, inside the window: it is constant, and reading it here instead of before
        // the first snapshot keeps the window's start close to the process's real state.
        var identity = new Dictionary<int, (string Start, string Description, DateTime Created)>();
        foreach (var tid in before.Keys)
        {
            identity[tid] = ReadThreadIdentity(tid, modules);
        }

        var remaining = sampleMs - (int)clock.ElapsedMilliseconds;
        if (remaining > 0)
        {
            Thread.Sleep(remaining);
        }
        var after = SnapshotThreads(pid);
        var seconds = clock.Elapsed.TotalSeconds;

        var threads = new List<ThreadActivity>();
        foreach (var (tid, a) in before)
        {
            // A thread that exited inside the window has no second sample; its partial delta would
            // be misleading, so it is left out.
            if (!after.TryGetValue(tid, out var b))
            {
                continue;
            }
            var id = identity[tid];
            threads.Add(new ThreadActivity(
                tid,
                id.Start,
                id.Description,
                id.Created,
                (b.Cpu100ns - a.Cpu100ns) / 10000.0 / seconds,
                (b.ContextSwitches - a.ContextSwitches) / seconds));
        }
        return new CensusResult(pid, seconds, threads);
    }

    /// <summary>
    /// Prints the four sections described in the file header.
    /// </summary>
    /// <param name="census">The census to print.</param>
    public static void Print(CensusResult census)
    {
        var threads = census.Threads;
        Console.WriteLine($"pid {census.Pid}  threads {threads.Count}  window {census.Seconds:F1}s");
        Console.WriteLine($"{"category",-70} {"n",4} {"active",6} {"cpu ms/s",9} {"wakes/s",8}");
        foreach (var g in threads.GroupBy(t => t.Category).OrderByDescending(g => g.Sum(t => t.CpuMsPerSecond)).ThenByDescending(g => g.Count()))
        {
            Console.WriteLine($"{g.Key,-70} {g.Count(),4} {g.Count(t => t.Active),6} {g.Sum(t => t.CpuMsPerSecond),9:F2} {g.Sum(t => t.WakesPerSecond),8:F1}");
        }

        var render = threads.Where(t => t.Description == RenderThreadDescription).ToList();
        Console.WriteLine();
        Console.WriteLine($"render threads (= initialized terminals): {render.Count}, painted in window: {render.Count(t => t.Active)}");
        foreach (var t in render.Where(t => t.Active).OrderByDescending(t => t.WakesPerSecond))
        {
            Console.WriteLine($"  tid {t.Tid,6}  wakes/s {t.WakesPerSecond,6:F1}  cpu ms/s {t.CpuMsPerSecond,6:F2}  created {t.Created:MM-dd HH:mm:ss}");
        }

        var nvidia = threads.Where(t => t.Module.Equals(NvidiaDriverModule, StringComparison.OrdinalIgnoreCase)).OrderBy(t => t.Created).ToList();
        Console.WriteLine();
        Console.WriteLine($"{NvidiaDriverModule} threads: {nvidia.Count} => ~{nvidia.Count / (double)NvidiaThreadsPerDevice:F1} D3D11 devices");
        Console.WriteLine($"  {"device burst",-20} {"thr",4} {"active",6} {"wakes/s",8}  nearest render thread (its wakes/s)");
        foreach (var burst in SplitIntoDeviceBursts(nvidia))
        {
            var t0 = burst[0].Created;
            var nearest = render.OrderBy(r => Math.Abs((r.Created - t0).TotalMilliseconds)).FirstOrDefault();
            var pairing = nearest is null ? "-" : $"tid {nearest.Tid} dt {(nearest.Created - t0).TotalMilliseconds:F0}ms ({nearest.WakesPerSecond:F1})";
            Console.WriteLine($"  {t0:MM-dd HH:mm:ss.fff}   {burst.Count,4} {burst.Count(t => t.Active),6} {burst.Sum(t => t.WakesPerSecond),8:F1}  {pairing}");
        }

        Console.WriteLine();
        Console.WriteLine("XAML and UI threads (exact start):");
        foreach (var t in threads.Where(t => t.Module.StartsWith("Windows.UI.Xaml", StringComparison.OrdinalIgnoreCase)
                                             || t.Module.Equals("Agentmaster.exe", StringComparison.OrdinalIgnoreCase)
                                             || t.Module.Equals("WindowsTerminal.exe", StringComparison.OrdinalIgnoreCase)))
        {
            Console.WriteLine($"  {t.Start,-40} tid {t.Tid,6} wakes/s {t.WakesPerSecond,7:F1} cpu ms/s {t.CpuMsPerSecond,7:F2}");
        }
    }

    /// <summary>
    /// Cuts creation-ordered NVIDIA threads into per-device bursts: a new burst starts after a gap of
    /// more than 1.5 s or once a burst holds 20 threads (one device's worth).
    /// </summary>
    /// <param name="nvidiaByCreation">The NVIDIA threads, sorted by creation time.</param>
    /// <returns>The bursts, oldest first; a short burst means a device whose threads started late.</returns>
    private static List<List<ThreadActivity>> SplitIntoDeviceBursts(IReadOnlyList<ThreadActivity> nvidiaByCreation)
    {
        var bursts = new List<List<ThreadActivity>>();
        foreach (var t in nvidiaByCreation)
        {
            var startNew = bursts.Count == 0
                           || (t.Created - bursts[^1][^1].Created).TotalMilliseconds > 1500
                           || bursts[^1].Count >= NvidiaThreadsPerDevice;
            if (startNew)
            {
                bursts.Add(new List<ThreadActivity>());
            }
            bursts[^1].Add(t);
        }
        return bursts;
    }

    /// <summary>
    /// Reads the target's loaded modules as address ranges, to name each thread's start address.
    /// </summary>
    /// <param name="pid">The target process.</param>
    /// <returns>Each module's [base, end) range and file name.</returns>
    /// <exception cref="ArgumentException">The process does not exist.</exception>
    /// <exception cref="System.ComponentModel.Win32Exception">Access to the module list is denied.</exception>
    private static List<(ulong Base, ulong End, string Name)> ReadModuleMap(int pid)
    {
        var map = new List<(ulong, ulong, string)>();
        using var process = Process.GetProcessById(pid);
        foreach (ProcessModule m in process.Modules)
        {
            var b = (ulong)m.BaseAddress;
            map.Add((b, b + (ulong)m.ModuleMemorySize, m.ModuleName));
        }
        return map;
    }

    /// <summary>
    /// Reads one thread's start address, description and creation time. Each read is optional: a
    /// thread that exits or refuses access keeps the placeholder values instead of failing the run.
    /// </summary>
    /// <param name="tid">The thread id.</param>
    /// <param name="modules">The module map from <see cref="ReadModuleMap"/>.</param>
    /// <returns>The start as <c>module+0xOFFSET</c> (or <c>?</c>), the description, and the creation time.</returns>
    private static (string Start, string Description, DateTime Created) ReadThreadIdentity(int tid, List<(ulong Base, ulong End, string Name)> modules)
    {
        var start = "?";
        var description = "";
        var created = DateTime.MinValue;
        var handle = Native.OpenThread(Native.ThreadQueryInformation | Native.ThreadQueryLimitedInformation, false, (uint)tid);
        if (handle == 0)
        {
            return (start, description, created);
        }
        try
        {
            if (Native.NtQueryInformationThread(handle, Native.ThreadQuerySetWin32StartAddress, out var address, nint.Size, out _) == 0)
            {
                start = NameAddress((ulong)address, modules);
            }
            // GetThreadDescription allocates the string with LocalAlloc; the caller frees it.
            if (Native.GetThreadDescription(handle, out var text) >= 0 && text != 0)
            {
                description = Marshal.PtrToStringUni(text) ?? "";
                Native.LocalFree(text);
            }
            if (Native.GetThreadTimes(handle, out var creation, out _, out _, out _))
            {
                created = DateTime.FromFileTime(creation);
            }
        }
        finally
        {
            Native.CloseHandle(handle);
        }
        return (start, description, created);
    }

    /// <summary>
    /// Names an address as <c>module+0xOFFSET</c>.
    /// </summary>
    /// <param name="address">The address to name.</param>
    /// <param name="modules">The module map.</param>
    /// <returns>The module-relative name, or <c>?0xADDRESS</c> when no module contains it.</returns>
    private static string NameAddress(ulong address, List<(ulong Base, ulong End, string Name)> modules)
    {
        foreach (var m in modules)
        {
            if (address >= m.Base && address < m.End)
            {
                return $"{m.Name}+0x{address - m.Base:X}";
            }
        }
        return $"?0x{address:X}";
    }

    /// <summary>
    /// One SystemProcessInformation snapshot of the target's threads: CPU time and context switches.
    /// The buffer grows until the kernel's whole process list fits.
    /// </summary>
    /// <param name="pid">The target process.</param>
    /// <returns>Per thread id: total CPU (100 ns units) and the context-switch counter.</returns>
    /// <exception cref="InvalidOperationException">NtQuerySystemInformation failed with a status other than "buffer too small".</exception>
    private static Dictionary<int, (long Cpu100ns, uint ContextSwitches)> SnapshotThreads(int pid)
    {
        var result = new Dictionary<int, (long, uint)>();
        var length = 4 << 20;
        while (true)
        {
            var buffer = Marshal.AllocHGlobal(length);
            try
            {
                var status = Native.NtQuerySystemInformation(Native.SystemProcessInformation, buffer, length, out var needed);
                if (status == Native.StatusInfoLengthMismatch)
                {
                    // The process list grew between the size probe and the copy: retry with headroom.
                    length = Math.Max(length * 2, needed + 65536);
                    continue;
                }
                if (status != 0)
                {
                    throw new InvalidOperationException($"NtQuerySystemInformation failed: 0x{status:X8}");
                }
                ReadProcessThreads(buffer, pid, result);
                return result;
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }
    }

    /// <summary>
    /// Walks the SystemProcessInformation list and copies the target's thread rows. x64 layout:
    /// SYSTEM_PROCESS_INFORMATION is 256 bytes (UniqueProcessId at +80) and is followed by its
    /// SYSTEM_THREAD_INFORMATION array (80 bytes each: KernelTime +0, UserTime +8, ClientId.UniqueThread
    /// +48, ContextSwitches +64).
    /// </summary>
    /// <param name="buffer">The filled NtQuerySystemInformation buffer.</param>
    /// <param name="pid">The target process.</param>
    /// <param name="into">Receives one entry per thread of the target.</param>
    private static void ReadProcessThreads(nint buffer, int pid, Dictionary<int, (long, uint)> into)
    {
        long offset = 0;
        while (true)
        {
            var entry = buffer + (nint)offset;
            var next = (uint)Marshal.ReadInt32(entry, 0);
            var threadCount = (uint)Marshal.ReadInt32(entry, 4);
            if (Marshal.ReadInt64(entry, 80) == pid)
            {
                for (var i = 0; i < threadCount; i++)
                {
                    var t = entry + 256 + i * 80;
                    var cpu = Marshal.ReadInt64(t, 0) + Marshal.ReadInt64(t, 8);
                    var tid = (int)Marshal.ReadInt64(t, 48);
                    var switches = (uint)Marshal.ReadInt32(t, 64);
                    into[tid] = (cpu, switches);
                }
                return;
            }
            if (next == 0)
            {
                return;
            }
            offset += next;
        }
    }
}

/// <summary>
/// The Win32 / NT entry points the census uses. All of them only query.
/// </summary>
internal static class Native
{
    /// <summary>SYSTEM_INFORMATION_CLASS SystemProcessInformation.</summary>
    public const int SystemProcessInformation = 5;

    /// <summary>THREADINFOCLASS ThreadQuerySetWin32StartAddress: the address CreateThread was given.</summary>
    public const int ThreadQuerySetWin32StartAddress = 9;

    /// <summary>STATUS_INFO_LENGTH_MISMATCH: the buffer was too small.</summary>
    public const int StatusInfoLengthMismatch = unchecked((int)0xC0000004);

    /// <summary>THREAD_QUERY_INFORMATION (needed for the start address).</summary>
    public const uint ThreadQueryInformation = 0x0040;

    /// <summary>THREAD_QUERY_LIMITED_INFORMATION (enough for the description and the times).</summary>
    public const uint ThreadQueryLimitedInformation = 0x0800;

    /// <summary>Reads system-wide information; here the process + thread list.</summary>
    /// <param name="infoClass">The information class.</param>
    /// <param name="buffer">The output buffer.</param>
    /// <param name="length">The buffer size in bytes.</param>
    /// <param name="returnLength">The size the call needed.</param>
    /// <returns>An NTSTATUS; 0 is success.</returns>
    [DllImport("ntdll.dll")]
    public static extern int NtQuerySystemInformation(int infoClass, nint buffer, int length, out int returnLength);

    /// <summary>Reads one piece of thread information; here the Win32 start address.</summary>
    /// <param name="thread">A thread handle with THREAD_QUERY_INFORMATION.</param>
    /// <param name="infoClass">The information class.</param>
    /// <param name="value">Receives the value.</param>
    /// <param name="length">The size of <paramref name="value"/> in bytes.</param>
    /// <param name="returnLength">The size the call wrote.</param>
    /// <returns>An NTSTATUS; 0 is success.</returns>
    [DllImport("ntdll.dll")]
    public static extern int NtQueryInformationThread(nint thread, int infoClass, out nint value, int length, out int returnLength);

    /// <summary>Opens a thread for querying.</summary>
    /// <param name="access">The requested access rights.</param>
    /// <param name="inherit">Whether child processes inherit the handle (never needed here).</param>
    /// <param name="threadId">The thread id.</param>
    /// <returns>The handle, or 0 on failure.</returns>
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern nint OpenThread(uint access, bool inherit, uint threadId);

    /// <summary>Closes a handle.</summary>
    /// <param name="handle">The handle to close.</param>
    /// <returns>True on success.</returns>
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(nint handle);

    /// <summary>Reads a thread's description (set with SetThreadDescription).</summary>
    /// <param name="thread">A thread handle with THREAD_QUERY_LIMITED_INFORMATION.</param>
    /// <param name="description">Receives a LocalAlloc'ed string the caller frees with <see cref="LocalFree"/>.</param>
    /// <returns>An HRESULT; non-negative is success.</returns>
    [DllImport("kernel32.dll")]
    public static extern int GetThreadDescription(nint thread, out nint description);

    /// <summary>Frees memory returned by <see cref="GetThreadDescription"/>.</summary>
    /// <param name="memory">The memory to free.</param>
    /// <returns>0 on success.</returns>
    [DllImport("kernel32.dll")]
    public static extern nint LocalFree(nint memory);

    /// <summary>Reads a thread's creation, exit, kernel and user times (FILETIME units).</summary>
    /// <param name="thread">A thread handle with THREAD_QUERY_LIMITED_INFORMATION.</param>
    /// <param name="creation">The creation time.</param>
    /// <param name="exit">The exit time (unset while the thread runs).</param>
    /// <param name="kernel">The kernel-mode time.</param>
    /// <param name="user">The user-mode time.</param>
    /// <returns>True on success.</returns>
    [DllImport("kernel32.dll")]
    public static extern bool GetThreadTimes(nint thread, out long creation, out long exit, out long kernel, out long user);
}
