// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

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

    std::wstring BuildForwarderScript()
    {
        // Pure-ASCII PowerShell. Reads the hook JSON from stdin, correlates via env, and
        // posts one HookWire.h line to the named pipe. Best-effort throughout: any failure
        // is swallowed so a hook never breaks the Claude turn.
        return LR"PSHOOK(param([string]$Event = "")
$ErrorActionPreference = "SilentlyContinue"
try {
  $raw = ""
  try { $raw = [Console]::In.ReadToEnd() } catch { }

  $j = $null
  if (-not [string]::IsNullOrEmpty($raw)) {
    try { $j = $raw | ConvertFrom-Json } catch { $j = $null }
  }

  # Session id: prefer the env we inject at Launch; otherwise take it from the hook payload
  # so a session we did NOT launch (a hand-typed `claude` in a `+` tab) still correlates.
  $sid = $env:CCMGR_SESSION_ID
  if ([string]::IsNullOrEmpty($sid) -and $j -ne $null -and $j.session_id) { $sid = [string]$j.session_id }
  if ([string]::IsNullOrEmpty($sid)) { return }

  # Pipe: prefer the inherited env; otherwise the bridge discovery file (covers a shell that
  # did not inherit CCMGR_HOOK_PIPE).
  $pipe = $env:CCMGR_HOOK_PIPE
  if ([string]::IsNullOrEmpty($pipe)) {
    try {
      $disc = Join-Path $env:USERPROFILE ".agentmaster\bridge.json"
      if (Test-Path -LiteralPath $disc) {
        $b = (Get-Content -LiteralPath $disc -Raw) | ConvertFrom-Json
        if ($b.pipe) { $pipe = [string]$b.pipe }
      }
    } catch { }
  }
  if ([string]::IsNullOrEmpty($pipe)) { return }

  $cwd = ""
  $isQ = "0"
  $perm = "0"
  $tool = ""
  if ($j -ne $null) {
    if ($j.cwd) { $cwd = [string]$j.cwd }
    if ($j.tool_name) { $tool = [string]$j.tool_name }
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

  $line = ($Event, $sid, $cwd, $isQ, $perm, $tool, $tab) -join "`t"

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
} catch { }
)PSHOOK";
    }

    std::wstring BuildHooksSettingsJson(std::wstring_view forwarderPath)
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
        json += L"  }\n}\n";
        return json;
    }

    std::wstring BuildClaudeCommandline(std::wstring_view settingsPath, std::wstring_view sessionId, bool resume)
    {
        // Agentmaster: every spawned session runs with --dangerously-skip-permissions. The
        // app drives claude programmatically (Autopilot + injected prompts) and gates risky
        // actions through its own Approval Policy, so the per-tool permission prompts are
        // redundant here. Critically, permission mode `bypassPermissions` ALSO skips the
        // per-folder "Do you trust the files in this folder?" trust dialog at startup (the
        // dialog block is gated on `mode !== "bypassPermissions"`), which would otherwise
        // wedge an unattended ConPTY session waiting on a keypress. It does NOT suppress the
        // one-time GLOBAL "Bypass Permissions mode" acceptance (~/.claude.json
        // `bypassPermissionsModeAccepted`) — that shows once, ever, until accepted.
        if (resume)
        {
            // Resume the existing conversation by id; --resume implies the session id.
            std::wstring cmd = L"claude --dangerously-skip-permissions --resume ";
            cmd += sessionId;
            cmd += L" --settings \"";
            cmd += settingsPath;
            cmd += L"\"";
            return cmd;
        }
        std::wstring cmd = L"claude --dangerously-skip-permissions --settings \"";
        cmd += settingsPath;
        cmd += L"\" --session-id ";
        cmd += sessionId;
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

    bool ClaudeConversationExists(std::wstring_view sessionId)
    {
        if (sessionId.empty())
        {
            return false;
        }
        // Claude stores transcripts under <config>/projects/<encoded-cwd>/<session-id>.jsonl.
        // The config dir is CLAUDE_CONFIG_DIR if set, else ~/.claude.
        std::wstring base = GetEnvW(L"CLAUDE_CONFIG_DIR");
        if (base.empty())
        {
            const std::wstring home = GetEnvW(L"USERPROFILE");
            if (home.empty())
            {
                return false;
            }
            base = home + L"\\.claude";
        }
        const std::wstring projects = base + L"\\projects";
        const std::wstring leaf = std::wstring{ sessionId } + L".jsonl";

        // Session ids are unique UUIDs, so rather than reproduce Claude's cwd->dir encoding we
        // just look for <session-id>.jsonl inside any project directory.
        const std::wstring pattern = projects + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        bool found = false;
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
                found = true;
                break;
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
        return found;
    }

    std::wstring AgentmasterStateDir()
    {
        // CRITICAL: this path must resolve to the SAME real location for both this app
        // (which may be MSIX-packaged) AND the EXTERNAL claude.exe we spawn — the spawn
        // passes this path to `claude --settings` and the hook forwarder script lives here.
        // A packaged app's %LOCALAPPDATA% is redirected to the package's LocalCache, so a
        // path string under %LOCALAPPDATA% would point an external process at an empty real
        // folder. %USERPROFILE% is NOT redirected, so anchor under it (mirrors ~/.claude).
        std::wstring base = GetEnvW(L"USERPROFILE");
        std::wstring dir;
        if (!base.empty())
        {
            dir = base + L"\\.agentmaster";
        }
        else
        {
            std::wstring fallback = GetEnvW(L"LOCALAPPDATA");
            if (fallback.empty())
            {
                fallback = GetEnvW(L"TEMP");
            }
            if (fallback.empty())
            {
                fallback = L".";
            }
            dir = fallback + L"\\Agentmaster";
        }
        try
        {
            std::filesystem::create_directories(std::filesystem::path{ dir });
        }
        catch (...)
        {
        }
        return dir;
    }

    void AppendStateLog(std::wstring_view fileLeaf, std::wstring_view line)
    {
        static std::mutex mtx;
        std::lock_guard<std::mutex> lk{ mtx };
        try
        {
            const auto path = AgentmasterStateDir() + L"\\" + std::wstring{ fileLeaf };
            std::ofstream f(std::filesystem::path{ path }, std::ios::binary | std::ios::app);
            if (f)
            {
                const auto bytes = Utf16ToUtf8(line);
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
        }
        catch (...)
        {
        }
    }

    std::pair<std::wstring, std::wstring> MaterializeSharedHookFiles(const std::wstring& stateDir)
    {
        const std::wstring forwarderPath = stateDir + L"\\agentmaster-hook.ps1";
        const std::wstring settingsPath = stateDir + L"\\hooks-settings.json";

        const auto forwarderFwd = ToForwardSlashes(forwarderPath);

        if (!WriteFileUtf8(forwarderPath, BuildForwarderScript()))
        {
            return { std::wstring{}, std::wstring{} };
        }
        if (!WriteFileUtf8(settingsPath, BuildHooksSettingsJson(forwarderFwd)))
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

    ClaudeSpawnSpec BuildClaudeSpawn(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName, std::wstring_view resumeSessionId)
    {
        ClaudeSpawnSpec spec;
        spec.workingDir = std::wstring{ workingDir };
        spec.title = std::wstring{ title };
        spec.pipeName = std::wstring{ pipeName };

        const bool resume = !resumeSessionId.empty();
        spec.sessionId = resume ? std::wstring{ resumeSessionId } : NewSessionId();

        const auto stateDir = AgentmasterStateDir();
        auto [settingsPath, forwarderPath] = MaterializeSharedHookFiles(stateDir);
        spec.settingsPath = settingsPath;
        spec.forwarderPath = forwarderPath;

        const auto settingsFwd = ToForwardSlashes(settingsPath);
        spec.commandline = BuildClaudeCommandline(settingsFwd, spec.sessionId, resume);

        spec.env.emplace_back(L"CCMGR_SESSION_ID", spec.sessionId);
        spec.env.emplace_back(L"CCMGR_HOOK_PIPE", spec.pipeName);
        return spec;
    }
}
