using System.Globalization;
using System.Text;

namespace Amb.GuestAgent;

/// <summary>
/// The agent's own log: one line per event, appended to a file in the guest (C:\amb\agent.log) and echoed to stderr.
/// </summary>
/// <remarks>
/// The log is local on purpose: when the network or the coordinator is the problem, the coordinator cannot be told,
/// and the file is what a person reads afterwards (through the VM's console or by copying it out). Writes never throw:
/// a log that fails must not stop the agent.
/// </remarks>
internal sealed class AgentLog
{
    private readonly string _path;
    private readonly object _gate = new();

    /// <summary>Opens a log file (its folder is created when missing).</summary>
    /// <param name="path">The file.</param>
    internal AgentLog(string path)
    {
        _path = path;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        }
        catch (IOException)
        {
            // Deliberately ignored: Write falls back to stderr only, which the bootstrap's own log captures.
        }
        catch (UnauthorizedAccessException)
        {
            // Deliberately ignored, as above.
        }
    }

    /// <summary>Writes one line, stamped with the UTC time.</summary>
    /// <param name="message">The line (line breaks are replaced by " | ").</param>
    internal void Write(string message)
    {
        string line = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ", CultureInfo.InvariantCulture) + " "
            + message.Replace("\r", "", StringComparison.Ordinal).Replace("\n", " | ", StringComparison.Ordinal);
        lock (_gate)
        {
            try
            {
                File.AppendAllText(_path, line + Environment.NewLine, Encoding.UTF8);
            }
            catch (IOException)
            {
                // Deliberately ignored: the line still goes to stderr below.
            }
            catch (UnauthorizedAccessException)
            {
                // Deliberately ignored, as above.
            }
            Console.Error.WriteLine(line);
        }
    }
}
