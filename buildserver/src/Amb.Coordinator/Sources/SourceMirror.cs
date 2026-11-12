using Amb.Coordinator.Slots;
using Amb.Core.Protocol;

namespace Amb.Coordinator.Sources;

/// <summary>How much of a repository a bundle carries for a VM.</summary>
public enum BundleMode
{
    /// <summary>No bundle: the VM's clone holds the commit already (its last build was of the same commit).</summary>
    None,

    /// <summary>Only what the VM lacks: the objects between its last built commit (the basis) and the new one.</summary>
    Incremental,

    /// <summary>The commit's whole history (~147 MB for this repository, measured): a VM with no clone yet, or a basis
    /// the mirror no longer has.</summary>
    Full,
}

/// <summary>What was written for a VM: its bundles and the refs they carry.</summary>
/// <param name="Commit">The commit to build.</param>
/// <param name="Ref">The ref the repository's bundle names (the guest fetches it).</param>
/// <param name="Mode">How much the repository's bundle carries.</param>
/// <param name="BundleFile">The repository's bundle file's name in the folder, or null when <paramref name="Mode"/> is
/// None.</param>
/// <param name="SdkCommit">The submodule's pinned commit, or null when the commit has no submodule.</param>
/// <param name="SdkRef">The ref the submodule's bundle names.</param>
/// <param name="SdkBundleFile">The submodule's bundle file's name, or null when none is needed.</param>
/// <param name="Bytes">The bundles' total size.</param>
public sealed record SourceBundle(string Commit, string Ref, BundleMode Mode, string? BundleFile, string? SdkCommit,
    string SdkRef, string? SdkBundleFile, long Bytes);

/// <summary>
/// The site's bare mirrors of the repository and its one submodule (DESIGN.md §11): builds come from commits, never from
/// the live tree. The mirror fetches the source repository's branches and tags by path (it only reads K:), and a build
/// VM gets a git bundle of what its clone lacks, written into its share. A site that cannot read K: (the worker
/// machine's, <see cref="CoordinatorOptions.PushesSource"/>) takes commits by push instead: `amb` sends a bundle of what
/// the mirror lacks (<see cref="TipsAsync"/>, <see cref="ReceiveBundleAsync"/>).
/// </summary>
/// <remarks>
/// <para>Measured 2026-10-26: the first fetch of K:\source\Agentmaster through WSL's automount took 92 s (145 MB); a full
/// bundle of HEAD 6 s (147 MB); an incremental one over 20 commits 0.8 MB.</para>
/// <para>Every git call passes <c>safe.directory=*</c>: the source repository on /mnt/k is not owned by the distro's
/// root, and git refuses to read a repository owned by someone else without it. Nothing is configured globally.</para>
/// </remarks>
public sealed class SourceMirror(CoordinatorOptions options, ILogger log)
{
    /// <summary>The submodule's path in the repository (its gitlink names the pinned commit).</summary>
    public const string SubmodulePath = "refs/claude-agent-sdk-cpp";

    /// <summary>Where snapshots of uncommitted work live (DESIGN.md §11): <c>refs/buildserver/snapshots/&lt;commit&gt;</c>,
    /// in the source repository (made by the CLI) and in the mirror.</summary>
    public const string SnapshotRefs = "refs/buildserver/snapshots/";

    /// <summary>The API's name of the repository's mirror (<c>?repo=agentmaster</c>).</summary>
    public const string RepoName = "agentmaster";

    /// <summary>The API's name of the submodule's mirror (<c>?repo=sdk</c>).</summary>
    public const string SdkName = "sdk";

    /// <summary>The prefix of the one ref a pushed bundle carries: <c>refs/push/&lt;commit&gt;</c> (`amb` makes it in a
    /// scratch repository of its own, never in the owner's).</summary>
    public const string PushRefPrefix = "refs/push/";

    /// <summary>Where a pushed commit's ref lives in the mirror: <c>refs/amb/pushed/&lt;commit&gt;</c>. Kept: the next push
    /// leaves out everything such a ref reaches (<see cref="TipsAsync"/>).</summary>
    public const string PushedRefs = "refs/amb/pushed/";

