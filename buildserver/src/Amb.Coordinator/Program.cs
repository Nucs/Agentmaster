using System.Reflection;
using Amb.Coordinator;
using Amb.Coordinator.Slots;
using Amb.Core.Json;
using Amb.Core.Model;
using Amb.Core.Protocol;
using Amb.Core.Records;

// amb-coordinator: the build server's one writer (buildserver/DESIGN.md §6, §10, §12).
//
// Settings come from environment variables only (CoordinatorOptions). Two API groups:
//   /api/v1/...    the CLI's: requests, events, builds, status; every call carries the admin token
//   /guest/v1/...  the guest agents': hello, next, result, heartbeat, log; every call carries its slot's token
// plus /healthz, open. Exits by itself after IdleExitMinutes with nothing to do (the site is lazy).

CoordinatorOptions options = CoordinatorOptions.FromEnvironment(Environment.GetEnvironmentVariable);
string adminToken = AdminAccess.LoadOrCreate(options.AdminTokenFile);
string version = typeof(Scheduler).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
    ?? "0.0.0";
DateTimeOffset startedAt = DateTimeOffset.UtcNow;

WebApplicationBuilder builder = WebApplication.CreateSlimBuilder(args);
builder.WebHost.UseUrls(options.Listen);
builder.Logging.ClearProviders();
builder.Logging.AddSimpleConsole(o =>
{
    o.SingleLine = true;
    o.TimestampFormat = "yyyy-MM-ddTHH:mm:ss.fffZ ";
    o.UseUtcTimestamp = true;
});
// ASP.NET Core logs four lines per request at Information; guests long-poll every 25 s and heartbeat every 10 s, so the
// log would be nothing but those. Its warnings and errors still come through.
builder.Logging.AddFilter("Microsoft.AspNetCore", LogLevel.Warning);
builder.Services.ConfigureHttpJsonOptions(o => o.SerializerOptions.TypeInfoResolverChain.Insert(0, AmbJsonLine.Default));
builder.Services.AddSingleton(options);
builder.Services.AddSingleton(TimeProvider.System);
builder.Services.AddSingleton(_ => new RecordStore(options.RecordsDir));
builder.Services.AddSingleton<RequestBook>();
builder.Services.AddSingleton<GuestBroker>();
if (options.SlotDriver == "local")
{
    builder.Services.AddSingleton<ISlotDriver, LocalSlotDriver>();
}
else
{
    builder.Services.AddSingleton<ISlotDriver, PodmanSlotDriver>();
}
builder.Services.AddSingleton<SlotManager>();
builder.Services.AddSingleton<HostCapacity>();
builder.Services.AddHostedService(sp => sp.GetRequiredService<HostCapacity>());
builder.Services.AddSingleton<Scheduler>();
builder.Services.AddHostedService(sp => sp.GetRequiredService<Scheduler>());
builder.Services.AddSingleton<IdleWatch>();
builder.Services.AddHostedService(sp => sp.GetRequiredService<IdleWatch>());

WebApplication app = builder.Build();
app.Logger.LogInformation("amb-coordinator {Version} on {Listen}; root {Root}; slot driver {Driver}; guests reach it at {Guest}",
    version, options.Listen, options.Root, options.SlotDriver, options.GuestUrl);

app.MapGet("/healthz", () => Results.Text("ok"));

// Every API call counts as activity for the idle exit, including a CLI that only watches a request.
app.Use(async (http, next) =>
{
    if (http.Request.Path.StartsWithSegments("/api", StringComparison.Ordinal))
    {
        http.RequestServices.GetRequiredService<IdleWatch>().Touch();
    }
    await next(http).ConfigureAwait(false);
});

RouteGroupBuilder api = app.MapGroup("/api/v1").AddEndpointFilter(new AdminAccess(adminToken));
api.MapPost("/requests", (CreateRequest body, RequestBook book) =>
{
    try
    {
        RequestRecord created = book.Create(body);
        return Results.Accepted($"/api/v1/requests/{created.Id}", created);
    }
    catch (RequestRejectedException ex)
    {
        return Results.Text(ex.Message, statusCode: 400);
    }
});
api.MapGet("/requests", (int? limit, RequestBook book) => Results.Ok(book.Newest(limit ?? 20).ToList()));
api.MapGet("/requests/{id}", (string id, RequestBook book) =>
    book.Get(id) is { } record ? Results.Ok(record) : Results.Text("no such request", statusCode: 404));
api.MapGet("/requests/{id}/events", async (string id, int? skip, int? wait, RequestBook book, CancellationToken cancel) =>
{
    RequestRecord? record = book.Get(id);
    if (record is null)
    {
        return Results.Text("no such request", statusCode: 404);
    }
    int seen = Math.Max(0, skip ?? 0);
    IReadOnlyList<RequestEvent> events = book.Events(id, seen);
    if (events.Count == 0 && !RequestStates.IsFinal(record.State) && wait is > 0)
    {
        // A long poll: the CLI's --wait asks again at once after each answer, so nothing polls on a timer.
        await book.WaitForChangeAsync(id, TimeSpan.FromSeconds(Math.Min(wait.Value, 30)), cancel).ConfigureAwait(false);
        events = book.Events(id, seen);
        record = book.Get(id) ?? record;
    }
    return Results.Ok(new EventPage(seen + events.Count, events, record));
});
api.MapPost("/requests/{id}/cancel", (string id, Scheduler scheduler) =>
    scheduler.Cancel(id) ? Results.Accepted() : Results.Text("no such request, or it has ended", statusCode: 404));
