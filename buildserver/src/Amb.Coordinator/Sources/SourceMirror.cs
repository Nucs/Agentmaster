using Amb.Coordinator.Slots;

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
/// VM gets a git bundle of what its clone lacks, written into its share.
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

    /// <summary>The repository's mirror.</summary>
    private string RepoMirror => Path.Combine(options.GitDir, "agentmaster.git");

    /// <summary>The submodule's mirror.</summary>
    private string SdkMirror => Path.Combine(options.GitDir, "claude-agent-sdk-cpp.git");

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
    public async Task<string?> FetchAsync(string commit, CancellationToken cancel)
    {
        await EnsureBareAsync(RepoMirror, cancel).ConfigureAwait(false);
        await GitAsync(RepoMirror, ["fetch", "--prune", "--no-tags", options.SourceRepo,
            "+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*"], TimeSpan.FromMinutes(20), cancel).ConfigureAwait(false);
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
        string? sdk = await GitlinkAsync(commit, cancel).ConfigureAwait(false);
        if (sdk is null)
        {
            return null;
        }
        await EnsureBareAsync(SdkMirror, cancel).ConfigureAwait(false);
        // The source repository's own copy of the submodule first (the pinned commit is usually its detached HEAD), then
        // the URL .gitmodules names (GitHub) as the last resort.
        string local = Path.Combine(options.SourceRepo, ".git", "modules", "refs", "claude-agent-sdk-cpp");
        if (Directory.Exists(local) && !await HasCommitAsync(SdkMirror, sdk, cancel).ConfigureAwait(false))
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
