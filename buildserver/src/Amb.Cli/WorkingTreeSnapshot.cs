using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace Amb.Cli;

/// <summary>A refusal to snapshot: the uncommitted work holds files too big to copy into the repository's objects.</summary>
/// <param name="message">What is too big, and what to do instead.</param>
internal sealed class SnapshotRefusedException(string message) : Exception(message);

/// <summary>
/// Makes a SNAPSHOT commit of a working tree's uncommitted work, for `amb build --snapshot` (buildserver/DESIGN.md §11:
/// "uncommitted work builds only as a snapshot commit under refs/buildserver/snapshots/&lt;id&gt;; no branch or HEAD moves").
/// </summary>
/// <remarks>
/// <para><b>Nothing the user works with changes:</b> the snapshot is built in a PRIVATE index (GIT_INDEX_FILE, a copy of
/// the repository's own index, or HEAD's tree when paths are named), so the real index, HEAD, every branch and the working
/// tree stay as they were. What it writes into the repository: the new files' objects and one ref under
/// <see cref="RefPrefix"/>, which the site's mirror fetches (it only reads the repository) and <see cref="PruneBuilt"/>
/// deletes once the snapshot's build exists.</para>
/// <para><b>The same work makes the same commit:</b> the author, committer and their dates are fixed (the dates are
/// HEAD's committer date) and the message depends only on HEAD and the tree, so snapshotting an unchanged working tree
/// twice gives one commit, and the coordinator answers the second request with the first build's id.</para>
/// <para><b>Whose work:</b> the whole working tree by default, which on this PC includes other sessions' uncommitted
/// edits; naming paths takes HEAD plus only those paths' working-tree state (what a session that owns only those files
/// wants). Untracked files are taken unless ignored; a submodule is taken at its checked-out commit, never its own
/// uncommitted files.</para>
/// </remarks>
internal static class WorkingTreeSnapshot
{
    /// <summary>Where snapshots live in the repository (and in the site's mirror).</summary>
    internal const string RefPrefix = "refs/buildserver/snapshots/";

    /// <summary>The largest file a snapshot takes (100 MiB): K:, where the repository's objects live, is nearly full, and
    /// a dump or a build output left in the tree is never meant to be built.</summary>
    internal const long MaxFileBytes = 100L << 20;

    /// <summary>The most a snapshot's new files may hold together (300 MiB), for the same reason.</summary>
    internal const long MaxTotalBytes = 300L << 20;

    /// <summary>The fixed identity of every snapshot commit (part of what makes them reproducible).</summary>
    private const string Author = "amb snapshot";

    /// <summary>The fixed e-mail of every snapshot commit (.invalid: it names no mailbox, by RFC 2606).</summary>
    private const string AuthorEmail = "amb-snapshot@buildserver.invalid";

    /// <summary>One uncommitted change.</summary>
    /// <param name="Status">git status' two-letter code ("??" untracked, " M" modified, " D" deleted, ...).</param>
    /// <param name="Path">The path, relative to the repository's root, with '/' separators.</param>
    /// <param name="Bytes">The file's size in the working tree (0 when deleted, or a folder such as a submodule).</param>
    internal sealed record Change(string Status, string Path, long Bytes);

    /// <summary>What a snapshot made.</summary>
    /// <param name="Commit">The commit to build: the snapshot, or HEAD itself when there was nothing to take.</param>
    /// <param name="Head">The commit the snapshot was made on.</param>
    /// <param name="Changed">Whether the snapshot differs from HEAD (false: build HEAD).</param>
    internal sealed record Result(string Commit, string Head, bool Changed);

