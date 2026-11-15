#:property AllowUnsafeBlocks=true
#:property PublishAot=false
// Agentmaster: uistalls — how often, and for how long, one process's UI thread stops answering messages: the delay a key
// press or a click on its windows waits. The evidence behind doc/agentmaster/UI_THREAD.md §19 ("Typing waited behind
// the observer lane").
//
//   dotnet run tools/uistalls.cs -- <pid> [seconds=60] [periodMs=20]
//
// Every <periodMs> it sends WM_NULL (a no-op message) to the largest visible top-level window of <pid> and times the
// round trip, the way Windows' own "Not Responding" check does. Agentmaster runs every window on ONE UI thread, so one
// window speaks for all of them; the first line lists the UI thread ids of the visible windows to confirm it.
//
// What it prints: the round trips' p50 / p90 / p99 / max; how many were over 16, 50, 100, 250, 500 and 1000 ms; the time
// spent inside stalls over 50 ms; the 8 longest with their time into the run; the worst per 10 s.
//
// SAFE ON PRODUCTION: nothing is suspended, injected or written. The target only answers a WM_NULL; the sender waits.
// Windows hands a thread its SENT messages before its input, so a round trip is roughly the least a key press waits.
//
// Caveats:
//   - A send blocks this thread for as long as the target is busy (up to the 10 s timeout), so the samples are not evenly
//     spaced during a stall; the stall itself is still measured exactly by that one long round trip.
//   - The sending thread runs at TIME_CRITICAL, so its own scheduling delay does not read as the target's stall; on a
//     machine whose CPU is full, the target's wait for a CPU still counts (that is a delay the user feels too).
//   - Run it from a folder outside the repo if the repo's Directory.Build.props / NuGet.config ever breaks a file-based
//     build (copy the file next to the scratch output; it needs no package).

using System.Diagnostics;
using System.Runtime.InteropServices;

int pid = int.Parse(args[0]);
int seconds = args.Length > 1 ? int.Parse(args[1]) : 60;
int periodMs = args.Length > 2 ? int.Parse(args[2]) : 20;
return Tracker.Run(pid, seconds, periodMs);

/// <summary>
/// Sends WM_NULL on a fixed period and summarizes the round trips: counts above
/// thresholds, the longest stalls with their time, the share of the window spent
/// waiting, and the per-10-second worst case.
/// </summary>
/// <remarks>
/// Footgun: a send blocks this thread for as long as the target is busy (up to the
/// 10 s timeout), so the samples are not evenly spaced during a stall; the stall
/// itself is still measured exactly by that one long round trip.
/// </remarks>
static unsafe class Tracker
{
    [DllImport("user32.dll")] static extern int EnumWindows(IntPtr cb, IntPtr l);
    [DllImport("user32.dll")] static extern int IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern int GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr SendMessageTimeoutW(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeoutMs, out IntPtr result);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentThread();
    [DllImport("kernel32.dll")] static extern int SetThreadPriority(IntPtr h, int p);

    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }

    static readonly List<IntPtr> found = new();

    /// <summary>EnumWindows callback collecting visible top-level windows.</summary>
    /// <param name="h">A top-level window.</param>
    /// <param name="l">Unused.</param>
    /// <returns>1 to keep enumerating.</returns>
    [UnmanagedCallersOnly]
    static int Enum(IntPtr h, IntPtr l) { if (IsWindowVisible(h) != 0) found.Add(h); return 1; }

    /// <summary>Runs the measurement and prints the summary.</summary>
    /// <param name="pid">Target process.</param>
    /// <param name="seconds">Measurement length.</param>
    /// <param name="periodMs">Pause between sends while the target answers promptly.</param>
    /// <returns>0 on success; 1 when the process has no visible window.</returns>
    public static int Run(int pid, int seconds, int periodMs)
    {
        // TIME_CRITICAL so our own scheduling delay never shows up as the target's stall.
        SetThreadPriority(GetCurrentThread(), 15);
        EnumWindows((IntPtr)(delegate* unmanaged<IntPtr, IntPtr, int>)&Enum, IntPtr.Zero);
        var mine = new List<(IntPtr h, uint tid, long area)>();
        foreach (var h in found)
        {
            uint tid = GetWindowThreadProcessId(h, out uint p);
            if (p != pid) continue;
            GetWindowRect(h, out var r);
            mine.Add((h, tid, (long)(r.R - r.L) * (r.B - r.T)));
        }
        if (mine.Count == 0) { Console.WriteLine("no visible window"); return 1; }
        Console.WriteLine($"{mine.Count} visible windows; UI thread ids: {string.Join(", ", mine.Select(m => m.tid).Distinct())}");
        var target = mine.OrderByDescending(m => m.area).First().h;

        var rtt = new List<(double atS, double ms)>();
        var sw = Stopwatch.StartNew();
        while (sw.Elapsed.TotalSeconds < seconds)
        {
            double at = sw.Elapsed.TotalSeconds;
            long t0 = Stopwatch.GetTimestamp();
            IntPtr ok = SendMessageTimeoutW(target, 0, IntPtr.Zero, IntPtr.Zero, 0, 10_000, out _);
            double ms = (Stopwatch.GetTimestamp() - t0) * 1000.0 / Stopwatch.Frequency;
            rtt.Add((at, ok == IntPtr.Zero ? 10_000 : ms));
            Thread.Sleep(periodMs);
        }

        var a = rtt.Select(x => x.ms).OrderBy(x => x).ToArray();
        double P(double p) => a[(int)Math.Clamp(Math.Ceiling(p / 100 * a.Length) - 1, 0, a.Length - 1)];
        Console.WriteLine($"sends={a.Length} p50={P(50):F2} p90={P(90):F2} p99={P(99):F1} max={a[^1]:F0} ms");
        foreach (int th in new[] { 16, 50, 100, 250, 500, 1000 })
            Console.WriteLine($"  > {th,4} ms: {a.Count(x => x > th),4} sends");
        double blocked = rtt.Where(x => x.ms > 50).Sum(x => x.ms) / 1000.0;
        Console.WriteLine($"time spent waiting in stalls > 50 ms: {blocked:F1} s of {sw.Elapsed.TotalSeconds:F0} s ({100 * blocked / sw.Elapsed.TotalSeconds:F1} %)");
        Console.WriteLine("longest 8: " + string.Join(", ", rtt.OrderByDescending(x => x.ms).Take(8).Select(x => $"{x.ms:F0} ms @ {x.atS:F1}s")));
        Console.WriteLine("worst per 10 s: " + string.Join(" | ", rtt.GroupBy(x => (int)(x.atS / 10)).Select(g => $"{g.Key * 10}s {g.Max(x => x.ms):F0}")));
        return 0;
    }
}