    /// <summary>The most tips <see cref="TipsAsync"/> returns (the newest by commit date): enough that a push of today's
    /// work leaves out what the mirror holds, few enough to pass to git on its standard input.</summary>
    internal const int MaxTips = 100;

    /// <summary>One push into the mirrors at a time (the whole site): two pushes of the same commit would race for its
    /// ref's lock, and the second would fail for nothing.</summary>
    private static readonly SemaphoreSlim PushLock = new(1, 1);

    /// <summary>The repository's mirror.</summary>
    private string RepoMirror => Path.Combine(options.GitDir, "agentmaster.git");

    /// <summary>The submodule's mirror.</summary>
    private string SdkMirror => Path.Combine(options.GitDir, "claude-agent-sdk-cpp.git");

    /// <summary>A mirror's folder by its API name.</summary>
    /// <param name="repo"><see cref="RepoName"/> or <see cref="SdkName"/>.</param>
    /// <returns>The mirror's folder.</returns>
    /// <exception cref="ArgumentException">Another name (the API answers 400).</exception>
    private string MirrorOf(string repo) => repo switch
    {
        RepoName => RepoMirror,
        SdkName => SdkMirror,
        _ => throw new ArgumentException($"no mirror named '{repo}' (agentmaster or sdk)", nameof(repo)),
    };

    /// <summary>Decides how much a VM's bundle must carry.</summary>
    /// <param name="commit">The commit to build.</param>
    /// <param name="basis">The commit the VM's clone holds (its last build), or null when it has none.</param>
    /// <param name="basisInMirror">Whether the mirror still has the basis (it can only bundle "since the basis" then).</param>
    /// <param name="commitReachableFromBasis">Whether the commit is the basis or one of its ancestors: the clone then holds
    /// every object it needs (an older commit built after a newer one), and git would refuse the empty bundle.</param>
    /// <returns>None when the clone holds the commit, Incremental when a basis is usable, else Full.</returns>
    public static BundleMode PlanBundle(string commit, string? basis, bool basisInMirror, bool commitReachableFromBasis) =>
        basis == commit || (basisInMirror && commitReachableFromBasis) ? BundleMode.None
        : basis is not null && basisInMirror ? BundleMode.Incremental
        : BundleMode.Full;

    /// <summary>Brings the mirrors up to date for a commit: the repository's branches and tags, and the submodule's
    /// pinned commit.</summary>
    /// <param name="commit">The commit to build (a full SHA).</param>
    /// <param name="cancel">Cancels the fetches.</param>
    /// <returns>The submodule's pinned commit at <paramref name="commit"/>, or null when that commit has no submodule.</returns>
    /// <exception cref="InvalidOperationException">The commit, or the submodule's commit, is in none of the sources.</exception>
    /// <exception cref="TimeoutException">A fetch took longer than its limit.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    /// <remarks>On a site whose mirror takes pushes (<see cref="CoordinatorOptions.PushesSource"/>) nothing is fetched
    /// from a repository: the commit must have been pushed, and the submodule's commit too, else it is fetched from the
    /// URL its .gitmodules names (GitHub), the same last resort as homepc's.</remarks>
    public async Task<string?> FetchAsync(string commit, CancellationToken cancel)
    {
        await EnsureCommitAsync(commit, cancel).ConfigureAwait(false);
        string? sdk = await GitlinkAsync(commit, cancel).ConfigureAwait(false);
        if (sdk is null)
        {
            return null;
        }
        await EnsureBareAsync(SdkMirror, cancel).ConfigureAwait(false);
        // The source repository's own copy of the submodule first (the pinned commit is usually its detached HEAD), then
        // the URL .gitmodules names (GitHub) as the last resort. A pushing site has no source repository: `amb` pushed
        // the submodule's commit from the owner's copy already, or the URL is all there is.
        string local = Path.Combine(options.SourceRepo, ".git", "modules", "refs", "claude-agent-sdk-cpp");
        if (!options.PushesSource && Directory.Exists(local) && !await HasCommitAsync(SdkMirror, sdk, cancel).ConfigureAwait(false))
        {
            await GitAsync(SdkMirror, ["fetch", "--no-tags", local, "+refs/heads/*:refs/heads/*", "+HEAD:refs/amb/source-head"],
                TimeSpan.FromMinutes(10), cancel, check: false).ConfigureAwait(false);
        }
        if (!await HasCommitAsync(SdkMirror, sdk, cancel).ConfigureAwait(false))
        {
            CommandResult url = await GitAsync(RepoMirror, ["config", "--blob", commit + ":.gitmodules", "--get",
                "submodule." + SubmodulePath + ".url"], TimeSpan.FromSeconds(30), cancel, check: false).ConfigureAwait(false);
            if (url.ExitCode == 0 && url.Output.Trim().Length > 0)
            {
                await GitAsync(SdkMirror, ["fetch", "--no-tags", url.Output.Trim(), "+refs/heads/*:refs/heads/*"],
                    TimeSpan.FromMinutes(10), cancel, check: false).ConfigureAwait(false);
            }
        }
        if (!await HasCommitAsync(SdkMirror, sdk, cancel).ConfigureAwait(false))
        {
            throw new InvalidOperationException($"the submodule's commit {sdk} ({SubmodulePath} at {commit[..9]}) is in none of its sources");
        }
        return sdk;
    }

