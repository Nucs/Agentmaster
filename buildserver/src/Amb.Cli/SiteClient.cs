using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json.Serialization.Metadata;

namespace Amb.Cli;

/// <summary>The coordinator could not be reached or woken: the CLI exits 98.</summary>
/// <param name="message">What failed.</param>
internal sealed class SiteUnavailableException(string message) : Exception(message);

/// <summary>
/// The CLI's way to the coordinator: HTTP to 127.0.0.1:2207, the admin token read from the site (and cached in the
/// user's own folder), and the site woken when the coordinator does not answer (the site is lazy: it stops after 10 idle
/// minutes, DESIGN.md §5.2).
/// </summary>
internal sealed class SiteClient : IDisposable
{
    /// <summary>The site distro's name.</summary>
    private const string Distro = "buildserver";

    private readonly HttpClient _http;
    private readonly string _tokenCache;
    private string? _token;

    /// <summary>Creates a client.</summary>
    /// <param name="baseUrl">The coordinator's URL (AMB_URL, default http://127.0.0.1:2207).</param>
    internal SiteClient(string baseUrl)
    {
        _http = new HttpClient { BaseAddress = new Uri(baseUrl.TrimEnd('/') + "/"), Timeout = TimeSpan.FromSeconds(60) };
        _tokenCache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amb",
            "token");
    }

    /// <summary>Releases the HTTP client.</summary>
    public void Dispose() => _http.Dispose();

    /// <summary>Sends a GET and parses the answer.</summary>
    /// <typeparam name="T">The answer's type.</typeparam>
    /// <param name="path">The path under /api/v1/.</param>
    /// <param name="type">Its source-generated metadata.</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>The status code and the parsed answer (default when the answer is not 2xx).</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    internal async Task<(HttpStatusCode Status, T? Value, string Text)> GetAsync<T>(string path, JsonTypeInfo<T> type,
        CancellationToken cancel) =>
        await SendAsync(() => new HttpRequestMessage(HttpMethod.Get, "api/v1/" + path), type, cancel).ConfigureAwait(false);

    /// <summary>Sends a POST with a JSON body and parses the answer.</summary>
    /// <typeparam name="TBody">The body's type.</typeparam>
    /// <typeparam name="T">The answer's type.</typeparam>
    /// <param name="path">The path under /api/v1/.</param>
    /// <param name="body">The body, or default for none.</param>
    /// <param name="bodyType">The body's metadata.</param>
    /// <param name="type">The answer's metadata.</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>The status code and the parsed answer (default when the answer is not 2xx).</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    internal async Task<(HttpStatusCode Status, T? Value, string Text)> PostAsync<TBody, T>(string path, TBody? body,
        JsonTypeInfo<TBody> bodyType, JsonTypeInfo<T> type, CancellationToken cancel) =>
        await SendAsync(() => new HttpRequestMessage(HttpMethod.Post, "api/v1/" + path)
        {
            Content = body is null ? null : JsonContent.Create(body, bodyType),
        }, type, cancel).ConfigureAwait(false);

    /// <summary>Sends a call, waking the site or refreshing the token once when needed.</summary>
    /// <typeparam name="T">The answer's type.</typeparam>
    /// <param name="make">Makes the request (a request can be sent once, so a retry needs a new one).</param>
    /// <param name="type">The answer's metadata.</param>
    /// <param name="cancel">Cancels the call.</param>
    /// <returns>The status, the parsed answer, and the answer's text (for errors).</returns>
    /// <exception cref="SiteUnavailableException">The coordinator could not be reached or woken.</exception>
    private async Task<(HttpStatusCode, T?, string)> SendAsync<T>(Func<HttpRequestMessage> make, JsonTypeInfo<T> type,
        CancellationToken cancel)
    {
        bool woke = false, refreshed = false;
        while (true)
        {
            using HttpRequestMessage request = make();
            request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", Token(refresh: false));
            HttpResponseMessage response;
            try
            {
                response = await _http.SendAsync(request, cancel).ConfigureAwait(false);
            }
            catch (HttpRequestException) when (!woke)
            {
                // Expected when the site idled out: start the coordinator and try once more.
                Wake();
                woke = true;
                continue;
            }
            catch (HttpRequestException ex)
            {
                throw new SiteUnavailableException($"the coordinator does not answer: {ex.Message}");
            }
            using (response)
            {
                if (response.StatusCode == HttpStatusCode.Unauthorized && !refreshed)
                {
                    // A new site (or a new token after a reinstall): read it from the site again.
                    Token(refresh: true);
                    refreshed = true;
                    continue;
                }
                string text = await response.Content.ReadAsStringAsync(cancel).ConfigureAwait(false);
                T? value = response.IsSuccessStatusCode && text.Length > 0
                    ? System.Text.Json.JsonSerializer.Deserialize(text, type)
                    : default;
                return (response.StatusCode, value, text);
            }
        }
    }

    /// <summary>The admin token: from the cache, or read from the site (then cached in the user's local folder, whose
    /// default ACL is the user's own).</summary>
    /// <param name="refresh">Read it from the site even when cached.</param>
    /// <returns>The token.</returns>
    /// <exception cref="SiteUnavailableException">The site has no token (the coordinator never started).</exception>
    private string Token(bool refresh)
    {
        if (!refresh && _token is not null)
        {
            return _token;
        }
        if (!refresh && File.Exists(_tokenCache))
        {
            _token = File.ReadAllText(_tokenCache).Trim();
            if (_token.Length >= 32)
            {
                return _token;
            }
        }
        (int code, string output) = Wsl("cat", "/srv/buildserver/coordinator/admin.token");
        string token = output.Trim();
        if (code != 0 || token.Length < 32)
        {
            throw new SiteUnavailableException("the site has no admin token yet (start it: python buildserver/site/sitectl.py up)");
        }
        Directory.CreateDirectory(Path.GetDirectoryName(_tokenCache)!);
        File.WriteAllText(_tokenCache, token);
        _token = token;
        return token;
    }

    /// <summary>Starts the coordinator in the site (coordinator-up.sh waits until it answers).</summary>
    /// <exception cref="SiteUnavailableException">It did not start.</exception>
    private static void Wake()
    {
        Console.Error.WriteLine("amb: the coordinator does not answer; starting it in the site...");
        (int code, string output) = Wsl("/srv/buildserver/bin/coordinator-up.sh");
        if (code != 0)
        {
            throw new SiteUnavailableException($"the coordinator did not start: {output.Trim()}");
        }
    }

    /// <summary>Runs one command in the site distro as root.</summary>
    /// <param name="args">The command and its arguments (passed as they are: wsl.exe -e does no shell parsing).</param>
    /// <returns>The exit code and the output (stdout and stderr).</returns>
    /// <exception cref="SiteUnavailableException">wsl.exe could not be started or did not finish within 2 minutes.</exception>
    private static (int Code, string Output) Wsl(params string[] args)
    {
        var info = new ProcessStartInfo("wsl.exe")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (string arg in (string[])["-d", Distro, "-u", "root", "-e", .. args])
        {
            info.ArgumentList.Add(arg);
        }
        info.Environment["WSL_UTF8"] = "1";
        try
        {
            using Process process = Process.Start(info) ?? throw new SiteUnavailableException("wsl.exe did not start");
            string output = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
            if (!process.WaitForExit(120_000))
            {
                process.Kill(entireProcessTree: true);
                throw new SiteUnavailableException("wsl.exe did not finish within 2 minutes");
            }
            return (process.ExitCode, output);
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            throw new SiteUnavailableException($"wsl.exe: {ex.Message}");
        }
    }
}
