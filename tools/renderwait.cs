#:property PublishAot=false
#:property AllowUnsafeBlocks=true
// Agentmaster: renderwait — a READ-ONLY probe of every terminal render thread in a running Agentmaster
// (or Windows Terminal) process: which Renderer flag the thread is blocked on, the Renderer's flags
// read from memory, and when the thread entered its current wait.
// The evidence behind doc/agentmaster/RENDERING.md §4c ("Second check") and step 4 of §5.
//
//   dotnet run tools/renderwait.cs -- <pid>
//
// What it prints:
//   1. The process's visible top-level windows, with minimized=True/False (a minimized window suspends
//      even its on-screen terminal, §4c).
//   2. One row per "Rendering Output Thread" (one per INITIALIZED terminal):
//        _suspended (parked)    — the terminal is off screen (§4c). "waiting since" is the release time
//                                 once the device was given back (the thread then waits with no timeout).
//        _redraw (idle, shown)  — the terminal is drawable and waits for its next frame.
//        _enable (painting off) — painting is disabled: the renderer gave up after repeated failures,
//                                 or the terminal is torn down. A visible black tab in this state is the
//                                 renderer's error path; one in the _redraw state believes it presents fine.
//      plus the flags read at the Renderer's address (enable / redraw / keepRunning / suspended /
//      the release delay in ms), so the inference from the wait address is checked against memory.
//
// SAFE ON PRODUCTION: nothing is suspended, injected or written. It reads
//   - NtQueryInformationThread(ThreadLastSystemCall = 21), which needs THREAD_GET_CONTEXT and answers
//     only for a WAITING thread: WaitOnAddress ends in NtWaitForAlertByThreadId(address, timeout), so
//     the call's first argument is the address the thread waits on;
//   - NtQuerySystemInformation(SystemProcessInformation), whose per-thread WaitTime is the low 32 bits
//     of the interrupt tick count when the wait began (KTHREAD.WaitTime) — dated against
//     KUSER_SHARED_DATA's tick count, the pair GetTickCount64 uses. Verified 2026-10-03: five parked
//     threads dated 27–49 ms after "hidden + 60 s", hide times taken from hooks.log;
//   - GetThreadDescription, ReadProcessMemory (PROCESS_VM_READ) and the window list.
//
// Renderer layout this reads (src/renderer/base/renderer.hpp; no vtable, heap-allocated, so 16-byte
// aligned). KEEP IN STEP with the member order there — a new member before _suspendedReleaseDelayMs
// moves these offsets:
//   +0x00 _renderSettings   +0x08 _pData   +0x10 _threadMutex   +0x18 _thread
//   +0x20 _enable (wil slim_event, a LONG)        +0x24 _redraw (atomic<bool>)
//   +0x25 _threadKeepRunning                      +0x26 _suspended (atomic<bool>, §4c)
//   +0x28 _suspendedReleaseDelayMs (atomic<DWORD>, §4c; 0xFFFFFFFF = never)
//
// Caveats:
//   - A thread that is running (or between waits) at the instant of the query prints "not waiting";
//     run it again.
//   - Run it from a folder outside the repo if the repo's Directory.Build.props ever breaks a
//     file-based build (copy the file next to the scratch output; nothing else is needed).
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

[assembly: System.Runtime.Versioning.SupportedOSPlatform("windows")]

if (args.Length < 1 || !int.TryParse(args[0], out var pid))
{
    Console.Error.WriteLine("usage: dotnet run tools/renderwait.cs -- <pid>");
    return 2;
}

// PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION: read memory and query; nothing that changes the target.
using var process = Native.OpenProcess(0x0010 | 0x1000, false, pid);
if (process.IsInvalid)
{
    Console.Error.WriteLine($"OpenProcess({pid}) failed: {new Win32Exception(Marshal.GetLastWin32Error()).Message}");
    return 1;
}

Console.WriteLine($"pid {pid}  {DateTime.Now:HH:mm:ss}");
RenderWait.PrintWindows(pid);
Console.WriteLine();

// One kernel snapshot gives every waiting thread's wait start; the per-thread queries follow it.
var waitStarts = RenderWait.WaitStartTimes(pid);
Console.WriteLine("render threads (\"Rendering Output Thread\"):");
Console.WriteLine("  tid      created   state/reason         wait address        field                 enable redraw keep susp delayMs  waiting since");

