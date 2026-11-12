using System.Diagnostics;
using System.Globalization;
using System.Net;
using Amb.Core.Json;
using Amb.Core.Protocol;

namespace Amb.Cli;

/// <summary>
/// Pushes a commit to a site whose mirror takes commits only by push (the worker machine's: it cannot read K:), before
/// `amb build` or `amb e2e` asks it for that commit. Only what the site's mirror lacks travels: the site names its tips,
/// and the bundle leaves out everything they reach (a day's work is kilobytes; the whole history ~150 MB, once).
/// </summary>
/// <remarks>
/// <para><b>Nothing in the owner's repository changes:</b> a bundle needs a ref to carry, so it is made in a scratch bare
/// repository on Q: whose object store borrows the owner's through git's alternates file; the one ref
/// (<c>refs/push/&lt;commit&gt;</c>) is made there, and the scratch repository is deleted afterwards. The scratch
/// repository writes no objects of its own (a bundle only reads them), so nothing depends on it once it is gone.</para>
/// <para><b>The submodule</b> (refs/claude-agent-sdk-cpp) is pushed from the owner's own copy of it
/// (.git/modules/refs/claude-agent-sdk-cpp) the same way; when that copy lacks the pinned commit, the site fetches it
/// from the URL .gitmodules names, as homepc's site does.</para>
/// </remarks>
internal static class SourcePush
{
    /// <summary>The submodule's path in the repository (its gitlink names the pinned commit).</summary>
    internal const string SubmodulePath = "refs/claude-agent-sdk-cpp";

    /// <summary>Where the scratch repositories and bundles go: Q:, never K: (the owner's rule: everything a build makes is
    /// written to Q:).</summary>
    internal const string ScratchRoot = @"Q:\buildserver\tmp";

    /// <summary>The longest one upload may take: the whole history (~150 MB) takes ~12 s over the link home at ~105
    /// Mbit/s up (measured 2026-11-12); 30 minutes covers a link at a hundredth of that.</summary>
    internal static readonly TimeSpan UploadTimeout = TimeSpan.FromMinutes(30);

    /// <summary>Pushes a commit, and the submodule commit it pins, unless the site's mirror holds them.</summary>
    /// <param name="client">The site's client (its status said it pushes).</param>
    /// <param name="repo">The owner's repository (AMB_REPO, default K:\source\Agentmaster); a worktree works too.</param>
    /// <param name="commit">The commit (a full SHA the repository holds).</param>
    /// <param name="cancel">Cancels the uploads.</param>
    /// <returns>Null when the site holds both commits now; else what failed.</returns>
    /// <exception cref="SiteUnavailableException">The site could not be reached.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    internal static async Task<string?> PushAsync(SiteClient client, string repo, string commit, CancellationToken cancel)
    {
        try
        {
            // The common git folder: a worktree's .git is a file, but its objects live in the main repository's.
            string common = WorkingTreeSnapshot.Git(repo, ["rev-parse", "--path-format=absolute", "--git-common-dir"]).Output.Trim();
            string? error = await PushOneAsync(client, "agentmaster", Path.Combine(common, "objects"), commit, cancel)
                .ConfigureAwait(false);
            if (error is not null)
            {
                return error;
            }
            string? sdk = Gitlink(repo, commit);
            if (sdk is null)
            {
                return null;
            }
            string module = Path.Combine(common, "modules", "refs", "claude-agent-sdk-cpp");
            if (!Directory.Exists(module)
                || WorkingTreeSnapshot.Git(module, ["cat-file", "-e", sdk + "^{commit}"], check: false).ExitCode != 0)
            {
                Console.WriteLine($"the submodule's commit {sdk[..9]} is not in your clone: the {client.Site} site fetches it from its URL");
                return null;
            }
            return await PushOneAsync(client, "sdk", Path.Combine(module, "objects"), sdk, cancel).ConfigureAwait(false);
        }
        catch (InvalidOperationException ex)
        {
            // git failed (or is not on PATH): the caller reports it and sends nothing.
            return ex.Message;
        }
        catch (IOException ex)
        {
            return "the scratch repository on Q: could not be written: " + ex.Message;
        }
    }

    /// <summary>The submodule's pinned commit at a commit (its gitlink), or null when the tree has none.</summary>
    /// <param name="repo">The repository.</param>
    /// <param name="commit">The commit.</param>
    /// <returns>The pinned SHA, or null.</returns>
    /// <exception cref="InvalidOperationException">git failed.</exception>
    internal static string? Gitlink(string repo, string commit)
    {
        string line = WorkingTreeSnapshot.Git(repo, ["ls-tree", commit, "--", SubmodulePath]).Output.Trim();
        // "160000 commit <sha>\t<path>": mode 160000 is a gitlink.
        string[] parts = line.Split((char[])[' ', '\t'], StringSplitOptions.RemoveEmptyEntries);
        return parts.Length >= 3 && parts[0] == "160000" && parts[1] == "commit" ? parts[2] : null;
    }

    /// <summary>The site's tips that a repository holds: git refuses to leave out a commit it does not know.</summary>
    /// <param name="batchCheck">The output of <c>git cat-file --batch-check</c> over the tips, one line each
    /// ("&lt;sha&gt; commit &lt;size&gt;" or "&lt;sha&gt; missing").</param>
    /// <returns>The tips the repository holds as commits.</returns>
    internal static IReadOnlyList<string> KnownTips(string batchCheck) =>
        [.. batchCheck.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(line => line.Split(' '))
            .Where(parts => parts.Length == 3 && parts[1] == "commit" && parts[0].Length == 40)
            .Select(parts => parts[0])];

