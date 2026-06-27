// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and
// the standalone test harness compiles it directly).
#include "ClaudeSpawn.h"

#include <windows.h>
#include <objbase.h> // CoCreateGuid

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Persistence.h" // GetDirEnv (the per-directory env overrides; ResolveSessionEnv reads it)
#include "ProcessInspect.h" // SnapshotProcesses / FindDescendantByImage / ReadProcessCwd (moved here)
#include "ProfileBootstrap.h" // the per-install state PROFILE (AgentmasterStateDir now resolves through it)

namespace
{
    // Read an environment variable into a std::wstring (no CRT deprecation, robust to size).
    std::wstring GetEnvW(const wchar_t* name)
    {
        const DWORD need = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (need == 0)
        {
            return {};
        }
        std::wstring buf(need, L'\0');
        const DWORD got = ::GetEnvironmentVariableW(name, buf.data(), need);
        if (got == 0 || got >= need)
        {
            return {};
        }
        buf.resize(got);
        return buf;
    }

    std::string Utf16ToUtf8(std::wstring_view s)
    {
        if (s.empty())
        {
            return {};
        }
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
        {
            return {};
        }
        std::string out(static_cast<size_t>(needed), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed, nullptr, nullptr);
        return out;
    }

    bool WriteFileUtf8(const std::wstring& path, std::wstring_view content)
    {
        try
        {
            std::ofstream f(std::filesystem::path{ path }, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                return false;
            }
            const auto bytes = Utf16ToUtf8(content);
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return f.good();
        }
        catch (...)
        {
            return false;
        }
    }