var renderThreads = 0;
foreach (ProcessThread thread in Process.GetProcessById(pid).Threads)
{
    var row = RenderWait.InspectRenderThread(process, thread);
    if (row is null)
    {
        continue;
    }
    renderThreads++;
    var since = waitStarts.TryGetValue(thread.Id, out var start) ? $"  {start:HH:mm:ss.fff} ({(DateTime.Now - start).TotalSeconds:F0} s)" : "";
    Console.WriteLine(row + since);
}
Console.WriteLine($"render threads: {renderThreads}");
return 0;

/// <summary>
/// The probe's steps. Every member only reads the target: no thread is suspended and nothing is written.
/// </summary>
internal static class RenderWait
{
    /// <summary>The thread description every terminal's Renderer thread sets (renderer.cpp EnablePainting).</summary>
    private const string RenderThreadDescription = "Rendering Output Thread";

    /// <summary>
    /// Describes one thread when it is a terminal render thread. Reads its description, its last
    /// system call (only answered while it waits) and, when the wait address lands on a known Renderer
    /// field, the Renderer's flags from the target's memory.
    /// </summary>
    /// <param name="process">The target process, opened with PROCESS_VM_READ.</param>
    /// <param name="thread">One thread of the target.</param>
    /// <returns>A formatted row, or null for a thread that is not a render thread or cannot be opened.</returns>
    /// <remarks>
    /// Never throws for an ordinary failure (the thread exited, access was denied): such a thread is
    /// skipped, or its row reports what could not be read.
    /// </remarks>
    public static string? InspectRenderThread(SafeProcessHandle process, ProcessThread thread)
    {
        // THREAD_QUERY_LIMITED_INFORMATION (description) | THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT
        // (required by ThreadLastSystemCall). None of these rights suspends or changes the thread.
        using var handle = Native.OpenThread(0x0800 | 0x0040 | 0x0008, false, thread.Id);
        if (handle.IsInvalid)
        {
            return null;
        }
        if (!string.Equals(Description(handle), RenderThreadDescription, StringComparison.Ordinal))
        {
            return null;
        }

        string state;
        try
        {
            state = thread.ThreadState == System.Diagnostics.ThreadState.Wait ? $"Wait/{thread.WaitReason}" : thread.ThreadState.ToString();
        }
        catch (InvalidOperationException)
        {
            // The thread exited after the enumeration; its row still prints with what is known.
            state = "?";
        }

        var created = "?";
        try
        {
            created = thread.StartTime.ToString("HH:mm:ss");
        }
        catch (Exception ex) when (ex is InvalidOperationException or Win32Exception)
        {
            // Same: an exited thread has no readable start time.
        }

        if (!TryLastSystemCall(handle, out var waitAddress))
        {
            return $"  {thread.Id,-8} {created}  {state,-20} (not waiting in a system call right now; run it again)";
        }

        // Which field the address lands on, by its offset inside the 16-byte-aligned Renderer.
        var (field, offset) = (waitAddress & 0xF) switch
        {
            0x0 => ("_enable (painting off)", 0x20UL),
            0x4 => ("_redraw (idle, shown)", 0x24UL),
            0x6 => ("_suspended (parked)", 0x26UL),
            _ => ("? (not a Renderer flag)", 0UL),
        };
        var flags = "";
        if (offset != 0 && TryReadRendererFlags(process, waitAddress - offset, out var enable, out var redraw, out var keepRunning, out var suspended, out var delayMs))
        {
            var delayText = delayMs == 0xFFFFFFFF ? "never" : delayMs.ToString();
            flags = $"{enable,6} {redraw,6} {keepRunning,4} {suspended,4} {delayText,7}";
        }
        return $"  {thread.Id,-8} {created}  {state,-20} 0x{waitAddress:X16}  {field,-21} {flags}";
    }

    /// <summary>
    /// Prints the target's visible top-level windows with their minimized state, so a minimized window
    /// (every terminal suspended, §4c) is told apart from a normal one.
    /// </summary>
    /// <param name="pid">The target process id.</param>
    public static void PrintWindows(int pid)
    {
        Native.EnumWindows((hwnd, _) =>
        {
            Native.GetWindowThreadProcessId(hwnd, out var owner);
            if (owner != pid || !Native.IsWindowVisible(hwnd))
            {
                return true;
            }
            var title = new StringBuilder(256);
            Native.GetWindowText(hwnd, title, title.Capacity);
            var className = new StringBuilder(256);
            Native.GetClassName(hwnd, className, className.Capacity);
            Console.WriteLine($"window 0x{hwnd:X}  minimized={Native.IsIconic(hwnd)}  class={className}  title=\"{title}\"");
            return true;
        }, IntPtr.Zero);
    }

