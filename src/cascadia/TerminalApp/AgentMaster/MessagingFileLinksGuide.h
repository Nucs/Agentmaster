// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the FILE LINKS' "Copy AI Impl Instructions" (MESSAGING.md §21.32.5) -- the PURE half.
//
// The cog's button copies ONE Markdown brief that an AI agent with a shell on this PC (Claude Code, Codex, ...) follows to
// give this install's file server a public HTTPS address, the way the owner's own hosted route works: what the server
// accepts, the one URL shape a public route may forward, the routes that fit (a Cloudflare Tunnel on the user's own
// domain, Tailscale Funnel, a reverse proxy already running, a throwaway trial tunnel), the Windows firewall, where
// Agentmaster takes the address, the checks that prove the chain WITHOUT the session secret, and how to undo it. This
// install's own facts (station, port, addresses, paths, the mode the cog shows) are written in, so the brief is pasted as
// it is -- the user's "one-copy prompt".
//
// Pure C++ (no Windows header): the m5 harness checks the text (tests_messaging_files.cpp, TestMessagingFiles). The cog
// gathers the facts and puts the text on the clipboard (AgentManagerContent.Settings.cpp, _CopyFileLinksAiGuide).
//
// ⚠ The brief's own words are ASCII on purpose: it is pasted into terminals and agents whose code pages are unknown, and it
// carries commands the agent copies verbatim (a typographic dash or quote in a command breaks it). Only the facts are
// written as they are (a path or an adapter name may hold any character).

#pragma once

