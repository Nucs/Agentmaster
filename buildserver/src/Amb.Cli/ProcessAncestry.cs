using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Amb.Cli;

/// <summary>
/// Finds this process's ancestors on Windows (Toolhelp32's process list), for `amb install`: the nearest claude.exe
/// ancestor is the Claude session that asked, whose turn the install waits for before the swap ends its claude.
/// </summary>
/// <remarks>
/// <para>A parent process id outlives its process: Windows reuses ids, so a listed "parent" may be a newer, unrelated
/// process (seen on this PC: an explorer.exe whose recorded parent id belonged to a later OpenConsole.exe). The walk
/// therefore stops at a parent that started AFTER its child.</para>
/// </remarks>
internal static partial class ProcessAncestry
{
    /// <summary>Toolhelp32's flag for a snapshot of every process.</summary>
    private const uint SnapProcess = 0x00000002;

    /// <summary>The longest walk: a deeper chain means a cycle of reused ids.</summary>
    private const int MaxDepth = 64;

    /// <summary>One process of a Toolhelp32 snapshot (PROCESSENTRY32W).</summary>
    [StructLayout(LayoutKind.Sequential)]
    private unsafe struct ProcessEntry32
    {
        /// <summary>The structure's size, set before the first call.</summary>
        public uint Size;

        /// <summary>Unused (always 0).</summary>
        public uint Usage;

        /// <summary>The process id.</summary>
        public uint ProcessId;

        /// <summary>Unused.</summary>
        public nuint DefaultHeapId;

        /// <summary>Unused.</summary>
        public uint ModuleId;

        /// <summary>The process's thread count.</summary>
        public uint Threads;

        /// <summary>The id of the process that created it (possibly reused since).</summary>
        public uint ParentProcessId;

        /// <summary>The base priority of its threads.</summary>
        public int PriorityClassBase;

        /// <summary>Unused.</summary>
        public uint Flags;

        /// <summary>The executable's file name (MAX_PATH UTF-16 characters, NUL-terminated).</summary>
        public fixed char ExeFile[260];
    }

    /// <summary>Takes a snapshot of the processes.</summary>
    /// <param name="flags">What to take (<see cref="SnapProcess"/>).</param>
    /// <param name="processId">Unused for process snapshots (0).</param>
    /// <returns>The snapshot's handle, or INVALID_HANDLE_VALUE (-1).</returns>
    [LibraryImport("kernel32.dll", SetLastError = true)]
    private static partial nint CreateToolhelp32Snapshot(uint flags, uint processId);

    /// <summary>Reads a snapshot's first process.</summary>
    /// <param name="snapshot">The snapshot.</param>
    /// <param name="entry">Receives the process (its Size set beforehand).</param>
    /// <returns>False when the snapshot is empty.</returns>
    [LibraryImport("kernel32.dll", EntryPoint = "Process32FirstW", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static unsafe partial bool Process32First(nint snapshot, ProcessEntry32* entry);

    /// <summary>Reads a snapshot's next process.</summary>
    /// <param name="snapshot">The snapshot.</param>
    /// <param name="entry">Receives the process.</param>
    /// <returns>False after the last one.</returns>
    [LibraryImport("kernel32.dll", EntryPoint = "Process32NextW", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static unsafe partial bool Process32Next(nint snapshot, ProcessEntry32* entry);

    /// <summary>Closes a handle.</summary>
    /// <param name="handle">The handle.</param>
    /// <returns>Whether it was closed.</returns>
    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool CloseHandle(nint handle);

    /// <summary>The nearest ancestor of this process with an executable name.</summary>
    /// <param name="exeName">The name (claude.exe), compared without case.</param>
    /// <returns>Its process id, or null when no living ancestor has that name (or the list cannot be read).</returns>
    internal static int? NearestAncestor(string exeName)
    {
        Dictionary<int, (int Parent, string Name)> table = Snapshot();
        int child = Environment.ProcessId;
        DateTime childStart = StartTime(child) ?? DateTime.MaxValue;
        for (int depth = 0; depth < MaxDepth && table.TryGetValue(child, out var me); depth++)
        {
            int parent = me.Parent;
            if (parent == 0 || !table.TryGetValue(parent, out var up))
            {
                return null;
            }
            // A reused id: the "parent" started after its child, so it is a stranger and the chain ends here.
            if (StartTime(parent) is not { } parentStart || parentStart > childStart)
            {
                return null;
            }
            if (string.Equals(up.Name, exeName, StringComparison.OrdinalIgnoreCase))
            {
                return parent;
            }
            child = parent;
            childStart = parentStart;
        }
        return null;
    }

    /// <summary>When a process started.</summary>
    /// <param name="processId">The process.</param>
    /// <returns>Its start time, or null when it is gone or may not be read (another user's, an elevated one).</returns>
    private static DateTime? StartTime(int processId)
    {
        try
        {
            using Process process = Process.GetProcessById(processId);
            return process.StartTime;
        }
        catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception
            or NotSupportedException)
        {
            // Expected: the process ended, or its times may not be read; the walk stops there.
            return null;
        }
    }

    /// <summary>Every process's parent id and executable name.</summary>
    /// <returns>The table, keyed by process id; empty when the snapshot cannot be taken.</returns>
    private static unsafe Dictionary<int, (int Parent, string Name)> Snapshot()
    {
        var table = new Dictionary<int, (int Parent, string Name)>();
        nint snapshot = CreateToolhelp32Snapshot(SnapProcess, 0);
        if (snapshot == -1)
        {
            return table;
        }
        try
        {
            ProcessEntry32 entry = default;
            entry.Size = (uint)sizeof(ProcessEntry32);
            for (bool ok = Process32First(snapshot, &entry); ok; ok = Process32Next(snapshot, &entry))
            {
                table[(int)entry.ProcessId] = ((int)entry.ParentProcessId, new string(entry.ExeFile));
            }
        }
        finally
        {
            // Cleanup in every outcome: a snapshot handle holds the whole process list.
            CloseHandle(snapshot);
        }
        return table;
    }
}
