using System.Globalization;
using System.Net;
using System.Text.Json;
using Amb.Cli;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Protocol;

// amb: the build server's command line (buildserver/DESIGN.md §10).
//
//   amb status | list [--limit N] | show <id> | builds | cancel <id>
//   amb logs <id> [-f] [--wait-max S]
//   amb import <zip> [--commit SHA] [--wait] [--wait-max S]
//   amb smoke [--image am-base] [--wait] [--wait-max S]
//   amb e2e <build id> [--image am-e2e] [--suite PATH] [--wait] [--wait-max S] [-- pytest arguments]
//   amb build [<rev>] [--config Release|Debug] [--identity dev|release] [--purge none|force] [--wait] [--wait-max S]
//   amb build --snapshot [--config ...] [-- <paths>]     (uncommitted work: the whole working tree, or only <paths>)
//   amb artifacts <build id>
//   amb fetch <build id> [--kind portable-zip|layout-zip|symbols-zip|build-log|build-binlog|build-json|all] [--to DIR]
//   amb install <build id> --instance dev|production [--dry-run] [--wait-others MIN] [--wait] [--wait-max S]
//   import, smoke, e2e, build and install also take --run-timeout MIN and --wait-timeout MIN (1-1440; else the kind's
//   defaults, e.g. e2e 120 / 120, build 180 / 60: the whole e2e suite needs a longer run timeout).
//   build resolves <rev> (default HEAD) in K:\source\Agentmaster (AMB_REPO overrides) and sends that commit: a build id
//   names immutable bytes, so the service is never asked for a branch that may move. --snapshot makes a commit of the
//   uncommitted work instead (WorkingTreeSnapshot.cs: a private index, a ref under refs/buildserver/snapshots/, nothing
//   else in the repository changes). fetch writes to Q:\buildserver\out\<build id> unless --to says otherwise, never to
//   K: (every build writes to Q:).
//   install (alias deploy) moves an instance onto a build through the swap script (Jobs/DeployJob.cs): it stops the
//   instance and ends every claude it hosts. Run from a session INSIDE that instance it waits for this session's turn to
//   end; --wait then returns as soon as the install starts that wait (end the turn; `amb logs <id> -f` from elsewhere).
//   --json on any command prints the coordinator's JSON instead of the text form; AMB_URL overrides the address.
//
// Exit codes: 0 succeeded (or accepted, without --wait); 1 the request ended otherwise; 2 usage; 97 still running when
// --wait-max ran out (attach again with `amb logs <id> -f`); 98 the coordinator could not be reached or woken.

string url = Environment.GetEnvironmentVariable("AMB_URL") is { Length: > 0 } u ? u : "http://127.0.0.1:2207";
var options = Options.Parse(args);
if (options.Command is null)
{
    Console.Error.WriteLine("usage: amb status|list|show|logs|cancel|builds|import|smoke|e2e|build|artifacts|fetch|install ... (see the header of Program.cs)");
    return 2;
}
using var client = new SiteClient(url);
using var stop = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    // Ctrl+C stops watching; the request itself goes on (cancel it with `amb cancel <id>`).
    e.Cancel = true;
    stop.Cancel();
};
try
{
    return options.Command switch
    {
        "status" => await Commands.StatusAsync(client, options, stop.Token),
        "list" => await Commands.ListAsync(client, options, stop.Token),
        "show" => await Commands.ShowAsync(client, options, stop.Token),
        "builds" => await Commands.BuildsAsync(client, options, stop.Token),
        "cancel" => await Commands.CancelAsync(client, options, stop.Token),
        "logs" => await Commands.FollowAsync(client, options.Positional.FirstOrDefault(), options, stop.Token),
        "import" or "smoke" or "e2e" or "build" or "install" or "deploy" => await Commands.SubmitAsync(client, options, stop.Token),
        "artifacts" => await Commands.ArtifactsAsync(client, options, stop.Token),
        "fetch" => await Commands.FetchAsync(client, options, stop.Token),
        _ => Usage($"unknown command {options.Command}"),
    };
}
catch (SiteUnavailableException ex)
{
    Console.Error.WriteLine("amb: " + ex.Message);
    return 98;
}
catch (OperationCanceledException)
{
    return 97;
}

