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
//   --json on any command prints the coordinator's JSON instead of the text form; AMB_URL overrides the address.
//
// Exit codes: 0 succeeded (or accepted, without --wait); 1 the request ended otherwise; 2 usage; 97 still running when
// --wait-max ran out (attach again with `amb logs <id> -f`); 98 the coordinator could not be reached or woken.

string url = Environment.GetEnvironmentVariable("AMB_URL") is { Length: > 0 } u ? u : "http://127.0.0.1:2207";
var options = Options.Parse(args);
if (options.Command is null)
{
    Console.Error.WriteLine("usage: amb status|list|show|logs|cancel|builds|import|smoke|e2e ... (see the header of Program.cs)");
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
        "import" or "smoke" or "e2e" => await Commands.SubmitAsync(client, options, stop.Token),
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
    private static readonly HashSet<string> WithValue = ["--image", "--suite", "--commit", "--limit", "--wait-max"];

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
                $"{build.Id,-14} {build.Source,-8} {build.Version ?? "?",-12} {build.CreatedAt:yyyy-MM-dd HH:mm}Z  {zip?.Name} ({(zip?.Bytes ?? 0) / 1048576.0:0} MB){(build.Commit is null ? "" : "  " + build.Commit)}"));
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

    /// <summary>Submits an import, smoke or e2e request, and follows it with --wait.</summary>
    /// <param name="client">The client.</param>
    /// <param name="options">The command line.</param>
    /// <param name="cancel">Cancels the watch.</param>
    /// <returns>0 succeeded (or accepted without --wait), 1 ended otherwise, 2 usage, 97 still running at --wait-max.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> SubmitAsync(SiteClient client, Options options, CancellationToken cancel)
    {
        CreateRequest body;
        switch (options.Command)
        {
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
                Console.Error.WriteLine("amb import <zip> | amb smoke [--image I] | amb e2e <build id> [-- pytest args]");
                return 2;
        }
        (HttpStatusCode status, RequestRecord? created, string text) = await client.PostAsync("requests", body,
            AmbJsonLine.Default.CreateRequest, AmbJsonLine.Default.RequestRecord, cancel);
        if (created is null)
        {
            return Fail(status, text);
        }
        Console.WriteLine($"{created.Id} accepted ({created.Kind})");
        return options.Flags.Contains("--wait") || options.WaitMax is not null
            ? await FollowAsync(client, created.Id, options, cancel)
            : 0;
    }

    /// <summary>Prints a request's events as they come, until it ends or --wait-max runs out.</summary>
    /// <param name="client">The client.</param>
    /// <param name="id">The request's id.</param>
    /// <param name="options">The command line (-f, --wait-max).</param>
    /// <param name="cancel">Cancels the watch.</param>
    /// <returns>0 succeeded, 1 ended otherwise, 2 without an id, 97 still running.</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    public static async Task<int> FollowAsync(SiteClient client, string? id, Options options, CancellationToken cancel)
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
