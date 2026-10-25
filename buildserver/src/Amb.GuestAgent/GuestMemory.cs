using System.Runtime.InteropServices;

namespace Amb.GuestAgent;

/// <summary>
/// The guest's memory as Windows itself counts it: physical memory, what is available of it, and the commit charge.
/// </summary>
/// <remarks>
/// One GlobalMemoryStatusEx call (microseconds) instead of WMI or performance counters: the agent reports every 10 s
/// for the whole life of a VM, and Tiny11 Core's WMI is slow to start and can be trimmed. The commit charge is the
/// commit limit minus what is still available of it, which is what the balloon's target follows (DESIGN.md §3.4).
/// </remarks>
internal static partial class GuestMemory
{
    /// <summary>A snapshot of the guest's memory, in MiB.</summary>
    /// <param name="TotalMiB">Physical memory Windows sees (after the balloon took its share).</param>
    /// <param name="AvailableMiB">Available physical memory.</param>
    /// <param name="CommitMiB">Committed memory.</param>
    /// <param name="CommitLimitMiB">The commit limit (physical memory plus the page file).</param>
    internal readonly record struct Snapshot(long TotalMiB, long AvailableMiB, long CommitMiB, long CommitLimitMiB);

    /// <summary>Reads the guest's memory now.</summary>
    /// <returns>The snapshot.</returns>
    /// <exception cref="InvalidOperationException">GlobalMemoryStatusEx failed (it does not, short of a broken
    /// Windows; the message carries the Win32 error).</exception>
    internal static Snapshot Read()
    {
        var status = new MemoryStatusEx { Length = (uint)Marshal.SizeOf<MemoryStatusEx>() };
        if (!GlobalMemoryStatusEx(ref status))
        {
            throw new InvalidOperationException($"GlobalMemoryStatusEx failed: {Marshal.GetLastPInvokeError()}");
        }
        const long MiB = 1024 * 1024;
        long commit = (long)(status.TotalPageFile - status.AvailPageFile);
        return new Snapshot((long)(status.TotalPhys / MiB), (long)(status.AvailPhys / MiB), commit / MiB,
            (long)(status.TotalPageFile / MiB));
    }

    /// <summary>MEMORYSTATUSEX (sysinfoapi.h), field for field.</summary>
    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryStatusEx
    {
        /// <summary>dwLength: the struct's size, set by the caller.</summary>
        public uint Length;

        /// <summary>dwMemoryLoad: percent of physical memory in use.</summary>
        public uint MemoryLoad;

        /// <summary>ullTotalPhys.</summary>
        public ulong TotalPhys;

        /// <summary>ullAvailPhys.</summary>
        public ulong AvailPhys;

        /// <summary>ullTotalPageFile: the commit limit.</summary>
        public ulong TotalPageFile;

        /// <summary>ullAvailPageFile: what is left of the commit limit.</summary>
        public ulong AvailPageFile;

        /// <summary>ullTotalVirtual: the process's user address space.</summary>
        public ulong TotalVirtual;

        /// <summary>ullAvailVirtual.</summary>
        public ulong AvailVirtual;

        /// <summary>ullAvailExtendedVirtual: always 0.</summary>
        public ulong AvailExtendedVirtual;
    }

    /// <summary>GlobalMemoryStatusEx (kernel32).</summary>
    /// <param name="buffer">The struct to fill; its Length must be set.</param>
    /// <returns>True on success.</returns>
    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GlobalMemoryStatusEx(ref MemoryStatusEx buffer);
}
