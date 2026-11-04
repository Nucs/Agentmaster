using Amb.Cli;
using Xunit;

namespace Amb.Cli.Tests;

/// <summary>
/// The snapshot of uncommitted work (`amb build --snapshot`) against a throwaway repository: what it takes, that it
/// changes nothing the user works with, that the same work makes the same commit, and that built snapshots' refs go.
/// </summary>
public sealed class SnapshotTests : IDisposable
{
    /// <summary>The test's own folder (a repository and a temp folder for the private index), deleted after it.</summary>
    private readonly string _root = Path.Combine(Path.GetTempPath(), "amb-snapshot-" + Guid.NewGuid().ToString("N"));

    /// <summary>The throwaway repository.</summary>
    private string Repo => Path.Combine(_root, "repo");

    /// <summary>The folder the private index goes to.</summary>
    private string Temp => Path.Combine(_root, "tmp");

    /// <summary>Makes a repository with two committed files.</summary>
    public SnapshotTests()
    {
        Directory.CreateDirectory(Repo);
        Git("init", "-q");
        Git("config", "user.name", "test");
        Git("config", "user.email", "test@example.invalid");
        Git("config", "core.autocrlf", "false");
        File.WriteAllText(Path.Combine(Repo, "a.txt"), "a\n");
        File.WriteAllText(Path.Combine(Repo, "b.txt"), "b\n");
        Git("add", "-A");
        Git("commit", "-q", "-m", "first");
    }

    /// <summary>Deletes the test's folder (git marks its objects read-only).</summary>
    public void Dispose()
    {
        foreach (string file in Directory.EnumerateFiles(_root, "*", SearchOption.AllDirectories))
        {
            File.SetAttributes(file, FileAttributes.Normal);
        }
        Directory.Delete(_root, recursive: true);
    }

    /// <summary>Runs git in the repository and returns its output.</summary>
    /// <param name="args">git's arguments.</param>
    /// <returns>Its standard output, trimmed.</returns>
    private string Git(params string[] args) => WorkingTreeSnapshot.Git(Repo, args).Output.Trim();

    /// <summary>A snapshot takes modified and untracked files, leaves HEAD, the branch, the index and the working tree
    /// alone, sits on HEAD, and the same work snapshotted again is the same commit.</summary>
    [Fact]
    public void SnapshotTakesTheWorkAndChangesNothing()
    {
        File.WriteAllText(Path.Combine(Repo, "a.txt"), "a changed\n");
        File.WriteAllText(Path.Combine(Repo, "new.txt"), "new\n");
        string head = Git("rev-parse", "HEAD");
        string status = Git("status", "--porcelain");

        IReadOnlyList<WorkingTreeSnapshot.Change> changes = WorkingTreeSnapshot.Changes(Repo, []);
        Assert.Equal(["a.txt", "new.txt"], changes.Select(c => c.Path).Order(StringComparer.Ordinal));
        WorkingTreeSnapshot.Result first = WorkingTreeSnapshot.Create(Repo, [], Temp);

        Assert.True(first.Changed);
        Assert.Equal(head, first.Head);
        Assert.Equal(head, Git("rev-parse", first.Commit + "^"));
        Assert.Equal("a changed", Git("show", first.Commit + ":a.txt"));
        Assert.Equal("new", Git("show", first.Commit + ":new.txt"));
        Assert.Equal(first.Commit, Git("rev-parse", WorkingTreeSnapshot.RefPrefix + first.Commit));
        // Nothing the user works with moved: HEAD, the index (git status reads it) and the files.
        Assert.Equal(head, Git("rev-parse", "HEAD"));
        Assert.Equal(status, Git("status", "--porcelain"));
        Assert.Empty(Directory.EnumerateFiles(Temp));

        WorkingTreeSnapshot.Result again = WorkingTreeSnapshot.Create(Repo, [], Temp);
        Assert.Equal(first.Commit, again.Commit);
    }

    /// <summary>Named paths take only those paths' work; everything else stays as committed.</summary>
    [Fact]
    public void NamedPathsTakeOnlyThosePaths()
    {
        File.WriteAllText(Path.Combine(Repo, "a.txt"), "a changed\n");
        File.WriteAllText(Path.Combine(Repo, "b.txt"), "b changed\n");

        WorkingTreeSnapshot.Result snapshot = WorkingTreeSnapshot.Create(Repo, ["b.txt"], Temp);

        Assert.True(snapshot.Changed);
        Assert.Equal("a", Git("show", snapshot.Commit + ":a.txt"));
        Assert.Equal("b changed", Git("show", snapshot.Commit + ":b.txt"));
        Assert.Equal(["b.txt"], WorkingTreeSnapshot.Changes(Repo, ["b.txt"]).Select(c => c.Path));
    }