    /// <summary>Lists the uncommitted changes a snapshot would take.</summary>
    /// <param name="repo">The repository.</param>
    /// <param name="paths">Paths to limit it to (git pathspecs), or none for the whole working tree.</param>
    /// <returns>The changes, in git's order.</returns>
    /// <exception cref="InvalidOperationException">git failed.</exception>
    /// <remarks>GIT_OPTIONAL_LOCKS=0: git status would otherwise refresh and rewrite the repository's index, a write into
    /// the user's state that a read must never make (and a lock other sessions' git calls would wait on).</remarks>
    internal static IReadOnlyList<Change> Changes(string repo, IReadOnlyList<string> paths)
    {
        (_, string output, _) = Git(repo, ["status", "--porcelain=v1", "-z", "--untracked-files=all", "--no-renames", "--", .. paths],
            new Dictionary<string, string> { ["GIT_OPTIONAL_LOCKS"] = "0" });
        var changes = new List<Change>();
        foreach (string entry in output.Split('\0', StringSplitOptions.RemoveEmptyEntries))
        {
            // "XY path" (with --no-renames there is no second "from" path).
            if (entry.Length < 4)
            {
                continue;
            }
            string relative = entry[3..];
            string full = Path.Combine(repo, relative.Replace('/', Path.DirectorySeparatorChar));
            long bytes = File.Exists(full) ? new FileInfo(full).Length : 0;
            changes.Add(new Change(entry[..2], relative, bytes));
        }
        return changes;
    }

    /// <summary>Refuses changes that hold a file over <see cref="MaxFileBytes"/> or more than <see cref="MaxTotalBytes"/>.</summary>
    /// <param name="changes">The changes.</param>
    /// <exception cref="SnapshotRefusedException">A limit is passed; the message names the biggest files.</exception>
    internal static void CheckSize(IReadOnlyList<Change> changes)
    {
        long total = changes.Sum(c => c.Bytes);
        Change[] big = [.. changes.OrderByDescending(c => c.Bytes).Take(5)];
        if (big.Length > 0 && (big[0].Bytes > MaxFileBytes || total > MaxTotalBytes))
        {
            string list = string.Join(", ", big.Select(c => FormattableString.Invariant($"{c.Path} ({c.Bytes / 1048576.0:0.0} MB)")));
            throw new SnapshotRefusedException(FormattableString.Invariant(
                $"the uncommitted work holds {total / 1048576.0:0.0} MB (the biggest: {list}); a snapshot takes at most {MaxFileBytes >> 20} MB per file and {MaxTotalBytes >> 20} MB in all. Name the paths to take (amb build --snapshot -- <paths>), ignore the big files, or commit."));
        }
    }

