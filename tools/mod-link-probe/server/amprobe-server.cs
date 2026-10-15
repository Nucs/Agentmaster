// amprobe-server — a throwaway HTTP/1.1 responder used to measure which local transports a Claude
// Code mod's $.http.fetch can reach on Windows: plain TCP on 127.0.0.1, an AF_UNIX socket
// (socketPath), and a named pipe (socketPath = \\.\pipe\...). Every request is logged with the
// server's elapsed time, so the order and spacing of a mod's calls can be read back afterwards.
// It exits by itself at the deadline, so nothing ever has to kill it.
//
// Usage: dotnet run amprobe-server.cs -- <outDir> <socketDir> <deadlineSeconds>
//   outDir     receives server.log (one line per request) and ready.txt (port, socket, pipe)
//   socketDir  a SHORT folder for amp.sock: Windows caps an AF_UNIX path near 108 bytes
using System.Diagnostics;
using System.IO.Pipes;
using System.Net;
using System.Net.Sockets;
using System.Text;

var outDir = args[0];
var sockDir = args[1];
var deadlineSec = int.Parse(args[2]);

Directory.CreateDirectory(outDir);
Directory.CreateDirectory(sockDir);
var sockPath = Path.Combine(sockDir, "amp.sock");
var pipeName = $"amprobe-{Environment.ProcessId}";
var clock = Stopwatch.StartNew();
var logGate = new object();
var logPath = Path.Combine(outDir, "server.log");
File.WriteAllText(logPath, "");

// The deadline cancels every accept loop and every pending /poll, which is what ends the process.
using var stop = new CancellationTokenSource(TimeSpan.FromSeconds(deadlineSec));

var tcp = new TcpListener(IPAddress.Loopback, 0);
tcp.Start();
var port = ((IPEndPoint)tcp.LocalEndpoint).Port;

// A stale socket file from an earlier run would make Bind fail with "address in use".
if (File.Exists(sockPath)) File.Delete(sockPath);
var unix = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
unix.Bind(new UnixDomainSocketEndPoint(sockPath));
unix.Listen(64);

File.WriteAllText(Path.Combine(outDir, "ready.txt"), $"{port}\n{sockPath}\n\\\\.\\pipe\\{pipeName}\n");
Log($"READY tcp=127.0.0.1:{port} unix={sockPath} pipe=\\\\.\\pipe\\{pipeName}");

