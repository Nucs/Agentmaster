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
/// <para><b>The walk alone misses a session that ran amb from a script</b> (<see cref="RequestingClaude"/> adds the
/// fallback): when an MSYS program (Git Bash) runs another one (`bash script.sh`), the Windows process that bash forked
/// for it exits as soon as the new program starts, so the new bash's recorded parent is gone and the chain ends before
/// claude.exe. Measured 2026-11-04: every production install from 2026-11-03 21:34 on came from such a script and
/// carried no requester, so none waited for its session's turn; one session's swap cut its own turn off, Claude Code
/// continued the turn after the restart, and the turn re-ran its install: production restarted 6 times in 9 hours.</para>
/// </remarks>
internal static partial class ProcessAncestry
{
    /// <summary>The variable Claude Code puts in the environment of every tool call it runs: its own process id
    /// (<c>CLAUDE_PID: String(process.pid)</c> in claude.exe's bundle, read 2026-11-04). Every descendant inherits it,
    /// however broken the Windows parent chain between them is.</summary>
    internal const string ClaudePidVariable = "CLAUDE_PID";

    /// <summary>The executable name of a Claude Code session's process.</summary>
    internal const string ClaudeExe = "claude.exe";

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

    /// <summary>The Claude session this CLI runs under: the nearest claude.exe ancestor, else the claude.exe that this
    /// process's <see cref="ClaudePidVariable"/> names.</summary>
    /// <returns>The claude.exe's process id and whether it came from the environment (the walk found none), or null
    /// when neither names a living claude.exe (amb run by hand, by a scheduled task, by a Codex session).</returns>
    /// <remarks>
    /// <para>The walk comes first: it is proof by the process tree itself. The variable covers the chain a script
    /// breaks (see the class remarks).</para>
    /// <para>⚠ The variable can be stale: a long-lived program started from a session's tool call keeps it after that
    /// claude ended, and the id may since belong to another claude.exe that started earlier than this CLI. The install
    /// then waits for that session's turn too (at most its own-wait limit, then it refuses and changes nothing): an
    /// extra wait, never a swap that cuts a requester off.</para>
    /// </remarks>
    internal static (int Pid, bool FromEnvironment)? RequestingClaude()
    {
        if (NearestAncestor(ClaudeExe) is int ancestor)
        {
            return (ancestor, false);
        }
        Dictionary<int, (int Parent, string Name)> table = Snapshot();
        int? named = ClaudeFromVariable(Environment.GetEnvironmentVariable(ClaudePidVariable), ClaudeExe, id => LookUp(table, id),
            StartTime(Environment.ProcessId) ?? DateTime.MaxValue);
        return named is int pid ? (pid, true) : null;
    }

    /// <summary>A living process's executable name (from a <see cref="Snapshot"/>) and start time.</summary>
    /// <param name="table">The process list.</param>
    /// <param name="processId">The process.</param>
    /// <returns>Its name and start time, or null when the list lacks it or its start time may not be read (it ended
    /// since the snapshot, or belongs to another user): such a process cannot be vouched for as the requester.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="table"/> is null.</exception>
    internal static (string Name, DateTime Start)? LookUp(IReadOnlyDictionary<int, (int Parent, string Name)> table, int processId)
    {
        ArgumentNullException.ThrowIfNull(table);
        return table.TryGetValue(processId, out var entry) && StartTime(processId) is { } start ? (entry.Name, start) : null;
    }

    /// <summary>The process a <see cref="ClaudePidVariable"/> value names, when it can be the session that started this
    /// process: alive, running <paramref name="exeName"/>, and started no later than this process.</summary>
    /// <param name="value">The variable's value (null when it is not set).</param>
    /// <param name="exeName">The executable the process must run (claude.exe), compared without case.</param>
    /// <param name="lookUp">A living process's executable name and start time by id; null when it is gone or its start
    /// time may not be read.</param>
    /// <param name="selfStart">When this process started (<see cref="DateTime.MaxValue"/> when unknown: every living
    /// process then passes the age test, as in <see cref="NearestAncestor"/>).</param>
    /// <returns>The process id, or null when the value is missing, not a positive number, or names a process that
    /// cannot be the requester.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="lookUp"/> is null.</exception>
    /// <remarks>Pure apart from <paramref name="lookUp"/>, so every refusal is unit-tested. The age test is the
    /// reused-id guard of <see cref="NearestAncestor"/>: a session's tool call starts after its claude, so a claude.exe
    /// that started after this process holds a reused id and is a stranger.</remarks>
    internal static int? ClaudeFromVariable(string? value, string exeName, Func<int, (string Name, DateTime Start)?> lookUp,
        DateTime selfStart)
    {
        ArgumentNullException.ThrowIfNull(lookUp);
        // Invariant parsing: the variable is machine text (String(process.pid)); a sign, a fraction or garbage is refused.
        if (!int.TryParse(value?.Trim(), System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture,
                out int pid) || pid <= 0)
        {
            return null;
        }
        if (lookUp(pid) is not { } process)
        {
            return null;
        }
        if (!string.Equals(process.Name, exeName, StringComparison.OrdinalIgnoreCase) || process.Start > selfStart)
        {
            return null;
        }
        return pid;
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

    /// <summary>Every process's parent id and executable name (internal for the tests of <see cref="LookUp"/>).</summary>
    /// <returns>The table, keyed by process id; empty when the snapshot cannot be taken.</returns>
    internal static unsafe Dictionary<int, (int Parent, string Name)> Snapshot()
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