    // Standard base64 (RFC 4648) of a raw byte buffer. Used to build a pwsh -EncodedCommand payload
    // (which expects base64 of the command's UTF-16LE bytes). Hand-rolled so the pure helper has no
    // crypt32 dependency and the standalone test harness links it unchanged.
    std::string Base64Encode(const unsigned char* data, size_t len)
    {
        static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((len + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 3 <= len; i += 3)
        {
            const unsigned n = (static_cast<unsigned>(data[i]) << 16) | (static_cast<unsigned>(data[i + 1]) << 8) | static_cast<unsigned>(data[i + 2]);
            out.push_back(tbl[(n >> 18) & 0x3F]);
            out.push_back(tbl[(n >> 12) & 0x3F]);
            out.push_back(tbl[(n >> 6) & 0x3F]);
            out.push_back(tbl[n & 0x3F]);
        }
        if (const size_t rem = len - i; rem == 1)
        {
            const unsigned n = static_cast<unsigned>(data[i]) << 16;
            out.push_back(tbl[(n >> 18) & 0x3F]);
            out.push_back(tbl[(n >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
        else if (rem == 2)
        {
            const unsigned n = (static_cast<unsigned>(data[i]) << 16) | (static_cast<unsigned>(data[i + 1]) << 8);
            out.push_back(tbl[(n >> 18) & 0x3F]);
            out.push_back(tbl[(n >> 12) & 0x3F]);
            out.push_back(tbl[(n >> 6) & 0x3F]);
            out.push_back('=');
        }
        return out;
    }
}

namespace Agentmaster
{
    std::wstring JsonEscape(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size() + 8);
        for (const wchar_t c : s)
        {
            switch (c)
            {
            case L'\"':
                out += L"\\\"";
                break;
            case L'\\':
                out += L"\\\\";
                break;
            case L'\b':
                out += L"\\b";
                break;
            case L'\f':
                out += L"\\f";
                break;
            case L'\n':
                out += L"\\n";
                break;
            case L'\r':
                out += L"\\r";
                break;
            case L'\t':
                out += L"\\t";
                break;
            default:
                if (c < 0x20)
                {
                    wchar_t buf[8];
                    ::swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                }
                else
                {
                    out += c;
                }
                break;
            }
        }
        return out;
    }

    std::wstring ToForwardSlashes(std::wstring_view path)
    {
        std::wstring out{ path };
        for (auto& c : out)
        {
            if (c == L'\\')
            {
                c = L'/';
            }
        }
        return out;
    }

    std::wstring PsSingleQuote(std::wstring_view s)
    {
        // PowerShell single-quoted literal: the only escape inside '…' is doubling the quote
        // itself; backslashes and $ are inert. So any absolute Windows path round-trips.
        std::wstring out;
        out.reserve(s.size() + 2);
        out.push_back(L'\'');
        for (const wchar_t c : s)
        {
            out.push_back(c);
            if (c == L'\'')
            {
                out.push_back(L'\'');
            }
        }
        out.push_back(L'\'');
        return out;
    }

    std::wstring BuildForwarderScript(const std::wstring& stateDir)
    {
        // Pure-ASCII PowerShell. Reads the hook JSON from stdin, correlates via env, and
        // posts one HookWire.h line to the named pipe. Best-effort throughout: any failure
        // is swallowed so a hook never breaks the Claude turn — but no longer SILENTLY (#5):
        // each dropped delivery appends one line to the per-profile forwarder-errors.log. The
        // engine's hooks.log is written by the BRIDGE, which a failed delivery never reaches,
        // so a dead pipe / stale bridge.json used to leave state stale (e.g. stuck Running off
        // a dropped Stop) with zero trace anywhere. The note itself is try/catch-wrapped —
        // diagnostics must never break the turn either.
        //
        // The bridge DISCOVERY fallback must point into THIS profile's stateDir (each profile
        // runs its own engine + pipe + bridge.json) — a fixed ~/.agentmaster path would route
        // a dev-profile session's hooks to the release instance's bridge. The placeholders are
        // substituted below with the PowerShell-quoted per-profile paths.
        std::wstring script = LR"PSHOOK(param([string]$Event = "")
$ErrorActionPreference = "SilentlyContinue"
$AmErrLog = {{AM_FWD_ERRLOG}}
function NoteFwdDrop([string]$why) {
  # Local fallback trace for a hook delivery the engine never received (otherwise INVISIBLE -
  # the bridge-side hooks.log only sees lines that arrived). One short ASCII line, append-only;
  # own try/catch so the diagnostic can never break the Claude turn.
  try {
    $stamp = [DateTimeOffset]::UtcNow.ToString("yyyy-MM-dd HH:mm:ss.fff") + "Z"
    Add-Content -LiteralPath $AmErrLog -Value ($stamp + " " + $Event + " sid=" + $sid + " " + $why)
  } catch { }
}
try {
  # Hook FIRE time (unix ms, UTC) - stamped FIRST, before stdin/transcript work, so the wire
  # `ts` field orders events even when this forwarder runs slow (the Stop path reads the
  # transcript below). Matches C++ NowMs(); see HookWire.h + NextSessionStateOrdered.
  $ts = [string][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
  $raw = ""
  try { $raw = [Console]::In.ReadToEnd() } catch { }

  $j = $null
  if (-not [string]::IsNullOrEmpty($raw)) {
    try { $j = $raw | ConvertFrom-Json } catch { $j = $null }
  }

  # Session id: prefer Claude's OWN current conversation id from the hook PAYLOAD. It is
  # authoritative and FOLLOWS /resume, /clear and /compact; the launch-time env does NOT (it is
  # pinned at spawn). The env (set only for sessions WE launched) is the FALLBACK for the rare
  # payload that lacks a session_id. Env-FIRST stranded a managed session whose conversation
  # diverged from its launch id: every post-divergence hook fed the dead launch-id record (which
  # has no transcript of its own, so the scanner could never reconcile it), leaving a phantom
  # stuck in the last mis-attributed state (e.g. a Notification -> NeedsApproval that never cleared,
  # or Idle while the real conversation runs). For a normal spawn the FIRST conversation's payload
  # id == our --session-id == the env, so this is a no-op until a divergence actually happens.
  $sid = ""
  if ($j -ne $null -and $j.session_id) { $sid = [string]$j.session_id }
  if ([string]::IsNullOrEmpty($sid)) { $sid = $env:CCMGR_SESSION_ID }
  if ([string]::IsNullOrEmpty($sid)) { NoteFwdDrop "drop: no session id (payload had no session_id and env CCMGR_SESSION_ID unset)"; return }

  # Pipe: prefer the inherited env; otherwise the bridge discovery file (covers a shell that
  # did not inherit CCMGR_HOOK_PIPE).
  $pipe = $env:CCMGR_HOOK_PIPE
  if ([string]::IsNullOrEmpty($pipe)) {
    try {
      $disc = {{AM_BRIDGE_JSON}}
      if (Test-Path -LiteralPath $disc) {
        $b = (Get-Content -LiteralPath $disc -Raw) | ConvertFrom-Json
        if ($b.pipe) { $pipe = [string]$b.pipe }
      }
    } catch { }
  }
  if ([string]::IsNullOrEmpty($pipe)) { NoteFwdDrop "drop: no pipe (env CCMGR_HOOK_PIPE unset; bridge.json missing or empty)"; return }

  $cwd = ""
  $isQ = "0"
  $perm = "0"
  $tool = ""
  $prompt = ""
  if ($j -ne $null) {
    if ($j.cwd) { $cwd = [string]$j.cwd }
    if ($j.tool_name) { $tool = [string]$j.tool_name }
    # The submitted prompt text, so the Manager's Flight Plan reflects EVERY message the
    # session got (including ones typed straight into this terminal). Escaped to keep the
    # wire record single-line + TAB-free (mirrors HookWire.h WireEscape): \ -> \\ then tab/CR/LF.
    if ($Event -eq "UserPromptSubmit" -and $j.prompt) {
      $prompt = [string]$j.prompt
      $prompt = $prompt -replace '\\','\\' -replace "`t",'\t' -replace "`r",'\r' -replace "`n",'\n'
    }
    if ($Event -eq "Notification") {
      $m = ""
      if ($j.message) { $m = [string]$j.message }
      if ($m -match "(?i)permission|approve|allow|grant") { $perm = "1" }
    }
    if ($Event -eq "Stop") {
      $tp = ""
      if ($j.transcript_path) { $tp = [string]$j.transcript_path }
      if ((-not [string]::IsNullOrEmpty($tp)) -and (Test-Path -LiteralPath $tp)) {
        $tail = @(Get-Content -LiteralPath $tp -Tail 60 -ErrorAction SilentlyContinue)
        for ($i = $tail.Length - 1; $i -ge 0; $i--) {
          $obj = $null
          try { $obj = $tail[$i] | ConvertFrom-Json } catch { continue }
          if ($obj.type -eq "assistant" -and $obj.message -and $obj.message.content) {
            $txt = ""
            foreach ($c in $obj.message.content) {
              if ($c.type -eq "text" -and $c.text) { $txt = [string]$c.text }
            }
            $txt = $txt.TrimEnd()
            if ($txt.EndsWith("?")) { $isQ = "1" }
            break
          }
        }
      }
    }
  }

  # The hosting terminal's WT_SESSION GUID (inherited from the ConPTY). Lets the app correlate
  # an adopted session back to its connection to bind a stdin injector.
  $tab = $env:WT_SESSION
  if ($null -eq $tab) { $tab = "" }

  $name = $pipe
  $bs = $pipe.LastIndexOf("\")
  if ($bs -ge 0) { $name = $pipe.Substring($bs + 1) }

  $line = ($Event, $sid, $cwd, $isQ, $perm, $tool, $tab, $prompt, $ts) -join "`t"

  $client = New-Object System.IO.Pipes.NamedPipeClientStream(".", $name, [System.IO.Pipes.PipeDirection]::Out)
  try {
    # A bounded connect: with the app running the local pipe answers in <50ms; a stale
    # discovery file fails in ~1s rather than stalling the Claude turn.
    $client.Connect(1000)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($line + "`n")
    $client.Write($bytes, 0, $bytes.Length)
    $client.Flush()
  } finally {
    $client.Dispose()
  }
} catch {
  # The pipe connect/write path lands here (engine gone, stale bridge.json -> ~1s timeout, ...).
  NoteFwdDrop ("drop: " + $_.Exception.Message)
}
)PSHOOK";

        const std::wstring token = L"{{AM_BRIDGE_JSON}}";
        const auto at = script.find(token);
        if (at != std::wstring::npos)
        {
            script.replace(at, token.size(), PsSingleQuote(stateDir + L"\\bridge.json"));
        }
        // The silent-drop fallback log — per-profile like the bridge discovery (#5).
        const std::wstring errToken = L"{{AM_FWD_ERRLOG}}";
        const auto errAt = script.find(errToken);
        if (errAt != std::wstring::npos)
        {
            script.replace(errAt, errToken.size(), PsSingleQuote(stateDir + L"\\forwarder-errors.log"));
        }
        return script;
    }

    std::wstring BuildHooksSettingsJson(std::wstring_view forwarderPath, std::wstring_view model, bool includeCoAuthoredBy, bool skipPermissions)
    {
        const auto base = std::wstring{ L"powershell -NoProfile -ExecutionPolicy Bypass -File \"" } + std::wstring{ forwarderPath } + L"\"";

        const auto block = [&](std::wstring_view evt) -> std::wstring {
            const auto cmd = base + L" -Event " + std::wstring{ evt };
            return L"    \"" + std::wstring{ evt } + L"\": [ { \"hooks\": [ { \"type\": \"command\", \"command\": \"" + JsonEscape(cmd) + L"\" } ] } ]";
        };

        std::wstring json = L"{\n  \"hooks\": {\n";
        json += block(L"SessionStart") + L",\n";
        json += block(L"UserPromptSubmit") + L",\n";
        json += block(L"Notification") + L",\n";
        json += block(L"Stop") + L",\n";
        json += block(L"SubagentStop") + L",\n";
        json += block(L"SessionEnd") + L"\n";
        json += L"  }"; // close "hooks"

        // Optional Claude-session settings from the cog. Each is emitted ONLY when it differs
        // from Claude's own default, so an all-defaults config stays byte-for-byte the prior
        // hooks-only file (Json schema validates these keys; see the settings discussion).
        if (!model.empty())
        {
            json += L",\n  \"model\": \"" + JsonEscape(model) + L"\"";
        }
        if (!includeCoAuthoredBy)
        {
            json += L",\n  \"includeCoAuthoredBy\": false";
        }
        if (!skipPermissions)
        {
            // The "other variation": when we do NOT pass --dangerously-skip-permissions, pin
            // the standard permission mode in the settings file (normal prompts + trust apply).
            json += L",\n  \"permissions\": { \"defaultMode\": \"default\" }";
        }

        json += L"\n}\n";
        return json;
    }

    std::wstring BuildClaudeCommandline(std::wstring_view settingsPath, std::wstring_view sessionId, bool resume, bool skipPermissions, std::wstring_view forkFromSessionId, std::wstring_view claudeLauncher)
    {
        // When skipPermissions is ON (the cog default), spawn with --dangerously-skip-permissions:
        // the app drives claude programmatically (Autopilot + injected prompts) and gates risky
        // actions through its own Approval Policy, so the per-tool permission prompts are
        // redundant. Critically, permission mode `bypassPermissions` ALSO skips the per-folder
        // "Do you trust the files in this folder?" trust dialog at startup (the dialog block is
        // gated on `mode !== "bypassPermissions"`), which would otherwise wedge an unattended
        // ConPTY session waiting on a keypress. It does NOT suppress the one-time GLOBAL "Bypass
        // Permissions mode" acceptance (~/.claude.json `bypassPermissionsModeAccepted`).
        // When OFF, the flag is omitted and BuildHooksSettingsJson pins permissions.defaultMode
        // instead (normal prompts + trust apply).
        const std::wstring flag = skipPermissions ? L"--dangerously-skip-permissions " : L"";

        // How to invoke claude. The programmatic spawn runs through ConPTY's CreateProcessW, which —
        // unlike a shell — appends only ".exe" and NEVER consults PATHEXT. So a bare `claude` token
        // resolves ONLY a native claude.exe and silently misses the npm `claude.cmd` (the common
        // install), dying with ERROR_FILE_NOT_FOUND (0x80070002). We therefore launch the REAL
        // resolved launcher BY FULL PATH: a .exe runs directly (quoted, so spaces in the path are
        // safe); a .cmd/.bat is a batch script CreateProcessW cannot execute directly, so it is run
        // via `cmd /c`. `claudeLauncher` is resolved in Engine init BEFORE our shim dir is prepended
        // to PATH, so it is always the real claude, never our own shim. Empty (claude not found on
        // PATH at init) falls back to the bare token — the spawn then surfaces the not-found error,
        // and a genuinely PATH-resolvable claude.exe still works.
        bool batch = false;
        if (!claudeLauncher.empty())
        {
            const auto dot = claudeLauncher.find_last_of(L'.');
            if (dot != std::wstring_view::npos)
            {
                std::wstring ext{ claudeLauncher.substr(dot) };
                for (auto& c : ext)
                {
                    if (c >= L'A' && c <= L'Z')
                    {
                        c = static_cast<wchar_t>(c - L'A' + L'a');
                    }
                }
                batch = (ext == L".cmd" || ext == L".bat");
            }
        }
        const std::wstring exe = claudeLauncher.empty() ? std::wstring{ L"claude" } : (L"\"" + std::wstring{ claudeLauncher } + L"\"");

        std::wstring cmd;
        if (!forkFromSessionId.empty())
        {
            // Fork (Agentmaster): resume the SOURCE conversation's history, but --fork-session writes to
            // a NEW transcript whose id we pin with --session-id (`sessionId`, freshly minted by the
            // caller). The source <forkFrom>.jsonl is never written to -> no two-writers corruption from
            // duplicating a tab; and because the new id is known up front, the forked session registers
            // and binds exactly like a fresh launch.
            cmd = exe + L" " + flag + L"--resume " + std::wstring{ forkFromSessionId } + L" --fork-session --session-id " + std::wstring{ sessionId } + L" --settings \"" + std::wstring{ settingsPath } + L"\"";
        }
        else if (resume)
        {
            // Resume the existing conversation by id; --resume implies the session id.
            cmd = exe + L" " + flag + L"--resume " + std::wstring{ sessionId } + L" --settings \"" + std::wstring{ settingsPath } + L"\"";
        }
        else
        {
            cmd = exe + L" " + flag + L"--settings \"" + std::wstring{ settingsPath } + L"\" --session-id " + std::wstring{ sessionId };
        }

        if (batch)
        {
            // cmd /c with MORE than two quote chars (we quote both the launcher AND the --settings
            // path): cmd strips the FIRST and LAST quote of the remainder, then runs what's between.
            // So wrap the whole command in ONE outer pair — the inner quotes (launcher + settings)
            // survive intact. (See `cmd /?`: the >2-quotes case falls to "strip leading+trailing quote".)
            return L"cmd /c \"" + cmd + L"\"";
        }
        return cmd;
    }

    std::wstring BuildCodexCommandline(std::wstring_view resumeCodexUuid, std::wstring_view forkCodexUuid, std::wstring_view codexLauncher)
    {
        // How to invoke codex. Like claude, the programmatic spawn runs through ConPTY's CreateProcessW,
        // which — unlike a shell — appends only ".exe" and NEVER consults PATHEXT. So a bare `codex`
        // token resolves ONLY a native codex.exe and silently misses the npm `codex.cmd` (a common
        // install), dying with ERROR_FILE_NOT_FOUND (0x80070002). We therefore launch the resolved
        // launcher BY FULL PATH: a .exe runs directly (quoted, so spaces in the path are safe); a
        // .cmd/.bat is a batch script CreateProcessW cannot execute directly, so it is run via `cmd /c`.
        // `codexLauncher` is resolved at engine init (ResolveCodexLauncher). Empty (codex not found)
        // falls back to the bare token — the spawn then surfaces the not-found error, and a genuinely
        // PATH-resolvable codex.exe still works.
        bool batch = false;
        if (!codexLauncher.empty())
        {
            const auto dot = codexLauncher.find_last_of(L'.');
            if (dot != std::wstring_view::npos)
            {
                std::wstring ext{ codexLauncher.substr(dot) };
                for (auto& c : ext)
                {
                    if (c >= L'A' && c <= L'Z')
                    {
                        c = static_cast<wchar_t>(c - L'A' + L'a');
                    }
                }
                batch = (ext == L".cmd" || ext == L".bat");
            }
        }
        const std::wstring exe = codexLauncher.empty() ? std::wstring{ L"codex" } : (L"\"" + std::wstring{ codexLauncher } + L"\"");

        // Fork WINS over resume (mutually exclusive). `codex fork <uuid>` branches the source rollout
        // into a NEW session/rollout (the source is untouched — safe even while it is live: the
        // two-writers fix for adopting a LIVE external). Resume continues the existing rollout by its
        // uuid (model/sandbox/approval inherited). Neither => a fresh launch (just the launcher;
        // config.toml governs, cwd via the ConPTY, AM_SESSION via the child env).
        std::wstring cmd = exe;
        if (!forkCodexUuid.empty())
        {
            cmd += L" fork " + std::wstring{ forkCodexUuid };
        }
        else if (!resumeCodexUuid.empty())
        {
            cmd += L" resume " + std::wstring{ resumeCodexUuid };
        }

        if (batch)
        {
            // cmd /c with MORE than two quote chars (the quoted launcher, plus our outer pair): cmd
            // strips the FIRST and LAST quote of the remainder, then runs what's between. So wrap the
            // whole command in ONE outer pair — the inner launcher quote survives intact. (Mirrors
            // BuildClaudeCommandline; see `cmd /?`: the >2-quotes case falls to "strip leading+trailing quote".)
            return L"cmd /c \"" + cmd + L"\"";
        }
        return cmd;
    }

    std::wstring NewSessionId()
    {
        GUID g{};
        if (FAILED(::CoCreateGuid(&g)))
        {
            return L"00000000-0000-0000-0000-000000000000";
        }
        wchar_t buf[40];
        ::swprintf(
            buf,
            40,
            L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            g.Data1,
            g.Data2,
            g.Data3,
            g.Data4[0],
            g.Data4[1],
            g.Data4[2],
            g.Data4[3],
            g.Data4[4],
            g.Data4[5],
            g.Data4[6],
            g.Data4[7]);
        return std::wstring{ buf };
    }

    std::wstring ClaudeProjectsDir()
    {
        // Claude stores transcripts under <config>/projects/<encoded-cwd>/<session-id>.jsonl.
        // The config dir is CLAUDE_CONFIG_DIR if set, else ~/.claude.
        std::wstring base = GetEnvW(L"CLAUDE_CONFIG_DIR");
        if (base.empty())
        {
            const std::wstring home = GetEnvW(L"USERPROFILE");
            if (home.empty())
            {
                return {};
            }
            base = home + L"\\.claude";
        }
        return base + L"\\projects";
    }

    std::wstring ClaudeCwdForShell(uint32_t shellPid)
    {
        // Find the claude.exe serving this tab (descendant-or-self: the pid itself when a
        // Manager-launched claude IS the ConPTY root, else shell -> claude or shell -> cmd-shim
        // -> claude) and read ITS process cwd from the PEB. The helpers now live in ProcessInspect
        // (the Observer's reusable primitives); this stays a thin convenience over a fresh snapshot.
        const auto snap = SnapshotProcesses();
        const uint32_t claudePid = FindDescendantByImage(snap, shellPid, L"claude.exe");
        if (claudePid == 0)
        {
            return {}; // this shell isn't running a claude (also our scoping: skip non-claude tabs)
        }
        return ReadProcessCwd(claudePid);
    }

    std::wstring ResolveClaudeTranscriptPath(std::wstring_view sessionId)
    {
        if (sessionId.empty())
        {
            return {};
        }
        const std::wstring projects = ClaudeProjectsDir();
        if (projects.empty())
        {
            return {};
        }
        const std::wstring leaf = std::wstring{ sessionId } + L".jsonl";

        // Session ids are unique UUIDs, so rather than reproduce Claude's cwd->dir encoding we
        // just look for <session-id>.jsonl inside any project directory.
        const std::wstring pattern = projects + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        std::wstring found;
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const std::wstring candidate = projects + L"\\" + name + L"\\" + leaf;
            const DWORD attr = ::GetFileAttributesW(candidate.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                found = candidate;
                break;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
        return found;
    }

    bool ClaudeConversationExists(std::wstring_view sessionId)
    {
        return !ResolveClaudeTranscriptPath(sessionId).empty();
    }

    std::wstring AgentmasterStateDir()
    {
        // CRITICAL: this path must resolve to the SAME real location for both this app
        // (which may be MSIX-packaged) AND the EXTERNAL claude.exe we spawn — the spawn
        // passes this path to `claude --settings` and the hook forwarder script lives here.
        // A packaged app's %LOCALAPPDATA% is redirected to the package's LocalCache, so a
        // path string under %LOCALAPPDATA% would point an external process at an empty real
        // folder. The profile defaults anchor under %USERPROFILE% (mirrors ~/.claude).
        //
        // The location is the ACTIVE PROFILE (ProfileBootstrap.h): env AGENTMASTER_PROFILE
        // (exported by the WindowEmperor's startup bootstrap/picker, BEFORE any engine code
        // runs) > the portable marker > the saved per-install choice > the per-identity
        // default — ~/.agentmaster for the release package AND for unpackaged runs (tests,
        // tools: the historical location, unchanged), ~/.agentmaster-dev for AgentmasterDev.
        // Cached process-wide by ResolveProfileDir(): a profile cannot change mid-run.
        const std::wstring dir = Profiles::ResolveProfileDir();
        try
        {
            // Re-assert per call (the resolver created it once; this heals a mid-run delete,
            // matching the pre-profile behavior).
            std::filesystem::create_directories(std::filesystem::path{ dir });
        }
        catch (...)
        {
        }
        return dir;
    }

    // Agentmaster: a local-time [HH:MM:SS.mmm] stamp prefixed to the START of every log record (below), so
    // the hook event stream / engine-mechanism tags / [nav] trail / observer census in hooks.log (and the
    // autopilot/scanner logs) are all time-ordered — turn time, Enter-retry gaps, observer lag, and the
    // span between a [nav] begin and its end read straight off the log. LOCAL time (the user's wall clock)
    // so it lines up with what they saw on screen. (Closes the documented "no timestamp" Known gap.)
    static std::wstring LogTimestampPrefix()
    {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        wchar_t buf[24];
        ::swprintf(buf,
                   24,
                   L"[%02u:%02u:%02u.%03u] ",
                   static_cast<unsigned>(st.wHour),
                   static_cast<unsigned>(st.wMinute),
                   static_cast<unsigned>(st.wSecond),
                   static_cast<unsigned>(st.wMilliseconds));
        return std::wstring{ buf };
    }

    void AppendStateLog(std::wstring_view fileLeaf, std::wstring_view line)
    {
        static std::mutex mtx;
        // Per-file "is the cursor at a fresh line?" so the stamp prefixes only at a line boundary — a
        // (hypothetical) partial-line caller is never split mid-line. Every current caller writes a whole
        // '\n'-terminated record, so in practice each record gets exactly one leading stamp.
        static std::unordered_map<std::wstring, bool> atLineStart;
        std::lock_guard<std::mutex> lk{ mtx };
        try
        {
            const auto path = AgentmasterStateDir() + L"\\" + std::wstring{ fileLeaf };
            std::ofstream f(std::filesystem::path{ path }, std::ios::binary | std::ios::app);
            if (f)
            {
                const std::wstring leaf{ fileLeaf };
                const auto it = atLineStart.find(leaf);
                const bool fresh = (it == atLineStart.end()) || it->second; // first write this run => assume a fresh line
                std::wstring out;
                out.reserve(line.size() + 16);
                if (fresh && !line.empty())
                {
                    out += LogTimestampPrefix();
                }
                out.append(line);
                const auto bytes = Utf16ToUtf8(out);
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                atLineStart[leaf] = !line.empty() && line.back() == L'\n'; // next write starts a line iff this ended one
            }
        }
        catch (...)
        {
        }
    }

    void LogNav(std::wstring_view msg)
    {
        AppendStateLog(L"hooks.log", L"[nav] " + std::wstring{ msg } + L"\n");
    }

    std::wstring ShortId(const std::wstring& id)
    {
        return id.empty() ? std::wstring{ L"(none)" } : id.substr(0, 8);
    }

    std::pair<std::wstring, std::wstring> MaterializeSharedHookFiles(const std::wstring& stateDir, const AppSettings& settings)
    {
        const std::wstring forwarderPath = stateDir + L"\\agentmaster-hook.ps1";
        const std::wstring settingsPath = stateDir + L"\\hooks-settings.json";

        const auto forwarderFwd = ToForwardSlashes(forwarderPath);

        if (!WriteFileUtf8(forwarderPath, BuildForwarderScript(stateDir)))
        {
            return { std::wstring{}, std::wstring{} };
        }
        if (!WriteFileUtf8(settingsPath, BuildHooksSettingsJson(forwarderFwd, settings.model, settings.includeCoAuthoredBy, settings.skipPermissions)))
        {
            return { std::wstring{}, std::wstring{} };
        }
        return { settingsPath, forwarderPath };
    }

    std::wstring ResolveRealClaude()
    {
        const std::wstring path = GetEnvW(L"PATH");
        if (path.empty())
        {
            return {};
        }
        // Try the common Windows launcher extensions, in the order the shim can invoke them
        // most cleanly (.exe runs directly; .cmd/.bat need `call`).
        static const wchar_t* const exts[] = { L".exe", L".cmd", L".bat" };
        size_t start = 0;
        while (start <= path.size())
        {
            size_t sc = path.find(L';', start);
            if (sc == std::wstring::npos)
            {
                sc = path.size();
            }
            std::wstring dir = path.substr(start, sc - start);
            start = sc + 1;
            // Trim surrounding quotes / whitespace, skip empties.
            while (!dir.empty() && (dir.front() == L'"' || dir.front() == L' '))
            {
                dir.erase(dir.begin());
            }
            while (!dir.empty() && (dir.back() == L'"' || dir.back() == L' '))
            {
                dir.pop_back();
            }
            if (dir.empty())
            {
                continue;
            }
            if (dir.back() != L'\\' && dir.back() != L'/')
            {
                dir.push_back(L'\\');
            }
            for (const auto* e : exts)
            {
                const std::wstring cand = dir + L"claude" + e;
                const DWORD attr = ::GetFileAttributesW(cand.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    return cand;
                }
            }
        }
        return {};
    }

    std::wstring ResolveCodexLauncher()
    {
        // Codex analog of ResolveRealClaude — but NOT native-exe-only: the Fleet Observer finds
        // codex.exe as a DESCENDANT of the tab shell (FindDescendantByImage), so a .cmd/.bat that
        // re-execs the native binary is fine. Walk PATH for codex.exe / codex.cmd / codex.bat
        // (per-dir, .exe preferred), then fall back to the native installer's ~/.local/bin\codex.exe.
        // Full path so BuildCodexCommandline can launch it by path (ConPTY's CreateProcessW appends
        // only ".exe" and ignores PATHEXT, so a bare `codex` would miss an npm codex.cmd -> 0x80070002).
        const std::wstring path = GetEnvW(L"PATH");
        static const wchar_t* const exts[] = { L".exe", L".cmd", L".bat" };
        size_t start = 0;
        while (start <= path.size())
        {
            size_t sc = path.find(L';', start);
            if (sc == std::wstring::npos)
            {
                sc = path.size();
            }
            std::wstring dir = path.substr(start, sc - start);
            start = sc + 1;
            // Trim surrounding quotes / whitespace, skip empties.
            while (!dir.empty() && (dir.front() == L'"' || dir.front() == L' '))
            {
                dir.erase(dir.begin());
            }
            while (!dir.empty() && (dir.back() == L'"' || dir.back() == L' '))
            {
                dir.pop_back();
            }
            if (dir.empty())
            {
                continue;
            }
            if (dir.back() != L'\\' && dir.back() != L'/')
            {
                dir.push_back(L'\\');
            }
            for (const auto* e : exts)
            {
                const std::wstring cand = dir + L"codex" + e;
                const DWORD attr = ::GetFileAttributesW(cand.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    return cand;
                }
            }
        }
        // The native installer's default location (~/.local/bin\codex.exe).
        std::wstring home = GetEnvW(L"USERPROFILE");
        if (!home.empty())
        {
            if (home.back() != L'\\' && home.back() != L'/')
            {
                home.push_back(L'\\');
            }
            const std::wstring cand = home + L".local\\bin\\codex.exe";
            const DWORD attr = ::GetFileAttributesW(cand.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                return cand;
            }
        }
        return {};
    }

    std::wstring ResolvePwshLauncher()
    {
        // Prefer pwsh.exe (PowerShell 7) on PATH — mirror ResolveRealClaude's PATH walk.
        const std::wstring path = GetEnvW(L"PATH");
        size_t start = 0;
        while (start <= path.size())
        {
            size_t sc = path.find(L';', start);
            if (sc == std::wstring::npos)
            {
                sc = path.size();
            }
            std::wstring dir = path.substr(start, sc - start);
            start = sc + 1;
            // Trim surrounding quotes / whitespace, skip empties.
            while (!dir.empty() && (dir.front() == L'"' || dir.front() == L' '))
            {
                dir.erase(dir.begin());
            }
            while (!dir.empty() && (dir.back() == L'"' || dir.back() == L' '))
            {
                dir.pop_back();
            }
            if (dir.empty())
            {
                continue;
            }
            if (dir.back() != L'\\' && dir.back() != L'/')
            {
                dir.push_back(L'\\');
            }
            const std::wstring cand = dir + L"pwsh.exe";
            const DWORD attr = ::GetFileAttributesW(cand.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                return cand;
            }
        }
        // Fall back to Windows PowerShell, always present under System32.
        wchar_t sys[MAX_PATH];
        const UINT n = ::GetSystemDirectoryW(sys, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            std::wstring cand{ sys, n };
            if (cand.back() != L'\\' && cand.back() != L'/')
            {
                cand.push_back(L'\\');
            }
            cand += L"WindowsPowerShell\\v1.0\\powershell.exe";
            const DWORD attr = ::GetFileAttributesW(cand.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                return cand;
            }
        }
        return {};
    }

    std::wstring BuildPwshHostedCommandline(std::wstring_view pwshLauncher, std::wstring_view innerCommandline)
    {
        // The pwsh script: invoke the inner command line via the call operator (&) so a QUOTED exe
        // path is EXECUTED (a bare quoted string would just be echoed). The inner keeps its own double
        // quotes — they are literal inside the -EncodedCommand payload (no nested-shell parsing).
        const std::wstring script = L"& " + std::wstring{ innerCommandline };
        // -EncodedCommand wants base64 of the command's UTF-16LE bytes. On Windows wchar_t IS UTF-16LE,
        // so the wstring's raw bytes are exactly that (no BOM).
        const std::string b64 = Base64Encode(reinterpret_cast<const unsigned char*>(script.data()), script.size() * sizeof(wchar_t));
        const std::wstring b64w(b64.begin(), b64.end()); // base64 is pure ASCII -> widen verbatim
        const std::wstring pwsh = pwshLauncher.empty() ? std::wstring{ L"pwsh.exe" } : std::wstring{ pwshLauncher };
        // -NoExit keeps the host alive (drops to an interactive prompt at the ConPTY cwd) AFTER the
        // inner agent exits; -NoLogo suppresses the startup banner so claude/codex paints immediately.
        return L"\"" + pwsh + L"\" -NoLogo -NoExit -EncodedCommand " + b64w;
    }

    namespace
    {
        // True iff `path` exists and is a regular file (not a directory).
        bool FileExistsNotDir(const std::wstring& path)
        {
            const DWORD attr = ::GetFileAttributesW(path.c_str());
            return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        // Case-insensitive ".exe" suffix test (ASCII fold). Pure.
        bool EndsWithExeCI(std::wstring_view s)
        {
            if (s.size() < 4)
            {
                return false;
            }
            std::wstring tail{ s.substr(s.size() - 4) };
            for (auto& c : tail)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return tail == L".exe";
        }

        // Normalize a PATH dir entry: strip surrounding quotes/spaces, ensure a trailing backslash.
        // Returns "" for an empty/blank entry.
        std::wstring NormalizeDir(std::wstring dir)
        {
            while (!dir.empty() && (dir.front() == L'"' || dir.front() == L' '))
            {
                dir.erase(dir.begin());
            }
            while (!dir.empty() && (dir.back() == L'"' || dir.back() == L' '))
            {
                dir.pop_back();
            }
            if (dir.empty())
            {
                return {};
            }
            if (dir.back() != L'\\' && dir.back() != L'/')
            {
                dir.push_back(L'\\');
            }
            return dir;
        }

        // Follow a npm `claude.cmd`/`.bat` in `launcherDir` (already trailing-slash normalized) to the
        // NATIVE binary it ultimately runs: the per-platform optional dependency (or a vendored copy)
        // under <launcherDir>\node_modules\@anthropic-ai\. Bounded + deterministic (known subpaths, then
        // one glob level under @anthropic-ai) — NOT a deep tree walk. Empty if no claude.exe is there
        // (a pure-Node install). [Agentmaster — "a .cmd is OK iff it points to the exe"]
        std::wstring FollowNpmCmdToExe(const std::wstring& launcherDir)
        {
            const std::wstring base = launcherDir + L"node_modules\\@anthropic-ai\\";
            static const wchar_t* const candidates[] = {
                L"claude-code-win32-x64\\claude.exe",
                L"claude-code-win32-arm64\\claude.exe",
                L"claude-code\\vendor\\claude.exe",
                L"claude-code\\claude.exe",
            };
            for (const auto* c : candidates)
            {
                const std::wstring p = base + c;
                if (FileExistsNotDir(p))
                {
                    return p;
                }
            }
            // One glob level: node_modules\@anthropic-ai\*\claude.exe (covers a renamed platform pkg).
            WIN32_FIND_DATAW fd{};
            HANDLE h = ::FindFirstFileW((base + L"*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    {
                        continue;
                    }
                    const std::wstring name = fd.cFileName;
                    if (name == L"." || name == L"..")
                    {
                        continue;
                    }
                    const std::wstring p = base + name + L"\\claude.exe";
                    if (FileExistsNotDir(p))
                    {
                        ::FindClose(h);
                        return p;
                    }
                } while (::FindNextFileW(h, &fd));
                ::FindClose(h);
            }
            return {};
        }
    }

    std::wstring ResolveClaudeExeIn(std::wstring_view overridePath, const std::vector<std::wstring>& pathDirs, std::wstring_view homeDir)
    {
        // 1. Explicit override — must be an existing .exe (an invalid override is "not detected",
        //    so the Settings UI flags it rather than silently auto-detecting around it).
        if (!overridePath.empty())
        {
            std::wstring p{ overridePath };
            return (EndsWithExeCI(p) && FileExistsNotDir(p)) ? p : std::wstring{};
        }

        // 2. claude.exe directly on PATH (EXACT leaf — never a .cmd/.bat).
        for (const auto& raw : pathDirs)
        {
            const std::wstring dir = NormalizeDir(raw);
            if (!dir.empty() && FileExistsNotDir(dir + L"claude.exe"))
            {
                return dir + L"claude.exe";
            }
        }

        // 3. The native installer's default location (~/.local/bin\claude.exe).
        if (!homeDir.empty())
        {
            std::wstring home{ homeDir };
            if (home.back() != L'\\' && home.back() != L'/')
            {
                home.push_back(L'\\');
            }
            const std::wstring cand = home + L".local\\bin\\claude.exe";
            if (FileExistsNotDir(cand))
            {
                return cand;
            }
        }

        // 4. Follow a claude.cmd / claude.bat on PATH to its npm native binary.
        for (const auto& raw : pathDirs)
        {
            const std::wstring dir = NormalizeDir(raw);
            if (dir.empty())
            {
                continue;
            }
            if (FileExistsNotDir(dir + L"claude.cmd") || FileExistsNotDir(dir + L"claude.bat"))
            {
                if (auto p = FollowNpmCmdToExe(dir); !p.empty())
                {
                    return p;
                }
            }
        }

        return {};
    }

    std::wstring ResolveClaudeExe(std::wstring_view overridePath)
    {
        // Gather PATH dirs + USERPROFILE from the environment, then delegate to the testable core.
        std::vector<std::wstring> dirs;
        const std::wstring path = GetEnvW(L"PATH");
        size_t start = 0;
        while (start <= path.size())
        {
            size_t sc = path.find(L';', start);
            if (sc == std::wstring::npos)
            {
                sc = path.size();
            }
            dirs.push_back(path.substr(start, sc - start));
            start = sc + 1;
        }
        return ResolveClaudeExeIn(overridePath, dirs, GetEnvW(L"USERPROFILE"));
    }

    std::wstring MaterializeClaudeShim(const std::wstring& stateDir, const std::wstring& settingsPath)
    {
        // Resolve the real claude FIRST (PATH is still un-mutated here, so this never finds
        // our own shim). If claude isn't installed, there is nothing to shim.
        const std::wstring real = ResolveRealClaude();
        if (real.empty())
        {
            return {};
        }

        const std::wstring shimDir = stateDir + L"\\shim";
        try
        {
            std::filesystem::create_directories(std::filesystem::path{ shimDir });
        }
        catch (...)
        {
            return {};
        }

        // Pick the invocation form by the real launcher's extension (ASCII, case-insensitive).
        std::wstring ext;
        if (const auto dot = real.find_last_of(L'.'); dot != std::wstring::npos)
        {
            ext = real.substr(dot);
            for (auto& c : ext)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
        }
        const bool needsCall = (ext == L".cmd" || ext == L".bat");
        const std::wstring invoke = needsCall ? (L"call \"" + real + L"\"") : (L"\"" + real + L"\"");

        // claude.cmd — covers cmd.exe, PowerShell and pwsh (all honor PATHEXT for .CMD). If
        // the caller already passed --settings, pass straight through (don't double-wire).
        // Detect an existing --settings via a substring replace on a quoted capture of the
        // args (NOT `echo %*|findstr`, which a `&`/`|`/`<`/`>` in the args would break). If
        // the user already passed --settings, run untouched; else prepend ours. (A literal
        // `!` in args is the one delayed-expansion edge we accept for this dev convenience.)
        std::wstring cmd;
        cmd += L"@echo off\r\n";
        cmd += L"setlocal EnableDelayedExpansion\r\n";
        cmd += L"rem Agentmaster managed-claude shim: inject hooks --settings, then run real claude.\r\n";
        cmd += L"set \"AMARGS=%*\"\r\n";
        cmd += L"set \"AMHAS=\"\r\n";
        cmd += L"if defined AMARGS if not \"!AMARGS:--settings=!\"==\"!AMARGS!\" set \"AMHAS=1\"\r\n";
        cmd += L"if defined AMHAS (\r\n";
        cmd += L"  " + invoke + L" %*\r\n";
        cmd += L") else (\r\n";
        cmd += L"  " + invoke + L" --dangerously-skip-permissions --settings \"" + settingsPath + L"\" %*\r\n";
        cmd += L")\r\n";
        WriteFileUtf8(shimDir + L"\\claude.cmd", cmd);

        // Extensionless POSIX `claude` for git-bash / MSYS. cmd/PowerShell ignore it (not on
        // PATHEXT); only a *nix-style shell picks it up. Best-effort (forward-slash paths).
        const std::wstring realFwd = ToForwardSlashes(real);
        const std::wstring settingsFwd = ToForwardSlashes(settingsPath);
        std::wstring sh;
        sh += L"#!/bin/sh\n";
        sh += L"# Agentmaster managed-claude shim: inject hooks --settings, then run real claude.\n";
        sh += L"case \"$*\" in\n";
        sh += L"  *--settings*) exec \"" + realFwd + L"\" \"$@\" ;;\n";
        sh += L"  *) exec \"" + realFwd + L"\" --dangerously-skip-permissions --settings \"" + settingsFwd + L"\" \"$@\" ;;\n";
        sh += L"esac\n";
        WriteFileUtf8(shimDir + L"\\claude", sh);

        return shimDir;
    }

    void WriteBridgeDiscovery(std::wstring_view pipeName)
    {
        const auto dir = AgentmasterStateDir();
        std::wstring json = L"{ \"pid\": ";
        json += std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId()));
        json += L", \"pipe\": \"";
        json += JsonEscape(pipeName);
        json += L"\" }\n";
        WriteFileUtf8(dir + L"\\bridge.json", json);
    }

    namespace
    {
        // Trim ASCII whitespace (incl. CR/LF) from both ends. Shared by the env parser + lexer.
        std::wstring_view EnvTrim(std::wstring_view v)
        {
            size_t a = 0, b = v.size();
            while (a < b && (v[a] == L' ' || v[a] == L'\t' || v[a] == L'\r' || v[a] == L'\n'))
            {
                ++a;
            }
            while (b > a && (v[b - 1] == L' ' || v[b - 1] == L'\t' || v[b - 1] == L'\r' || v[b - 1] == L'\n'))
            {
                --b;
            }
            return v.substr(a, b - a);
        }

        // ASCII upper-fold for case-INsensitive env-name comparison (Windows env names are
        // case-insensitive; env names are conventionally ASCII).
        std::wstring EnvNameFold(std::wstring_view n)
        {
            std::wstring f{ n };
            for (auto& c : f)
            {
                if (c >= L'a' && c <= L'z')
                {
                    c = static_cast<wchar_t>(c - L'a' + L'A');
                }
            }
            return f;
        }

        // A reserved name the spawn owns: CCMGR_* (hook correlation). A user entry that names one is
        // dropped at merge time so it can never clobber the bridge wiring.
        bool IsReservedEnvName(std::wstring_view name)
        {
            return EnvNameFold(name).rfind(L"CCMGR_", 0) == 0;
        }

        // A name Agentmaster sets itself on every connection (ConPTY / Engine), so a user value is
        // ignored — the lexer warns about these (distinct from the dropped-entirely CCMGR_*).
        bool IsAgentmasterOwnedEnvName(std::wstring_view name)
        {
            const std::wstring f = EnvNameFold(name);
            return f == L"AM_SESSION" || f == L"WT_SESSION" || f == L"WT_PROFILE_ID";
        }

        // Env var name rule: [A-Za-z_][A-Za-z0-9_]*  (POSIX-portable, what every shell accepts).
        bool IsValidEnvName(std::wstring_view n)
        {
            if (n.empty())
            {
                return false;
            }
            const wchar_t c0 = n[0];
            if (!((c0 >= L'A' && c0 <= L'Z') || (c0 >= L'a' && c0 <= L'z') || c0 == L'_'))
            {
                return false;
            }
            for (size_t i = 1; i < n.size(); ++i)
            {
                const wchar_t c = n[i];
                if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_'))
                {
                    return false;
                }
            }
            return true;
        }
    }

    std::vector<std::pair<std::wstring, std::wstring>> ParseEnvAssignments(std::wstring_view spec)
    {
        const auto isSep = [](wchar_t c) { return c == L';' || c == L'\n' || c == L'\r'; };

        std::vector<std::pair<std::wstring, std::wstring>> out;
        size_t i = 0;
        while (i <= spec.size())
        {
            size_t sep = i;
            while (sep < spec.size() && !isSep(spec[sep]))
            {
                ++sep;
            }
            const std::wstring_view entry = EnvTrim(spec.substr(i, sep - i));
            // Skip blanks + '#' comments (a "#FOO=bar" line must NOT spawn a var named "#FOO").
            if (!entry.empty() && entry.front() != L'#')
            {
                const size_t eq = entry.find(L'=');
                if (eq != std::wstring_view::npos)
                {
                    const std::wstring_view name = EnvTrim(entry.substr(0, eq));
                    const std::wstring_view value = EnvTrim(entry.substr(eq + 1));
                    if (!name.empty())
                    {
                        out.emplace_back(std::wstring{ name }, std::wstring{ value });
                    }
                }
            }
            if (sep >= spec.size())
            {
                break;
            }
            i = sep + 1;
        }
        return out;
    }

    std::vector<std::pair<std::wstring, std::wstring>> MergeSessionEnv(std::wstring_view globalEnv, std::wstring_view perDirEnv)
    {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        std::unordered_map<std::wstring, size_t> indexByFold; // fold(NAME) -> position in `out`
        const auto apply = [&](const std::vector<std::pair<std::wstring, std::wstring>>& src) {
            for (const auto& [name, value] : src)
            {
                if (IsReservedEnvName(name))
                {
                    continue; // CCMGR_* is the spawn's — never user-overridable
                }
                const std::wstring fold = EnvNameFold(name);
                const auto it = indexByFold.find(fold);
                if (it == indexByFold.end())
                {
                    indexByFold.emplace(fold, out.size());
                    out.emplace_back(name, value);
                }
                else
                {
                    out[it->second].second = value; // last writer wins; keep the first-seen NAME spelling + position
                }
            }
        };
        apply(ParseEnvAssignments(globalEnv)); // base
        apply(ParseEnvAssignments(perDirEnv)); // per-dir overrides the global same-named entry
        return out;
    }

    std::vector<std::pair<std::wstring, std::wstring>> ResolveSessionEnv(const AppSettings& settings, std::wstring_view workingDir)
    {
        return MergeSessionEnv(settings.env, GetDirEnv(std::wstring{ workingDir }));
    }

    std::pair<std::wstring, uint32_t> ApplyEnvDefaults(std::wstring_view envText, uint32_t seededVersion)
    {
        struct Default
        {
            uint32_t introVersion;
            const wchar_t* name;
            const wchar_t* value;
        };
        // The shipped global env defaults, in introduction order. Bump kEnvDefaultsVersion (ClaudeSpawn.h)
        // and give a new entry introVersion = the new version when adding one.
        static const Default kDefaults[] = {
            { 1, L"CLAUDE_CODE_MAX_RETRIES", L"50000" },
        };

        // Names already present in the editor (case-insensitive — Windows env semantics, the same fold
        // MergeSessionEnv uses) are never duplicated, so a user value / a prior seed always wins.
        const auto existing = ParseEnvAssignments(envText);
        const auto hasName = [&](const wchar_t* name) {
            const std::wstring fold = EnvNameFold(name);
            for (const auto& [k, v] : existing)
            {
                if (EnvNameFold(k) == fold)
                {
                    return true;
                }
            }
            return false;
        };

        std::wstring out{ envText };
        for (const auto& d : kDefaults)
        {
            if (d.introVersion <= seededVersion)
            {
                continue; // considered in a prior seed (a deletion since then must stick)
            }
            if (hasName(d.name))
            {
                continue; // user already has this NAME (or a prior seed added it)
            }
            if (!out.empty() && out.back() != L'\n')
            {
                out += L'\n'; // each default on its own line (the multi-line editor format)
            }
            out += d.name;
            out += L'=';
            out += d.value;
        }
        // We've now considered every default up through kEnvDefaultsVersion.
        const uint32_t newVersion = seededVersion < kEnvDefaultsVersion ? kEnvDefaultsVersion : seededVersion;
        return { out, newVersion };
    }

    EnvLexResult LexEnvText(std::wstring_view text)
    {
        EnvLexResult r;
        std::unordered_map<std::wstring, uint32_t> seen; // fold(NAME) of prior Ok lines -> their line no
        uint32_t lineNo = 0;
        size_t i = 0;
        const auto note = [&](EnvLineKind k, const std::wstring& msg, uint32_t at) {
            if ((k == EnvLineKind::Warn || k == EnvLineKind::Error) && r.firstIssueLine == 0)
            {
                r.firstIssueLine = at;
                r.firstIssue = msg;
            }
            if (k == EnvLineKind::Error)
            {
                ++r.error;
                if (r.worst != EnvLineKind::Error)
                {
                    r.worst = EnvLineKind::Error;
                }
            }
            else if (k == EnvLineKind::Warn)
            {
                ++r.warn;
                if (r.worst == EnvLineKind::Ok)
                {
                    r.worst = EnvLineKind::Warn;
                }
            }
            else if (k == EnvLineKind::Ok)
            {
                ++r.ok;
            }
        };

        // Walk lines (split on '\n'; a trailing '\r' is trimmed by EnvTrim). A lone ';'-delimited
        // legacy string is one "line" here — fine, it still lexes each entry below only if it has '\n';
        // for the multi-line editor (the only LexEnvText caller) every entry is its own line.
        while (i <= text.size())
        {
            size_t nl = text.find(L'\n', i);
            const size_t end = (nl == std::wstring_view::npos) ? text.size() : nl;
            ++lineNo;
            const std::wstring_view raw = text.substr(i, end - i);
            const std::wstring_view entry = EnvTrim(raw);
            EnvLineDiag d;
            d.line = lineNo;
            if (entry.empty() || entry.front() == L'#')
            {
                d.kind = EnvLineKind::Ignored;
            }
            else
            {
                const size_t eq = entry.find(L'=');
                if (eq == std::wstring_view::npos)
                {
                    d.kind = EnvLineKind::Error;
                    d.message = L"missing '=' (expected NAME=VALUE)";
                }
                else
                {
                    const std::wstring name{ EnvTrim(entry.substr(0, eq)) };
                    d.name = name;
                    if (name.empty())
                    {
                        d.kind = EnvLineKind::Error;
                        d.message = L"empty variable name";
                    }
                    else if (!IsValidEnvName(name))
                    {
                        d.kind = EnvLineKind::Error;
                        d.message = L"invalid name \"" + name + L"\" (use letters, digits, _ ; not starting with a digit)";
                    }
                    else if (IsReservedEnvName(name))
                    {
                        d.kind = EnvLineKind::Warn;
                        d.message = L"\"" + name + L"\" is reserved (ignored)";
                    }
                    else if (IsAgentmasterOwnedEnvName(name))
                    {
                        d.kind = EnvLineKind::Warn;
                        d.message = L"\"" + name + L"\" is set by Agentmaster (ignored)";
                    }
                    else if (const auto it = seen.find(EnvNameFold(name)); it != seen.end())
                    {
                        d.kind = EnvLineKind::Warn;
                        d.message = L"duplicate of line " + std::to_wstring(it->second) + L" (last value wins)";
                    }
                    else
                    {
                        d.kind = EnvLineKind::Ok;
                        seen.emplace(EnvNameFold(name), lineNo);
                    }
                }
            }
            note(d.kind, d.message, d.line);
            r.lines.push_back(std::move(d));
            if (nl == std::wstring_view::npos)
            {
                break;
            }
            i = nl + 1;
        }
        return r;
    }

    // The child-env block shared by every managed-Claude spawn — fresh launch, resume, fork, AND the
    // in-place RESTART rebuild (BuildClaudeRestartSpec). CCMGR_SESSION_ID + CCMGR_HOOK_PIPE drive hook
    // correlation; the cog's global env is layered on top but can NEVER clobber the CCMGR_* vars (a stray
    // user entry that begins CCMGR_ is skipped). Factored out so the launch and restart specs produce a
    // byte-identical env.
    static void AppendManagedClaudeEnv(ClaudeSpawnSpec& spec, const AppSettings& settings)
    {
        spec.env.emplace_back(L"CCMGR_SESSION_ID", spec.sessionId);
        spec.env.emplace_back(L"CCMGR_HOOK_PIPE", spec.pipeName);
        // The global env (settings.env) merged with this dir's per-dir overrides (dir-env.json) — per-dir
        // wins, CCMGR_* dropped (ResolveSessionEnv handles both), applied AFTER the CCMGR_* vars so the
        // hook correlation can never be clobbered.
        for (auto& kv : ResolveSessionEnv(settings, spec.workingDir))
        {
            spec.env.emplace_back(std::move(kv.first), std::move(kv.second));
        }
    }

    ClaudeSpawnSpec BuildClaudeSpawn(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName, std::wstring_view resumeSessionId, const AppSettings& settings, std::wstring_view forkFromSessionId, std::wstring_view claudeLauncher, std::wstring_view forkIntoSessionId)
    {
        ClaudeSpawnSpec spec;
        spec.workingDir = std::wstring{ workingDir };
        spec.title = std::wstring{ title };
        spec.pipeName = std::wstring{ pipeName };

        // Fork wins over resume (mutually exclusive). A fork normally mints a FRESH id (the fork target —
        // the commandline resumes the source but --fork-session writes to this new id); a resume reuses
        // the given id; a fresh launch mints a new one. A RESTORE re-fork passes forkIntoSessionId — the
        // fork's EXISTING id — so it forks back into the same id (re-materializing a never-messaged fork
        // whose own transcript was never written, preserving its identity across the restart instead of
        // churning a new id); collision-free since that id is unused on disk. forkIntoSessionId is honored
        // only when forking.
        const bool fork = !forkFromSessionId.empty();
        const bool resume = !fork && !resumeSessionId.empty();
        spec.sessionId = resume ? std::wstring{ resumeSessionId } :
            (fork && !forkIntoSessionId.empty()) ? std::wstring{ forkIntoSessionId } :
                                                   NewSessionId();

        const auto stateDir = AgentmasterStateDir();
        auto [settingsPath, forwarderPath] = MaterializeSharedHookFiles(stateDir, settings);
        spec.settingsPath = settingsPath;
        spec.forwarderPath = forwarderPath;

        const auto settingsFwd = ToForwardSlashes(settingsPath);
        spec.commandline = BuildClaudeCommandline(settingsFwd, spec.sessionId, resume, settings.skipPermissions, forkFromSessionId, claudeLauncher);

        // The cog's global env + the hook-correlation vars, applied to every session (CCMGR_* always win).
        AppendManagedClaudeEnv(spec, settings);
        return spec;
    }

    ClaudeSpawnSpec BuildClaudeRestartSpec(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName, std::wstring_view sessionId, const AppSettings& settings, std::wstring_view claudeLauncher)
    {
        // Relaunch an EXISTING managed conversation IN PLACE (the tab's connection died and the user hit
        // "Restart session"). Unlike BuildClaudeSpawn this NEVER mints a new id and NEVER forks:
        //   * a transcript exists -> RESUME it (claude --resume <id>), continuing the conversation;
        //   * no transcript yet    -> a FRESH launch that REUSES <id> (claude --session-id <id>). A
        //     never-prompted / early-crashed session wrote no transcript, so reusing the id is a legit
        //     fresh start with no "session id already in use" collision — and keeping the id holds the
        //     registry / tab / injector binding stable across the restart (no re-key).
        //
        // This is the whole point of the rewrite: the ORIGINAL launch commandline must NOT be replayed.
        // A fresh launch's stored commandline is `--session-id <id>` (NOT --resume), which after the first
        // turn collides with the now-existing transcript (claude refuses an in-use id -> the restart dies
        // immediately); a fork's is `--resume <parent> --fork-session`, which would re-fork from the
        // parent into a since-grown id. Deriving the command from the CURRENT transcript state fixes both.
        ClaudeSpawnSpec spec;
        spec.workingDir = std::wstring{ workingDir };
        spec.title = std::wstring{ title };
        spec.pipeName = std::wstring{ pipeName };
        spec.sessionId = std::wstring{ sessionId }; // ALWAYS keep the conversation id (no re-key)

        const auto stateDir = AgentmasterStateDir();
        auto [settingsPath, forwarderPath] = MaterializeSharedHookFiles(stateDir, settings);
        spec.settingsPath = settingsPath;
        spec.forwarderPath = forwarderPath;

        const bool resume = ClaudeConversationExists(spec.sessionId);
        const auto settingsFwd = ToForwardSlashes(settingsPath);
        // forkFromSessionId is empty: a restart resumes or starts fresh, it never forks.
        spec.commandline = BuildClaudeCommandline(settingsFwd, spec.sessionId, resume, settings.skipPermissions, {}, claudeLauncher);

        AppendManagedClaudeEnv(spec, settings);
        return spec;
    }
}