// Prints a usage error (to stderr) and gives the usage exit code, 2.
static int Usage(string message)
{
    Console.Error.WriteLine("amb: " + message);
    return 2;
}

/// <summary>The parsed command line: the command, its positional arguments, its options and what follows "--".</summary>
internal sealed class Options
{
    /// <summary>The command (the first argument), or null when there is none.</summary>
    public string? Command { get; private set; }

    /// <summary>The positional arguments after the command.</summary>
    public List<string> Positional { get; } = [];

    /// <summary>The arguments after "--" (pytest's, for e2e).</summary>
    public List<string> Rest { get; } = [];

    /// <summary>Options with a value (--image X).</summary>
    public Dictionary<string, string> Values { get; } = new(StringComparer.Ordinal);

    /// <summary>Flags (--wait, -f, --json).</summary>
    public HashSet<string> Flags { get; } = new(StringComparer.Ordinal);

    /// <summary>The options that take a value; every other "-x" is a flag.</summary>
    private static readonly HashSet<string> WithValue =
        ["--image", "--suite", "--commit", "--limit", "--wait-max", "--run-timeout", "--wait-timeout", "--config", "--identity",
         "--purge", "--kind", "--to", "--instance", "--wait-others", "--inject-failure"];

    /// <summary>Parses the arguments.</summary>
    /// <param name="args">The process's arguments.</param>
    /// <returns>The parsed form.</returns>
    public static Options Parse(string[] args)
    {
        var options = new Options();
        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i];
            if (arg == "--")
            {
                options.Rest.AddRange(args[(i + 1)..]);
                break;
            }
            if (WithValue.Contains(arg) && i + 1 < args.Length)
            {
                options.Values[arg] = args[++i];
            }
            else if (arg.StartsWith('-'))
            {
                options.Flags.Add(arg);
            }
            else if (options.Command is null)
            {
                options.Command = arg;
            }
            else
            {
                options.Positional.Add(arg);
            }
        }
        return options;
    }

    /// <summary>The --wait-max value, or null when not given (or not a number).</summary>
    public TimeSpan? WaitMax => Values.TryGetValue("--wait-max", out string? s)
        && int.TryParse(s, NumberStyles.None, CultureInfo.InvariantCulture, out int seconds)
        ? TimeSpan.FromSeconds(seconds)
        : null;

    /// <summary>Whether --json was given.</summary>
    public bool Json => Flags.Contains("--json");

    /// <summary>Reads a minutes option (--run-timeout, --wait-timeout, --wait-others).</summary>
    /// <param name="name">The option.</param>
    /// <param name="minutes">The value: null when the option was not given.</param>
    /// <param name="min">The least value allowed (1 for timeouts: a 0 would end the request at once).</param>
    /// <param name="max">The most value allowed.</param>
    /// <returns>False when it was given but is not a whole number of minutes from <paramref name="min"/> to
    /// <paramref name="max"/> (the caller reports a usage error: a typo must not become the kind's default silently).</returns>
    public bool TryMinutes(string name, out int? minutes, int min = 1, int max = 1440)
    {
        minutes = null;
        if (!Values.TryGetValue(name, out string? text))
        {
            return true;
        }
        if (int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out int value) && value >= min && value <= max)
        {
            minutes = value;
            return true;
        }
        return false;
    }
}

/// <summary>The CLI's commands: each prints and returns its exit code.</summary>
internal static class Commands
{
    private static readonly string[] Final = ["Succeeded", "Failed", "Cancelled", "NoCapacity", "Lost"];