    /// <summary>Makes sure the repository's mirror holds a commit: fetched from the source repository (its branches and
    /// tags, then the snapshots, then the commit by name), or, on a pushing site, checked only.</summary>
    /// <param name="commit">The commit (a full SHA).</param>
    /// <param name="cancel">Cancels the fetches.</param>
    /// <returns>A task that ends when the mirror holds the commit.</returns>
    /// <exception cref="InvalidOperationException">The commit is in neither the source repository nor (on a pushing site)
    /// the mirror.</exception>
    /// <exception cref="TimeoutException">A fetch took longer than its limit.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    public async Task EnsureCommitAsync(string commit, CancellationToken cancel)
    {
        if (options.PushesSource)
        {
            await EnsureBareAsync(RepoMirror, cancel).ConfigureAwait(false);
            if (!await HasCommitAsync(RepoMirror, commit, cancel).ConfigureAwait(false))
            {
                throw new InvalidOperationException(
                    $"commit {commit} is not in this site's mirror, which takes commits only by push: `amb` pushes a commit before it asks for a build or a suite (an older amb, or a push that failed?)");
            }
            return;
        }
        await RefreshAsync(cancel).ConfigureAwait(false);
        if (!await HasCommitAsync(RepoMirror, commit, cancel).ConfigureAwait(false))
        {
            // A snapshot of uncommitted work (`amb build --snapshot`): the CLI made it under refs/buildserver/snapshots/
            // in the source repository. Fetched without --prune, so the mirror keeps every snapshot it ever built: the CLI
            // deletes the source's ref once its build exists, and the VM's worktree may still use the commit as its basis
            // (a basis the mirror lost would cost the next build the whole history).
            await GitAsync(RepoMirror, ["fetch", "--no-tags", options.SourceRepo,
                "+" + SnapshotRefs + "*:" + SnapshotRefs + "*"], TimeSpan.FromMinutes(10), cancel, check: false).ConfigureAwait(false);
        }
        if (!await HasCommitAsync(RepoMirror, commit, cancel).ConfigureAwait(false))
        {
            // A commit on no branch (a detached HEAD, a stash): ask the source for it by name; git allows that from a
            // local repository.
            await GitAsync(RepoMirror, ["fetch", "--no-tags", options.SourceRepo, commit], TimeSpan.FromMinutes(10), cancel,
                check: false).ConfigureAwait(false);
            if (!await HasCommitAsync(RepoMirror, commit, cancel).ConfigureAwait(false))
            {
                throw new InvalidOperationException($"commit {commit} is not in {options.SourceRepo}");
            }
        }
    }

