using System.Runtime.InteropServices;
using System.Text.Json;
using Amb.Core.Json;
using Amb.Core.Protocol;

// amb-hostfacts [drive]: one line of JSON (Amb.Core.Protocol.HostFactsReport) about this Windows, for the coordinator.
// The drive defaults to Q:\ (the site's dedicated folder lives there). Exit code 0, or 1 when a Windows call failed (the
// message goes to stderr and nothing to stdout, so the coordinator never reads half a report).

string drive = args.Length > 0 ? args[0] : @"Q:\";
try
{
    HostFacts.MemoryStatus memory = HostFacts.Memory();
    long qFree = HostFacts.FreeBytes(drive) / (1024 * 1024);
    int state = HostFacts.UserNotificationState();
    var report = new HostFactsReport(
        DateTimeOffset.UtcNow,
        (long)(memory.AvailPhys / (1024 * 1024)),
        (long)(memory.AvailPageFile / (1024 * 1024)),
        qFree,
        FullScreenGame: state == HostFacts.QunsRunningD3dFullScreen,
        UserState: state);
    Console.Out.WriteLine(JsonSerializer.Serialize(report, AmbJsonLine.Default.HostFactsReport));
    return 0;
}
catch (InvalidOperationException ex)
{
    Console.Error.WriteLine(ex.Message);
    return 1;
}

/// <summary>The three Windows calls behind a host-facts report.</summary>
internal static partial class HostFacts
{
    /// <summary>SHQueryUserNotificationState's QUNS_RUNNING_D3D_FULL_SCREEN: a full-screen Direct3D program (a game) runs.</summary>
    internal const int QunsRunningD3dFullScreen = 3;

    /// <summary>MEMORYSTATUSEX (sysinfoapi.h).</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct MemoryStatus
    {
        /// <summary>dwLength: the struct's size, set by the caller.</summary>
        public uint Length;

        /// <summary>dwMemoryLoad: percent of physical memory in use.</summary>
        public uint MemoryLoad;

        /// <summary>ullTotalPhys.</summary>
        public ulong TotalPhys;

        /// <summary>ullAvailPhys: Windows' available physical memory (what DESIGN.md §3.2 calls "available").</summary>
        public ulong AvailPhys;

        /// <summary>ullTotalPageFile: the commit limit.</summary>
        public ulong TotalPageFile;

        /// <summary>ullAvailPageFile: free commit.</summary>
        public ulong AvailPageFile;

        /// <summary>ullTotalVirtual.</summary>
        public ulong TotalVirtual;

        /// <summary>ullAvailVirtual.</summary>
        public ulong AvailVirtual;

        /// <summary>ullAvailExtendedVirtual: always 0.</summary>
        public ulong AvailExtendedVirtual;
    }

    /// <summary>Reads Windows' memory status.</summary>
    /// <returns>The filled struct.</returns>
    /// <exception cref="InvalidOperationException">GlobalMemoryStatusEx failed.</exception>
    internal static MemoryStatus Memory()
    {
        var status = new MemoryStatus { Length = (uint)Marshal.SizeOf<MemoryStatus>() };
        if (!GlobalMemoryStatusEx(ref status))
        {
            throw new InvalidOperationException($"GlobalMemoryStatusEx failed: {Marshal.GetLastPInvokeError()}");
        }
        return status;
    }

    /// <summary>The free space a drive has for this user.</summary>
    /// <param name="root">The drive's root, e.g. "Q:\".</param>
    /// <returns>Free bytes.</returns>
    /// <exception cref="InvalidOperationException">GetDiskFreeSpaceExW failed (no such drive).</exception>
    internal static long FreeBytes(string root)
    {
        if (!GetDiskFreeSpaceExW(root, out ulong free, out _, out _))
        {
            throw new InvalidOperationException($"GetDiskFreeSpaceExW({root}) failed: {Marshal.GetLastPInvokeError()}");
        }
        return (long)free;
    }

    /// <summary>The signed-in user's notification state (presentation mode, a full-screen game, quiet time).</summary>
    /// <returns>The QUERY_USER_NOTIFICATION_STATE value, or 0 when the call failed (unknown: treated as no game).</returns>
    internal static int UserNotificationState() => SHQueryUserNotificationState(out int state) == 0 ? state : 0;

    /// <summary>GlobalMemoryStatusEx (kernel32).</summary>
    /// <param name="buffer">The struct to fill.</param>
    /// <returns>True on success.</returns>
    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GlobalMemoryStatusEx(ref MemoryStatus buffer);

    /// <summary>GetDiskFreeSpaceExW (kernel32).</summary>
    /// <param name="directory">A folder on the drive.</param>
    /// <param name="freeToCaller">Free bytes this user may use.</param>
    /// <param name="total">The drive's size.</param>
    /// <param name="totalFree">Free bytes in all.</param>
    /// <returns>True on success.</returns>
    [LibraryImport("kernel32.dll", StringMarshalling = StringMarshalling.Utf16, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetDiskFreeSpaceExW(string directory, out ulong freeToCaller, out ulong total,
        out ulong totalFree);

    /// <summary>SHQueryUserNotificationState (shell32).</summary>
    /// <param name="state">Receives the state.</param>
    /// <returns>An HRESULT (0 on success).</returns>
    [LibraryImport("shell32.dll")]
    private static partial int SHQueryUserNotificationState(out int state);
}