    /// <summary>Prints the coordinator's status: homepc's free amounts, the active requests and slots.</summary>
    /// <param name="client">The client.</param>
    /// <param name="options">The command line.</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>0, or 1 when the coordinator refused.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> StatusAsync(SiteClient client, Options options, CancellationToken cancel)
    {
        (HttpStatusCode status, CoordinatorStatus? value, string text) =
            await client.GetAsync("status", AmbJsonLine.Default.CoordinatorStatus, cancel);
        if (value is null)
        {
            return Fail(status, text);
        }
        if (options.Json)
        {
            Console.WriteLine(text);
            return 0;
        }
        Console.WriteLine($"amb-coordinator {value.Version}, up since {value.StartedAt:yyyy-MM-dd HH:mm:ss}Z");
        if (value.HostFacts is { } facts)
        {
            Console.WriteLine(FormattableString.Invariant(
                $"Windows: {facts.AvailableMiB / 1024.0:0.0} GB available, {facts.FreeCommitMiB / 1024.0:0.0} GB free commit, Q: {facts.QFreeMiB / 1024.0:0.0} GB free{(facts.FullScreenGame ? ", a full-screen game runs" : "")} ({facts.At:HH:mm:ss}Z)"));
        }
        foreach (var site in value.Sites)
        {
            Console.WriteLine($"{site.Site}: free {site.Free}{(site.Holds.Count > 0 ? "; holds: " + string.Join(", ", site.Holds) : "")}");
        }
        Console.WriteLine($"active requests: {value.Active.Count}");
        foreach (RequestRecord request in value.Active)
        {
            Console.WriteLine($"  {Line(request)}");
        }
        Console.WriteLine($"slots: {value.Slots.Count}");
        foreach (SlotRecord slot in value.Slots)
        {
            Console.WriteLine($"  {slot.Id}  {slot.State,-8} {slot.Image,-10} {slot.Grant}  for {slot.RequestId}");
        }
        return 0;
    }

    /// <summary>Lists the newest requests.</summary>
    /// <param name="client">The client.</param>
    /// <param name="options">The command line (--limit).</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>0, or 1 when the coordinator refused.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> ListAsync(SiteClient client, Options options, CancellationToken cancel)
    {
        string limit = options.Values.GetValueOrDefault("--limit", "20");
        (HttpStatusCode status, List<RequestRecord>? value, string text) =
            await client.GetAsync($"requests?limit={Uri.EscapeDataString(limit)}", AmbJsonLine.Default.ListRequestRecord, cancel);
        if (value is null)
        {
            return Fail(status, text);
        }
        if (options.Json)
        {
            Console.WriteLine(text);
            return 0;
        }
        foreach (RequestRecord request in value)
        {
            Console.WriteLine(Line(request));
        }
        return 0;
    }

    /// <summary>Prints one request's record.</summary>
    /// <param name="client">The client.</param>
    /// <param name="options">The command line (the id).</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>0, 1 when there is no such request, 2 without an id.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> ShowAsync(SiteClient client, Options options, CancellationToken cancel)
    {
        if (options.Positional.Count == 0)
        {
            Console.Error.WriteLine("amb show <request id>");
            return 2;
        }
        (HttpStatusCode status, RequestRecord? value, string text) =
            await client.GetAsync($"requests/{Uri.EscapeDataString(options.Positional[0])}", AmbJsonLine.Default.RequestRecord, cancel);
        if (value is null)
        {
            return Fail(status, text);
        }
        Console.WriteLine(options.Json ? text : JsonSerializer.Serialize(value, AmbJson.Default.RequestRecord));
        return 0;
    }