    /// <summary>The revisions a bundle is made of, as <c>git bundle create --stdin</c> reads them: the push ref, and every
    /// known tip left out.</summary>
    /// <param name="commit">The commit to push.</param>
    /// <param name="exclude">The tips to leave out (none: the whole history).</param>
    /// <returns>One revision per line.</returns>
    internal static string BundleRevisions(string commit, IReadOnlyList<string> exclude) =>
        "refs/push/" + commit + "\n" + string.Concat(exclude.Select(tip => "^" + tip + "\n"));

    /// <summary>Pushes one commit of one repository, unless the site's mirror holds it.</summary>
    /// <param name="client">The site's client.</param>
    /// <param name="name">The mirror's API name: "agentmaster" or "sdk".</param>
    /// <param name="objectsDir">The owner's object store the scratch repository borrows.</param>
    /// <param name="commit">The commit.</param>
    /// <param name="cancel">Cancels the upload.</param>
    /// <returns>Null when the site holds the commit now; else what failed.</returns>
    /// <exception cref="InvalidOperationException">git failed.</exception>
    /// <exception cref="IOException">The scratch repository could not be made.</exception>
    /// <exception cref="SiteUnavailableException">The site could not be reached.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    private static async Task<string?> PushOneAsync(SiteClient client, string name, string objectsDir, string commit,
        CancellationToken cancel)
    {
        (HttpStatusCode status, SourceTips? tips, string text) = await client.GetAsync(
            $"source/tips?repo={name}&want={commit}", AmbJsonLine.Default.SourceTips, cancel).ConfigureAwait(false);
        if (tips is null)
        {
            return $"{(int)status} {text}";
        }
        if (tips.Has == true)
        {
            Console.WriteLine($"the {client.Site} site has {name} {commit[..9]}");
            return null;
        }
        Directory.CreateDirectory(ScratchRoot);
        // One scratch repository per process: two `amb` calls at once never share a ref or a bundle file.
        string scratch = Path.Combine(ScratchRoot, $"push-{name}-{Environment.ProcessId}.git");
        string bundle = Path.Combine(ScratchRoot, $"push-{name}-{Environment.ProcessId}.bundle");
        try
        {
            DeleteTree(scratch);
            WorkingTreeSnapshot.Git(ScratchRoot, ["init", "-q", "--bare", scratch]);
            // Forward slashes: git reads the alternates file the same way on every platform.
            await File.WriteAllTextAsync(Path.Combine(scratch, "objects", "info", "alternates"),
                objectsDir.Replace('\\', '/') + "\n", cancel).ConfigureAwait(false);
            IReadOnlyList<string> exclude = KnownTips(WorkingTreeSnapshot.Git(scratch, ["cat-file", "--batch-check"],
                stdin: string.Concat(tips.Commits.Select(c => c + "\n"))).Output);
            WorkingTreeSnapshot.Git(scratch, ["update-ref", "refs/push/" + commit, commit]);
            for (int attempt = 0; ; attempt++)
            {
                bool whole = attempt > 0;
                WorkingTreeSnapshot.Git(scratch, ["bundle", "create", "-q", bundle, "--stdin"],
                    stdin: BundleRevisions(commit, whole ? [] : exclude));
                long bytes = new FileInfo(bundle).Length;
                var clock = Stopwatch.StartNew();
                (HttpStatusCode pushed, SourceTips? took, string answer) = await client.UploadAsync(
                    $"source/bundle?repo={name}", bundle, AmbJsonLine.Default.SourceTips, UploadTimeout, cancel).ConfigureAwait(false);
                if (took is not null)
                {
                    Console.WriteLine(string.Create(CultureInfo.InvariantCulture,
                        $"pushed {name} {commit[..9]} to the {client.Site} site: {bytes / 1048576.0:0.0} MB in {clock.Elapsed.TotalSeconds:0.0} s{(whole ? " (the whole history)" : "")}"));
                    return null;
                }
                // The site's mirror lacks what the bundle builds on (a mirror made anew, a tip it lost): once more with
                // the whole history, which needs nothing.
                if (!whole && exclude.Count > 0 && pushed == HttpStatusCode.BadRequest
                    && answer.Contains("does not apply", StringComparison.Ordinal))
                {
                    Console.WriteLine($"the {client.Site} site's {name} mirror lacks what the bundle builds on: pushing the whole history");
                    continue;
                }
                return $"{(int)pushed} {answer}";
            }
        }
        finally
        {
            // Cleanup in every outcome: the bundle can be ~150 MB, and the scratch repository names nothing anyone uses.
            try
            {
                File.Delete(bundle);
                DeleteTree(scratch);
            }
            catch (IOException ex)
            {
                Console.Error.WriteLine($"amb: could not remove {scratch}: {ex.Message}");
            }
            catch (UnauthorizedAccessException ex)
            {
                Console.Error.WriteLine($"amb: could not remove {scratch}: {ex.Message}");
            }
        }
    }

    /// <summary>Deletes a folder tree when it exists (git makes some of its files read-only, which Windows refuses to
    /// delete until the flag is cleared).</summary>
    /// <param name="folder">The folder.</param>
    /// <exception cref="IOException">A file could not be deleted.</exception>
    /// <exception cref="UnauthorizedAccessException">A file may not be deleted.</exception>
    private static void DeleteTree(string folder)
    {
        if (!Directory.Exists(folder))
        {
            return;
        }
        foreach (string file in Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories))
        {
            File.SetAttributes(file, FileAttributes.Normal);
        }
        Directory.Delete(folder, recursive: true);
    }
}