    /// <summary>The commits a mirror holds as tips, and whether it holds one commit (<c>GET /api/v1/source/tips</c>).</summary>
    /// <param name="repo"><see cref="RepoName"/> or <see cref="SdkName"/>.</param>
    /// <param name="want">A commit to look up (a full SHA), or null.</param>
    /// <param name="cancel">Cancels git.</param>
    /// <returns>The newest <see cref="MaxTips"/> tips of the pushed refs and branches (by commit date), and whether the
    /// mirror holds <paramref name="want"/> (null when none was asked).</returns>
    /// <exception cref="ArgumentException">An unknown repo, or a want that is not a full SHA.</exception>
    /// <exception cref="InvalidOperationException">git failed.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    /// <remarks>`amb` leaves out of its bundle everything these tips reach, so a push carries only what the mirror lacks;
    /// it first drops the tips its own repository does not have (git refuses to exclude an unknown commit).</remarks>
    public async Task<SourceTips> TipsAsync(string repo, string? want, CancellationToken cancel)
    {
        string mirror = MirrorOf(repo);
        if (want is not null && !IsFullSha(want))
        {
            throw new ArgumentException("want must be a full 40-character commit SHA", nameof(want));
        }
        await EnsureBareAsync(mirror, cancel).ConfigureAwait(false);
        CommandResult refs = await GitAsync(mirror, ["for-each-ref", "--sort=-committerdate",
            "--count=" + MaxTips.ToString(System.Globalization.CultureInfo.InvariantCulture), "--format=%(objectname)",
            PushedRefs, "refs/heads/"], TimeSpan.FromSeconds(60), cancel).ConfigureAwait(false);
        string[] tips = [.. refs.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Where(IsFullSha).Distinct(StringComparer.Ordinal)];
        bool? has = want is null ? null : await HasCommitAsync(mirror, want.ToLowerInvariant(), cancel).ConfigureAwait(false);
        return new SourceTips(repo, tips, has);
    }