    /// <summary>Lists the newest builds.</summary>
    /// <param name="client">The client.</param>
    /// <param name="options">The command line.</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>0, or 1 when the coordinator refused.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> BuildsAsync(SiteClient client, Options options, CancellationToken cancel)
    {
        (HttpStatusCode status, List<BuildRecord>? value, string text) =
            await client.GetAsync("builds", AmbJsonLine.Default.ListBuildRecord, cancel);
        if (value is null)
        {
            return Fail(status, text);
        }
        if (options.Json)
        {
            Console.WriteLine(text);
            return 0;
        }
        foreach (BuildRecord build in value)
        {
            ArtifactRef? zip = build.Artifacts.FirstOrDefault();
            Console.WriteLine(FormattableString.Invariant(
                $"{build.Id,-14} {build.Source,-8} {build.Version ?? "?",-12} {build.CreatedAt:yyyy-MM-dd HH:mm}Z  {zip?.Name} ({(zip?.Bytes ?? 0) / 1048576.0:0} MB){(build.Commit is null ? "" : "  " + build.Commit)}{(build.SnapshotOf is null ? "" : "  (snapshot of " + build.SnapshotOf[..9] + ")")}"));
        }
        return 0;
    }

    /// <summary>Cancels a request.</summary>
    /// <param name="client">The client.</param>
    /// <param name="options">The command line (the id).</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>0 when it was cancelled, 1 when it had ended or does not exist, 2 without an id.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> CancelAsync(SiteClient client, Options options, CancellationToken cancel)
    {
        if (options.Positional.Count == 0)
        {
            Console.Error.WriteLine("amb cancel <request id>");
            return 2;
        }
        (HttpStatusCode status, CreateRequest? _, string text) = await client.PostAsync<CreateRequest, CreateRequest>(
            $"requests/{Uri.EscapeDataString(options.Positional[0])}/cancel", null, AmbJsonLine.Default.CreateRequest,
            AmbJsonLine.Default.CreateRequest, cancel);
        if (status == HttpStatusCode.Accepted)
        {
            Console.WriteLine($"cancel sent to {options.Positional[0]}");
            return 0;
        }
        return Fail(status, text);
    }

