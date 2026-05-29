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
  $sid  = $env:CCMGR_SESSION_ID
  $pipe = $env:CCMGR_HOOK_PIPE
  if ([string]::IsNullOrEmpty($sid) -or [string]::IsNullOrEmpty($pipe)) { return }

  $raw = ""
  try { $raw = [Console]::In.ReadToEnd() } catch { }

  $cwd = ""
  $isQ = "0"
  $perm = "0"
  $tool = ""
  if (-not [string]::IsNullOrEmpty($raw)) {
    try {
      $j = $raw | ConvertFrom-Json
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
    } catch { }
  }

  $server = "."
  $name = $pipe
  $bs = $pipe.LastIndexOf("\")
  if ($bs -ge 0) { $name = $pipe.Substring($bs + 1) }

  $line = ($Event, $sid, $cwd, $isQ, $perm, $tool) -join "`t"

  $client = New-Object System.IO.Pipes.NamedPipeClientStream($server, $name, [System.IO.Pipes.PipeDirection]::Out)
  try {
    $client.Connect(2000)
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

    std::wstring BuildClaudeCommandline(std::wstring_view settingsPath, std::wstring_view sessionId)
    {
        std::wstring cmd = L"claude --settings \"";
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

    std::wstring AgentmasterStateDir()
    {
        std::wstring base = GetEnvW(L"LOCALAPPDATA");
        if (base.empty())
        {
            base = GetEnvW(L"TEMP");
        }
        if (base.empty())
        {
            base = L".";
        }
        std::wstring dir = base + L"\\Agentmaster";
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

    ClaudeSpawnSpec BuildClaudeSpawn(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName)
    {
        ClaudeSpawnSpec spec;
        spec.workingDir = std::wstring{ workingDir };
        spec.title = std::wstring{ title };
        spec.pipeName = std::wstring{ pipeName };
        spec.sessionId = NewSessionId();

        const auto stateDir = AgentmasterStateDir();
        auto [settingsPath, forwarderPath] = MaterializeSharedHookFiles(stateDir);
        spec.settingsPath = settingsPath;
        spec.forwarderPath = forwarderPath;

        const auto settingsFwd = ToForwardSlashes(settingsPath);
        spec.commandline = BuildClaudeCommandline(settingsFwd, spec.sessionId);

        spec.env.emplace_back(L"CCMGR_SESSION_ID", spec.sessionId);
        spec.env.emplace_back(L"CCMGR_HOOK_PIPE", spec.pipeName);
        return spec;
    }
}