    /// <summary>Takes a pushed bundle into a mirror (<c>POST /api/v1/source/bundle</c>): verifies that it applies, then
    /// fetches its <c>refs/push/&lt;commit&gt;</c> refs as <c>refs/amb/pushed/&lt;commit&gt;</c>.</summary>
    /// <param name="repo"><see cref="RepoName"/> or <see cref="SdkName"/>.</param>
    /// <param name="bundleFile">The bundle, received into the site's scratch folder (the caller deletes it).</param>
    /// <param name="cancel">Cancels git.</param>
    /// <returns>The commits the bundle's refs name, now in the mirror.</returns>
    /// <exception cref="ArgumentException">An unknown repo.</exception>
    /// <exception cref="InvalidOperationException">The bundle does not apply (its prerequisites are not in the mirror:
    /// `amb` then pushes the whole history), carries a ref other than <c>refs/push/&lt;its commit&gt;</c>, or git
    /// failed.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    /// <remarks>Only <c>refs/push/&lt;sha&gt;</c> refs whose name is their own commit are taken, so a bundle can never move
    /// a branch or another push's ref. A bare mirror runs no hooks on a fetch.</remarks>
    public async Task<IReadOnlyList<string>> ReceiveBundleAsync(string repo, string bundleFile, CancellationToken cancel)
    {
        string mirror = MirrorOf(repo);
        await PushLock.WaitAsync(cancel).ConfigureAwait(false);
        try
        {
            await EnsureBareAsync(mirror, cancel).ConfigureAwait(false);
            CommandResult verify = await GitAsync(mirror, ["bundle", "verify", "-q", bundleFile], TimeSpan.FromMinutes(10),
                cancel, check: false).ConfigureAwait(false);
            if (verify.ExitCode != 0)
            {
                throw new InvalidOperationException($"the bundle does not apply to this site's {repo} mirror: {verify.Error.Trim()}");
            }
            CommandResult heads = await GitAsync(mirror, ["bundle", "list-heads", bundleFile], TimeSpan.FromMinutes(2), cancel)
                .ConfigureAwait(false);
            var commits = new List<string>();
            foreach (string line in heads.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            {
                // "<sha> <ref>": the ref must be refs/push/ followed by that same sha.
                string[] parts = line.Split(' ', 2);
                if (parts.Length != 2 || !IsFullSha(parts[0]) || parts[1] != PushRefPrefix + parts[0])
                {
                    throw new InvalidOperationException($"the bundle carries '{line}': only {PushRefPrefix}<commit> refs are taken");
                }
                commits.Add(parts[0]);
            }
            if (commits.Count == 0)
            {
                throw new InvalidOperationException($"the bundle carries no {PushRefPrefix}<commit> ref");
            }
            await GitAsync(mirror, ["fetch", "--no-tags", "-q", bundleFile, "+" + PushRefPrefix + "*:" + PushedRefs + "*"],
                TimeSpan.FromMinutes(20), cancel).ConfigureAwait(false);
            log.LogInformation("push into {Repo}: {Commits}", repo, string.Join(' ', commits));
            return commits;
        }
        finally
        {
            PushLock.Release();
        }
    }

    /// <summary>Writes one folder of a commit out of the repository's mirror (an e2e suite: <c>e2e</c>).</summary>
    /// <param name="commit">The commit (a full SHA the mirror holds; <see cref="EnsureCommitAsync"/> first).</param>
    /// <param name="path">The folder in the tree, '/'-separated, e.g. "e2e".</param>
    /// <param name="target">An empty folder (made when missing) the folder's files go into.</param>
    /// <param name="cancel">Cancels git.</param>
    /// <returns>How many files were written.</returns>
    /// <exception cref="InvalidOperationException">The commit has no such folder, or git failed.</exception>
    /// <exception cref="IOException">A file could not be written, or an entry would land outside the target.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    /// <remarks>Through <c>git archive</c>: the committed files as a checkout on Linux writes them (the repository's own
    /// attributes apply, the machine's core.autocrlf does not), never a working tree's. Only folders and regular files are
    /// written (a suite holds nothing else; tar's PAX header with the commit id is skipped).</remarks>
    public async Task<int> ExportTreeAsync(string commit, string path, string target, CancellationToken cancel)
    {
        string root = Path.GetFullPath(target);
        Directory.CreateDirectory(root);
        var info = new System.Diagnostics.ProcessStartInfo("git")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        // core.autocrlf off: the export must not depend on the machine's global git config (true on homepc, which would
        // turn every LF into CRLF); the repository's own attributes still apply, as in a checkout on Linux.
        foreach (string arg in (string[])["-c", "safe.directory=*", "-c", "core.autocrlf=false", "-C", RepoMirror, "archive",
            "--format=tar", commit + ":" + path])
        {
            info.ArgumentList.Add(arg);
        }
        using var git = System.Diagnostics.Process.Start(info) ?? throw new InvalidOperationException("git did not start");
        // stderr is drained beside the archive, or a chatty git would block on a full pipe while the reader waits.
        Task<string> error = git.StandardError.ReadToEndAsync(CancellationToken.None);
        int files = 0;
        Stream archive = git.StandardOutput.BaseStream;
        // A failed git writes nothing, and the reader then fails on the empty stream before git's own words can be read:
        // the read's error is kept, and git's exit decides which one is reported.
        Exception? cutShort = null;
        try
        {
            // leaveOpen: the reader stops at tar's end marker, but git pads the archive to a 10 KB record after it;
            // closing the pipe there ends git with a broken pipe (a non-zero exit, no message), so the rest is drained.
            await using var tar = new System.Formats.Tar.TarReader(archive, leaveOpen: true);
            while (await tar.GetNextEntryAsync(copyData: false, cancel).ConfigureAwait(false) is { } entry)
            {
                if (entry.EntryType is not (System.Formats.Tar.TarEntryType.Directory or System.Formats.Tar.TarEntryType.RegularFile
                    or System.Formats.Tar.TarEntryType.V7RegularFile))
                {
                    continue;
                }
                string full = Path.GetFullPath(Path.Combine(root, entry.Name));
                if (!full.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.Ordinal) && full != root)
                {
                    throw new IOException($"{entry.Name} leaves the target folder");
                }
                if (entry.EntryType == System.Formats.Tar.TarEntryType.Directory)
                {
                    Directory.CreateDirectory(full);
                    continue;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(full)!);
                await entry.ExtractToFileAsync(full, overwrite: true, cancel).ConfigureAwait(false);
                files++;
            }
        }
        catch (Exception ex) when (ex is EndOfStreamException or InvalidDataException)
        {
            cutShort = ex;
        }
        await archive.CopyToAsync(Stream.Null, cancel).ConfigureAwait(false);
        await git.WaitForExitAsync(cancel).ConfigureAwait(false);
        if (git.ExitCode != 0)
        {
            throw new InvalidOperationException($"{path} of {commit[..Math.Min(9, commit.Length)]} could not be exported: {(await error.ConfigureAwait(false)).Trim()}");
        }
        if (cutShort is not null)
        {
            throw new InvalidOperationException($"{path} of {commit[..Math.Min(9, commit.Length)]} came out of git cut short: {cutShort.Message}");
        }
        return files;
    }