    /// <summary>Submits an import, smoke, e2e or build request, and follows it with --wait.</summary>
    /// <param name="client">The client.</param>
    /// <param name="options">The command line.</param>
    /// <param name="cancel">Cancels the watch.</param>
    /// <returns>0 succeeded (or accepted without --wait), 1 ended otherwise, 2 usage (or a rev that names no commit),
    /// 97 still running at --wait-max.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> SubmitAsync(SiteClient client, Options options, CancellationToken cancel)
    {
        CreateRequest body;
        Func<RequestEvent, string?>? stopAt = null;
        switch (options.Command)
        {
            case "build" when options.Flags.Contains("--snapshot") && options.Positional.Count == 0:
                (int code, RequestInputs? snapshot) = await SnapshotAsync(client, options, cancel);
                if (snapshot is null)
                {
                    return code;
                }
                body = new CreateRequest(RequestKind.Build, snapshot);
                break;
            case "build" when !options.Flags.Contains("--snapshot") && options.Positional.Count <= 1:
                string rev = options.Positional.FirstOrDefault() ?? "HEAD";
                string repo = Repo();
                string? commit = ResolveCommit(repo, rev);
                if (commit is null)
                {
                    Console.Error.WriteLine($"amb: {rev} names no commit in {repo}");
                    return 2;
                }
                Console.WriteLine($"{rev} = {commit}");
                body = new CreateRequest(RequestKind.Build, BuildInputs(options, commit));
                break;
            case "install" or "deploy" when options.Positional.Count == 1 && options.Values.ContainsKey("--instance"):
                if (!options.TryMinutes("--wait-others", out int? waitOthers, max: 240, min: 0))
                {
                    Console.Error.WriteLine("amb: --wait-others takes whole minutes, 0 to 240");
                    return 2;
                }
                // The claude.exe this CLI runs under (a Claude session's tool call): when it is one of the instance's
                // sessions, the install waits for its turn to end, because the swap ends its claude.
                int? requestedBy = ProcessAncestry.NearestAncestor("claude.exe");
                body = new CreateRequest(RequestKind.Deploy, new RequestInputs
                {
                    BuildId = options.Positional[0],
                    Instance = options.Values["--instance"],
                    RequestedByPid = requestedBy,
                    WaitOthersMinutes = waitOthers,
                    DryRun = options.Flags.Contains("--dry-run") ? true : null,
                    InjectFailure = options.Values.GetValueOrDefault("--inject-failure"),
                });
                if (requestedBy is { } claudePid)
                {
                    Console.WriteLine($"requested from the Claude session of claude.exe pid {claudePid}");
                    // A --wait from inside the instance would hold this very turn open while the install waits for it to
                    // end: the watch stops where the install starts that wait.
                    stopAt = e => e.Message.Contains($"claude pid {claudePid}) to end its turn", StringComparison.Ordinal)
                        ? "the install now waits for THIS session's turn to end (the swap ends its claude): end the turn; watch it from elsewhere with `amb logs <request id> -f`"
                        : null;
                }
                break;
            case "import" when options.Positional.Count == 1:
                body = new CreateRequest(RequestKind.Import, new RequestInputs
                {
                    Source = Path.GetFullPath(options.Positional[0]),
                    Commit = options.Values.GetValueOrDefault("--commit"),
                });
                break;
            case "smoke":
                body = new CreateRequest(RequestKind.Smoke, new RequestInputs
                {
                    Image = options.Values.GetValueOrDefault("--image", "am-base"),
                });
                break;
            case "e2e" when options.Positional.Count == 1:
                body = new CreateRequest(RequestKind.E2e, new RequestInputs
                {
                    BuildId = options.Positional[0],
                    Image = options.Values.GetValueOrDefault("--image", "am-e2e"),
                    SuiteSource = options.Values.TryGetValue("--suite", out string? suite) ? Path.GetFullPath(suite) : null,
                    SuiteArgs = options.Rest.Count > 0 ? options.Rest : null,
                });
                break;
            default:
                Console.Error.WriteLine("amb import <zip> | amb smoke [--image I] | amb e2e <build id> [-- pytest args] | amb build [<rev>] | amb build --snapshot [-- <paths>] | amb install <build id> --instance dev|production");
                return 2;
        }
        // The kind's defaults (DESIGN.md §2) fit a suite or two; a long run (the whole e2e suite takes hours) or a
        // request that may wait for a busy PC sets its own.
        if (!options.TryMinutes("--run-timeout", out int? runMinutes) || !options.TryMinutes("--wait-timeout", out int? waitMinutes))
        {
            Console.Error.WriteLine("amb: --run-timeout and --wait-timeout take whole minutes, 1 to 1440");
            return 2;
        }
        body = body with { RunTimeoutMinutes = runMinutes, WaitTimeoutMinutes = waitMinutes };
        (HttpStatusCode status, RequestRecord? created, string text) = await client.PostAsync("requests", body,
            AmbJsonLine.Default.CreateRequest, AmbJsonLine.Default.RequestRecord, cancel);
        if (created is null)
        {
            return Fail(status, text);
        }
        Console.WriteLine($"{created.Id} accepted ({created.Kind})");
        return options.Flags.Contains("--wait") || options.WaitMax is not null
            ? await FollowAsync(client, created.Id, options, cancel, stopAt)
            : 0;
    }

    /// <summary>The repository `amb build` works in: AMB_REPO, else <see cref="DefaultRepo"/>.</summary>
    /// <returns>Its folder.</returns>
    private static string Repo() => Environment.GetEnvironmentVariable("AMB_REPO") is { Length: > 0 } r ? r : DefaultRepo;

    /// <summary>A build request's inputs from the command line.</summary>
    /// <param name="options">The command line (--config, --identity, --purge).</param>
    /// <param name="commit">The commit to build.</param>
    /// <param name="snapshotOf">The commit a snapshot was made on, or null.</param>
    /// <returns>The inputs.</returns>
    private static RequestInputs BuildInputs(Options options, string commit, string? snapshotOf = null) => new()
    {
        Commit = commit,
        Configuration = options.Values.GetValueOrDefault("--config", "Release"),
        Identity = options.Values.GetValueOrDefault("--identity", "dev"),
        Purge = options.Values.GetValueOrDefault("--purge", "none"),
        SnapshotOf = snapshotOf,
    };

