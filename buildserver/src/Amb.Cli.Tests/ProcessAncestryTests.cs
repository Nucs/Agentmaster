using System.Globalization;
using Amb.Cli;
using Xunit;

namespace Amb.Cli.Tests;

/// <summary>
/// The requester of `amb install` named by Claude Code's CLAUDE_PID when the process chain is broken (a script run with
/// `bash script.sh`, 2026-11-04): the value must name a living claude.exe that started no later than amb itself, else
/// nobody is named and the install waits for no session (the walk's own behavior is <see cref="SnapshotTests"/>'s).
/// </summary>
public sealed class ProcessAncestryTests
{
    /// <summary>When this test's process started, as amb reads its own start.</summary>
    private static readonly DateTime SelfStart = new(2026, 11, 4, 16, 56, 0, DateTimeKind.Local);

    /// <summary>A process list holding one claude.exe (pid 10788, started a minute before amb) and one older
    /// node.exe (pid 4242).</summary>
    /// <param name="processId">The process asked for.</param>
    /// <returns>Its name and start time, or null for any other id (gone).</returns>
    private static (string Name, DateTime Start)? Fake(int processId) => processId switch
    {
        10788 => ("claude.exe", SelfStart.AddMinutes(-1)),
        4242 => ("node.exe", SelfStart.AddHours(-1)),
        _ => null,
    };

    /// <summary>A living claude.exe that started before amb is the requester; its name is compared without case, and
    /// the value's surrounding blanks do not matter.</summary>
    [Fact]
    public void LivingOlderClaudeIsTheRequester()
    {
        Assert.Equal(10788, ProcessAncestry.ClaudeFromVariable("10788", "claude.exe", Fake, SelfStart));
        Assert.Equal(10788, ProcessAncestry.ClaudeFromVariable(" 10788 ", "CLAUDE.EXE", Fake, SelfStart));
        // An unknown start of amb's own lets every living claude.exe pass, as the walk does.
        Assert.Equal(10788, ProcessAncestry.ClaudeFromVariable("10788", "claude.exe", Fake, DateTime.MaxValue));
    }

    /// <summary>No variable, a value that is not a plain positive number, a gone process, another program under the id,
    /// or a claude.exe that started after amb (a reused id) names nobody.</summary>
    [Fact]
    public void AnythingElseNamesNobody()
    {
        foreach (string? value in new[] { null, "", "   ", "abc", "0", "-10788", "+10788", "10788.0", "0x2A24", "99999999999" })
        {
            Assert.Null(ProcessAncestry.ClaudeFromVariable(value, "claude.exe", Fake, SelfStart));
        }
        Assert.Null(ProcessAncestry.ClaudeFromVariable("777", "claude.exe", Fake, SelfStart));
        Assert.Null(ProcessAncestry.ClaudeFromVariable("4242", "claude.exe", Fake, SelfStart));
        Assert.Null(ProcessAncestry.ClaudeFromVariable("10788", "claude.exe", Fake, SelfStart.AddMinutes(-2)));
        Assert.Throws<ArgumentNullException>(() => ProcessAncestry.ClaudeFromVariable("10788", "claude.exe", null!, SelfStart));
    }

    /// <summary>The real process list vouches for a living process by its own executable name and start time: this test
    /// process names itself when its own name is asked for, and is no claude.exe.</summary>
    [Fact]
    public void RealLookUpVouchesForALivingProcess()
    {
        Dictionary<int, (int Parent, string Name)> table = ProcessAncestry.Snapshot();
        int self = Environment.ProcessId;
        string value = self.ToString(CultureInfo.InvariantCulture);
        string selfName = Path.GetFileName(Environment.ProcessPath!);
        Assert.Equal(self, ProcessAncestry.ClaudeFromVariable(value, selfName, id => ProcessAncestry.LookUp(table, id),
            DateTime.Now.AddMinutes(1)));
        Assert.Null(ProcessAncestry.ClaudeFromVariable(value, ProcessAncestry.ClaudeExe, id => ProcessAncestry.LookUp(table, id),
            DateTime.MaxValue));
        // A process the list does not hold (ids are multiples of 4; 2 is never one) cannot be vouched for.
        Assert.Null(ProcessAncestry.LookUp(table, 2));
    }
}