api.MapGet("/builds", (RecordStore store) => Results.Ok(store.List("builds").Reverse().Take(100)
    .Select(id => store.Read("builds", id, AmbJson.Default.EnvelopeBuildRecord)?.Value).OfType<BuildRecord>().ToList()));
api.MapGet("/builds/{id}", (string id, RecordStore store) =>
    Ids.IsSafe(id) && store.Read("builds", id, AmbJson.Default.EnvelopeBuildRecord) is { } build
        ? Results.Ok(build.Value)
        : Results.Text("no such build", statusCode: 404));
// A build's file, streamed from the store (`amb fetch` copies it to Q: and checks its SHA-256 against the manifest). Only
// a name the build's manifest lists is served, so no path a caller makes up can reach outside the store.
api.MapGet("/builds/{id}/artifacts/{name}", (string id, string name, RecordStore store, CoordinatorOptions options) =>
{
    if (!Ids.IsSafe(id) || store.Read("builds", id, AmbJson.Default.EnvelopeBuildRecord) is not { } build)
    {
        return Results.Text("no such build", statusCode: 404);
    }
    ArtifactRef? artifact = build.Value.Artifacts.FirstOrDefault(a => a.Name == name);
    string? path = artifact is null ? null : Path.GetFullPath(Path.Combine(options.StoreDir, artifact.StorePath));
    if (path is null || !path.StartsWith(Path.GetFullPath(options.StoreDir) + Path.DirectorySeparatorChar, StringComparison.Ordinal)
        || !File.Exists(path))
    {
        return Results.Text($"build {id} has no file {name}", statusCode: 404);
    }
    return Results.File(path, "application/octet-stream", artifact!.Name, enableRangeProcessing: true);
});
api.MapGet("/status", (RequestBook book, SlotManager slots, HostCapacity capacity) =>
    Results.Ok(new CoordinatorStatus(version, startedAt, [capacity.HomepcFree()], book.Active, slots.Active, capacity.Last)));
api.MapPost("/capacity/homepc", (HostFactsReport report, HostCapacity capacity) =>
{
    // For a site without the helper (or tests): the caller reports Windows' numbers itself.
    capacity.Override(report);
    return Results.Accepted();
});

RouteGroupBuilder guest = app.MapGroup("/guest/v1");
guest.MapPost("/hello", (HttpContext http, GuestHello hello, GuestBroker broker) =>
{
    SlotSession? session = broker.Authenticate(hello.SlotId, http.Request.Headers.Authorization);
    if (session is null)
    {
        return Results.Text("unknown slot or wrong token", statusCode: 401);
    }
    session.Hello(hello);
    return Results.Accepted();
});
guest.MapGet("/next", async (HttpContext http, string slot, int? wait, GuestBroker broker, CancellationToken cancel) =>
{
    SlotSession? session = broker.Authenticate(slot, http.Request.Headers.Authorization);
    if (session is null)
    {
        return Results.Text("unknown slot or wrong token", statusCode: 401);
    }
    GuestStep? step = await session.NextAsync(TimeSpan.FromSeconds(Math.Clamp(wait ?? 25, 0, 30)), cancel)
        .ConfigureAwait(false);
    return step is null ? Results.NoContent() : Results.Ok(step);
});
guest.MapPost("/result", (HttpContext http, StepResult result, GuestBroker broker) =>
{
    SlotSession? session = broker.Authenticate(result.SlotId, http.Request.Headers.Authorization);
    if (session is null)
    {
        return Results.Text("unknown slot or wrong token", statusCode: 401);
    }
    // A duplicate (the agent posting again after a lost answer) is accepted too: the agent must stop retrying.
    session.Complete(result);
    return Results.Accepted();
});
guest.MapPost("/heartbeat", (HttpContext http, GuestHeartbeat heartbeat, GuestBroker broker) =>
{
    SlotSession? session = broker.Authenticate(heartbeat.SlotId, http.Request.Headers.Authorization);
    return session is null
        ? Results.Text("unknown slot or wrong token", statusCode: 401)
        : Results.Ok(session.Heartbeat(heartbeat));
});
guest.MapPost("/log", (HttpContext http, StepLog log, GuestBroker broker) =>
{
    SlotSession? session = broker.Authenticate(log.SlotId, http.Request.Headers.Authorization);
    if (session is null)
    {
        return Results.Text("unknown slot or wrong token", statusCode: 401);
    }
    session.Log(log);
    return Results.Accepted();
});

app.Run();
