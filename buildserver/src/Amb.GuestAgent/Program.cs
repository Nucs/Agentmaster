using System.Text.Json;
using Amb.Core.Json;
using Amb.Core.Protocol;
using Amb.GuestAgent;

// amb-guest: the build server's guest agent (buildserver/DESIGN.md §10).
//
//   amb-guest [--config \\host.lan\Data\amb\slot.json] [--work C:\amb]
//
// It is started at logon by the image's bootstrap (image/oem/bootstrap.ps1), which copies this file from the slot's
// share first. It reads the slot's config by its exact path (never by listing the share: the guest's SMB client serves
// listings from a lease cache), waiting while the share is not up yet, then runs the slot until a shutdown step.
// Exit codes: 0 after a shutdown step or Ctrl+C; 2 when the config never appeared or is invalid.

string configPath = @"\\host.lan\Data\amb\slot.json";
string workDir = @"C:\amb";
for (int i = 0; i < args.Length - 1; i++)
{
    if (args[i] == "--config")
    {
        configPath = args[++i];
    }
    else if (args[i] == "--work")
    {
        workDir = args[++i];
    }
}

var log = new AgentLog(Path.Combine(workDir, "agent.log"));
log.Write($"amb-guest {GuestAgent.Version} starting; config {configPath}");
using var stop = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    // Ctrl+C when run by hand: end cleanly (the running step's tree is killed by its own cancellation).
    e.Cancel = true;
    stop.Cancel();
};

SlotConfig? config = null;
var deadline = DateTime.UtcNow.AddMinutes(10);
while (config is null)
{
    try
    {
        config = JsonSerializer.Deserialize(File.ReadAllBytes(configPath), AmbJsonLine.Default.SlotConfig);
    }
    catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
    {
        // Expected right after logon: the share answers some seconds after the desktop (24-32 s from boot, measured).
        if (DateTime.UtcNow > deadline)
        {
            log.Write($"no slot config at {configPath} after 10 minutes: {ex.Message}");
            return 2;
        }
        Thread.Sleep(2000);
    }
    catch (JsonException ex)
    {
        log.Write($"invalid slot config {configPath}: {ex.Message}");
        return 2;
    }
}
if (string.IsNullOrEmpty(config.SlotId) || string.IsNullOrEmpty(config.Token) || string.IsNullOrEmpty(config.Coordinator))
{
    log.Write($"incomplete slot config {configPath}");
    return 2;
}

using var agent = new GuestAgent(config, workDir, log);
try
{
    await agent.RunAsync(stop.Token);
}
catch (OperationCanceledException)
{
    log.Write("stopped");
}
return 0;