    /// <summary>A clean tree makes no snapshot: HEAD itself is built.</summary>
    [Fact]
    public void CleanTreeBuildsHead()
    {
        WorkingTreeSnapshot.Result result = WorkingTreeSnapshot.Create(Repo, [], Temp);
        Assert.False(result.Changed);
        Assert.Equal(Git("rev-parse", "HEAD"), result.Commit);
        Assert.Empty(Git("for-each-ref", WorkingTreeSnapshot.RefPrefix));
    }

    /// <summary>A file over the limit is refused before anything is written, and the message names it.</summary>
    [Fact]
    public void BigFilesAreRefused()
    {
        WorkingTreeSnapshot.Change[] changes =
            [new("??", "dump.bin", WorkingTreeSnapshot.MaxFileBytes + 1), new(" M", "a.txt", 10)];
        SnapshotRefusedException refused = Assert.Throws<SnapshotRefusedException>(() => WorkingTreeSnapshot.CheckSize(changes));
        Assert.Contains("dump.bin", refused.Message, StringComparison.Ordinal);
        WorkingTreeSnapshot.CheckSize([new(" M", "a.txt", 10)]);
    }

    /// <summary>The refs of built snapshots go; the others stay.</summary>
    [Fact]
    public void BuiltSnapshotsLoseTheirRefs()
    {
        File.WriteAllText(Path.Combine(Repo, "a.txt"), "one\n");
        string built = WorkingTreeSnapshot.Create(Repo, [], Temp).Commit;
        File.WriteAllText(Path.Combine(Repo, "a.txt"), "two\n");
        string pending = WorkingTreeSnapshot.Create(Repo, [], Temp).Commit;

        Assert.Equal(1, WorkingTreeSnapshot.PruneBuilt(Repo, new HashSet<string> { built, "0123456789012345678901234567890123456789" }));

        Assert.Equal(WorkingTreeSnapshot.RefPrefix + pending, Git("for-each-ref", "--format=%(refname)", WorkingTreeSnapshot.RefPrefix));
    }

    /// <summary>The message depends on nothing but HEAD and the tree (part of what makes a snapshot reproducible).</summary>
    [Fact]
    public void MessageIsReproducible()
    {
        string head = new('1', 40);
        string tree = new('2', 40);
        Assert.Equal(WorkingTreeSnapshot.Message(head, tree), WorkingTreeSnapshot.Message(head, tree));
        Assert.Contains(head, WorkingTreeSnapshot.Message(head, tree), StringComparison.Ordinal);
    }

    /// <summary>A help flag asks for the usage, and an option amb does not know is reported so the command is refused
    /// before anything is sent: an ignored `--help` once submitted a build, and an ignored `--dryrun` would install for
    /// real. A value option given without its value counts as unknown too.</summary>
    [Fact]
    public void UnknownOptionsAreReported()
    {
        Assert.True(global::Options.Parse(["build", "--help"]).Help);
        Assert.True(global::Options.Parse(["install", "b1", "-h"]).Help);
        Assert.Equal(["--dryrun"], global::Options.Parse(["install", "b1", "--instance", "dev", "--dryrun"]).UnknownOptions);
        Assert.Equal(["--instance"], global::Options.Parse(["install", "b1", "--instance"]).UnknownOptions);
        global::Options good = global::Options.Parse(["install", "b1", "--instance", "dev", "--dry-run", "--wait"]);
        Assert.Empty(good.UnknownOptions);
        // --reinstall is a flag of install's (the same build installed again on purpose), never a typo to refuse.
        Assert.Empty(global::Options.Parse(["install", "b1", "--instance", "production", "--reinstall"]).UnknownOptions);
        Assert.False(good.Help);
        Assert.Equal("dev", good.Values["--instance"]);
        // What follows "--" belongs to pytest or names snapshot paths: never options of amb's.
        Assert.Empty(global::Options.Parse(["e2e", "b1", "--", "-v", "--collect-only"]).UnknownOptions);
    }

    /// <summary>The test process runs under no claude.exe; the walk ends without one (and without throwing).</summary>
    [Fact]
    public void AncestryWalkEnds()
    {
        int? claude = ProcessAncestry.NearestAncestor("no-such-program-amb.exe");
        Assert.Null(claude);
    }
}