    /// <summary>
    /// For every WAITING thread of the process, the local time at which it entered its current wait:
    /// "now" minus the ticks elapsed since SYSTEM_THREAD_INFORMATION.WaitTime (KTHREAD.WaitTime, the low
    /// 32 bits of the interrupt tick count at the wait's start). The current tick count and its
    /// milliseconds multiplier are read from KUSER_SHARED_DATA in THIS process — it is mapped at the same
    /// address in every process and the tick counter is machine-wide.
    /// </summary>
    /// <param name="pid">The target process id.</param>
    /// <returns>Thread id → wait start (local time). Threads that are not waiting are left out.</returns>
    /// <exception cref="Win32Exception">NtQuerySystemInformation failed for a reason other than a short buffer.</exception>
    public static Dictionary<int, DateTime> WaitStartTimes(int pid)
    {
        var result = new Dictionary<int, DateTime>();
        var length = 4 << 20;
        while (true)
        {
            var buffer = Marshal.AllocHGlobal(length);
            try
            {
                var status = Native.NtQuerySystemInformation(5, buffer, length, out var needed);
                if (status == unchecked((int)0xC0000004))
                {
                    // STATUS_INFO_LENGTH_MISMATCH: the process table grew; retry with room to spare.
                    length = Math.Max(length * 2, needed + (1 << 16));
                    continue;
                }
                if (status != 0)
                {
                    throw new Win32Exception($"NtQuerySystemInformation(5) = 0x{status:X8}");
                }
                // KUSER_SHARED_DATA: TickCountQuad at 0x7FFE0320, TickCountMultiplier at 0x7FFE0004.
                var nowTicksLow = (uint)Marshal.ReadInt64(new IntPtr(0x7FFE0320));
                var multiplier = (uint)Marshal.ReadInt32(new IntPtr(0x7FFE0004));
                var now = DateTime.Now;
                long offset = 0;
                while (true)
                {
                    var entry = buffer + (nint)offset;
                    var next = (uint)Marshal.ReadInt32(entry, 0);
                    var threadCount = (uint)Marshal.ReadInt32(entry, 4);
                    // SYSTEM_PROCESS_INFORMATION (x64): UniqueProcessId at 0x50; the thread array starts at 0x100.
                    var processId = (long)Marshal.ReadIntPtr(entry, 0x50);
                    if (processId == pid)
                    {
                        for (var i = 0; i < threadCount; i++)
                        {
                            // SYSTEM_THREAD_INFORMATION (x64, 80 bytes): WaitTime at 24, ClientId.UniqueThread
                            // at 48, ThreadState at 68 (5 = Waiting). WaitTime is stale for other states.
                            var thread = entry + 0x100 + (i * 80);
                            if (Marshal.ReadInt32(thread, 68) != 5)
                            {
                                continue;
                            }
                            var waitTicks = (uint)Marshal.ReadInt32(thread, 24);
                            var tid = (int)(long)Marshal.ReadIntPtr(thread, 48);
                            // Unsigned subtraction: correct across the 32-bit wrap (every ~776 days at 15.6 ms).
                            var elapsedTicks = unchecked(nowTicksLow - waitTicks);
                            var elapsedMs = ((ulong)elapsedTicks * multiplier) >> 24;
                            result[tid] = now - TimeSpan.FromMilliseconds(elapsedMs);
                        }
                        return result;
                    }
                    if (next == 0)
                    {
                        return result;
                    }
                    offset += next;
                }
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }
    }

    /// <summary>Reads a thread's description (SetThreadDescription).</summary>
    /// <param name="handle">A thread handle with THREAD_QUERY_LIMITED_INFORMATION.</param>
    /// <returns>The description, or an empty string when none is set or the read fails.</returns>
    private static string Description(SafeThreadHandle handle)
    {
        if (Native.GetThreadDescription(handle, out var text) < 0 || text == IntPtr.Zero)
        {
            return "";
        }
        try
        {
            return Marshal.PtrToStringUni(text) ?? "";
        }
        finally
        {
            Native.LocalFree(text);
        }
    }

    /// <summary>
    /// Reads a waiting thread's last system call and returns its first argument.
    /// </summary>
    /// <param name="handle">A thread handle with THREAD_GET_CONTEXT.</param>
    /// <param name="firstArgument">The first argument of the last system call: for a WaitOnAddress waiter, the address waited on.</param>
    /// <returns>True when the kernel answered; it refuses for a thread that is not in a wait.</returns>
    private static bool TryLastSystemCall(SafeThreadHandle handle, out ulong firstArgument)
    {
        firstArgument = 0;
        // ThreadLastSystemCall = 21. THREAD_LAST_SYSCALL_INFORMATION is 24 bytes on current Windows
        // (it gained WaitTime) and 16 bytes on older builds, so the long form is tried first.
        Span<byte> buffer = stackalloc byte[24];
        foreach (var length in new[] { 24, 16 })
        {
            unsafe
            {
                fixed (byte* p = buffer)
                {
                    if (Native.NtQueryInformationThread(handle, 21, (IntPtr)p, length, out _) == 0)
                    {
                        firstArgument = BitConverter.ToUInt64(buffer[..8]);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    /// <summary>
    /// Reads Renderer::_enable, _redraw, _threadKeepRunning, _suspended and _suspendedReleaseDelayMs from
    /// the target's memory.
    /// </summary>
    /// <param name="process">The target process, opened with PROCESS_VM_READ.</param>
    /// <param name="rendererBase">The Renderer object's address in the target (wait address − field offset).</param>
    /// <param name="enable">The slim event's state: 1 = painting enabled.</param>
    /// <param name="redraw">The pending-frame flag: 1 = a frame is due.</param>
    /// <param name="keepRunning">The render thread's keep-running flag.</param>
    /// <param name="suspended">The §4c suspension flag: 1 = off screen.</param>
    /// <param name="delayMs">The §4c release delay in milliseconds; 0xFFFFFFFF = never release.</param>
    /// <returns>True when all 12 bytes (+0x20 to +0x2B) were read.</returns>
    private static bool TryReadRendererFlags(SafeProcessHandle process, ulong rendererBase, out int enable, out byte redraw, out byte keepRunning, out byte suspended, out uint delayMs)
    {
        enable = 0;
        redraw = keepRunning = suspended = 0;
        delayMs = 0;
        var bytes = new byte[12];
        if (!Native.ReadProcessMemory(process, (IntPtr)(long)(rendererBase + 0x20), bytes, bytes.Length, out var read) || read != bytes.Length)
        {
            return false;
        }
        enable = BitConverter.ToInt32(bytes, 0);
        redraw = bytes[4];
        keepRunning = bytes[5];
        suspended = bytes[6];
        delayMs = BitConverter.ToUInt32(bytes, 8);
        return true;
    }
}

/// <summary>An owned process handle; closed on dispose.</summary>
internal sealed class SafeProcessHandle : Microsoft.Win32.SafeHandles.SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>Creates an empty owned handle; P/Invoke fills it in.</summary>
    public SafeProcessHandle() : base(true) { }

    /// <inheritdoc/>
    protected override bool ReleaseHandle() => Native.CloseHandle(handle);
}

/// <summary>An owned thread handle; closed on dispose.</summary>
internal sealed class SafeThreadHandle : Microsoft.Win32.SafeHandles.SafeHandleZeroOrMinusOneIsInvalid
{
    /// <summary>Creates an empty owned handle; P/Invoke fills it in.</summary>
    public SafeThreadHandle() : base(true) { }

    /// <inheritdoc/>
    protected override bool ReleaseHandle() => Native.CloseHandle(handle);
}

/// <summary>The Win32 and native calls the probe uses. Every one of them only queries.</summary>
internal static class Native
{
    /// <summary>The EnumWindows callback: return true to continue the enumeration.</summary>
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern SafeProcessHandle OpenProcess(uint access, bool inherit, int pid);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern SafeThreadHandle OpenThread(uint access, bool inherit, int tid);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll")]
    public static extern int GetThreadDescription(SafeThreadHandle thread, out IntPtr description);

    [DllImport("kernel32.dll")]
    public static extern IntPtr LocalFree(IntPtr memory);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadProcessMemory(SafeProcessHandle process, IntPtr address, byte[] buffer, int size, out int read);

    [DllImport("ntdll.dll")]
    public static extern int NtQueryInformationThread(SafeThreadHandle thread, int infoClass, IntPtr info, int length, out int returned);

    [DllImport("ntdll.dll")]
    public static extern int NtQuerySystemInformation(int infoClass, IntPtr info, int length, out int returned);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out int pid);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hwnd);

    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hwnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int max);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hwnd, StringBuilder text, int max);
}