    /// <summary>Makes the snapshot `amb build --snapshot` builds (DESIGN.md §11): HEAD plus the uncommitted work of the
    /// whole working tree, or of the paths after "--".</summary>
    /// <param name="client">The client (to learn which snapshots were built, whose refs then go).</param>
    /// <param name="options">The command line.</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>The build's inputs, or null with the exit code (1 a refusal, 2 a repository git cannot read).</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    private static async Task<(int Code, RequestInputs? Inputs)> SnapshotAsync(SiteClient client, Options options,
        CancellationToken cancel)
    {
        string repo = Repo();
        IReadOnlyList<string> paths = options.Rest;
        try
        {
            // Tidy first: the refs of snapshots already built go (the site's mirror keeps them); this one's ref stays
            // until its build exists.
            var (_, builds, _) = await client.GetAsync("builds", AmbJsonLine.Default.ListBuildRecord, cancel);
            if (builds is not null)
            {
                int pruned = WorkingTreeSnapshot.PruneBuilt(repo, builds.Where(b => b.Commit is not null).Select(b => b.Commit!).ToHashSet());
                if (pruned > 0)
                {
                    Console.WriteLine($"deleted {pruned} built snapshot ref(s) in {repo}");
                }
            }
            IReadOnlyList<WorkingTreeSnapshot.Change> changes = WorkingTreeSnapshot.Changes(repo, paths);
            WorkingTreeSnapshot.CheckSize(changes);
            string scope = paths.Count == 0 ? "the working tree" : string.Join(' ', paths);
            Console.WriteLine(FormattableString.Invariant(
                $"{scope}: {changes.Count} uncommitted change(s), {changes.Sum(c => c.Bytes) / 1048576.0:0.0} MB"));
            foreach (WorkingTreeSnapshot.Change c in changes.Take(20))
            {
                Console.WriteLine($"  {c.Status} {c.Path}");
            }
            if (changes.Count > 20)
            {
                Console.WriteLine($"  ... and {changes.Count - 20} more");
            }
            WorkingTreeSnapshot.Result snapshot = WorkingTreeSnapshot.Create(repo, paths, @"Q:\buildserver\tmp");
            if (!snapshot.Changed)
            {
                Console.WriteLine($"nothing differs from HEAD: building HEAD = {snapshot.Head}");
                return (0, BuildInputs(options, snapshot.Head));
            }
            Console.WriteLine($"snapshot {snapshot.Commit} of HEAD {snapshot.Head[..9]} ({WorkingTreeSnapshot.RefPrefix}{snapshot.Commit[..9]}...)");
            return (0, BuildInputs(options, snapshot.Commit, snapshot.Head));
        }
        catch (SnapshotRefusedException ex)
        {
            Console.Error.WriteLine("amb: " + ex.Message);
            return (1, null);
        }
        catch (InvalidOperationException ex)
        {
            Console.Error.WriteLine($"amb: no snapshot of {repo}: {ex.Message}");
            return (2, null);
        }
    }

    /// <summary>Prints a request's events as they come, until it ends or --wait-max runs out.</summary>
    /// <param name="client">The client.</param>
    /// <param name="id">The request's id.</param>
    /// <param name="options">The command line (-f, --wait-max).</param>
    /// <param name="cancel">Cancels the watch.</param>
    /// <param name="stopAt">Ends the watch early, returning 0, at the first event it gives a note for (printed): an install
    /// that starts waiting for the very session that watches it. Null: watch to the end.</param>
    /// <returns>0 succeeded (or stopped by <paramref name="stopAt"/>), 1 ended otherwise, 2 without an id, 97 still running.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> FollowAsync(SiteClient client, string? id, Options options, CancellationToken cancel,
        Func<RequestEvent, string?>? stopAt = null)
    {
        if (id is null)
        {
            Console.Error.WriteLine("amb logs <request id> [-f]");
            return 2;
        }
        bool follow = options.Command != "logs" || options.Flags.Contains("-f") || options.WaitMax is not null;
        DateTimeOffset? deadline = options.WaitMax is { } max ? DateTimeOffset.UtcNow + max : null;
        int skip = 0;
        while (true)
        {
            int wait = follow ? 25 : 0;
            (HttpStatusCode status, EventPage? page, string text) = await client.GetAsync(
                $"requests/{Uri.EscapeDataString(id)}/events?skip={skip}&wait={wait}", AmbJsonLine.Default.EventPage, cancel);
            if (page is null)
            {
                return Fail(status, text);
            }
            foreach (RequestEvent e in page.Events)
            {
                Console.WriteLine($"{e.At:HH:mm:ss}  {e.Kind,-9} {e.Message}");
                if (stopAt?.Invoke(e) is { } note)
                {
                    Console.WriteLine($"{id}: {note}");
                    return 0;
                }
            }
            skip = page.Next;
            string state = page.Request.State.ToString();
            if (Final.Contains(state))
            {
                Console.WriteLine($"{state}: {page.Request.Outcome}");
                return state == "Succeeded" ? 0 : 1;
            }
            if (!follow)
            {
                return 0;
            }
            if (deadline is { } d && DateTimeOffset.UtcNow > d)
            {
                Console.WriteLine($"still {state}; attach again: amb logs {id} -f");
                return 97;
            }
        }
    }

    /// <summary>The repository `amb build` resolves revisions in (the one the site's mirror fetches).</summary>
    private const string DefaultRepo = @"K:\source\Agentmaster";

    /// <summary>The kinds `amb fetch` copies on request ("all" copies every one).</summary>
    private static readonly string[] ArtifactKinds = ["portable-zip", "layout-zip", "symbols-zip", "build-log", "build-binlog", "build-json"];

    /// <summary>Resolves a revision to the full SHA of the commit it names, with git on this PC.</summary>
    /// <param name="repo">The repository.</param>
    /// <param name="rev">The revision (HEAD, a branch, a tag, a short or full SHA).</param>
    /// <returns>The full SHA, or null when git does not know it (or git cannot be run).</returns>
    internal static string? ResolveCommit(string repo, string rev)
    {
        var info = new System.Diagnostics.ProcessStartInfo("git")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (string arg in (string[])["-C", repo, "rev-parse", "--verify", "--quiet", rev + "^{commit}"])
        {
            info.ArgumentList.Add(arg);
        }
        try
        {
            using System.Diagnostics.Process git = System.Diagnostics.Process.Start(info)!;
            string output = git.StandardOutput.ReadToEnd().Trim();
            git.WaitForExit();
            return git.ExitCode == 0 && output.Length == 40 ? output.ToLowerInvariant() : null;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            // git is not on PATH: the caller says the rev names no commit, which is what it can tell.
            return null;
        }
    }

    /// <summary>Whether a folder is on K: (the owner's rule: every build writes to Q:, never to K:).</summary>
    /// <param name="path">The folder.</param>
    /// <returns>True for any path whose root is K:.</returns>
    internal static bool IsOnK(string path) =>
        Path.GetPathRoot(Path.GetFullPath(path)) is { } root && root.StartsWith("K:", StringComparison.OrdinalIgnoreCase);

    /// <summary>Lists a build's files.</summary>
    /// <param name="client">The client.</param>
    /// <param name="options">The command line (the build id).</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>0, 1 when there is no such build, 2 without a build id.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> ArtifactsAsync(SiteClient client, Options options, CancellationToken cancel)
    {
        if (options.Positional.Count != 1)
        {
            Console.Error.WriteLine("amb artifacts <build id>");
            return 2;
        }
        (HttpStatusCode status, BuildRecord? build, string text) = await client.GetAsync(
            $"builds/{Uri.EscapeDataString(options.Positional[0])}", AmbJsonLine.Default.BuildRecord, cancel);
        if (build is null)
        {
            return Fail(status, text);
        }
        if (options.Json)
        {
            Console.WriteLine(text);
            return 0;
        }
        Console.WriteLine(FormattableString.Invariant(
            $"{build.Id}  {build.Source}  {build.Version ?? "?"}  {build.Configuration ?? ""} {build.Identity ?? ""}  {build.Commit ?? ""}{(build.SnapshotOf is null ? "" : "  (a snapshot of uncommitted work on " + build.SnapshotOf + ")")}"));
        foreach (ArtifactRef a in build.Artifacts)
        {
            Console.WriteLine(FormattableString.Invariant($"  {a.Kind,-13} {a.Bytes / 1048576.0,8:0.0} MB  {a.Sha256[..12]}  {a.Name}"));
        }
        return 0;
    }

    /// <summary>Copies a build's files from the site's store to a folder on Q:, checking each one's SHA-256.</summary>
    /// <param name="client">The client.</param>
    /// <param name="options">The command line (the build id, --kind, --to).</param>
    /// <param name="cancel">Cancels the copies.</param>
    /// <returns>0 copied; 1 no such build, no file of that kind, or a SHA-256 mismatch; 2 usage or a folder on K:.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> FetchAsync(SiteClient client, Options options, CancellationToken cancel)
    {
        string kind = options.Values.GetValueOrDefault("--kind", "portable-zip");
        if (options.Positional.Count != 1 || (kind != "all" && !ArtifactKinds.Contains(kind)))
        {
            Console.Error.WriteLine("amb fetch <build id> [--kind portable-zip|layout-zip|symbols-zip|build-log|build-binlog|build-json|all] [--to DIR]");
            return 2;
        }
        string id = options.Positional[0];
        string folder = Path.GetFullPath(options.Values.GetValueOrDefault("--to", Path.Combine(@"Q:\buildserver\out", id)));
        if (IsOnK(folder))
        {
            Console.Error.WriteLine($"amb: {folder} is on K:; builds and their files go to Q: (the owner's rule)");
            return 2;
        }
        (HttpStatusCode status, BuildRecord? build, string text) = await client.GetAsync(
            $"builds/{Uri.EscapeDataString(id)}", AmbJsonLine.Default.BuildRecord, cancel);
        if (build is null)
        {
            return Fail(status, text);
        }
        ArtifactRef[] wanted = [.. build.Artifacts.Where(a => kind == "all" || a.Kind == kind)];
        if (wanted.Length == 0)
        {
            Console.Error.WriteLine($"amb: build {id} has no {kind}");
            return 1;
        }
        Directory.CreateDirectory(folder);
        foreach (ArtifactRef a in wanted)
        {
            string target = Path.Combine(folder, a.Name);
            string? error = await client.DownloadAsync(
                $"builds/{Uri.EscapeDataString(id)}/artifacts/{Uri.EscapeDataString(a.Name)}", target, a.Sha256, cancel);
            if (error is not null)
            {
                Console.Error.WriteLine($"amb: {a.Name}: {error}");
                return 1;
            }
            Console.WriteLine(FormattableString.Invariant($"{target}  ({a.Bytes / 1048576.0:0.0} MB, sha256 {a.Sha256[..12]} checked)"));
        }
        return 0;
    }

    /// <summary>One request as one line.</summary>
    /// <param name="request">The request.</param>
    /// <returns>"id state kind outcome".</returns>
    private static string Line(RequestRecord request) =>
        $"{request.Id}  {request.State,-10} {request.Kind,-7} {request.BuildId ?? request.Inputs.Image ?? request.Inputs.NewImage ?? ""}  {request.Outcome}";

    /// <summary>Prints a refusal.</summary>
    /// <param name="status">The HTTP status.</param>
    /// <param name="text">The answer's text.</param>
    /// <returns>1.</returns>
    private static int Fail(HttpStatusCode status, string text)
    {
        Console.Error.WriteLine($"amb: {(int)status} {text}");
        return 1;
    }
}
