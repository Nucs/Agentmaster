using System.Diagnostics;
using Amb.Cli;
using Xunit;

namespace Amb.Cli.Tests;

/// <summary>The CLI's two sites (since 2026-11-12): names, which site a build id lives on, the options that pick one, and
/// the push's pieces that need no site (the tips git knows, the bundle's revisions, the submodule's gitlink).</summary>
public sealed class SitesTests : IDisposable
{
    /// <summary>A scratch repository under %TEMP%, deleted after the test.</summary>
    private readonly string _repo = Path.Combine(Path.GetTempPath(), "amb-sites-" + Guid.NewGuid().ToString("N"));

    /// <summary>Makes the scratch folder.</summary>
    public SitesTests() => Directory.CreateDirectory(_repo);

    /// <summary>Deletes the scratch folder (git makes some files read-only, which Windows refuses to delete as they are).</summary>
    public void Dispose()
    {
        foreach (string file in Directory.EnumerateFiles(_repo, "*", SearchOption.AllDirectories))
        {
            File.SetAttributes(file, FileAttributes.Normal);
        }
        Directory.Delete(_repo, recursive: true);
    }

    /// <summary>Site names are read case-free ("homepc", the coordinator's own name, is home); a build id's letter names
    /// its site; anything else names none.</summary>
    [Fact]
    public void SiteNamesAndBuildLetters()
    {
        Assert.True(Sites.TryParse("Worker", out string? worker));
        Assert.Equal(Sites.Worker, worker);
        Assert.True(Sites.TryParse("homepc", out string? home));
        Assert.Equal(Sites.Home, home);
        Assert.False(Sites.TryParse("dock", out _));
        Assert.False(Sites.TryParse(null, out _));
        Assert.Equal(Sites.Home, Sites.Other(Sites.Worker));
        Assert.Equal(Sites.Worker, Sites.Other(Sites.Home));
        Assert.Equal(Sites.Worker, Sites.OfBuild("w20261112-1"));
        Assert.Equal(Sites.Home, Sites.OfBuild("b20261025-3"));
        Assert.Null(Sites.OfBuild("r20261112-101010-abcdef"));
        Assert.Null(Sites.OfBuild(null));
    }

    /// <summary>--site and --suite-rev take values (so they are not unknown flags); a missing value is reported.</summary>
    [Fact]
    public void SiteOptionsAreKnown()
    {
        global::Options build = global::Options.Parse(["build", "--site", "home", "--wait"]);
        Assert.Empty(build.UnknownOptions);
        Assert.Equal("home", build.Values["--site"]);
        global::Options e2e = global::Options.Parse(["e2e", "w20261112-1", "--suite-rev", "HEAD~1", "--", "-v"]);
        Assert.Empty(e2e.UnknownOptions);
        Assert.Equal("HEAD~1", e2e.Values["--suite-rev"]);
        Assert.Equal(["--site"], global::Options.Parse(["status", "--site"]).UnknownOptions);
    }

    /// <summary>Only the tips git holds as commits are left out of a bundle; the revisions list the push ref first.</summary>
    [Fact]
    public void BundleLeavesOutWhatTheSiteHolds()
    {
        string a = new('a', 40), b = new('b', 40);
        Assert.Equal([a], SourcePush.KnownTips($"{a} commit 231\n{b} missing\n{new string('c', 40)} tree 40\n\n"));
        Assert.Equal($"refs/push/{a}\n^{b}\n", SourcePush.BundleRevisions(a, [b]));
        Assert.Equal($"refs/push/{a}\n", SourcePush.BundleRevisions(a, []));
    }

    /// <summary>The submodule's gitlink is read from a commit's tree; a commit without one has none.</summary>
    [Fact]
    public void GitlinkIsReadFromTheTree()
    {
        Git("init", "-q");
        Git("config", "user.email", "test@example.invalid");
        Git("config", "user.name", "test");
        File.WriteAllText(Path.Combine(_repo, "a.txt"), "a");
        Git("add", "a.txt");
        Git("commit", "-q", "-m", "one");
        string plain = Git("rev-parse", "HEAD").Trim();
        Assert.Null(SourcePush.Gitlink(_repo, plain));
        const string Pinned = "0123456789abcdef0123456789abcdef01234567";
        Git("update-index", "--add", "--cacheinfo", "160000," + Pinned + "," + SourcePush.SubmodulePath);
        Git("commit", "-q", "-m", "two");
        Assert.Equal(Pinned, SourcePush.Gitlink(_repo, Git("rev-parse", "HEAD").Trim()));
    }

    /// <summary>Runs git in the scratch repository and fails the test when it fails.</summary>
    /// <param name="args">git's arguments.</param>
    /// <returns>Its standard output.</returns>
    private string Git(params string[] args)
    {
        (int code, string output, string error) = WorkingTreeSnapshot.Git(_repo, args, check: false);
        Assert.True(code == 0, $"git {string.Join(' ', args)}: {error}");
        return output;
    }
}