#include "MessagingFileLinks.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Agentmaster::Msg
{
    /// <summary>
    /// What the brief says about THIS install, read by the cog at the moment the button is pressed (the boxes as they are,
    /// saved or not). Every member may be empty: the brief then states the default or says the fact is unknown instead of
    /// printing a blank -- so a brief copied from a half-filled cog is still followable.
    /// </summary>
    struct FileLinksGuideFacts
    {
        /// <summary>True for the AgentmasterDev package: the brief names the install, so a dev route is never mistaken for the release one.</summary>
        bool devInstall{ false };
        /// <summary>
        /// The station the links carry, already valid (NormalizeFileLinkStation), "" = the install's default. The route's path
        /// filter names exactly this station: a route built for another one forwards nothing.
        /// </summary>
        std::wstring station;
        /// <summary>The server's TCP port, 0 = the install's default (32594 release, 32595 dev).</summary>
        uint16_t port{ 0 };
        /// <summary>The mode the cog shows: Off means the server is not running, so the brief has the user turn it on first.</summary>
        FileLinkMode mode{ FileLinkMode::Off };
        /// <summary>
        /// The hosted address typed in the cog, normalized (NormalizeFileLinkBase), "" = not set. Not set (or the placeholder
        /// default itself) makes the brief's commands use https://files.example.com, marked as the address to replace.
        /// </summary>
        std::wstring hostedBase;
        /// <summary>This PC's LAN choices as the cog's dropdown words them ("http://&lt;address&gt;:&lt;port&gt;  --  &lt;adapter&gt;").</summary>
        std::vector<std::wstring> lanChoices;
        /// <summary>
        /// The LAN forwarders' target, the first of the cog's choices: the first private address ("http://192.168.1.17:32594"),
        /// or the PC's DNS name when it has no private address; "" = neither (the brief then writes a stand-in to replace).
        /// </summary>
        std::wstring lanUrl;
        /// <summary>The running Agentmaster.exe: a firewall rule for a forwarder on another LAN machine names this program.</summary>
        std::wstring exePath;
        /// <summary>The profile folder: settings.json and hooks.log (the [file-link] request lines) live in it.</summary>
        std::wstring profileDir;
        /// <summary>
        /// Credential Manager's target of the session keys' secret. The brief NAMES it (deleting it voids every link) and
        /// forbids reading it; the secret itself is never part of the facts.
        /// </summary>
        std::wstring secretTarget;
        /// <summary>The service's status line as the cog shows it ("" = unknown): tells the agent whether the server runs.</summary>
        std::wstring serverStatus;
    };

    /// <summary>
    /// The path a public route forwards, as a regular expression: "^/station-&lt;station&gt;/[0-9a-f]{64}/" -- the station,
    /// then a session key of exactly 64 lowercase hex characters, then the file's path. Everything else must stop at the
    /// route, before this PC.
    /// </summary>
    /// <remarks>A station is [a-z0-9-] only (IsValidFileLinkStation), so it needs no escaping: '-' is literal outside a bracket.</remarks>
    /// <param name="station">A valid station name.</param>
    /// <returns>The expression.</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::wstring FileLinkRoutePathRegex(std::wstring_view station)
    {
        return L"^/station-" + std::wstring{ station } + L"/[0-9a-f]{" + std::to_wstring(kFileLinkHashChars) + L"}/";
    }

    /// <summary>
    /// Builds the Markdown brief that the cog's "Copy AI Impl Instructions" puts on the clipboard: the task and its ground
    /// rules, this install's facts, the link contract the route must keep, the four routes, the firewall, the setting the
    /// user changes, the checks (each provable without the session secret: a well-formed unknown key must come back 404 FROM
    /// THIS PC, and every malformed request must stop at the route), the failure signs and the undo.
    /// </summary>
    /// <remarks>
    /// Never names the owner's own domain or infrastructure: every address a user would replace is under example.com. The
    /// checks rely on the server's /healthz and on its hooks.log line per request, both of which exist only while file links
    /// are on and the Telegram bot runs -- the brief says so before the first check.
    /// </remarks>
    /// <param name="facts">This install's facts (see <see cref="FileLinksGuideFacts"/>).</param>
    /// <returns>The brief, ASCII Markdown with "\n" line ends.</returns>
    /// <exception cref="std::bad_alloc">The text could not be allocated.</exception>
    inline std::wstring BuildFileLinksAiGuide(const FileLinksGuideFacts& facts)
    {
        const std::wstring station = facts.station.empty() ? DefaultFileLinkStation(facts.devInstall) : facts.station;
        const std::wstring port = std::to_wstring(facts.port != 0 ? facts.port : DefaultFileLinkPort(facts.devInstall));
        const std::wstring local = L"http://127.0.0.1:" + port;
        const std::wstring regex = FileLinkRoutePathRegex(station);
        const std::wstring prefix = L"/station-" + station + L"/";
        // A typed address is the one the user already chose: the commands use it. Otherwise a marked stand-in, never the
        // placeholder default (a check against example.com would prove nothing about the user's route).
        const bool hostedChosen = !facts.hostedBase.empty() && std::wstring_view{ facts.hostedBase } != kDefaultFileLinkHostedBase;
        const std::wstring host = hostedChosen ? facts.hostedBase : std::wstring{ L"https://files.example.com" };
        // "scheme://authority/path" -> the authority, with or without its port: the tunnel's hostname, the WAF's http.host and a
        // Caddy site name want the bare name; a reverse proxy's upstream wants address and port.
        const auto authorityOf = [](const std::wstring& url, bool keepPort) {
            const size_t scheme = url.find(L"://");
            std::wstring a = url.substr(scheme == std::wstring::npos ? 0 : scheme + 3);
            a = a.substr(0, a.find(L'/'));
            if (!keepPort && a.rfind(L':') != std::wstring::npos && a.find(L']') == std::wstring::npos)
            {
                a = a.substr(0, a.rfind(L':'));
            }
            return a;
        };
        const std::wstring hostName = authorityOf(host, false);
        const std::wstring probe = host + prefix + std::wstring(kFileLinkHashChars, L'0') + L"/agentmaster-route-check.txt";
        // A LAN forwarder's target: the first private address the cog found, else a stand-in the agent must replace.
        const std::wstring lanTarget = facts.lanUrl.empty() ? std::wstring{ L"<this PC's LAN address>:" } + port : authorityOf(facts.lanUrl, true);
        const std::wstring exe = facts.exePath.empty() ? std::wstring{ L"<the full path of Agentmaster.exe>" } : facts.exePath;
        const std::wstring profile = facts.profileDir.empty() ? std::wstring{ L"%USERPROFILE%\\.agentmaster" } : facts.profileDir;
        const std::wstring secret = facts.secretTarget.empty() ? std::wstring{ L"Agentmaster:messaging:telegram-filelinks:<profile folder name>" } : facts.secretTarget;

        std::wstring md;
        md.reserve(16 * 1024);
        const auto line = [&md](std::wstring_view text) {
            md += text;
            md += L'\n';
        };

        line(L"# Give Agentmaster's file links a public download address");
        line(L"");
        line(L"You are an AI agent with a shell on the user's Windows PC. Agentmaster (a Windows Terminal-based manager for Claude Code "
             L"sessions) runs here and mirrors its sessions into the user's Telegram. When Claude links a file of this PC in a reply, "
             L"Agentmaster rewrites the link into a download link, and `/download <path>` in a session's topic makes one for any file of "
             L"the session. A phone can open such a link only when its address reaches this PC.");
        line(L"");
        line(L"**Your task:** give this install's file server a public HTTPS address that forwards ONLY Agentmaster's download links to "
             L"it, have the user point Agentmaster at that address, and prove the whole chain works. Afterwards the user opens, from "
             L"anywhere, the files Claude links.");
        line(L"");
        line(L"**Ground rules**");
        line(L"- Show the user your plan first. Ask before anything that costs money, changes DNS, a router or a firewall, installs a "
             L"service or signs in to an account, and wait for a yes.");
        line(L"- Forward only the link shape below: never the whole port, never anything else on this PC.");
        line(L"- Never read, print or move a secret: not the session-key secret in Windows Credential Manager (`" + secret +
             L"`), not the Telegram bot token, not a tunnel's credentials file or token.");
        line(L"- Do not put a login, basic auth, client certificates, cookies, a captcha or a challenge page in front of the links: "
             L"Telegram's in-app browser and Android's download manager cannot pass them. The URL itself is the credential (see below).");
        line(L"- Do not edit Agentmaster's settings.json while Agentmaster runs: its own Save writes its in-memory copy back over it. "
             L"Ask the user to change settings in Agentmaster's Settings instead.");
        line(L"");

        line(L"## This install (as Agentmaster saw it when this was copied)");
        line(L"");
        line(std::wstring{ L"- Install: " } + (facts.devInstall ? L"Agentmaster Dev (the development install)" : L"Agentmaster"));
        line(L"- Station (the links' first path segment): `" + station + L"`");
        line(L"- File server: `" + local + L"` (it listens on 0.0.0.0:" + port + L" while file links are on and the Telegram bot runs)");
        line(L"- Health check: `" + local + L"/healthz` answers `ok`");
        if (facts.lanChoices.empty())
        {
            line(L"- This PC's LAN addresses: none found (a forwarder on another machine has nothing to reach; run it on this PC)");
        }
        else
        {
            line(L"- This PC's LAN addresses:");
            for (const auto& choice : facts.lanChoices)
            {
                line(L"  - `" + choice + L"`");
            }
        }
        std::wstring now = L"- File links now: ";
        switch (facts.mode)
        {
        case FileLinkMode::Lan:
            now += L"on the home network (links point at a LAN address)";
            break;
        case FileLinkMode::Hosted:
            now += L"through the hosted address";
            break;
        default:
            now += L"Off (the server is not running)";
            break;
        }
        now += hostedChosen ? L"; hosted address: `" + facts.hostedBase + L"`" :
                              L"; hosted address: not set (the default, " + std::wstring{ kDefaultFileLinkHostedBase } + L", is a placeholder that reaches nothing)";
        line(now);
        if (!facts.serverStatus.empty())
        {
            line(L"- The server's status line: " + facts.serverStatus);
        }
        line(L"- Agentmaster.exe: `" + exe + L"`");
        line(L"- Log: `" + profile + L"\\hooks.log` (one `[file-link]` line per request that reaches the server)");
        line(L"- Settings: `" + profile + L"\\settings.json` (read it if you need to; do not write it while Agentmaster runs)");
        line(L"");
        line(L"If file links are Off, or the status line does not say the server listens, the server is not running: ask the user to "
             L"set File links to \"On the home network\" first (Settings > Messaging > FILE LINKS, then Save) and to make sure the "
             L"Telegram bot is set up and running (same tab). Then `" + local + L"/healthz` must answer before anything else.");
        line(L"");

        line(L"## How a link works (do not change any of this)");
        line(L"");
        line(L"A link is `<hosted address>" + prefix + L"<key>/<path>`:");
        line(L"- `<key>`: the session's key, " + std::to_wstring(kFileLinkHashChars) +
             L" lowercase hex characters (a keyed SHA-256 of the station and the session id; the secret never leaves this PC).");
        line(L"- `<path>`: the file's path, drive letter first, each segment percent-encoded, e.g. `/K/source/project/notes.md`.");
        line(L"");
        line(L"The server answers a link only when the key is one of this install's sessions, the path is a file Claude (or /download) "
             L"linked in that session within the last 30 days, and the file still passes the secret-file and excluded-folder rules; "
             L"every other path is a 404. Anyone holding a link can download that one file until it expires -- that is the design: a "
             L"leaked link cannot be widened to another file.");
        line(L"");
        line(L"The server itself:");
        line(L"- Accepts connections only from loopback (127.x) and private LAN addresses (10.x, 172.16-31.x, 192.168.x). Anything "
             L"else, Tailscale's 100.64.x addresses included, is closed unread. So the program that forwards the public requests must "
             L"run ON THIS PC (it then connects to 127.0.0.1) or on a LAN machine that reaches one of the LAN addresses above.");
        line(L"- Answers GET and HEAD only (405 otherwise), `/healthz`, and the one link shape; supports Range; sends "
             L"`Content-Disposition: attachment` and `Cache-Control: no-store`.");
        line(L"- Reads `Cf-Connecting-Ip`, then `X-Forwarded-For`, only to log who asked.");
        line(L"");

        line(L"## What to build");
        line(L"");
        line(L"    phone --HTTPS--> your public name --> filter: only the link shape --> tunnel or proxy --> " + local);
        line(L"");
        line(L"The filter, in one sentence: forward a request only when its method is GET or HEAD, its path matches `" + regex +
             L"` and it has no query string; answer 404 (or 403) to everything else before it reaches this PC. With several "
             L"Agentmaster installs (each its own station and port), add one such rule per station.");
        line(L"");
        line(L"Pick one route with the user:");
        line(L"");

        line(L"### A. A Cloudflare Tunnel on the user's own domain (recommended; how Agentmaster's author runs it)");
        line(L"");
        line(L"Needs a domain whose DNS is on Cloudflare (the Free plan is enough). Nothing listens on the internet at home and no "
             L"router port opens: `cloudflared` connects out.");
        line(L"");
        line(L"1. Install cloudflared: `winget install --id Cloudflare.cloudflared`.");
        line(L"2. Create the tunnel. Simplest on Windows: in Cloudflare's dashboard, Zero Trust > Networks > Tunnels > Create a tunnel "
             L"(Cloudflared), then run the `cloudflared.exe service install <token>` command it shows, in an elevated shell, on THIS PC "
             L"(or on an always-on LAN machine). Treat the token as a secret: never print it back.");
        line(L"3. Give the tunnel a public hostname chosen with the user (`" + hostName + L"` below): path `" + regex +
             L"`, service HTTP `127.0.0.1:" + port + L"` (cloudflared on another LAN machine: `" + lanTarget +
             L"`). A request no hostname-and-path rule matches gets the tunnel's catch-all 404. A locally managed tunnel says the "
             L"same in its config.yml:");
        line(L"");
        line(L"       ingress:");
        line(L"         - hostname: " + hostName);
        line(L"           path: " + regex);
        line(L"           service: " + local);
        line(L"         - service: http_status:404");
        line(L"");
        line(L"4. Add a WAF custom rule for the hostname (Security > WAF > Custom rules), action Block:");
        line(L"");
        line(L"       (http.host eq \"" + hostName + L"\" and (not http.request.method in {\"GET\" \"HEAD\"} or not starts_with(http.request.uri.path, \"" +
             prefix + L"\") or http.request.uri.query ne \"\"))");
        line(L"");
        line(L"   Rules on the Free plan have no regular expressions (starts_with and len work) and the plan allows 5 custom rules; the "
             L"tunnel's path expression is the exact check. Optional: a country rule. Keep Bot Fight Mode, Managed Challenges and "
             L"\"Under Attack\" off for this hostname: the downloads come from apps, not browsers.");
        line(L"5. The server sends `no-store`, so Cloudflare does not cache the files; no cache rule is needed.");
        line(L"");

        line(L"### B. Tailscale Funnel (no domain needed; an address under ts.net)");
        line(L"");
        line(L"Needs Tailscale on this PC and Funnel allowed in the tailnet's policy (the first `tailscale funnel` prints the link that "
             L"enables it). Funnel serves ports 443, 8443 or 10000 only and reaches the server over loopback, so the server accepts it.");
        line(L"");
        line(L"1. `tailscale funnel --bg --set-path " + prefix + L" " + local + prefix +
             L"` -- check `tailscale funnel --help` for your version's syntax, and use check 2 below to confirm that the server "
             L"receives the FULL path `" + prefix + L"<key>/...` (versions differ in whether they strip the mounted path; the path "
             L"on the target is there for the ones that do).");
        line(L"2. `tailscale funnel status` shows the public address (`https://<machine>.<tailnet>.ts.net`): that is the hosted address.");
        line(L"");
        line(L"Funnel cannot check the key's 64 characters or the method: the server's own 404 and 405 do.");
        line(L"");

        line(L"### C. A reverse proxy the user already runs (Caddy, nginx, Traefik; on the LAN, or a VPS that reaches the LAN over a VPN)");
        line(L"");
        line(L"Route only the link shape to this PC's LAN address; TLS from the proxy's own ACME. Caddy:");
        line(L"");
        line(L"    " + hostName + L" {");
        line(L"        @link {");
        line(L"            method GET HEAD");
        line(L"            path_regexp " + regex);
        line(L"        }");
        line(L"        handle @link {");
        line(L"            reverse_proxy " + lanTarget);
        line(L"        }");
        line(L"        handle {");
        line(L"            respond 404");
        line(L"        }");
        line(L"    }");
        line(L"");
        line(L"nginx: `location ~ " + regex + L" { limit_except GET HEAD { deny all; } if ($args != \"\") { return 404; } proxy_pass http://" +
             lanTarget + L"; proxy_set_header X-Forwarded-For $remote_addr; proxy_buffering off; }` plus `location / { return 404; }`.");
        line(L"");
        line(L"A router port-forward straight to this PC does NOT work (the server refuses public addresses) and would skip the "
             L"filter: always put the proxy in between.");
        line(L"");

        line(L"### D. A five-minute trial: `cloudflared tunnel --url " + local + L"`");
        line(L"");
        line(L"It prints a `https://<random>.trycloudflare.com` address that changes at every start and has no filter (the server's own "
             L"rules still apply). Good for proving the chain once, not for daily use: every link dies with the address.");
        line(L"");

        line(L"## The Windows firewall");
        line(L"");
        line(L"- A forwarder on this PC (A or D on this PC, or B): nothing to open; it reaches 127.0.0.1.");
        line(L"- A forwarder on another LAN machine (A elsewhere, or C): let the port in for Agentmaster.exe from the LAN only, in an "
             L"elevated shell:");
        line(L"");
        line(L"      New-NetFirewallRule -DisplayName \"Agentmaster file links\" -Direction Inbound -Program \"" + exe +
             L"\" -Protocol TCP -LocalPort " + port + L" -RemoteAddress LocalSubnet -Action Allow");
        line(L"");
        line(L"  An Agentmaster update may run from another folder (a new Agentmaster.exe path): the rule then needs the new path, or "
             L"a port-only rule scoped to the forwarder's address instead.");
        line(L"");

        line(L"## Point Agentmaster at the address");
        line(L"");
        line(L"Ask the user to open Agentmaster's Settings (the cog) > Messaging > FILE LINKS and set:");
        line(L"- File links: \"Through your hosted address\"");
        line(L"- Hosted address: the public address, scheme and host only, no path (e.g. `" + host + L"`)");
        line(L"- then Save. The station and the port stay as they are: a new station voids every link already posted, and the route "
             L"names this one.");
        line(L"");

        line(L"## Verify (run each check and report its result)");
        line(L"");
        line(L"1. Local: `curl.exe -s -o NUL -w \"%{http_code}\\n\" " + local + L"/healthz` -> `200`.");
        line(L"2. Through the route, a well-formed key that no session has: `curl.exe -s -o NUL -w \"%{http_code}\\n\" " + probe +
             L"` -> `404`, AND hooks.log gains a new `[file-link] 404` line for it: the request reached this PC through the route.");
        line(L"3. Through the route, what the filter must stop. Each must be refused (403 or 404) with NO new `[file-link]` line:");
        line(L"   - `" + host + L"/`");
        line(L"   - `" + host + L"/healthz`");
        line(L"   - `" + host + prefix + L"abc/x.txt`");
        line(L"   - `" + probe + L"?x=1`");
        line(L"   - `curl.exe -s -o NUL -w \"%{http_code}\\n\" -X POST " + probe + L"`");
        line(L"4. A real link: ask the user to send `/download <a harmless file>` in a session's topic in Telegram and to tap its "
             L"Download link on the phone with Wi-Fi off: the file downloads, and hooks.log shows `[file-link] 200`.");
        line(L"");

        line(L"## When it fails");
        line(L"");
        line(L"- Check 1 fails: file links are Off, the Telegram bot is not running, or another program holds the port (`netstat -ano | findstr :" +
             port + L"`).");
        line(L"- Check 2 times out, resets or answers 502, 530 or 1033: the forwarder cannot reach the server -- a wrong address or "
             L"port, the server not running, a LAN forwarder without the firewall rule, or a forwarder that connects from an address "
             L"the server refuses (a public or 100.64.x address; the server closes such a connection unread): run it on this PC or "
             L"on the LAN.");
        line(L"- Check 2 answers 404 but hooks.log stays silent: the route answered by itself -- its path rule does not match the link "
             L"(another station, a typo in the expression). Every request that reaches the server, served or refused, writes a "
             L"`[file-link]` line (only `/healthz` does not), with the reason in parentheses: a path the server could not read (a "
             L"route that stripped or rewrote it) reads differently from an unknown key.");
        line(L"- A real link answers 404: the station in the link and in the route differ, the file moved, or its last mention is "
             L"older than 30 days.");
        line(L"- The phone shows a challenge page: remove the challenge or bot rule for this hostname.");
        line(L"");

        line(L"## Undo");
        line(L"");
        line(L"Remove the route (the tunnel's public hostname or the whole tunnel, its DNS record and the WAF rule; or "
             L"`tailscale funnel reset`; or the proxy's site), the firewall rule if one was added, and ask the user to set File links "
             L"back to Off or \"On the home network\". To void every link ever posted, the user deletes the credential `" + secret +
             L"` in Windows Credential Manager and restarts Agentmaster.");
        return md;
    }
}