    /// <summary>Whether a string is a full lowercase-or-uppercase hex SHA-1.</summary>
    /// <param name="text">The text.</param>
    /// <returns>True for 40 hex digits.</returns>
    internal static bool IsFullSha(string text) => text.Length == 40 && text.All(char.IsAsciiHexDigit);

    /// <summary>Writes a VM's bundles into a folder (its share's src/).</summary>
    /// <param name="requestId">The request (its temporary refs are named by it, and deleted afterwards).</param>
    /// <param name="commit">The commit to build.</param>
    /// <param name="basis">The commit the VM's worktree holds, or null.</param>
    /// <param name="sdkCommit">The submodule's pinned commit, or null.</param>
    /// <param name="sdkBasis">The submodule commit the VM holds, or null.</param>
    /// <param name="folder">Where the bundles go.</param>
    /// <param name="forceFull">Bundle the whole history even when a basis is known (the guest reported that its clone
    /// lacks the basis: a data disk made anew).</param>
    /// <param name="cancel">Cancels git.</param>
    /// <returns>What was written.</returns>
    /// <exception cref="InvalidOperationException">git failed to make a bundle.</exception>
    /// <exception cref="IOException">The folder could not be made.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    public async Task<SourceBundle> BundleAsync(string requestId, string commit, string? basis, string? sdkCommit,
        string? sdkBasis, string folder, bool forceFull, CancellationToken cancel)
    {
        Directory.CreateDirectory(folder);
        string bundleRef = "refs/amb/req/" + requestId;
        bool basisKnown = basis is not null && await HasCommitAsync(RepoMirror, basis, cancel).ConfigureAwait(false);
        bool reachable = basisKnown && (await GitAsync(RepoMirror, ["merge-base", "--is-ancestor", commit, basis!],
            TimeSpan.FromSeconds(60), cancel, check: false).ConfigureAwait(false)).ExitCode == 0;
        BundleMode mode = forceFull ? BundleMode.Full : PlanBundle(commit, basis, basisKnown, reachable);
        string? file = null;
        long bytes = 0;
        if (mode != BundleMode.None)
        {
            file = "agentmaster.bundle";
            bytes += await WriteBundleAsync(RepoMirror, bundleRef, commit, mode == BundleMode.Incremental ? basis : null,
                Path.Combine(folder, file), cancel).ConfigureAwait(false);
        }
        string? sdkFile = null;
        string sdkRef = "refs/amb/sdk/" + requestId;
        if (sdkCommit is not null && (forceFull || sdkBasis != sdkCommit))
        {
            // The submodule's repository is ~0.6 MB: always its whole history, never a basis to get wrong.
            sdkFile = "claude-agent-sdk-cpp.bundle";
            bytes += await WriteBundleAsync(SdkMirror, sdkRef, sdkCommit, null, Path.Combine(folder, sdkFile), cancel)
                .ConfigureAwait(false);
        }
        log.LogInformation("bundles for {Commit}: {Mode}, {Bytes} bytes", commit, mode, bytes);
        return new SourceBundle(commit, bundleRef, mode, file, sdkCommit, sdkRef, sdkFile, bytes);
    }

    /// <summary>Makes one bundle through a temporary ref, deleted again afterwards.</summary>
    /// <param name="mirror">The mirror.</param>
    /// <param name="bundleRef">The ref the bundle carries.</param>
    /// <param name="commit">The commit it points at.</param>
    /// <param name="basis">Leave out what this commit reaches (an incremental bundle), or null for the whole history.</param>
    /// <param name="path">The bundle file.</param>
    /// <param name="cancel">Cancels git.</param>
    /// <returns>The bundle's size.</returns>
    /// <exception cref="InvalidOperationException">git failed.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    private async Task<long> WriteBundleAsync(string mirror, string bundleRef, string commit, string? basis, string path,
        CancellationToken cancel)
    {
        await GitAsync(mirror, ["update-ref", bundleRef, commit], TimeSpan.FromSeconds(30), cancel).ConfigureAwait(false);
        try
        {
            List<string> args = ["bundle", "create", "-q", path, bundleRef];
            if (basis is not null)
            {
                args.Add("^" + basis);
            }
            await GitAsync(mirror, args, TimeSpan.FromMinutes(10), cancel).ConfigureAwait(false);
            return new FileInfo(path).Length;
        }
        finally
        {
            // Cleanup-on-failure too: the temporary ref would otherwise keep the mirror's history of every request.
            await GitAsync(mirror, ["update-ref", "-d", bundleRef], TimeSpan.FromSeconds(30), CancellationToken.None,
                check: false).ConfigureAwait(false);
        }
    }

    /// <summary>Brings the repository's mirror up to date with the source's branches and tags (made when missing).</summary>
    /// <param name="cancel">Cancels the fetch.</param>
    /// <returns>A task.</returns>
    /// <exception cref="InvalidOperationException">git failed.</exception>
    /// <exception cref="TimeoutException">The fetch took longer than 20 minutes.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    /// <remarks>The first fetch of K:\source\Agentmaster took 92 s (145 MB), later ones 0-14 s (measured 2026-10-26).
    /// --prune only touches refs/heads and refs/tags: snapshots under <see cref="SnapshotRefs"/> stay. A pushing site
    /// (<see cref="CoordinatorOptions.PushesSource"/>) has no source to fetch: the mirror is only made when missing.</remarks>
    public async Task RefreshAsync(CancellationToken cancel)
    {
        await EnsureBareAsync(RepoMirror, cancel).ConfigureAwait(false);
        if (options.PushesSource)
        {
            return;
        }
        await GitAsync(RepoMirror, ["fetch", "--prune", "--no-tags", options.SourceRepo,
            "+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*"], TimeSpan.FromMinutes(20), cancel).ConfigureAwait(false);
    }

    /// <summary>The first parent of a commit in the repository's mirror.</summary>
    /// <param name="commit">The commit (a full SHA the mirror has).</param>
    /// <param name="cancel">Cancels git.</param>
    /// <returns>The parent's full SHA, or null for a root commit.</returns>
    /// <exception cref="InvalidOperationException">git failed (the mirror lacks the commit).</exception>
    public async Task<string?> ParentAsync(string commit, CancellationToken cancel)
    {
        CommandResult parents = await GitAsync(RepoMirror, ["rev-list", "--parents", "-n", "1", commit],
            TimeSpan.FromSeconds(30), cancel).ConfigureAwait(false);
        // "<commit> <parent1> [<parent2> ...]"
        string[] shas = parents.Output.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return shas.Length >= 2 ? shas[1] : null;
    }

    /// <summary>Writes one committed file of the repository's mirror to disk, byte for byte.</summary>
    /// <param name="rev">The revision (a ref such as refs/heads/agentmaster, or a commit).</param>
    /// <param name="path">The file's path in the tree, with '/' separators (tools/Swap-AgentmasterLayout.ps1).</param>
    /// <param name="target">Where it goes (overwritten).</param>
    /// <param name="cancel">Cancels git.</param>
    /// <returns>The commit <paramref name="rev"/> names (the deploy records which copy it ran).</returns>
    /// <exception cref="InvalidOperationException">The revision or the file is not in the mirror.</exception>
    /// <exception cref="IOException">The file could not be written.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    /// <remarks>Raw bytes, never decoded text: a script keeps its exact encoding and line ends (a decoded and re-encoded
    /// copy would drop a byte-order mark, and a .sh with CR line ends breaks bash).</remarks>
    public async Task<string> ExportFileAsync(string rev, string path, string target, CancellationToken cancel)
    {
        CommandResult resolved = await GitAsync(RepoMirror, ["rev-parse", "--verify", "--quiet", rev + "^{commit}"],
            TimeSpan.FromSeconds(30), cancel, check: false).ConfigureAwait(false);
        string commit = resolved.Output.Trim();
        if (resolved.ExitCode != 0 || commit.Length != 40)
        {
            throw new InvalidOperationException($"the mirror has no {rev}");
        }
        var info = new System.Diagnostics.ProcessStartInfo("git")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (string arg in (string[])["-c", "safe.directory=*", "-C", RepoMirror, "cat-file", "blob", commit + ":" + path])
        {
            info.ArgumentList.Add(arg);
        }
        using var git = System.Diagnostics.Process.Start(info)
            ?? throw new InvalidOperationException("git did not start");
        Task<string> error = git.StandardError.ReadToEndAsync(CancellationToken.None);
        string temp = target + ".part";
        await using (var file = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            await git.StandardOutput.BaseStream.CopyToAsync(file, cancel).ConfigureAwait(false);
        }
        await git.WaitForExitAsync(cancel).ConfigureAwait(false);
        if (git.ExitCode != 0)
        {
            File.Delete(temp);
            throw new InvalidOperationException($"{path} is not in {rev} ({commit[..9]}): {(await error.ConfigureAwait(false)).Trim()}");
        }
        File.Move(temp, target, overwrite: true);
        return commit;
    }

    /// <summary>The submodule's pinned commit at a commit (its gitlink), or null when the tree has none.</summary>
    /// <param name="commit">The commit.</param>
    /// <param name="cancel">Cancels git.</param>
    /// <returns>The pinned SHA, or null.</returns>
    /// <exception cref="InvalidOperationException">git failed.</exception>
    private async Task<string?> GitlinkAsync(string commit, CancellationToken cancel)
    {
        CommandResult tree = await GitAsync(RepoMirror, ["ls-tree", commit, "--", SubmodulePath], TimeSpan.FromSeconds(30),
            cancel).ConfigureAwait(false);
        // "160000 commit <sha>\t<path>": mode 160000 is a gitlink.
        string[] parts = tree.Output.Trim().Split((char[])[' ', '\t'], StringSplitOptions.RemoveEmptyEntries);
        return parts.Length >= 3 && parts[0] == "160000" && parts[1] == "commit" ? parts[2] : null;
    }

    /// <summary>Whether a mirror has a commit.</summary>
    /// <param name="mirror">The mirror.</param>
    /// <param name="commit">The commit.</param>
    /// <param name="cancel">Cancels git.</param>
    /// <returns>True when the commit's object is there.</returns>
    private async Task<bool> HasCommitAsync(string mirror, string commit, CancellationToken cancel) =>
        (await GitAsync(mirror, ["cat-file", "-e", commit + "^{commit}"], TimeSpan.FromSeconds(30), cancel, check: false)
            .ConfigureAwait(false)).ExitCode == 0;

    /// <summary>Makes a bare repository unless it exists.</summary>
    /// <param name="mirror">Its folder.</param>
    /// <param name="cancel">Cancels git.</param>
    /// <returns>A task.</returns>
    /// <exception cref="InvalidOperationException">git failed.</exception>
    private async Task EnsureBareAsync(string mirror, CancellationToken cancel)
    {
        if (Directory.Exists(Path.Combine(mirror, "objects")))
        {
            return;
        }
        Directory.CreateDirectory(mirror);
        await GitAsync(mirror, ["init", "-q", "--bare"], TimeSpan.FromSeconds(30), cancel).ConfigureAwait(false);
    }

    /// <summary>Runs git in a mirror.</summary>
    /// <param name="mirror">The mirror (git's -C).</param>
    /// <param name="args">git's arguments.</param>
    /// <param name="timeout">The longest wait.</param>
    /// <param name="cancel">Cancels it.</param>
    /// <param name="check">Throw when git fails.</param>
    /// <returns>git's exit code and output.</returns>
    /// <exception cref="InvalidOperationException">git failed and <paramref name="check"/> is set.</exception>
    /// <exception cref="TimeoutException">It took longer than <paramref name="timeout"/>.</exception>
    /// <exception cref="OperationCanceledException"><paramref name="cancel"/> was cancelled.</exception>
    private static async Task<CommandResult> GitAsync(string mirror, IEnumerable<string> args, TimeSpan timeout,
        CancellationToken cancel, bool check = true)
    {
        CommandResult result = await Commands.RunAsync("git", ["-c", "safe.directory=*", "-C", mirror, .. args], timeout,
            cancel).ConfigureAwait(false);
        if (check && result.ExitCode != 0)
        {
            throw new InvalidOperationException($"git {string.Join(' ', args)} failed ({result.ExitCode}): {result.Error.Trim()}");
        }
        return result;
    }
}