    /// <summary>Makes the snapshot commit and its ref.</summary>
    /// <param name="repo">The repository.</param>
    /// <param name="paths">Paths to take (git pathspecs; HEAD's state for everything else), or none for the whole tree.</param>
    /// <param name="tempDir">A folder for the private index (removed again).</param>
    /// <returns>The commit to build.</returns>
    /// <exception cref="InvalidOperationException">git failed.</exception>
    /// <exception cref="IOException">The private index could not be made.</exception>
    internal static Result Create(string repo, IReadOnlyList<string> paths, string tempDir)
    {
        string head = Git(repo, ["rev-parse", "--verify", "HEAD^{commit}"]).Output.Trim();
        Directory.CreateDirectory(tempDir);
        string index = Path.Combine(tempDir, "amb-snapshot-" + Guid.NewGuid().ToString("N") + ".index");
        var env = new Dictionary<string, string> { ["GIT_INDEX_FILE"] = index };
        try
        {
            if (paths.Count == 0)
            {
                // A copy of the repository's index: its stat data lets `git add -A` hash only what changed. Read with
                // every sharing mode, so a git call of another session that replaces the index meanwhile is never blocked.
                string real = Git(repo, ["rev-parse", "--git-path", "index"]).Output.Trim();
                real = Path.IsPathRooted(real) ? real : Path.Combine(repo, real);
                using (var source = new FileStream(real, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var target = new FileStream(index, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    source.CopyTo(target);
                }
                Git(repo, ["add", "-A"], env);
            }
            else
            {
                // HEAD plus only the named paths: everything else stays as committed, whatever the working tree holds.
                Git(repo, ["read-tree", head], env);
                Git(repo, ["add", "-A", "--", .. paths], env);
            }
            string tree = Git(repo, ["write-tree"], env).Output.Trim();
            string headTree = Git(repo, ["rev-parse", "--verify", head + "^{tree}"]).Output.Trim();
            if (tree == headTree)
            {
                return new Result(head, head, Changed: false);
            }
            string stamp = Git(repo, ["log", "-1", "--format=%ct", head]).Output.Trim() + " +0000";
            var identity = new Dictionary<string, string>
            {
                ["GIT_AUTHOR_NAME"] = Author,
                ["GIT_AUTHOR_EMAIL"] = AuthorEmail,
                ["GIT_AUTHOR_DATE"] = stamp,
                ["GIT_COMMITTER_NAME"] = Author,
                ["GIT_COMMITTER_EMAIL"] = AuthorEmail,
                ["GIT_COMMITTER_DATE"] = stamp,
            };
            // --no-gpg-sign: a commit.gpgSign setting would sign it, and a signature differs every time.
            string commit = Git(repo, ["commit-tree", tree, "-p", head, "--no-gpg-sign", "-m", Message(head, tree)], identity)
                .Output.Trim();
            Git(repo, ["update-ref", RefPrefix + commit, commit]);
            return new Result(commit, head, Changed: true);
        }
        finally
        {
            // Cleanup in every outcome: the private index (a few MB) and the lock git may have left beside it.
            File.Delete(index);
            File.Delete(index + ".lock");
        }
    }

    /// <summary>The snapshot commit's message: it depends on nothing but HEAD and the tree.</summary>
    /// <param name="head">The commit the snapshot is made on.</param>
    /// <param name="tree">The snapshot's tree.</param>
    /// <returns>The message.</returns>
    internal static string Message(string head, string tree) =>
        $"amb snapshot of {head}\n\nUncommitted work on top of {head[..9]}, built by the build server (`amb build --snapshot`).\nTree {tree}.\n";

    /// <summary>Deletes the repository's snapshot refs whose commits were built (the site's mirror keeps its own copy).</summary>
    /// <param name="repo">The repository.</param>
    /// <param name="builtCommits">The commits of every build the coordinator holds.</param>
    /// <returns>How many refs were deleted.</returns>
    /// <exception cref="InvalidOperationException">git failed to list the refs.</exception>
    /// <remarks>A snapshot not built yet keeps its ref: the mirror fetches it when its build starts.</remarks>
    internal static int PruneBuilt(string repo, IReadOnlySet<string> builtCommits)
    {
        int deleted = 0;
        string refs = Git(repo, ["for-each-ref", "--format=%(refname)", RefPrefix]).Output;
        foreach (string name in refs.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            if (builtCommits.Contains(name[RefPrefix.Length..]) && Git(repo, ["update-ref", "-d", name], check: false).ExitCode == 0)
            {
                deleted++;
            }
        }
        return deleted;
    }

    /// <summary>The checkout a folder is in, when that checkout belongs to <paramref name="repository"/>'s repository: the
    /// repository's own working tree or one of its linked worktrees (`git worktree add`), which share its common git
    /// folder.</summary>
    /// <param name="folder">The folder (the command line's current directory).</param>
    /// <param name="repository">A checkout of the repository the folder's checkout must belong to
    /// (K:\source\Agentmaster).</param>
    /// <returns>The checkout's top folder, or null when the folder is in no checkout of that repository: another
    /// repository, no repository, a folder that does not exist, or git failing (the caller then keeps its default).</returns>
    /// <remarks>
    /// <para>Why: `amb build`, `amb build --snapshot` and an e2e run's suite take their commit or their uncommitted work
    /// from this checkout, so a session working in a worktree builds its own tree. Before 2026-11-13 they always read
    /// K:\source\Agentmaster, and a snapshot made from a worktree silently built the main checkout's files.</para>
    /// <para>A folder inside a submodule (refs/claude-agent-sdk-cpp) answers with the checkout that holds the
    /// submodule, the tree a session working there builds. Never throws: every failure reads as "no such checkout".</para>
    /// </remarks>
    internal static string? CheckoutOf(string folder, string repository)
    {
        try
        {
            if (TopFolder(folder) is not string top)
            {
                return null;
            }
            // A submodule's own checkout belongs to another repository: take the checkout that holds it.
            (int code, string superproject, _) = Git(top, ["rev-parse", "--show-superproject-working-tree"], check: false);
            if (code == 0 && superproject.Trim() is { Length: > 0 } holder)
            {
                top = Path.GetFullPath(holder);
            }
            return CommonFolder(top) is string mine && CommonFolder(repository) is string theirs && SamePath(mine, theirs)
                ? top
                : null;
        }
        catch (InvalidOperationException)
        {
            // Expected when git cannot run at all: the caller's default checkout is used, and its own git calls then
            // report the problem.
            return null;
        }
    }

    /// <summary>The top folder of the checkout a folder is in.</summary>
    /// <param name="folder">The folder.</param>
    /// <returns>The checkout's top folder as a full path, or null when the folder does not exist or is in no checkout.</returns>
    /// <exception cref="InvalidOperationException">git cannot be run.</exception>
    private static string? TopFolder(string folder)
    {
        if (!Directory.Exists(folder))
        {
            return null;
        }
        (int code, string output, _) = Git(folder, ["rev-parse", "--show-toplevel"], check: false);
        // git prints the path with forward slashes on Windows ("K:/source/Agentmaster"): the full path normalizes it.
        return code == 0 && output.Trim() is { Length: > 0 } top ? Path.GetFullPath(top) : null;
    }

    /// <summary>A checkout's common git folder: the main repository's .git, which every linked worktree shares.</summary>
    /// <param name="checkout">The checkout.</param>
    /// <returns>The folder as a full path, or null when git cannot read the checkout.</returns>
    /// <exception cref="InvalidOperationException">git cannot be run.</exception>
    private static string? CommonFolder(string checkout)
    {
        if (!Directory.Exists(checkout))
        {
            return null;
        }
        (int code, string output, _) = Git(checkout, ["rev-parse", "--path-format=absolute", "--git-common-dir"], check: false);
        return code == 0 && output.Trim() is { Length: > 0 } common ? Path.GetFullPath(common) : null;
    }

    /// <summary>Whether two full paths name one folder (case-insensitive on Windows, whose file systems are; a trailing
    /// separator ignored).</summary>
    /// <param name="a">One path.</param>
    /// <param name="b">The other.</param>
    /// <returns>True when they are the same folder.</returns>
    private static bool SamePath(string a, string b) =>
        string.Equals(Path.TrimEndingDirectorySeparator(a), Path.TrimEndingDirectorySeparator(b),
            OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

    /// <summary>Runs git in a repository.</summary>
    /// <param name="repo">The repository (git's -C).</param>
    /// <param name="args">git's arguments.</param>
    /// <param name="env">Variables to set for this call, or null.</param>
    /// <param name="check">Throw when git fails.</param>
    /// <param name="stdin">Text for git's standard input (UTF-8 without a byte-order mark, which git would read as part
    /// of the first revision), or null for none (git then reads an empty input).</param>
    /// <returns>git's exit code, standard output and standard error (UTF-8).</returns>
    /// <exception cref="InvalidOperationException">git failed and <paramref name="check"/> is set, or git is not on PATH.</exception>
    internal static (int ExitCode, string Output, string Error) Git(string repo, IReadOnlyList<string> args,
        IReadOnlyDictionary<string, string>? env = null, bool check = true, string? stdin = null)
    {
        var info = new ProcessStartInfo("git")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = stdin is not null,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
            StandardInputEncoding = stdin is null ? null : new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
        };
        foreach (string arg in (string[])["-C", repo, "-c", "core.quotepath=off", .. args])
        {
            info.ArgumentList.Add(arg);
        }
        foreach ((string name, string value) in env ?? new Dictionary<string, string>())
        {
            info.Environment[name] = value;
        }
        try
        {
            using Process git = Process.Start(info) ?? throw new InvalidOperationException("git did not start");
            Task<string> error = git.StandardError.ReadToEndAsync();
            string output;
            if (stdin is null)
            {
                output = git.StandardOutput.ReadToEnd();
            }
            else
            {
                // The output is read while the input is written: `cat-file --batch-check` answers line by line, and a
                // full output pipe would stop git reading its input, and this write with it.
                Task<string> reading = git.StandardOutput.ReadToEndAsync();
                git.StandardInput.Write(stdin);
                git.StandardInput.Close();
                output = reading.GetAwaiter().GetResult();
            }
            git.WaitForExit();
            string errorText = error.GetAwaiter().GetResult();
            if (check && git.ExitCode != 0)
            {
                throw new InvalidOperationException(string.Create(CultureInfo.InvariantCulture,
                    $"git {string.Join(' ', args)} failed ({git.ExitCode}): {errorText.Trim()}"));
            }
            return (git.ExitCode, output, errorText);
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            throw new InvalidOperationException("git cannot be run (is it on PATH?): " + ex.Message, ex);
        }
    }
}