var loops = new List<Task>
{
    Task.Run(async () =>
    {
        while (!stop.IsCancellationRequested)
        {
            var client = await tcp.AcceptTcpClientAsync(stop.Token);
            _ = Serve(client.GetStream(), "tcp");
        }
    }),
    Task.Run(async () =>
    {
        while (!stop.IsCancellationRequested)
        {
            var client = await unix.AcceptAsync(stop.Token);
            _ = Serve(new NetworkStream(client, ownsSocket: true), "unix");
        }
    }),
    Task.Run(async () =>
    {
        while (!stop.IsCancellationRequested)
        {
            // One server instance per connection: a byte-mode pipe answers exactly one client.
            var pipe = new NamedPipeServerStream(pipeName, PipeDirection.InOut,
                NamedPipeServerStream.MaxAllowedServerInstances, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
            await pipe.WaitForConnectionAsync(stop.Token);
            _ = Serve(pipe, "pipe");
        }
    }),
};

try
{
    await Task.WhenAll(loops);
}
catch (OperationCanceledException)
{
    // The deadline: the expected way out.
}
catch (Exception ex)
{
    Log($"loop ended: {ex.GetType().Name} {ex.Message}");
}

Log("DEADLINE: exiting");
tcp.Stop();
unix.Dispose();
try { File.Delete(sockPath); } catch (IOException) { /* best effort: the folder is removed by hand afterwards */ }

/// <summary>Appends one line to server.log, stamped with the server's elapsed milliseconds.</summary>
/// <param name="line">The text to log; one line, no trailing newline.</param>
void Log(string line)
{
    // Requests arrive on several threads at once; the lock keeps whole lines whole.
    lock (logGate)
    {
        File.AppendAllText(logPath, $"{clock.ElapsedMilliseconds,7} ms  {line}{Environment.NewLine}");
    }
}

/// <summary>
/// Reads one HTTP/1.1 request from <paramref name="stream"/>, logs it, answers 200 with a small JSON
/// body and closes. <c>/poll?ms=N</c> answers only after N milliseconds, to measure how long a
/// mod's fetch may stay open (a long-poll).
/// </summary>
/// <param name="stream">The accepted connection; owned and disposed here.</param>
/// <param name="via">Which transport accepted it (<c>tcp</c>, <c>unix</c>, <c>pipe</c>), for the log.</param>
/// <returns>A task that ends when the response is written or the connection failed.</returns>
/// <remarks>Never throws: every failure is logged, since an unobserved task exception would be lost.</remarks>
async Task Serve(Stream stream, string via)
{
    try
    {
        await using var s = stream;
        var buf = new byte[1 << 20];
        var len = 0;
        var end = -1;
        var crlf2 = "\r\n\r\n"u8.ToArray();
        while (end < 0)
        {
            var n = await s.ReadAsync(buf.AsMemory(len), stop.Token);
            if (n == 0) { Log($"{via} closed before its headers ended"); return; }
            len += n;
            end = Find(buf, len, crlf2);
        }
        var head = Encoding.UTF8.GetString(buf, 0, end).Split("\r\n");
        var contentLength = 0;
        foreach (var h in head.Skip(1))
        {
            if (h.StartsWith("content-length:", StringComparison.OrdinalIgnoreCase))
                contentLength = int.Parse(h["content-length:".Length..].Trim());
        }
        var bodyStart = end + 4;
        while (len - bodyStart < contentLength)
        {
            var n = await s.ReadAsync(buf.AsMemory(len), stop.Token);
            if (n == 0) break;
            len += n;
        }
        var body = Encoding.UTF8.GetString(buf, bodyStart, Math.Min(contentLength, len - bodyStart));
        var target = head[0].Split(' ')[1];
        Log($"{via,-4} {head[0]}  headers=[{string.Join(" | ", head.Skip(1))}]{(body.Length > 0 ? "  body=" + body : "")}");
        if (target.StartsWith("/poll"))
        {
            var query = target.Contains('?') ? target[(target.IndexOf('?') + 1)..] : "";
            var ms = 10000;
            foreach (var kv in query.Split('&'))
            {
                if (kv.StartsWith("ms=")) ms = int.Parse(kv[3..]);
            }
            await Task.Delay(ms, stop.Token);
            Log($"{via} poll answering after {ms} ms");
        }
        var payload = Encoding.UTF8.GetBytes($"{{\"ok\":true,\"via\":\"{via}\",\"t\":{clock.ElapsedMilliseconds}}}");
        var header = Encoding.ASCII.GetBytes(
            $"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {payload.Length}\r\nConnection: close\r\n\r\n");
        await s.WriteAsync(header, stop.Token);
        await s.WriteAsync(payload, stop.Token);
        await s.FlushAsync(stop.Token);
    }
    catch (OperationCanceledException)
    {
        // The deadline cut a pending request: nothing to answer any more.
    }
    catch (Exception ex)
    {
        Log($"{via} error {ex.GetType().Name}: {ex.Message}");
    }
}

/// <summary>Finds the first occurrence of <paramref name="pattern"/> in the first <paramref name="len"/> bytes.</summary>
/// <param name="buf">The bytes read so far.</param>
/// <param name="len">How many bytes of <paramref name="buf"/> are filled.</param>
/// <param name="pattern">The byte sequence to find (the header terminator).</param>
/// <returns>The index of the first match, or -1 when the pattern is not there yet.</returns>
static int Find(byte[] buf, int len, byte[] pattern)
{
    for (var i = 0; i + pattern.Length <= len; i++)
    {
        var isHit = true;
        for (var j = 0; j < pattern.Length; j++)
        {
            if (buf[i + j] != pattern[j]) { isHit = false; break; }
        }
        if (isHit) return i;
    }
    return -1;
}
