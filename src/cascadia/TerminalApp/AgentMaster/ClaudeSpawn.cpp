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

#include "Json.h" // the workspace-trust splice validates ~/.claude.json through this (never reprints it)
#include "PendingPaste.h" // PENDING_INPUT.md §2b — the pure paste-cache marker resolver (adapter below)
#include "Persistence.h" // GetDirEnv (the per-directory env overrides; ResolveSessionEnv reads it)
#include "ProcessInspect.h" // SnapshotProcesses / FindDescendantByImage / ReadProcessCwd (moved here)
#include "ProfileBootstrap.h" // the per-install state PROFILE (AgentmasterStateDir now resolves through it)
#include "RegexUtil.h" // COMMANDS.md §6b — the successor-title regex rewrite (DeriveHandoverSuccessorTitle)
#include "Sha256.h" // the shipped-command-definition version history is a list of SHA-256 digests
#include "TranscriptStore.h" // FindGitRootForDir (the workspace-trust key is the enclosing repo)

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

    // Read a whole UTF-8 file as UTF-16. "" for a missing/unreadable/empty file — every caller here
    // treats "no text" as "nothing to do", never as "rebuild it" (Rule #16 / the no-clobber rule).
    std::wstring ReadFileUtf8(const std::wstring& path)
    {
        try
        {
            std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
            if (!f)
            {
                return {};
            }
            const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (bytes.empty())
            {
                return {};
            }
            const int needed = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (needed <= 0)
            {
                return {};
            }
            std::wstring out(static_cast<size_t>(needed), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), needed);
            return out;
        }
        catch (...)
        {
            // Same non-recursion argument as WriteFileUtf8 below (this helper is off the logging path).
            Agentmaster::LogSwallowedException(L"ReadFileUtf8"); // qualified: file-scope anonymous namespace
            return {};
        }
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
            // Forensics: a failed hook-config / shim / handover-definition write is state silently NOT
            // written (the caller's [persist-fail] line says WHICH file; this says WHAT was thrown and
            // from where). Safe to log here — this helper is NOT on the logging path (AppendStateLog
            // writes via its own ofstream, never through WriteFileUtf8), so there is no recursion.
            Agentmaster::LogSwallowedException(L"WriteFileUtf8"); // qualified: this helper sits in the file-scope anonymous namespace
            return false;
        }
    }

    // ATOMIC variant, for a file whose PARTIALLY-written content would be worse than no write at
    // all: write a temp sibling, flush it, then MoveFileExW(REPLACE_EXISTING) over the target — the
    // rename is atomic, so any reader (another install's engine init, or Claude Code enumerating
    // commands) sees either the OLD or the NEW complete file, never a truncated one.
    //
    // WHY the shipped slash-command DEFINITIONS need it (COMMANDS.md §6): their identity IS their
    // bytes. A trunc-write that dies after truncating (disk full, I/O error, a crash mid-write) —
    // or a concurrent reader catching the window — leaves bytes that match NO shipped digest, so
    // the file reads as USER-OWNED from then on and is NEVER repaired: a permanently broken
    // /handover, silently. The temp+rename removes that state entirely, and a FAILED rename (target
    // locked by an editor, read-only) leaves the ORIGINAL definition intact instead of a stub.
    // (Updater.h's WriteUpdateState learned the same lesson — "was a torn-file-prone trunc ofstream".)
    // The temp leaf deliberately does NOT end in .md, so a leftover from a killed process can never
    // be picked up as a command.
    bool WriteFileUtf8Atomic(const std::wstring& path, std::wstring_view content)
    {
        try
        {
            const std::wstring tmp = path + L".am-tmp";
            {
                std::ofstream f(std::filesystem::path{ tmp }, std::ios::binary | std::ios::trunc);
                if (!f)
                {
                    return false;
                }
                const auto bytes = Utf16ToUtf8(content);
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                f.flush();
                if (!f.good())
                {
                    f.close();
                    ::DeleteFileW(tmp.c_str()); // never leave a half-written temp behind
                    return false;
                }
            } // closed before the rename — MoveFileExW cannot replace through an open handle
            if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                ::DeleteFileW(tmp.c_str()); // target locked / read-only — the ORIGINAL stays valid
                return false;
            }
            return true;
        }
        catch (...)
        {
            // Same forensics + same non-recursion argument as WriteFileUtf8 above.
            Agentmaster::LogSwallowedException(L"WriteFileUtf8Atomic"); // qualified: file-scope anonymous namespace
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

    std::wstring PsDoubleQuote(std::wstring_view s)
    {
        // PowerShell DOUBLE-quoted argument: inside "…" PS interprets exactly three characters —
        // the backtick (its escape char), the double quote (ends the string), and $ (variable /
        // subexpression expansion). Backtick-escape each; everything else (backslashes included)
        // is inert. Used for the initial-prompt positional arg, whose text embeds a filesystem
        // path we do not control (COMMANDS.md).
        std::wstring out;
        out.reserve(s.size() + 2);
        out.push_back(L'"');
        for (const wchar_t c : s)
        {
            if (c == L'`' || c == L'"' || c == L'$')
            {
                out.push_back(L'`');
            }
            out.push_back(c);
        }
        out.push_back(L'"');
        return out;
    }

    size_t PsEscapedCost(std::wstring_view text)
    {
        // The cost model MUST mirror PsDoubleQuote above: exactly ` " $ double (the backtick
        // escape), everything else is one char. Tested against PsDoubleQuote so they can't drift.
        size_t total = 0;
        for (const wchar_t c : text)
        {
            total += (c == L'`' || c == L'"' || c == L'$') ? 2u : 1u;
        }
        return total;
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
    # The submitted prompt text, so the Manager's Auto Testing reflects EVERY message the
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

    std::wstring BuildClaudeCommandline(std::wstring_view settingsPath, std::wstring_view sessionId, bool resume, bool skipPermissions, std::wstring_view forkFromSessionId, std::wstring_view claudeLauncher, std::wstring_view modelOverride, std::wstring_view initialPrompt)
    {
        // When skipPermissions is ON (the cog default), spawn with --dangerously-skip-permissions:
        // the app drives claude programmatically (Autorunner + injected prompts) and gates risky
        // actions through its own Approval Policy, so the per-tool permission prompts are
        // redundant. It does NOT suppress the one-time GLOBAL "Bypass Permissions mode" acceptance
        // (~/.claude.json `bypassPermissionsModeAccepted`).
        // ⚠ Nor does it suppress the startup WORKSPACE-TRUST dialog. This comment used to claim the
        // opposite ("the dialog block is gated on mode !== bypassPermissions") — that is WRONG on
        // 2.1.x and was proven false by launching claude with the flag in an untrusted directory on
        // a real PTY: the dialog appeared. Claude's trust gate reads only CLAUDE_CODE_SANDBOXED, the
        // in-memory session flag, background-agent mode, and the persisted per-project
        // hasTrustDialogAccepted — never the permission mode. EnsureClaudeWorkspaceTrusted (called
        // from the spawn prelude) is what keeps that dialog off an unattended ConPTY session.
        // When OFF, the flag is omitted and BuildHooksSettingsJson pins permissions.defaultMode
        // instead (normal prompts apply).
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

        // Launch-model picker (Agentmaster): the per-LAUNCH model from an "Open New Session Here"
        // submenu. Appended LAST so every form (fresh / resume / fork) carries it uniformly; a CLI
        // flag outranks the shared settings file's `model`, so only THIS session is affected.
        // Model ids are plain tokens (claude-opus-4-8), but a user-typed value could carry spaces —
        // quote only then (an always-quote would churn the cmd /c outer-quote wrap for nothing).
        if (!modelOverride.empty())
        {
            const bool needsQuotes = modelOverride.find_first_of(L" \t") != std::wstring_view::npos;
            cmd += needsQuotes ? (L" --model \"" + std::wstring{ modelOverride } + L"\"") :
                                 (L" --model " + std::wstring{ modelOverride });
        }

        // Initial prompt (Agentmaster, COMMANDS.md — the /handover successor): the trailing
        // POSITIONAL prompt claude submits as the session's first turn on startup. Appended LAST
        // (claude's arg parser takes the positional after any flags), PS-double-quoted — the
        // managed commandline is invoked by the pwsh host's `&` operator, so PowerShell parsing
        // governs it (a claude launch is never the `cmd /c` batch form below: the native-exe-only
        // policy resolves a real claude.exe, and the empty-launcher fallback is a bare token).
        if (!initialPrompt.empty())
        {
            cmd += L" " + PsDoubleQuote(initialPrompt);
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

    std::wstring ClaudePasteCacheDir()
    {
        // <CLAUDE_CONFIG_DIR | ~/.claude>\paste-cache — the same base resolution as
        // ClaudeProjectsDir (its sibling). Claude spills a large paste here AT PASTE TIME
        // (content-addressed <16-hex>.txt), which is what makes a draft's paste placeholder
        // resolvable to real content out-of-band (PENDING_INPUT.md §2b).
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
        return base + L"\\paste-cache";
    }

    // ---- Workspace trust (ClaudeSpawn.h) ----------------------------------------------------
    //
    // A small JSON-aware TEXT scanner, not a re-serializer: we must set one boolean inside the
    // user's real ~/.claude.json (their oauth account, 60+ project entries, float cost/fps stats)
    // without disturbing a single other byte. Json.h is used only to VALIDATE (before and after)
    // and to escape the key — never to reprint the file.

    // `i` sits on the opening quote of a JSON string; returns the index just past its closing
    // quote (honoring \\ escapes), or npos if unterminated.
    static size_t JsonSkipString(const std::wstring& t, size_t i)
    {
        if (i >= t.size() || t[i] != L'"')
        {
            return std::wstring::npos;
        }
        for (++i; i < t.size(); ++i)
        {
            if (t[i] == L'\\')
            {
                ++i; // the escaped char is consumed with it (a trailing '\' falls out of the loop)
                continue;
            }
            if (t[i] == L'"')
            {
                return i + 1;
            }
        }
        return std::wstring::npos;
    }

    static size_t JsonSkipWs(const std::wstring& t, size_t i)
    {
        while (i < t.size() && (t[i] == L' ' || t[i] == L'\t' || t[i] == L'\r' || t[i] == L'\n'))
        {
            ++i;
        }
        return i;
    }

    // `i` sits on the first char of a JSON value; returns the index just past it. Strings are
    // skipped as a unit (so braces/brackets INSIDE a string can never unbalance the walk).
    static size_t JsonSkipValue(const std::wstring& t, size_t i)
    {
        i = JsonSkipWs(t, i);
        if (i >= t.size())
        {
            return std::wstring::npos;
        }
        if (t[i] == L'"')
        {
            return JsonSkipString(t, i);
        }
        if (t[i] == L'{' || t[i] == L'[')
        {
            int depth = 0;
            for (; i < t.size(); ++i)
            {
                if (t[i] == L'"')
                {
                    const size_t after = JsonSkipString(t, i);
                    if (after == std::wstring::npos)
                    {
                        return std::wstring::npos;
                    }
                    i = after - 1;
                    continue;
                }
                if (t[i] == L'{' || t[i] == L'[')
                {
                    ++depth;
                }
                else if (t[i] == L'}' || t[i] == L']')
                {
                    if (--depth == 0)
                    {
                        return i + 1;
                    }
                }
            }
            return std::wstring::npos;
        }
        // A literal (true/false/null) or a number: run to the next structural character.
        const size_t end = t.find_first_of(L",}] \t\r\n", i);
        return end == std::wstring::npos ? t.size() : end;
    }

    struct JsonMemberSpan
    {
        bool found{ false };
        size_t valueBegin{ 0 }; // first char of the member's VALUE
        size_t valueEnd{ 0 }; // one past the value
    };

    // Find member `key` in the object whose '{' is at `braceIdx`. Only that object's OWN members
    // are considered (nested objects are skipped as values).
    static JsonMemberSpan JsonFindMember(const std::wstring& t, size_t braceIdx, const std::wstring& key)
    {
        JsonMemberSpan r;
        if (braceIdx >= t.size() || t[braceIdx] != L'{')
        {
            return r;
        }
        size_t i = JsonSkipWs(t, braceIdx + 1);
        if (i < t.size() && t[i] == L'}')
        {
            return r; // empty object
        }
        while (i < t.size())
        {
            if (t[i] != L'"')
            {
                return {}; // malformed — refuse rather than guess
            }
            const size_t keyEnd = JsonSkipString(t, i);
            if (keyEnd == std::wstring::npos)
            {
                return {};
            }
            // The member name as PARSED (so an escaped key still compares correctly).
            const auto parsedKey = json::Parse(t.substr(i, keyEnd - i));
            i = JsonSkipWs(t, keyEnd);
            if (i >= t.size() || t[i] != L':')
            {
                return {};
            }
            const size_t valueBegin = JsonSkipWs(t, i + 1);
            const size_t valueEnd = JsonSkipValue(t, valueBegin);
            if (valueEnd == std::wstring::npos)
            {
                return {};
            }
            if (parsedKey && parsedKey->type == json::Value::Type::Str && parsedKey->str == key)
            {
                r.found = true;
                r.valueBegin = valueBegin;
                r.valueEnd = valueEnd;
                return r;
            }
            i = JsonSkipWs(t, valueEnd);
            if (i < t.size() && t[i] == L',')
            {
                i = JsonSkipWs(t, i + 1);
                continue;
            }
            return r; // '}' or malformed — the key is simply absent
        }
        return r;
    }

    // The whitespace run that separates an object's '{' from its first member — reused verbatim as
    // the prefix of an inserted member so a pretty-printed file stays pretty. Empty for a compact
    // or empty object (the insert is then compact too).
    static std::wstring JsonMemberIndent(const std::wstring& t, size_t braceIdx)
    {
        const size_t first = JsonSkipWs(t, braceIdx + 1);
        if (first >= t.size() || t[first] == L'}')
        {
            return {};
        }
        return t.substr(braceIdx + 1, first - (braceIdx + 1));
    }

    static bool JsonObjectIsEmpty(const std::wstring& t, size_t braceIdx)
    {
        const size_t first = JsonSkipWs(t, braceIdx + 1);
        return first < t.size() && t[first] == L'}';
    }

    constexpr const wchar_t* kTrustFlagKey = L"hasTrustDialogAccepted";

    std::wstring ClaudeGlobalConfigPath()
    {
        // <CLAUDE_CONFIG_DIR | %USERPROFILE%>\.claude.json. Deliberately NOT ClaudeProjectsDir's
        // base: with CLAUDE_CONFIG_DIR unset the global config is ~/.claude.json (a FILE beside the
        // ~/.claude directory), not ~/.claude/.claude.json.
        std::wstring base = GetEnvW(L"CLAUDE_CONFIG_DIR");
        if (base.empty())
        {
            base = GetEnvW(L"USERPROFILE");
        }
        if (base.empty())
        {
            return {};
        }
        while (!base.empty() && (base.back() == L'\\' || base.back() == L'/'))
        {
            base.pop_back();
        }
        return base + L"\\.claude.json";
    }

    std::wstring ClaudeWorkspaceTrustKey(std::wstring_view dir)
    {
        if (dir.empty())
        {
            return {};
        }
        std::wstring d{ dir };
        // The nearest enclosing repo IS the workspace for Claude's purposes (it keys trust on the
        // git toplevel), so one entry per repo covers every subdirectory. A worktree answers itself
        // here while Claude's own canonical key answers the MAIN repo root — harmless: the worktree
        // dir is still an ancestor of the cwd, which is the second thing Claude's gate checks, so
        // the seed is found either way (and a mismatch merely shows the dialog once, never worse).
        if (std::wstring root = FindGitRootForDir(d); !root.empty())
        {
            d = std::move(root);
        }
        for (auto& ch : d)
        {
            if (ch == L'\\')
            {
                ch = L'/';
            }
        }
        // Strip a trailing separator ("K:/foo/" -> "K:/foo"), but never turn a root into "K:".
        while (d.size() > 1 && d.back() == L'/' && !(d.size() == 3 && d[1] == L':'))
        {
            d.pop_back();
        }
        return d;
    }

    std::optional<std::wstring> SpliceWorkspaceTrust(const std::wstring& configText, const std::wstring& key)
    {
        if (key.empty())
        {
            return std::nullopt;
        }
        // Refuse to touch a config we cannot read (Updater.h's no-clobber rule). An EMPTY file is
        // included: rebuilding one from scratch would race Claude's own first write.
        const auto parsed = json::Parse(configText);
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        // Already trusted? Then the steady state costs one read and zero writes.
        if (const auto* projects = parsed->Find(L"projects"); projects && projects->type == json::Value::Type::Obj)
        {
            if (const auto* entry = projects->Find(key); entry && entry->type == json::Value::Type::Obj && entry->BoolAt(kTrustFlagKey, false))
            {
                return std::nullopt;
            }
        }

        const size_t rootBrace = configText.find(L'{');
        if (rootBrace == std::wstring::npos)
        {
            return std::nullopt;
        }
        const std::wstring quotedKey = json::Dump(json::Value::MkStr(key));
        const std::wstring quotedFlag = json::Dump(json::Value::MkStr(kTrustFlagKey));

        std::wstring out;
        const auto projectsSpan = JsonFindMember(configText, rootBrace, L"projects");
        if (!projectsSpan.found || configText[projectsSpan.valueBegin] != L'{')
        {
            // No "projects" object at all (a brand-new config): add one as the root's first member.
            const std::wstring indent = JsonMemberIndent(configText, rootBrace);
            const std::wstring inner = indent.empty() ? L"" : indent + L"  ";
            std::wstring ins = indent + L"\"projects\": {" + inner + quotedKey + L": {" + inner + L"  " + quotedFlag + L": true" + inner + L"}" + indent + L"}";
            if (!JsonObjectIsEmpty(configText, rootBrace))
            {
                ins += L",";
            }
            out = configText.substr(0, rootBrace + 1) + ins + configText.substr(rootBrace + 1);
        }
        else
        {
            const size_t projectsBrace = projectsSpan.valueBegin;
            const auto entrySpan = JsonFindMember(configText, projectsBrace, key);
            if (!entrySpan.found || configText[entrySpan.valueBegin] != L'{')
            {
                // No entry for this workspace: add a minimal one. Claude merges its own defaults
                // over a partial entry on the next write, so only the flag needs to be present.
                const std::wstring indent = JsonMemberIndent(configText, projectsBrace);
                const std::wstring inner = indent.empty() ? L"" : indent + L"  ";
                std::wstring ins = indent + quotedKey + L": {" + inner + quotedFlag + L": true" + indent + L"}";
                if (!JsonObjectIsEmpty(configText, projectsBrace))
                {
                    ins += L",";
                }
                out = configText.substr(0, projectsBrace + 1) + ins + configText.substr(projectsBrace + 1);
            }
            else
            {
                const size_t entryBrace = entrySpan.valueBegin;
                const auto flagSpan = JsonFindMember(configText, entryBrace, kTrustFlagKey);
                if (flagSpan.found)
                {
                    // Flip the existing value token in place (it is false / null / anything else —
                    // the true case returned above).
                    out = configText.substr(0, flagSpan.valueBegin) + L"true" + configText.substr(flagSpan.valueEnd);
                }
                else
                {
                    const std::wstring indent = JsonMemberIndent(configText, entryBrace);
                    std::wstring ins = indent + quotedFlag + L": true";
                    if (!JsonObjectIsEmpty(configText, entryBrace))
                    {
                        ins += L",";
                    }
                    out = configText.substr(0, entryBrace + 1) + ins + configText.substr(entryBrace + 1);
                }
            }
        }

        // Belt: the splice must still parse AND read back as trusted, or we write nothing.
        const auto check = json::Parse(out);
        if (!check || check->type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        const auto* checkProjects = check->Find(L"projects");
        if (!checkProjects || checkProjects->type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        const auto* checkEntry = checkProjects->Find(key);
        if (!checkEntry || checkEntry->type != json::Value::Type::Obj || !checkEntry->BoolAt(kTrustFlagKey, false))
        {
            return std::nullopt;
        }
        return out;
    }

    bool EnsureClaudeWorkspaceTrusted(std::wstring_view dir)
    try
    {
        const std::wstring key = ClaudeWorkspaceTrustKey(dir);
        const std::wstring path = ClaudeGlobalConfigPath();
        if (key.empty() || path.empty())
        {
            return false;
        }

        // Fast path: read once, and if the key (or any ancestor of the launch dir) is already
        // trusted there is nothing to do — no lock, no write. This is every launch after the first.
        {
            const std::wstring text = ReadFileUtf8(path);
            if (!SpliceWorkspaceTrust(text, key).has_value())
            {
                // Either already trusted, or the config is unreadable/unspliceable. Distinguish, so
                // an unreadable config is reported (and logged) rather than read as success.
                const auto parsed = json::Parse(text);
                const auto* projects = parsed && parsed->type == json::Value::Type::Obj ? parsed->Find(L"projects") : nullptr;
                const auto* entry = projects && projects->type == json::Value::Type::Obj ? projects->Find(key) : nullptr;
                const bool trusted = entry && entry->type == json::Value::Type::Obj && entry->BoolAt(kTrustFlagKey, false);
                if (!trusted)
                {
                    AppendStateLog(L"hooks.log", L"[trust] skipped " + key + L" (config unreadable or unspliceable: " + path + L")\n");
                }
                return trusted;
            }
        }

        // A write is needed. Take Claude's OWN config lock so we can't interleave with its writer:
        // proper-lockfile's primitive is an atomic mkdir of "<path>.lock".
        const std::wstring lockPath = path + L".lock";
        bool held = false;
        for (int attempt = 0; attempt < 12 && !held; ++attempt)
        {
            if (::CreateDirectoryW(lockPath.c_str(), nullptr))
            {
                held = true;
                break;
            }
            if (::GetLastError() != ERROR_ALREADY_EXISTS)
            {
                break; // no permission / bad path — fail open, the dialog just shows once more
            }
            // Break a lock far past proper-lockfile's own 10 s staleness horizon (a killed claude).
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(lockPath.c_str(), GetFileExInfoStandard, &fad))
            {
                ULARGE_INTEGER then{}, now{};
                FILETIME nowFt{};
                ::GetSystemTimeAsFileTime(&nowFt);
                then.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                then.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                now.LowPart = nowFt.dwLowDateTime;
                now.HighPart = nowFt.dwHighDateTime;
                if (now.QuadPart > then.QuadPart && (now.QuadPart - then.QuadPart) > 60ULL * 10'000'000ULL)
                {
                    ::RemoveDirectoryW(lockPath.c_str());
                    continue;
                }
            }
            ::Sleep(25);
        }
        if (!held)
        {
            AppendStateLog(L"hooks.log", L"[trust] skipped " + key + L" (claude holds the config lock)\n");
            return false;
        }

        bool ok = false;
        {
            // Re-read INSIDE the lock — the freshest-disk RMW (another claude may have just written).
            const std::wstring text = ReadFileUtf8(path);
            if (const auto spliced = SpliceWorkspaceTrust(text, key))
            {
                ok = WriteFileUtf8Atomic(path, *spliced);
                AppendStateLog(L"hooks.log", ok ? (L"[trust] pre-trusted " + key + L"\n") : (L"[trust] FAILED to write " + path + L" (" + key + L")\n"));
            }
            else
            {
                ok = true; // someone trusted it while we waited for the lock
            }
        }
        ::RemoveDirectoryW(lockPath.c_str());
        return ok;
    }
    catch (...)
    {
        // Rule #18: never lose a swallowed exception. Recovery = "not trusted", so the launch still
        // proceeds and the user answers the dialog once.
        Agentmaster::LogSwallowedException(L"EnsureClaudeWorkspaceTrusted");
        return false;
    }

    namespace
    {
        // Read the paste cache for a marker resolve/expand (PENDING_INPUT.md §2b/§10). The files
        // are small pasted texts; the caps are sanity ceilings so a pathological cache can never
        // wedge the (background) caller: 2 MiB per file (larger is skipped — Claude's own
        // placeholder threshold is far below), 64 MiB total, 4096 files. Shared by
        // ResolvePendingPasteRefsIn (the annotation) and ExpandPendingDraftPastesIn (the restore
        // re-fill) so the two can never read the cache differently. [Agentmaster]
        std::vector<PasteFileText> ReadPasteCacheFileTexts(const std::wstring& cacheDir)
        {
            constexpr size_t kPerFileCap = 2 * 1024 * 1024;
            constexpr size_t kTotalCap = 64 * 1024 * 1024;
            constexpr size_t kMaxFiles = 4096;
            std::vector<PasteFileText> files;
            size_t total = 0;
            if (!cacheDir.empty())
            {
                WIN32_FIND_DATAW fd{};
                HANDLE h = ::FindFirstFileW((cacheDir + L"\\*.txt").c_str(), &fd);
                if (h != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        {
                            continue;
                        }
                        const ULONGLONG sz = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                        if (sz == 0 || sz > kPerFileCap || total + sz > kTotalCap || files.size() >= kMaxFiles)
                        {
                            continue;
                        }
                        std::string bytes;
                        {
                            std::ifstream f(std::filesystem::path{ cacheDir + L"\\" + fd.cFileName }, std::ios::binary);
                            if (!f)
                            {
                                continue;
                            }
                            bytes.resize(static_cast<size_t>(sz));
                            f.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                            bytes.resize(static_cast<size_t>(f.gcount()));
                        }
                        if (bytes.empty())
                        {
                            continue;
                        }
                        total += bytes.size();
                        const int need = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
                        if (need <= 0)
                        {
                            continue;
                        }
                        std::wstring wide(static_cast<size_t>(need), L'\0');
                        ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), need);
                        files.push_back(PasteFileText{ fd.cFileName, std::move(wide) });
                    } while (::FindNextFileW(h, &fd));
                    ::FindClose(h);
                }
            }
            return files;
        }
    }

    std::wstring ResolvePendingPasteRefsIn(const std::wstring& draft, const std::wstring& cacheDir)
    try
    {
        const auto markers = FindPasteMarkers(draft);
        if (markers.empty())
        {
            return {};
        }
        const auto files = ReadPasteCacheFileTexts(cacheDir);
        const auto res = ResolvePasteMarkers(markers, files);
        std::wstring out;
        for (size_t i = 0; i < markers.size() && i < res.size(); ++i)
        {
            if (!out.empty())
            {
                out.push_back(L'\n');
            }
            out += markers[i].truncated ? L"truncated #" : L"paste #";
            out += std::to_wstring(markers[i].index);
            out += L" (+" + std::to_wstring(markers[i].lines) + L" lines) -> ";
            out += res[i].resolved ? res[i].fileName : L"unresolved";
            // Honesty tier: a TRUNCATED marker is CONTENT-anchored (head prefix + tail suffix + the
            // exact segment offset — practically collision-proof), while a COLLAPSED one can only be
            // matched by its unique LINE COUNT (nothing of the paste is visible) — a real, weaker
            // tier the annotation names, since a count-coincident stale cache file is conceivable.
            if (res[i].resolved && !markers[i].truncated)
            {
                out += L" (by line count)";
            }
        }
        return out;
    }
    catch (...)
    {
        LogSwallowedException(L"ResolvePendingPasteRefsIn");
        return {}; // unresolved reads as "no annotation", never a wrong claim
    }

    std::wstring ResolvePendingPasteRefs(const std::wstring& draft)
    {
        return ResolvePendingPasteRefsIn(draft, ClaudePasteCacheDir());
    }

    DraftPasteExpansion ExpandPendingDraftPastesIn(const std::wstring& draft, const std::wstring& cacheDir)
    try
    {
        const auto markers = FindPasteMarkers(draft);
        if (markers.empty())
        {
            // Marker-free is the common case: no cache IO at all, the draft IS the fill.
            DraftPasteExpansion out;
            out.complete = true;
            out.text = draft;
            return out;
        }
        return ExpandDraftPasteMarkers(draft, ReadPasteCacheFileTexts(cacheDir));
    }
    catch (...)
    {
        LogSwallowedException(L"ExpandPendingDraftPastesIn");
        return {}; // refused — the caller keeps the placeholder form (never a wrong/lossy expansion)
    }

    DraftPasteExpansion ExpandPendingDraftPastes(const std::wstring& draft)
    {
        return ExpandPendingDraftPastesIn(draft, ClaudePasteCacheDir());
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
            // ⚠ DELIBERATELY BARE — do NOT add LogSwallowedException here (see the "logging-path
            // recursion" note above LogSwallowedExceptionCore). AppendStateLog calls THIS function to
            // build its path, so logging from here is: throw -> log -> AppendStateLog ->
            // AgentmasterStateDir -> throw -> ... unbounded recursion, and (when reached from inside
            // AppendStateLog) a self-deadlock on its non-recursive mutex. The state dir failing is
            // already visible as "no log file at all" — the loudest signal there is.
        }
        return dir;
    }

    // Agentmaster: a local-time [HH:MM:SS.mmm] stamp prefixed to the START of every log record (below), so
    // the hook event stream / engine-mechanism tags / [nav] trail / observer census in hooks.log (and the
    // autorunner/scanner logs) are all time-ordered — turn time, Enter-retry gaps, observer lag, and the
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
            // ⚠ DELIBERATELY BARE — do NOT add LogSwallowedException here (see the "logging-path
            // recursion" note above LogSwallowedExceptionCore). This IS the log sink: logging a
            // log failure re-enters AppendStateLog and SELF-DEADLOCKS on the non-recursive `mtx`
            // already held above (lock_guard, line ~728) before it could even recurse.
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

    // ── Exception forensics (the "never lose a swallowed exception" policy — see ClaudeSpawn.h) ──
    namespace
    {
        // ⚠ LOGGING-PATH RECURSION RULE (read before "completing" the catch(...) sweep in this file).
        // The never-lose-a-swallowed-exception policy says every catch(...) should call
        // LogSwallowedException. There is exactly ONE class of exemption, and it lives in this file:
        // a catch that is itself ON the logging path must stay BARE. The call chain is
        //     LogSwallowedException -> LogSwallowedExceptionCore
        //         -> ExcLogThrottleAllow / CaptureRecentThrowStacksText -> FormatAddressModuleRva
        //         -> AppendStateLog -> AgentmasterStateDir
        // so logging from any link re-enters the chain: unbounded recursion, and inside AppendStateLog
        // a hard self-deadlock on its non-recursive mutex. The bare links are marked individually:
        // AgentmasterStateDir, AppendStateLog, InstallThrowStackCapture, FormatAddressModuleRva,
        // CaptureRecentThrowStacksText, ExcLogThrottleAllow, LogSwallowedExceptionCore, and
        // LogSwallowedException's own two guards. Every OTHER catch(...) in the engine logs.
        constexpr ULONG kMsvcCppExceptionCode = 0xE06D7363UL; // the MSVC C++ `throw` SEH code
        constexpr size_t kThrowFrameCap = 64;
        constexpr size_t kThrowRingSize = 4;

        struct ThrowStackEntry
        {
            void* frames[kThrowFrameCap];
            USHORT count = 0;
            ULONGLONG tick = 0; // GetTickCount64 at raise time (for the "age Nms" label)
        };
        struct ThrowStackRing
        {
            ThrowStackEntry entries[kThrowRingSize];
            size_t next = 0;
            size_t seen = 0;
        };
        // Per-thread: a C++ throw is raised + unwound on one thread, so the catch site reads its own
        // thread's ring. A coroutine's stored exception RE-raises at the resuming co_await (captured
        // again, on the resuming thread) — the resume thread's ring then holds the rethrow, and when
        // throw + resume share a thread the older entries still hold the ORIGINAL raise.
        thread_local ThrowStackRing t_throwRing;

        // The VEH: capture-only, first-position, never handles — dispatch is untouched. Runs BEFORE
        // unwinding, i.e. while the throw-site stack is still intact (the whole point). Lock-free +
        // allocation-free (a raw RtlCaptureStackBackTrace into thread_local storage).
        LONG CALLBACK _ThrowStackCaptureVeh(PEXCEPTION_POINTERS info) noexcept
        {
            if (info && info->ExceptionRecord && info->ExceptionRecord->ExceptionCode == kMsvcCppExceptionCode)
            {
                auto& ring = t_throwRing;
                auto& e = ring.entries[ring.next];
                e.count = ::RtlCaptureStackBackTrace(1, static_cast<ULONG>(kThrowFrameCap), e.frames, nullptr);
                e.tick = ::GetTickCount64();
                ring.next = (ring.next + 1) % kThrowRingSize;
                ++ring.seen;
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Narrow (what()) -> wide, printable-ASCII-safe (exception messages are effectively ASCII;
        // anything else renders '?' rather than guessing a codepage).
        std::wstring WidenNarrowMsg(const char* s)
        {
            std::wstring w;
            if (!s)
            {
                return w;
            }
            for (; *s; ++s)
            {
                const unsigned char c = static_cast<unsigned char>(*s);
                w.push_back((c >= 0x20 && c < 0x7f) ? static_cast<wchar_t>(c) : L'?');
            }
            return w;
        }
    }

    void InstallThrowStackCapture() noexcept
    {
        try
        {
            static std::once_flag s_once;
            std::call_once(s_once, []() noexcept {
                ::AddVectoredExceptionHandler(1 /* first */, &_ThrowStackCaptureVeh);
            });
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (logging-path recursion rule): the capture isn't installed yet, so
            // a log here would report a throw with no stack — and this runs from the same init that
            // brings logging up. Failure degrades to "no throw-site stacks", never to a crash.
        }
    }

    std::wstring FormatAddressModuleRva(const void* address) noexcept
    {
        try
        {
            HMODULE mod{};
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(address),
                                     &mod) &&
                mod)
            {
                wchar_t path[MAX_PATH]{};
                const DWORD len = ::GetModuleFileNameW(mod, path, MAX_PATH);
                std::wstring leaf = (len > 0) ? std::wstring{ path, len } : std::wstring{};
                if (const auto slash = leaf.find_last_of(L"\\/"); slash != std::wstring::npos)
                {
                    leaf.erase(0, slash + 1);
                }
                if (leaf.empty())
                {
                    leaf = L"?";
                }
                wchar_t rva[24]{};
                swprintf_s(rva, L"+0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(mod)));
                return leaf + rva;
            }
            wchar_t raw[24]{};
            swprintf_s(raw, L"0x%p", address);
            return raw;
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (logging-path recursion rule): called BY the formatter for every
            // captured frame. "?" degrades one frame of one stack; logging would recurse per frame.
            return L"?";
        }
    }

    std::wstring CaptureRecentThrowStacksText(size_t maxEntries) noexcept
    {
        try
        {
            const ThrowStackRing ring = t_throwRing; // snapshot FIRST (a classification rethrow would displace entries)
            const ULONGLONG now = ::GetTickCount64();
            const size_t have = (ring.seen < kThrowRingSize) ? ring.seen : kThrowRingSize;
            const size_t emit = (maxEntries < have) ? maxEntries : have;
            constexpr size_t kFramesPerLine = 40; // keep the line greppable; deep tails elide
            std::wstring out;
            for (size_t k = 0; k < emit; ++k)
            {
                // newest-first: next-1 is the most recent write
                const auto& e = ring.entries[(ring.next + kThrowRingSize - 1 - k) % kThrowRingSize];
                if (e.count == 0)
                {
                    continue;
                }
                out += L"[exc]   throw#" + std::to_wstring(k) +
                       L" (age " + std::to_wstring(now >= e.tick ? now - e.tick : 0) + L"ms, " +
                       std::to_wstring(e.count) + L" frames):";
                const size_t n = (e.count < kFramesPerLine) ? e.count : kFramesPerLine;
                for (size_t i = 0; i < n; ++i)
                {
                    out += L" " + FormatAddressModuleRva(e.frames[i]);
                }
                if (e.count > kFramesPerLine)
                {
                    out += L" ...";
                }
                out += L"\n";
            }
            return out;
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (logging-path recursion rule): called BY LogSwallowedExceptionCore.
            // Degrades to "no stack lines" — the [exc] header (type/hr/message) is still emitted.
            return {};
        }
    }

    bool ExcLogThrottleAllow(const std::wstring& key, unsigned& suppressed) noexcept
    {
        suppressed = 0;
        try
        {
            static std::mutex s_mtx;
            static std::unordered_map<std::wstring, std::pair<ULONGLONG, unsigned>> s_last; // key -> {last-allowed tick, suppressed since}
            constexpr ULONGLONG kWindowMs = 2000;
            const ULONGLONG now = ::GetTickCount64();
            std::lock_guard<std::mutex> lk{ s_mtx };
            auto& slot = s_last[key];
            if (slot.first != 0 && now - slot.first < kWindowMs)
            {
                ++slot.second;
                return false;
            }
            slot.first = now;
            suppressed = slot.second;
            slot.second = 0;
            return true;
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (logging-path recursion rule): called BY LogSwallowedExceptionCore.
            return true; // fail OPEN — losing the throttle must never lose the log
        }
    }

    void LogSwallowedExceptionCore(const wchar_t* context, const std::wstring& detail, const std::wstring& stacksText) noexcept
    {
        try
        {
            const std::wstring ctx = context ? context : L"?";
            unsigned suppressed = 0;
            if (!ExcLogThrottleAllow(L"exc:" + ctx, suppressed))
            {
                return;
            }
            wchar_t tid[16]{};
            swprintf_s(tid, L"0x%X", static_cast<unsigned>(::GetCurrentThreadId()));
            std::wstring block = L"[exc] " + ctx + L": swallowed " + detail + L" tid=" + tid;
            if (suppressed != 0)
            {
                block += L" [suppressed " + std::to_wstring(suppressed) + L" earlier repeat(s)]";
            }
            block += L" (no crash)\n";
            block += stacksText;
            AppendStateLog(L"hooks.log", block);
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (logging-path recursion rule): this IS the log chokepoint. A throw
            // here (OOM composing the block) means the report is lost — reporting THAT would recurse.
        }
    }

    void LogSwallowedException(const wchar_t* context) noexcept
    {
        try
        {
            const std::wstring stacks = CaptureRecentThrowStacksText(); // BEFORE the classification rethrow
            std::wstring detail;
            try
            {
                throw; // classify the in-flight exception (caller guarantees we're inside a catch)
            }
            catch (const std::system_error& e)
            {
                detail = L"std::system_error code=" + std::to_wstring(e.code().value()) + L" msg=\"" + WidenNarrowMsg(e.what()) + L"\"";
            }
            catch (const std::exception& e)
            {
                detail = L"std::exception msg=\"" + WidenNarrowMsg(e.what()) + L"\"";
            }
            catch (...)
            {
                // Not a recursion exemption — this is the classifier's terminal arm (the rethrown
                // exception matched no known type). It RECORDS the unknown, it doesn't swallow.
                detail = L"unknown exception";
            }
            LogSwallowedExceptionCore(context, detail, stacks);
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (logging-path recursion rule): the outer noexcept guard of the
            // reporter itself. Keeps the policy's own machinery from ever becoming the crash.
        }
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

    // ---- the /handover command DEFINITION (COMMANDS.md §6) ----
    //
    // Its SHIPPED VERSION HISTORY, as the SHA-256 (lowercase hex) of each version's UTF-8 bytes
    // EXACTLY as WriteFileUtf8 lays them on disk — oldest first, the LAST entry being the digest of
    // the CURRENT text (kHandoverCommandV6 below). The engine harness asserts that last identity,
    // so editing the text without appending its digest FAILS the suite instead of silently
    // orphaning the upgrade rule.
    //
    // WHY DIGESTS AND NOT THE TEXTS: the upgrade rule only ever asks "are these bytes something WE
    // shipped, unmodified?" — a content-IDENTITY question a 64-char digest answers completely — so
    // a superseded version costs ONE line here instead of a permanently frozen 2–3 KB literal. The
    // texts themselves stay in git history (`git log -S<digest>` lands on the commit that retired
    // one; its parent still carries the full literal).
    //
    // ONLY EVER APPEND. An entry is frozen forever: deleting or editing one makes every install
    // still carrying that version look USER-OWNED, and it would never auto-upgrade again.
    //
    // TO SHIP A NEW VERSION: edit the text below (keeping the load-bearing "Write tool" +
    // "HANDOVER-" phrases the markdown await keys on), rename it kHandoverCommandV<N+1>, run the
    // harness — the failing check PRINTS the new digest — and append that digest here.
    const std::vector<std::string_view>& ShippedHandoverCommandHashes()
    {
        static const std::vector<std::string_view> kHashes{
            "adf05f2b979b6be9e83cea513949d2fdd70490c82763bc05440707d087bd3e84", // v1 — the original: write ONE HANDOVER-<topic>.md; the successor's first prompt POINTS at the file
            "1a240220b2c7b4fd28c23f3572ebff979b1f2a28a60d5489ecb6b3bae7f036d1", // v2 — content injection: the file's CONTENT is the successor's first user message (write it AS the briefing)
            "197dab657f66a44211c3bb33b49787e3f4de18cce9712c23a5295eca24c04482", // v3 — never truncate: dropped v2's "long documents truncate to a pointer" caution (the paste tier delivers in full)
            "581aad0004fae77a28bb215415e949e3d84f164eea2496ec0eb2520c6121136e", // v4 — multi-file: several HANDOVER-*.md in one turn, delivered JOINED into one successor
            "f4f1b98250822e018db0dbae0eb75d503078d1b45be99167715e11eb94f604b7", // v5 — self-invocation guard: a MODEL-invoked skill writes no echo, so it must write nothing and redirect the user to TYPE the command
            "f3c88984e10a9d8689c4f0ac70eab3f19d4aac1001c08bb9a04480d6850fbce5", // v6 — FAN-OUT: each file starts its OWN successor tab, so every file must be self-contained
            "e03874c5997929a37f923059e7e2e1ef3b61baa34869b28fe38b2cafc5c54e40", // v7 — CONFIGURABLE WRITE LOCATION: the folder moved to a rendered "WRITE IT IN:" line, defaulting to the session scratchpad
            "cf3ca81256a36a5f2a90cbdc113011682acf41beee77b49f64d3c0f4549ee91c", // v8 — ARGUMENT-HINT (current): the autocomplete frontmatter spelling the §6b "[model] [title] <context-or-filepath>" syntax
        };
        return kHashes;
    }

    // V8 (argument-hint) — the CURRENT text. Adds the `argument-hint:` frontmatter line — the
    // official Claude Code autocomplete field (shown beside the command in the / menu) — spelling
    // the family syntax the §6b per-message hints parse: `[model] [title] <context-or-filepath>`
    // (both brackets OPTIONAL + typed LITERALLY; a [x] naming no model falls back to the title).
    // The value is single-QUOTED YAML deliberately: a plain scalar starting `[` parses as a flow
    // sequence and would break the whole frontmatter block. The "/handover" token inside the hint
    // rides the §6a rename render like every other mention, so a renamed command's hint shows the
    // renamed example.
    // V7 (superseded) moved the file's DIRECTORY out of the prose onto its own rendered
    // "WRITE IT IN: <phrase>" line (AppSettings::commandHandoverWritePath, COMMANDS.md §6c) —
    // shipped default: the session SCRATCHPAD (a briefing is a transient hand-off courier whose
    // CONTENT is injected into the successor anyway — the HANDOVER-*.md litter fix). The line's
    // marker + one-line span are what make the location reversible for the digest identity
    // (RenderShippedCommandWritePath / its inverse below), so KEEP THE MARKER AND KEEP THE PHRASE
    // ON ONE LINE in every future version — V8 keeps it verbatim.
    static constexpr std::wstring_view kHandoverCommandV8 =
        LR"md(---
description: Hand this session's work over to a fresh successor session (Agentmaster opens it automatically)
argument-hint: '[model] [title] <context-or-filepath> - brackets optional + typed literally, e.g. /handover [fable] [my title] finish the tests; a [x] naming no model becomes the title'
---
The user wants to HAND OVER this session's work to a fresh successor Claude session.
Handover context from the user (inline context, or a path to a file you should read and fold in):

$ARGUMENTS

IMPORTANT - this pipeline is triggered ONLY by the user actually TYPING /handover as their
message (Agentmaster detects the typed command's transcript echo; a model-initiated Skill
invocation leaves no such echo, so nothing would be watching). If you are reading this because
YOU invoked the skill yourself rather than the user typing /handover: do NOT write any
handover file - tell the user to type `/handover <context>` themselves, then continue what
you were doing.

Do this NOW, in this exact order:
1. If the context above names a readable file, read it first and incorporate it.
2. Using the Write tool (NOT a shell redirect - the Write tool call itself is the signal
   Agentmaster detects), create ONE new markdown file named `HANDOVER-<short-topic>.md`
   (pick a short kebab-case topic slug; if that name already exists, append `-2`, `-3`, ...).
   WRITE IT IN: your session scratchpad directory (the temp scratchpad folder your own instructions name; if you have none, use the system temp folder)
   EACH `HANDOVER-*.md` file you write in this turn starts its OWN successor tab, in write
   order - so write ONE file for one successor, or write MORE THAN ONE file to fan out
   several parallel successors at once.
3. Each file's CONTENT is injected VERBATIM as ITS successor session's FIRST USER MESSAGE - so
   write every file as a direct briefing TO that successor (imperative, second person), fully
   self-contained: the goal, the current state, decisions made and why, work completed, work
   still in flight, concrete ordered next steps, key file paths (absolute), and any gotchas
   or constraints discovered along the way. A successor has NO other context, cannot see this
   conversation, and cannot see the OTHER files (each goes to a different session - never
   write "continue in file B"). Each file is delivered as ONE message whatever its size - be
   as thorough as the work demands.
4. End your turn right after writing the file(s) (a one-line confirmation is fine). Do not
   start new work.

Agentmaster is watching for those markdown writes: when your turn ends it automatically opens
a successor session tab PER FILE (named like this one, ending in "(handover)", "(handover 2)",
...) in this working directory and injects each document as its own tab's opening user message.
)md";

    std::wstring_view ShippedHandoverCommandText()
    {
        return kHandoverCommandV8;
    }

    // ---- the /handover-here command DEFINITION (COMMANDS.md — the IN-PLACE twin) ----
    //
    // Same shipped-history contract as /handover above, verbatim: SHA-256 per version, oldest
    // first, the last entry being the current text's digest, append-only, frozen forever. Its
    // command text keeps the SAME await-signal contract ("use the Write tool", the
    // `HANDOVER-<topic>.md` leaf) and the same content-injection + never-truncate briefing, but the
    // outcome it describes is the in-place REPLACE: Agentmaster restarts THIS tab into the fresh
    // successor instead of opening a new tab beside it.
    const std::vector<std::string_view>& ShippedHandoverHereCommandHashes()
    {
        static const std::vector<std::string_view> kHashes{
            "87c06df50f4c6fc85cae1786f7b30331152372775432d73932e834ce2b4163b9", // v1 — the original in-place twin (content injection + never-truncate from birth)
            "af205daa5ee0a81cf04355b87840a8aab28a0054023d8806da9085b729a63ae8", // v2 — multi-file: the /handover v4 split-permission line, delivered JOINED
            "7b8ecdc8599a8ad4621121e1cf6ff467bc477a355d019241878a9d0440188f32", // v3 — self-invocation guard (the /handover v5 guard, doubly important for the REPLACE-this-tab variant)
            "27fa954882260765c2348255aa026e1beb88332f8b590aef431f484c51d81c30", // v4 — FAN-OUT: the FIRST file's successor REPLACES this tab, each additional file opens its own beside it
            "fe96f6676fd80a61230bf38265461f331e4c94ec78b184644354d44d88009812", // v5 — CONFIGURABLE WRITE LOCATION: the /handover v7 "WRITE IT IN:" line, same family-wide setting
            "a4ea6a53bb04b954931b6f810ac9b3e1ac96ad73299f9f9aef5037be621f4b82", // v6 — ARGUMENT-HINT (current): the /handover v8 autocomplete syntax hint, in-place example
        };
        return kHashes;
    }

    // V6 (argument-hint) — the CURRENT text: the /handover V8 autocomplete syntax hint for the
    // in-place twin (same single-quoted-YAML rationale; the example names ITS OWN command).
    // V5 (superseded) was the configurable write location: the /handover V7 semantics — the same
    // "WRITE IT IN: <phrase>" line, rendered from the SAME family-wide setting (the location is
    // one rule for the whole family, like the file-match pattern).
    static constexpr std::wstring_view kHandoverHereCommandV6 =
        LR"md(---
description: Hand this session's work over to a fresh session that REPLACES this one in this same tab (Agentmaster restarts the tab automatically)
argument-hint: '[model] [title] <context-or-filepath> - brackets optional + typed literally, e.g. /handover-here [fable] [my title] take over in this tab; a [x] naming no model becomes the title'
---
The user wants to HAND OVER this session's work to a fresh successor Claude session that
REPLACES this conversation IN THIS SAME TAB - Agentmaster restarts the tab into the
successor automatically; this conversation is archived and stays resumable from the
Sessions browser.
Handover context from the user (inline context, or a path to a file you should read and fold in):

$ARGUMENTS

IMPORTANT - this pipeline is triggered ONLY by the user actually TYPING /handover-here as
their message (Agentmaster detects the typed command's transcript echo; a model-initiated
Skill invocation leaves no such echo, so nothing would be watching and no tab would ever be
replaced). If you are reading this because YOU invoked the skill yourself rather than the
user typing /handover-here: do NOT write any handover file - tell the user to type
`/handover-here <context>` themselves, then continue what you were doing.

Do this NOW, in this exact order:
1. If the context above names a readable file, read it first and incorporate it.
2. Using the Write tool (NOT a shell redirect - the Write tool call itself is the signal
   Agentmaster detects), create ONE new markdown file named `HANDOVER-<short-topic>.md`
   (pick a short kebab-case topic slug; if that name already exists, append `-2`, `-3`, ...).
   WRITE IT IN: your session scratchpad directory (the temp scratchpad folder your own instructions name; if you have none, use the system temp folder)
   EACH `HANDOVER-*.md` file you write in this turn starts its OWN successor, in write order:
   the FIRST file's successor REPLACES this tab, and each additional file (you may write
   MORE THAN ONE) opens its own new tab beside it.
3. Each file's CONTENT is injected VERBATIM as ITS successor session's FIRST USER MESSAGE - so
   write every file as a direct briefing TO that successor (imperative, second person), fully
   self-contained: the goal, the current state, decisions made and why, work completed, work
   still in flight, concrete ordered next steps, key file paths (absolute), and any gotchas
   or constraints discovered along the way. A successor has NO other context, cannot see this
   conversation, and cannot see the OTHER files (each goes to a different session - never
   write "continue in file B"). Each file is delivered as ONE message whatever its size - be
   as thorough as the work demands.
4. End your turn right after writing the file(s) (a one-line confirmation is fine). Do not
   start new work - this session is about to be replaced.

Agentmaster is watching for those markdown writes: when your turn ends it automatically
RESTARTS THIS TAB into the first file's successor session (and opens a new tab per additional
file) in this working directory, injecting each document as its session's opening user message.
)md";

    std::wstring_view ShippedHandoverHereCommandText()
    {
        return kHandoverHereCommandV6;
    }

    // ---- the /handover-standby command DEFINITION (COMMANDS.md §5b — the FILL-NOT-SEND member) ----
    //
    // Same shipped-history contract as its two siblings, verbatim: SHA-256 per version, oldest
    // first, the last entry being the current text's digest, append-only, frozen forever. Its text
    // keeps the SAME await-signal contract ("use the Write tool", the `HANDOVER-<topic>.md` leaf,
    // the "WRITE IT IN:" location line) and the same self-contained-briefing discipline, but the
    // outcome it describes is the STANDBY delivery: each file's successor opens as a new tab and
    // the document is TYPED into that session's input box WITHOUT being submitted — the user
    // reviews the pre-filled message and presses Enter themselves. Deliberately NO bare mention of
    // the sibling commands' names in the text: each definition renders only ITS OWN "/<name>"
    // token (RenderShippedCommandText), so a sibling reference would go stale under a rename.
    const std::vector<std::string_view>& ShippedHandoverStandbyCommandHashes()
    {
        static const std::vector<std::string_view> kHashes{
            "2d48a7b6d1024a472ea39bb4bf90cb01db8da75b275b78f29774b6571d4e9c16", // v1 — the original standby: fan-out successors whose briefings are PRE-TYPED into the input box, never submitted
            "66d13b169a66d09b4ee5ab74a2e25ff94dc96dc04ad0521b0e7425d54d3f2376", // v2 — ARGUMENT-HINT (current): the /handover v8 autocomplete syntax hint; its example shows the TITLE-ONLY fallback form
        };
        return kHashes;
    }

    // V2 (argument-hint) — the CURRENT text: the /handover V8 autocomplete syntax hint (same
    // single-quoted-YAML rationale); its example deliberately shows the TITLE-ONLY form — the
    // first bracket naming no model falling back to the title slot — the shape standby is most
    // typed with.
    // V1 (superseded) was the original standby text: the /handover V7 mechanics (fan-out,
    // self-contained files, the rendered "WRITE IT IN:" line, the self-invocation guard) with the
    // delivery description swapped for the fill-not-send contract. KEEP THE MARKER AND KEEP THE
    // PHRASE ON ONE LINE in every future version (the §6c identity fold), and keep the
    // "Write tool" + "HANDOVER-" phrases the markdown await keys on — V2 keeps all of it verbatim.
    static constexpr std::wstring_view kHandoverStandbyCommandV2 =
        LR"md(---
description: Hand this session's work over to fresh successor session(s) whose first message is PRE-TYPED but NOT sent - you review it and press Enter (a handover in standby)
argument-hint: '[model] [title] <context-or-filepath> - brackets optional + typed literally, e.g. /handover-standby [my title] review then press Enter; a [x] naming no model becomes the title'
---
The user wants to HAND OVER this session's work to a fresh successor Claude session IN
STANDBY: Agentmaster opens the successor tab(s) automatically and TYPES each briefing into
its session's input box WITHOUT sending it - the user reviews the pre-filled message there
and presses Enter themselves when ready. Nothing runs until they do.
Handover context from the user (inline context, or a path to a file you should read and fold in):

$ARGUMENTS

IMPORTANT - this pipeline is triggered ONLY by the user actually TYPING /handover-standby as
their message (Agentmaster detects the typed command's transcript echo; a model-initiated
Skill invocation leaves no such echo, so nothing would be watching). If you are reading this
because YOU invoked the skill yourself rather than the user typing /handover-standby: do NOT
write any handover file - tell the user to type `/handover-standby <context>` themselves,
then continue what you were doing.

Do this NOW, in this exact order:
1. If the context above names a readable file, read it first and incorporate it.
2. Using the Write tool (NOT a shell redirect - the Write tool call itself is the signal
   Agentmaster detects), create ONE new markdown file named `HANDOVER-<short-topic>.md`
   (pick a short kebab-case topic slug; if that name already exists, append `-2`, `-3`, ...).
   WRITE IT IN: your session scratchpad directory (the temp scratchpad folder your own instructions name; if you have none, use the system temp folder)
   EACH `HANDOVER-*.md` file you write in this turn starts its OWN successor tab, in write
   order - so write ONE file for one successor, or write MORE THAN ONE file to fan out
   several parallel successors at once.
3. Each file's CONTENT is PRE-TYPED VERBATIM into ITS successor session's input box as a
   ready-to-send first user message (typed, NOT submitted - the user presses Enter there) - so
   write every file as a direct briefing TO that successor (imperative, second person), fully
   self-contained: the goal, the current state, decisions made and why, work completed, work
   still in flight, concrete ordered next steps, key file paths (absolute), and any gotchas
   or constraints discovered along the way. A successor has NO other context, cannot see this
   conversation, and cannot see the OTHER files (each goes to a different session - never
   write "continue in file B"). Each file is delivered as ONE message whatever its size - be
   as thorough as the work demands.
4. End your turn right after writing the file(s) (a one-line confirmation is fine). Do not
   start new work.

Agentmaster is watching for those markdown writes: when your turn ends it automatically opens
a successor session tab PER FILE (named like this one, ending in "(handover)", "(handover 2)",
...) in this working directory and TYPES each document into its own tab's input box, ready to
send - nothing is submitted until the user presses Enter in that tab.
)md";

    std::wstring_view ShippedHandoverStandbyCommandText()
    {
        return kHandoverStandbyCommandV2;
    }

    // ---- customizable command names (COMMANDS.md §6a) — the render / identity pair ----
    //
    // A renamed command's definition text must SAY the new name (the self-invocation guard tells
    // the user what to TYPE), so materializing under a custom name substitutes the "/<default>"
    // token; and recognizing "a version WE shipped, unmodified" under a custom name substitutes it
    // BACK before hashing (the histories stay digests of the DEFAULT-name texts — no per-name
    // digest freezing). The two substitutions are exact inverses because both replace the token
    // only at a WORD BOUNDARY (the following char must not extend the slug — which also keeps a
    // "/handover" replacement from ever corrupting a "/handover-here" mention) and command names
    // are strict ASCII slugs (NormalizeCommandName), so the byte-level form can never split a
    // UTF-8 multi-byte sequence (every continuation byte has the high bit set == a boundary).

    // Does `next` END a "/<name>" token? (Anything that could extend the slug does not.)
    static bool CmdTokenBoundaryW(wchar_t next)
    {
        return !((next >= L'a' && next <= L'z') || (next >= L'A' && next <= L'Z') ||
                 (next >= L'0' && next <= L'9') || next == L'-' || next == L'_');
    }
    static bool CmdTokenBoundaryA(char next)
    {
        return !((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                 (next >= '0' && next <= '9') || next == '-' || next == '_');
    }

    std::wstring RenderShippedCommandText(std::wstring_view text, std::wstring_view defaultName, std::wstring_view commandName)
    {
        if (defaultName.empty() || commandName.empty() || defaultName == commandName)
        {
            return std::wstring{ text }; // the default name (or degenerate input) renders verbatim
        }
        const std::wstring token = L"/" + std::wstring{ defaultName };
        const std::wstring repl = L"/" + std::wstring{ commandName };
        std::wstring out;
        out.reserve(text.size() + 32);
        size_t i = 0;
        while (i < text.size())
        {
            if (text.compare(i, token.size(), token) == 0 &&
                (i + token.size() >= text.size() || CmdTokenBoundaryW(text[i + token.size()])))
            {
                out += repl;
                i += token.size();
            }
            else
            {
                out.push_back(text[i]);
                ++i;
            }
        }
        return out;
    }

    std::string NormalizeCommandBytesForIdentity(std::string_view bytes, std::wstring_view defaultName, std::wstring_view commandName)
    {
        if (defaultName.empty() || commandName.empty() || defaultName == commandName)
        {
            return std::string{ bytes }; // identity — nothing was renamed
        }
        // Narrow the two ASCII slugs. A non-ASCII char can't come out of NormalizeCommandName; if
        // one ever arrives (a hand-built caller), refuse the substitution rather than mangle bytes
        // — the file then just reads as user-owned (never a wrong delete/overwrite).
        const auto narrowSlug = [](std::wstring_view w, std::string& out) {
            out.clear();
            out.push_back('/');
            for (const wchar_t c : w)
            {
                if (c > 0x7F)
                {
                    return false;
                }
                out.push_back(static_cast<char>(c));
            }
            return true;
        };
        std::string token; // the CUSTOM token as it sits in the file
        std::string repl; // the default token the history hashes carry
        if (!narrowSlug(commandName, token) || !narrowSlug(defaultName, repl))
        {
            return std::string{ bytes };
        }
        std::string out;
        out.reserve(bytes.size() + 32);
        size_t i = 0;
        while (i < bytes.size())
        {
            if (bytes.compare(i, token.size(), token) == 0 &&
                (i + token.size() >= bytes.size() || CmdTokenBoundaryA(bytes[i + token.size()])))
            {
                out += repl;
                i += token.size();
            }
            else
            {
                out.push_back(bytes[i]);
                ++i;
            }
        }
        return out;
    }

    // ---- customizable WRITE LOCATION (COMMANDS.md §6c) — the render / identity pair ----
    //
    // The same trick the custom NAME uses, over a different span: the shipped texts carry ONE
    // "WRITE IT IN: <phrase>" line whose phrase is the SHIPPED DEFAULT (the session scratchpad);
    // materializing under a configured location swaps that phrase, and asking "are these bytes
    // something WE shipped?" swaps whatever is there BACK to the shipped phrase before hashing.
    //
    // The inverse here is DELIMITED, not value-driven: the span is "everything after the marker up
    // to the end of that line", so the normalization does NOT need to know which location the file
    // was written with — no per-install marker, and a location changed by hand still reads as ours.
    // That is also why the phrase must stay ONE LINE and the marker must stay in every version.
    // (Older versions — v1..v6 / v1..v4 — carry no marker at all, so the normalization is a no-op
    // on them and they still match their own historical digests, which is what keeps a pristine
    // pre-§6c file upgrading.)
    static constexpr std::wstring_view kWriteLocationMarker = L"WRITE IT IN: ";
    static constexpr std::string_view kWriteLocationMarkerA = "WRITE IT IN: ";
    // The phrase the SHIPPED texts literally contain (asserted by the harness against both texts).
    static constexpr std::wstring_view kWriteLocationShippedPhrase = L"your session scratchpad directory (the temp scratchpad folder your own instructions name; if you have none, use the system temp folder)";

    std::wstring CommandWritePathPhrase(std::wstring_view writePath)
    {
        const std::wstring v = NormalizeCommandWritePath(writePath);
        if (CommandWritePathIsScratchpad(v))
        {
            return std::wstring{ kWriteLocationShippedPhrase };
        }
        if (v == L"./" || v == L".\\" || v == L".")
        {
            return L"the current working directory"; // the pre-§6c behavior, as a preset
        }
        const bool absolute = (v.size() >= 2 && v[1] == L':') || (!v.empty() && (v[0] == L'\\' || v[0] == L'/'));
        std::wstring s = L"`" + v + L"`";
        s += absolute ? L" (create the folder if it does not exist)" :
                        L", relative to the current working directory (create the folder if it does not exist)";
        return s;
    }

    // Replace the phrase on EVERY "WRITE IT IN: …" line with `phrase`. `end` of a span is the line
    // terminator (a CR before an LF is kept, so a CRLF text round-trips). Shared by the wide render
    // and the narrow identity inverse, which differ only in their character type.
    template<typename StrT, typename ViewT>
    static StrT ReplaceWriteLocationSpans(ViewT text, ViewT marker, ViewT phrase, typename StrT::value_type lf, typename StrT::value_type cr)
    {
        StrT out{ text };
        size_t from = 0;
        for (;;)
        {
            const size_t m = out.find(marker, from);
            if (m == StrT::npos)
            {
                break;
            }
            const size_t b = m + marker.size();
            size_t e = out.find(lf, b);
            if (e == StrT::npos)
            {
                e = out.size();
            }
            if (e > b && out[e - 1] == cr)
            {
                --e;
            }
            out.replace(b, e - b, phrase);
            from = b + phrase.size();
        }
        return out;
    }

    std::wstring RenderShippedCommandWritePath(std::wstring_view text, std::wstring_view writePath)
    {
        const std::wstring phrase = CommandWritePathPhrase(writePath);
        if (phrase == kWriteLocationShippedPhrase)
        {
            return std::wstring{ text }; // the shipped default renders verbatim
        }
        return ReplaceWriteLocationSpans<std::wstring, std::wstring_view>(text, kWriteLocationMarker, phrase, L'\n', L'\r');
    }

    std::string NormalizeCommandWritePathBytesForIdentity(std::string_view bytes)
    {
        // ASCII marker + (normally) ASCII phrase; a non-ASCII folder name in the file is spanned by
        // BYTE positions bounded by '\n', and every UTF-8 continuation byte has the high bit set, so
        // the span can never split a multi-byte character.
        const std::string phrase = Utf16ToUtf8(kWriteLocationShippedPhrase);
        if (bytes.find(kWriteLocationMarkerA) == std::string_view::npos)
        {
            return std::string{ bytes }; // no marker (a pre-§6c version, or a rewritten text)
        }
        return ReplaceWriteLocationSpans<std::string, std::string_view>(bytes, kWriteLocationMarkerA, phrase, '\n', '\r');
    }

    // Read a definition file's bytes, bounded (our texts are ~3 KB; a file past 64 KiB is certainly
    // not a pristine ours, and a truncated read's digest matches nothing anyway). "" == unreadable
    // or empty, which every caller treats as USER-OWNED (never overwrite/delete blind).
    static std::string ReadCommandDefinitionBytes(const std::wstring& path)
    {
        std::string bytes;
        try
        {
            std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
            if (f)
            {
                bytes.resize(64 * 1024);
                f.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                bytes.resize(static_cast<size_t>(f.gcount()));
            }
        }
        catch (...)
        {
            // Rule #18 — this recovery is CONSEQUENTIAL, so it must never be silent: an unreadable
            // definition reads as USER-OWNED, and a user-owned file is never overwritten, so the
            // command silently stops upgrading. Safe to log (not on the logging path).
            LogSwallowedException(L"ReadCommandDefinitionBytes");
            bytes.clear();
        }
        return bytes;
    }

    // "Are these bytes a version WE shipped, unmodified?" — the ONE identity question behind the
    // upgrade, the rename/disable removal, and the cog's status line. Both customization spans are
    // undone before hashing: the NAME token (needs the configured pair) and the §6c WRITE-LOCATION
    // line (delimited, so it needs nothing). Two candidates are tried because the location
    // normalization must not disturb the historical texts: the name-normalized bytes match any
    // version verbatim, the additionally location-normalized bytes match a CURRENT version rendered
    // under a custom location. Returns the matched history index, or -1 for user-owned.
    static int MatchShippedCommandVersion(std::string_view bytes, const std::vector<std::string_view>& shippedHashes, std::wstring_view defaultName, std::wstring_view commandName)
    {
        if (bytes.empty() || shippedHashes.empty())
        {
            return -1;
        }
        const bool custom = !commandName.empty() && !defaultName.empty() && commandName != defaultName;
        const std::string nameNorm = custom ? NormalizeCommandBytesForIdentity(bytes, defaultName, commandName) : std::string{ bytes };
        const std::string locNorm = NormalizeCommandWritePathBytesForIdentity(nameNorm);
        const std::string digests[2] = { Sha256Hex(nameNorm), locNorm == nameNorm ? std::string{} : Sha256Hex(locNorm) };
        for (const auto& d : digests)
        {
            if (d.empty())
            {
                continue;
            }
            for (size_t i = 0; i < shippedHashes.size(); ++i)
            {
                if (d == shippedHashes[i])
                {
                    return static_cast<int>(i);
                }
            }
        }
        return -1;
    }

    // Shared core of the shipped slash-command DEFINITION writers (the /handover family —
    // COMMANDS.md §6). Write policy: create-if-absent PLUS a version-aware UPGRADE — a file whose
    // SHA-256 matches a PRIOR shipped version of THIS command is ours and untouched by the user, so
    // it silently upgrades to the current text; anything else (the user's own command, an edited
    // copy of ours, or the already-current text) is NEVER overwritten (the ApplyEnvDefaults
    // discipline: a user edit sticks forever). `shippedHashes` is that command's FULL history,
    // oldest first with the CURRENT text's digest last — so the walk below covers exactly the PRIOR
    // versions and an already-current file matches nothing (no rewrite, no log line). The
    // load-bearing instructions every version of every command in the family keeps: "use the Write
    // tool" (the transcript tool_use is the signal the markdown await keys on — a shell-redirect
    // write is invisible) and the "HANDOVER-" name (the await's leaf preference). `logLabel` names
    // the command in the log lines so the trails stay per-command ("handover" / "handover-here").
    // `defaultName`/`commandName` are the §6a custom-name seam and `writePath` the §6c
    // write-location seam: both are RENDERED into the text we write and UNDONE before hashing, so
    // "ours, unmodified" stays a content-identity question under any name and any location. Empty
    // values are a pass-through on both (the pre-customization behavior, byte-identical).
    //
    // The rewrite rule is "ours AND not already exactly what we would write": that covers a version
    // UPGRADE (an older digest) and a re-render of the CURRENT version under a changed name or
    // write location — while a file byte-identical to `textToWrite` is a no-op (no write, no log).
    static std::wstring EnsureShippedCommandFileCore(const std::wstring& configDir, std::wstring_view fileLeaf, const std::vector<std::string_view>& shippedHashes, std::wstring_view currentText, std::wstring_view logLabel, std::wstring_view defaultName, std::wstring_view commandName, std::wstring_view writePath)
    try
    {
        if (configDir.empty() || fileLeaf.empty() || shippedHashes.empty() || currentText.empty())
        {
            return {};
        }
        const bool custom = !commandName.empty() && !defaultName.empty() && commandName != defaultName;
        std::wstring textToWrite = custom ? RenderShippedCommandText(currentText, defaultName, commandName) : std::wstring{ currentText };
        textToWrite = RenderShippedCommandWritePath(textToWrite, writePath); // §6c (no-op on the shipped default)
        const std::wstring commandsDir = configDir + L"\\commands";
        const std::wstring path = commandsDir + L"\\" + std::wstring{ fileLeaf };
        if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            // Present. Ours (SHA-256 == some shipped version, both customization spans undone) and
            // not already the exact bytes we'd write => rewrite; anything else — user-owned, or
            // already current — is left exactly as it is.
            const std::string bytes = ReadCommandDefinitionBytes(path);
            const int matched = MatchShippedCommandVersion(bytes, shippedHashes, defaultName, commandName);
            if (matched >= 0)
            {
                if (bytes == Utf16ToUtf8(textToWrite))
                {
                    return path; // already exactly current, under this name + location — no-op
                }
                const size_t current = shippedHashes.size();
                const std::wstring vArrow = (static_cast<size_t>(matched) + 1 < current) ?
                                                (L" (shipped v" + std::to_wstring(matched + 1) + L" -> v" + std::to_wstring(current) + L"): ") :
                                                (L" (shipped v" + std::to_wstring(current) + L" re-rendered): ");
                if (WriteFileUtf8Atomic(path, textToWrite))
                {
                    AppendStateLog(L"hooks.log", L"[engine] " + std::wstring{ logLabel } + L" command upgraded" + vArrow + path + L"\n");
                }
                else
                {
                    // Silent-failure surfacing (the [persist-fail] convention the create path below
                    // already follows): the rewrite could not be written — a locked or read-only
                    // file, a full disk. Atomicity means the PRIOR definition is still intact and
                    // still works, and the next reconcile retries; but a definition stuck on an old
                    // version (or an old location) must not be invisible.
                    AppendStateLog(L"hooks.log", L"[persist-fail] " + std::wstring{ logLabel } + L" command upgrade" + vArrow + path + L"\n");
                }
                return path; // rewritten (or failed best-effort — the old text still works)
            }
            return path; // user-owned — never overwrite (the cog's Reinstall is the explicit escape)
        }
        ::CreateDirectoryW(configDir.c_str(), nullptr);
        ::CreateDirectoryW(commandsDir.c_str(), nullptr);
        if (!WriteFileUtf8Atomic(path, textToWrite))
        {
            AppendStateLog(L"hooks.log", L"[persist-fail] " + std::wstring{ logLabel } + L" command definition: " + path + L"\n");
            return {};
        }
        return path;
    }
    catch (...)
    {
        // Safeguard: engine init must never be derailed by this best-effort global-config write
        // (the feature simply stays dormant until a later launch succeeds).
        LogSwallowedException(L"EnsureShippedCommandFileIn");
        return {};
    }

    std::wstring EnsureShippedCommandFileIn(const std::wstring& configDir, std::wstring_view fileLeaf, const std::vector<std::string_view>& shippedHashes, std::wstring_view currentText, std::wstring_view logLabel)
    {
        // The nameless (default-name, default-location) form — the pre-customization signature.
        return EnsureShippedCommandFileCore(configDir, fileLeaf, shippedHashes, currentText, logLabel, {}, {}, {});
    }

    std::wstring EnsureShippedCommandFileNamedIn(const std::wstring& configDir, std::wstring_view defaultName, const std::vector<std::string_view>& shippedHashes, std::wstring_view currentText, std::wstring_view logLabel, std::wstring_view commandName, std::wstring_view writePath)
    {
        // Belt: the leaf is built from the name, so it MUST be a clean slug even if a caller skips
        // the settings-layer normalization (a path separator here would escape the commands dir).
        const std::wstring name = NormalizeCommandName(commandName);
        if (name.empty())
        {
            return {};
        }
        return EnsureShippedCommandFileCore(configDir, name + L".md", shippedHashes, currentText, logLabel, defaultName, name, writePath);
    }

    // COMMANDS.md §6c — the cog's "Reinstall definition files" escape hatch. The write policy above
    // NEVER overwrites a file it does not recognize, which is exactly right (a user edit sticks
    // forever) but leaves a hand-edited definition frozen on old instructions — and therefore
    // pointing at the old write location, silently ignoring the setting. This is the ONE
    // user-initiated, confirmed path that overwrites regardless of digest: same rendered text, same
    // atomic write, no identity question asked.
    std::wstring ForceReinstallShippedCommandFileNamedIn(const std::wstring& configDir, std::wstring_view defaultName, std::wstring_view currentText, std::wstring_view logLabel, std::wstring_view commandName, std::wstring_view writePath)
    try
    {
        const std::wstring name = NormalizeCommandName(commandName);
        if (configDir.empty() || name.empty() || currentText.empty())
        {
            return {};
        }
        std::wstring textToWrite = RenderShippedCommandText(currentText, defaultName, name);
        textToWrite = RenderShippedCommandWritePath(textToWrite, writePath);
        const std::wstring commandsDir = configDir + L"\\commands";
        const std::wstring path = commandsDir + L"\\" + name + L".md";
        ::CreateDirectoryW(configDir.c_str(), nullptr);
        ::CreateDirectoryW(commandsDir.c_str(), nullptr);
        if (!WriteFileUtf8Atomic(path, textToWrite))
        {
            AppendStateLog(L"hooks.log", L"[persist-fail] " + std::wstring{ logLabel } + L" command REINSTALL: " + path + L"\n");
            return {};
        }
        AppendStateLog(L"hooks.log", L"[engine] " + std::wstring{ logLabel } + L" command REINSTALLED (user-requested overwrite): " + path + L"\n");
        return path;
    }
    catch (...)
    {
        LogSwallowedException(L"ForceReinstallShippedCommandFileNamedIn");
        return {};
    }

    ShippedCommandFileState InspectShippedCommandFileNamedIn(const std::wstring& configDir, std::wstring_view defaultName, const std::vector<std::string_view>& shippedHashes, std::wstring_view currentText, std::wstring_view commandName, std::wstring_view writePath)
    try
    {
        const std::wstring name = NormalizeCommandName(commandName);
        if (configDir.empty() || name.empty() || currentText.empty() || shippedHashes.empty())
        {
            return ShippedCommandFileState::Missing;
        }
        const std::wstring path = configDir + L"\\commands\\" + name + L".md";
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return ShippedCommandFileState::Missing;
        }
        const std::string bytes = ReadCommandDefinitionBytes(path);
        if (MatchShippedCommandVersion(bytes, shippedHashes, defaultName, name) < 0)
        {
            return ShippedCommandFileState::UserOwned;
        }
        std::wstring textToWrite = RenderShippedCommandText(currentText, defaultName, name);
        textToWrite = RenderShippedCommandWritePath(textToWrite, writePath);
        return bytes == Utf16ToUtf8(textToWrite) ? ShippedCommandFileState::UpToDate : ShippedCommandFileState::OursStale;
    }
    catch (...)
    {
        // A read/hash hiccup must not be reported as "yours" (which would nudge the user to
        // Reinstall over a file we may well own) nor as "up to date" (which would hide a real
        // divergence): OursStale is the honest middle — the next reconcile re-decides from disk.
        LogSwallowedException(L"InspectShippedCommandFileNamedIn");
        return ShippedCommandFileState::OursStale;
    }

    bool RemoveShippedCommandFileNamedIn(const std::wstring& configDir, std::wstring_view defaultName, const std::vector<std::string_view>& shippedHashes, std::wstring_view logLabel, std::wstring_view commandName)
    try
    {
        const std::wstring name = NormalizeCommandName(commandName); // slug belt, like the named Ensure
        if (configDir.empty() || name.empty() || shippedHashes.empty())
        {
            return false;
        }
        const std::wstring path = configDir + L"\\commands\\" + name + L".md";
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return false; // already gone — nothing to migrate
        }
        const std::string bytes = ReadCommandDefinitionBytes(path);
        if (bytes.empty())
        {
            return false; // unreadable/empty — never delete blind (user-owned until proven ours)
        }
        // ANY shipped version counts here — INCLUDING the current one: a rename/disable must
        // migrate a fully-current pristine file away just the same as a stale one. A digest match
        // after the name + §6c write-location normalizations is proof the bytes are OURS,
        // unmodified, under whatever name and whatever configured location.
        if (MatchShippedCommandVersion(bytes, shippedHashes, defaultName, name) < 0)
        {
            return false; // user-owned (edited, or a same-named foreign command) — never delete
        }
        if (!::DeleteFileW(path.c_str()))
        {
            AppendStateLog(L"hooks.log", L"[persist-fail] " + std::wstring{ logLabel } + L" command removal (renamed/disabled): " + path + L"\n");
            return false;
        }
        AppendStateLog(L"hooks.log", L"[engine] " + std::wstring{ logLabel } + L" command removed (renamed/disabled; pristine ours): " + path + L"\n");
        return true;
    }
    catch (...)
    {
        LogSwallowedException(L"RemoveShippedCommandFileNamedIn");
        return false;
    }

    std::wstring ReconcileShippedCommandFileIn(const std::wstring& configDir, std::wstring_view defaultName, const std::vector<std::string_view>& shippedHashes, std::wstring_view currentText, std::wstring_view logLabel, std::wstring_view previouslyMaterializedName, std::wstring_view configuredName, bool enabled, std::wstring_view writePath)
    {
        const std::wstring prev = NormalizeCommandName(previouslyMaterializedName);
        std::wstring want = enabled ? NormalizeCommandName(configuredName) : std::wstring{};
        if (enabled && want.empty())
        {
            want = defaultName; // "enabled but nameless" is not a state — fall back to the default
        }
        // 1) The previously-materialized file is no longer wanted under that name (rename /
        //    disable) — migrate it away. Ours-pristine only: a file the user edited sticks
        //    forever and is simply left in place (their content, their command).
        if (!prev.empty() && prev != want)
        {
            RemoveShippedCommandFileNamedIn(configDir, defaultName, shippedHashes, logLabel, prev);
        }
        // 2) Disabled — nothing materialized (the marker goes empty; the binding side skips too).
        if (want.empty())
        {
            AppendStateLog(L"hooks.log", L"[engine] " + std::wstring{ logLabel } + L" command disabled (no definition materialized)\n");
            return {};
        }
        // 3) Materialize the configured name + write location (create-if-absent + the
        //    version-aware upgrade, which also re-renders on a changed location — §6c).
        const std::wstring path = EnsureShippedCommandFileNamedIn(configDir, defaultName, shippedHashes, currentText, logLabel, want, writePath);
        if (path.empty())
        {
            return {}; // failed write — marker stays empty, the next init just tries again
        }
        AppendStateLog(L"hooks.log", L"[engine] " + std::wstring{ logLabel } + L" command: " + path + L"\n");
        return want;
    }

    std::wstring EnsureHandoverCommandFileIn(const std::wstring& configDir)
    {
        return EnsureShippedCommandFileIn(configDir, L"handover.md", ShippedHandoverCommandHashes(), ShippedHandoverCommandText(), L"handover");
    }

    std::wstring EnsureHandoverHereCommandFileIn(const std::wstring& configDir)
    {
        return EnsureShippedCommandFileIn(configDir, L"handover-here.md", ShippedHandoverHereCommandHashes(), ShippedHandoverHereCommandText(), L"handover-here");
    }

    std::wstring EnsureHandoverStandbyCommandFileIn(const std::wstring& configDir)
    {
        return EnsureShippedCommandFileIn(configDir, L"handover-standby.md", ShippedHandoverStandbyCommandHashes(), ShippedHandoverStandbyCommandText(), L"handover-standby");
    }

    // CLAUDE_CONFIG_DIR > ~/.claude — the same resolution ClaudeProjectsDir applies (the
    // commands dir is a sibling of projects/ under the one Claude config root).
    static std::wstring ResolveClaudeCommandsBase()
    {
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
        return base;
    }

    std::wstring EnsureHandoverCommandFile()
    {
        const std::wstring base = ResolveClaudeCommandsBase();
        return base.empty() ? std::wstring{} : EnsureHandoverCommandFileIn(base);
    }

    std::wstring EnsureHandoverHereCommandFile()
    {
        const std::wstring base = ResolveClaudeCommandsBase();
        return base.empty() ? std::wstring{} : EnsureHandoverHereCommandFileIn(base);
    }

    std::wstring EnsureHandoverStandbyCommandFile()
    {
        const std::wstring base = ResolveClaudeCommandsBase();
        return base.empty() ? std::wstring{} : EnsureHandoverStandbyCommandFileIn(base);
    }

    HandoverFamilyNames ReconcileHandoverCommandFilesIn(const std::wstring& configDir, const AppSettings& settings)
    try
    {
        // Belt: heal the configured names again here (the Persistence load already does) so a
        // hand-built AppSettings can never reconcile two commands onto ONE name. The three
        // histories are digest-disjoint (test-asserted), so even a pathological marker overlap
        // can't make one command's migration delete ANOTHER's pristine file — the ours-check
        // hashes against each command's OWN history.
        std::wstring hoName = settings.commandHandoverName;
        std::wstring hhName = settings.commandHandoverHereName;
        std::wstring hsName = settings.commandHandoverStandbyName;
        ResolveCommandNameTriple(hoName, hhName, hsName);
        const std::wstring ho = ReconcileShippedCommandFileIn(configDir,
                                                              kDefaultHandoverCommandName,
                                                              ShippedHandoverCommandHashes(),
                                                              ShippedHandoverCommandText(),
                                                              L"handover",
                                                              settings.commandHandoverMaterializedName,
                                                              hoName,
                                                              settings.commandHandoverEnabled,
                                                              settings.commandHandoverWritePath);
        const std::wstring hh = ReconcileShippedCommandFileIn(configDir,
                                                              kDefaultHandoverHereCommandName,
                                                              ShippedHandoverHereCommandHashes(),
                                                              ShippedHandoverHereCommandText(),
                                                              L"handover-here",
                                                              settings.commandHandoverHereMaterializedName,
                                                              hhName,
                                                              settings.commandHandoverHereEnabled,
                                                              settings.commandHandoverWritePath);
        const std::wstring hs = ReconcileShippedCommandFileIn(configDir,
                                                              kDefaultHandoverStandbyCommandName,
                                                              ShippedHandoverStandbyCommandHashes(),
                                                              ShippedHandoverStandbyCommandText(),
                                                              L"handover-standby",
                                                              settings.commandHandoverStandbyMaterializedName,
                                                              hsName,
                                                              settings.commandHandoverStandbyEnabled,
                                                              settings.commandHandoverWritePath);
        return { ho, hh, hs };
    }
    catch (...)
    {
        // Safeguard + Rule #18. This runs at engine init from a point OUTSIDE Engine.cpp's own
        // init net, and only the leaf writers (EnsureShippedCommandFileCore /
        // RemoveShippedCommandFileNamedIn) carry function-try blocks — the name healing, the
        // marker plumbing and the log-line building above do not. An escape here would derail
        // engine init for the whole run. Report the PREVIOUS markers unchanged (the same recovery
        // the no-config-root path takes): never flip a marker over a failure we did not complete,
        // so the next init reconciles from the truth on disk.
        LogSwallowedException(L"ReconcileHandoverCommandFilesIn");
        return { settings.commandHandoverMaterializedName, settings.commandHandoverHereMaterializedName, settings.commandHandoverStandbyMaterializedName };
    }

    HandoverFamilyNames ReconcileHandoverCommandFiles(const AppSettings& settings)
    try
    {
        const std::wstring base = ResolveClaudeCommandsBase();
        if (base.empty())
        {
            // No config root resolvable — report the previous reality unchanged (never flip the
            // markers to "" over a transient env problem; nothing was migrated or written).
            return { settings.commandHandoverMaterializedName, settings.commandHandoverHereMaterializedName, settings.commandHandoverStandbyMaterializedName };
        }
        return ReconcileHandoverCommandFilesIn(base, settings);
    }
    catch (...)
    {
        // The env-resolution half of the same net (ResolveClaudeCommandsBase reads the environment
        // and builds a path); same recovery — markers unchanged, nothing claimed.
        LogSwallowedException(L"ReconcileHandoverCommandFiles");
        return { settings.commandHandoverMaterializedName, settings.commandHandoverHereMaterializedName, settings.commandHandoverStandbyMaterializedName };
    }

    // The name a /handover-family command's definition file lives under RIGHT NOW: the engine's
    // MATERIALIZED marker — DISK REALITY, never the configured name. "" == this command has no
    // definition file to act on (it is disabled this run; the marker is what the reconcile set).
    //
    // Deliberately marker-only: the configured name may be one the user just TYPED and Save has
    // not restarted into, so acting on it would materialize a file whose CommandWatch binding does
    // not exist yet — a typed /newname that does nothing while /oldname disappears. A RENAME stays
    // restart-applied (§6a), where the file and the binding move together. (A pre-§6a settings.json
    // reads the DEFAULT names as its markers, so "the file exists but the marker is empty" is not a
    // reachable state; a definition someone DELETED still has its marker and is recreated here.)
    static HandoverFamilyNames LiveHandoverCommandNames(const AppSettings& settings)
    {
        return { NormalizeCommandName(settings.commandHandoverMaterializedName),
                 NormalizeCommandName(settings.commandHandoverHereMaterializedName),
                 NormalizeCommandName(settings.commandHandoverStandbyMaterializedName) };
    }

    // COMMANDS.md §6c — re-render the LIVE definition files with the CURRENT write location (and
    // the current shipped text). This is what the cog's Save calls, and it is why the write path
    // applies to the NEXT handover with no restart: it only ever rewrites a file we RECOGNIZE
    // (EnsureShippedCommandFileNamedIn's policy is unchanged — a user-edited definition is left
    // frozen, which the cog surfaces + offers Reinstall for), and it never renames, migrates or
    // deletes anything.
    HandoverFamilyNames RefreshHandoverCommandWritePathIn(const std::wstring& configDir, const AppSettings& settings)
    try
    {
        const auto [hoName, hhName, hsName] = LiveHandoverCommandNames(settings);
        HandoverFamilyNames out;
        if (!hoName.empty())
        {
            out.handover = EnsureShippedCommandFileNamedIn(configDir, kDefaultHandoverCommandName, ShippedHandoverCommandHashes(), ShippedHandoverCommandText(), L"handover", hoName, settings.commandHandoverWritePath);
        }
        if (!hhName.empty())
        {
            out.here = EnsureShippedCommandFileNamedIn(configDir, kDefaultHandoverHereCommandName, ShippedHandoverHereCommandHashes(), ShippedHandoverHereCommandText(), L"handover-here", hhName, settings.commandHandoverWritePath);
        }
        if (!hsName.empty())
        {
            out.standby = EnsureShippedCommandFileNamedIn(configDir, kDefaultHandoverStandbyCommandName, ShippedHandoverStandbyCommandHashes(), ShippedHandoverStandbyCommandText(), L"handover-standby", hsName, settings.commandHandoverWritePath);
        }
        return out;
    }
    catch (...)
    {
        LogSwallowedException(L"RefreshHandoverCommandWritePathIn");
        return {};
    }

    HandoverFamilyNames RefreshHandoverCommandWritePath(const AppSettings& settings)
    try
    {
        const std::wstring base = ResolveClaudeCommandsBase();
        return base.empty() ? HandoverFamilyNames{} : RefreshHandoverCommandWritePathIn(base, settings);
    }
    catch (...)
    {
        LogSwallowedException(L"RefreshHandoverCommandWritePath");
        return {};
    }

    HandoverFamilyNames ReinstallHandoverCommandFilesIn(const std::wstring& configDir, const AppSettings& settings)
    try
    {
        const auto [hoName, hhName, hsName] = LiveHandoverCommandNames(settings);
        HandoverFamilyNames out;
        if (!hoName.empty())
        {
            out.handover = ForceReinstallShippedCommandFileNamedIn(configDir, kDefaultHandoverCommandName, ShippedHandoverCommandText(), L"handover", hoName, settings.commandHandoverWritePath);
        }
        if (!hhName.empty())
        {
            out.here = ForceReinstallShippedCommandFileNamedIn(configDir, kDefaultHandoverHereCommandName, ShippedHandoverHereCommandText(), L"handover-here", hhName, settings.commandHandoverWritePath);
        }
        if (!hsName.empty())
        {
            out.standby = ForceReinstallShippedCommandFileNamedIn(configDir, kDefaultHandoverStandbyCommandName, ShippedHandoverStandbyCommandText(), L"handover-standby", hsName, settings.commandHandoverWritePath);
        }
        return out;
    }
    catch (...)
    {
        LogSwallowedException(L"ReinstallHandoverCommandFilesIn");
        return {};
    }

    HandoverFamilyNames ReinstallHandoverCommandFiles(const AppSettings& settings)
    try
    {
        const std::wstring base = ResolveClaudeCommandsBase();
        return base.empty() ? HandoverFamilyNames{} : ReinstallHandoverCommandFilesIn(base, settings);
    }
    catch (...)
    {
        LogSwallowedException(L"ReinstallHandoverCommandFiles");
        return {};
    }

    HandoverFamilyFileStates InspectHandoverCommandFilesIn(const std::wstring& configDir, const AppSettings& settings)
    try
    {
        const auto [hoName, hhName, hsName] = LiveHandoverCommandNames(settings);
        HandoverFamilyFileStates out;
        out.handover = hoName.empty() ? ShippedCommandFileState::Missing :
                                        InspectShippedCommandFileNamedIn(configDir, kDefaultHandoverCommandName, ShippedHandoverCommandHashes(), ShippedHandoverCommandText(), hoName, settings.commandHandoverWritePath);
        out.here = hhName.empty() ? ShippedCommandFileState::Missing :
                                    InspectShippedCommandFileNamedIn(configDir, kDefaultHandoverHereCommandName, ShippedHandoverHereCommandHashes(), ShippedHandoverHereCommandText(), hhName, settings.commandHandoverWritePath);
        out.standby = hsName.empty() ? ShippedCommandFileState::Missing :
                                       InspectShippedCommandFileNamedIn(configDir, kDefaultHandoverStandbyCommandName, ShippedHandoverStandbyCommandHashes(), ShippedHandoverStandbyCommandText(), hsName, settings.commandHandoverWritePath);
        return out;
    }
    catch (...)
    {
        LogSwallowedException(L"InspectHandoverCommandFilesIn");
        return { ShippedCommandFileState::OursStale, ShippedCommandFileState::OursStale, ShippedCommandFileState::OursStale };
    }

    HandoverFamilyFileStates InspectHandoverCommandFiles(const AppSettings& settings)
    try
    {
        const std::wstring base = ResolveClaudeCommandsBase();
        return base.empty() ? HandoverFamilyFileStates{} :
                              InspectHandoverCommandFilesIn(base, settings);
    }
    catch (...)
    {
        LogSwallowedException(L"InspectHandoverCommandFiles");
        return { ShippedCommandFileState::OursStale, ShippedCommandFileState::OursStale, ShippedCommandFileState::OursStale };
    }

    std::wstring DeriveHandoverSuccessorTitle(std::wstring_view originTitle, std::wstring_view findRegex, std::wstring_view replacement)
    {
        if (findRegex.empty())
        {
            return {}; // rewrite not configured — the caller's default "(handover)" naming applies
        }
        bool applied = false;
        std::wstring out = RegexReplace(originTitle, findRegex, replacement, /*caseInsensitive*/ false, &applied);
        if (!applied)
        {
            return {}; // invalid pattern, or it matched nowhere in THIS title — default naming
        }
        // A title is never blank (Rule #11): trim, refuse whitespace-only, and apply the same
        // degenerate cap DeriveSessionTitle uses (252 + "..." == 255 — a safety net, not a look).
        size_t b = 0;
        size_t e = out.size();
        while (b < e && (out[b] == L' ' || out[b] == L'\t' || out[b] == L'\r' || out[b] == L'\n'))
        {
            ++b;
        }
        while (e > b && (out[e - 1] == L' ' || out[e - 1] == L'\t' || out[e - 1] == L'\r' || out[e - 1] == L'\n'))
        {
            --e;
        }
        out = out.substr(b, e - b);
        if (out.empty())
        {
            return {}; // the rewrite deleted everything — default naming beats a blank title
        }
        if (out.size() > 255)
        {
            out = out.substr(0, 252) + L"...";
        }
        return out;
    }

    HandoverArgsHints ParseHandoverArgsHints(std::wstring_view args, std::wstring_view launchModelsSpec)
    {
        HandoverArgsHints hints;
        const auto models = ParseLaunchModels(launchModelsSpec);
        // The characters-only fold both sides of the comparison go through: lowercase [a-z0-9],
        // everything else dropped — "Fable 5" == "fable5", "claude-fable-5" == "claudefable5",
        // "[fable]" == "fable". Caseless + separator-blind by construction.
        const auto fold = [](std::wstring_view s) {
            std::wstring out;
            out.reserve(s.size());
            for (wchar_t c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    out.push_back(static_cast<wchar_t>(c - L'A' + L'a'));
                }
                else if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))
                {
                    out.push_back(c);
                }
            }
            return out;
        };
        // First matching entry in LIST ORDER wins (the user's own ordering is the tie-break —
        // same rule the launch-model submenus render by). Returns the model ID to launch with.
        // An EMPTY launchModels list simply never matches — the TITLE slot must still parse
        // (with no models configured, every leading bracket is a title).
        const auto matchHint = [&](const std::wstring& hint) -> const std::wstring* {
            if (hint.empty())
            {
                return nullptr;
            }
            for (const auto& [display, id] : models)
            {
                if (fold(display).find(hint) != std::wstring::npos || fold(id).find(hint) != std::wstring::npos)
                {
                    return &id;
                }
            }
            return nullptr;
        };
        // A bracket body -> the explicit successor TITLE: VERBATIM (any characters — the title is
        // the user's to spell, "[my asd \n !!_ title]" keeps its literal backslash-n), edge-trimmed,
        // "" when blank (no usable title — Rule #11's never-empty invariant), degenerate-capped at
        // 255 (252 + "...") like every other title path (DeriveHandoverSuccessorTitle).
        const auto titleFromBody = [](std::wstring_view body) -> std::wstring {
            size_t b = 0;
            size_t e = body.size();
            while (b < e && (body[b] == L' ' || body[b] == L'\t' || body[b] == L'\r' || body[b] == L'\n'))
            {
                ++b;
            }
            while (e > b && (body[e - 1] == L' ' || body[e - 1] == L'\t' || body[e - 1] == L'\r' || body[e - 1] == L'\n'))
            {
                --e;
            }
            std::wstring t{ body.substr(b, e - b) };
            if (t.size() > 255)
            {
                t = t.substr(0, 252) + L"...";
            }
            return t;
        };
        // Only the FIRST LINE's leading portion is a hint position — a model word (or a bracket)
        // deep inside a multi-line briefing context must never hijack the successor. (substr
        // clamps an npos count natively — no-newline args read whole; std::min is a windows.h
        // macro hazard in this TU.)
        std::wstring_view line = args.substr(0, args.find(L'\n'));
        size_t pos = 0;
        while (pos < line.size() && (line[pos] == L' ' || line[pos] == L'\t' || line[pos] == L'\r'))
        {
            ++pos;
        }
        line.remove_prefix(pos);
        if (line.empty())
        {
            return hints;
        }
        // The optional SECOND slot: a bracketed title right after a recognized model hint —
        // "[fable] [my title] do x", "fable 5 [my title] fix x". Only the VERY NEXT token is
        // consulted (skip whitespace, require '['); absent/unterminated -> no title, the text is
        // just context ("[x]" deeper in the sentence never becomes a title).
        const auto titleBracketAfter = [&](size_t from) -> std::wstring {
            size_t p = from;
            while (p < line.size() && (line[p] == L' ' || line[p] == L'\t' || line[p] == L'\r'))
            {
                ++p;
            }
            if (p >= line.size() || line[p] != L'[')
            {
                return {};
            }
            const size_t close = line.find(L']', p + 1);
            if (close == std::wstring_view::npos)
            {
                return {};
            }
            return titleFromBody(line.substr(p + 1, close - p - 1));
        };
        if (line.front() == L'[')
        {
            const size_t close = line.find(L']', 1);
            if (close == std::wstring_view::npos)
            {
                return hints; // unterminated — plain context, no hints, never an error
            }
            // The explicit bracketed MODEL form: the bracket body is the hint, whole. An absurdly
            // long body (nobody types a 64+-char model hint — no folded entry could contain it) is
            // never even tried as a model; like a no-match it falls THROUGH to the title slot.
            if (close <= 65)
            {
                if (const auto* id = matchHint(fold(line.substr(1, close - 1))))
                {
                    hints.modelId = *id;
                    hints.title = titleBracketAfter(close + 1);
                    return hints;
                }
            }
            // The first bracket matched NO model -> it IS the explicit successor title (the
            // fallback semantics: "/handover-standby [my title] …" titles without picking).
            hints.title = titleFromBody(line.substr(1, close - 1));
            return hints;
        }
        // The bare MODEL form: the FIRST word must hit on its own (folded >= 3 chars, so a stray
        // short word can never accidentally pick a model), then greedily extend a word at a time
        // (up to 4) while the longer fold still matches — longest match wins, so "fable 5 fix x"
        // resolves "fable5" and stops at "fable5fix". bestEnd tracks where the MATCHED words end
        // (NOT the probe word that broke the extension) — the title slot starts there, so
        // "fable 5 [my title] fix x" finds its bracket right after the "5".
        std::wstring concat;
        std::wstring best;
        size_t bestEnd = 0;
        size_t at = 0;
        for (int words = 1; words <= 4; ++words)
        {
            while (at < line.size() && (line[at] == L' ' || line[at] == L'\t' || line[at] == L'\r'))
            {
                ++at;
            }
            if (at >= line.size())
            {
                break;
            }
            const size_t wordEnd = line.find_first_of(L" \t\r", at); // npos == the last word (substr clamps)
            concat += fold(line.substr(at, wordEnd - at));
            at = wordEnd == std::wstring_view::npos ? line.size() : wordEnd;
            if (words == 1 && concat.size() < 3)
            {
                return hints; // too short to be a deliberate bare hint ("a", "do", "5")
            }
            if (const auto* id = matchHint(concat))
            {
                best = *id;
                bestEnd = at;
            }
            else if (words == 1)
            {
                return hints; // the FIRST word must match — no scanning deeper into the sentence
            }
            else
            {
                break; // the extension stopped matching — keep the longest hit
            }
        }
        if (!best.empty())
        {
            hints.modelId = best;
            hints.title = titleBracketAfter(bestEnd);
        }
        return hints;
    }

    std::wstring PickModelFromArgsHint(std::wstring_view args, std::wstring_view launchModelsSpec)
    {
        // The model-only view — ONE parser (ParseHandoverArgsHints), so the two can never drift.
        return ParseHandoverArgsHints(args, launchModelsSpec).modelId;
    }

    std::wstring ReadHandoverDocumentPrompt(const std::wstring& mdPath)
    try
    {
        // The successor's first user message IS the handover document (COMMANDS.md §5 — "inject
        // the content AS IF it is the user message"), IN FULL — never truncated: the caller picks
        // commandline vs paste-injection by PsEscapedCost. The 4 MiB cap is a sanity ceiling only
        // (nothing that size is a handover briefing); a beyond-cap file returns "" -> the pointer
        // fallback, never a silent partial. Read cap+1 so "hit the cap" is detectable.
        constexpr size_t kReadCapBytes = 4 * 1024 * 1024;
        std::string bytes;
        {
            std::ifstream f(std::filesystem::path{ mdPath }, std::ios::binary);
            if (!f)
            {
                return {}; // unreadable — the caller falls back to the pointer-style prompt
            }
            bytes.resize(kReadCapBytes + 1);
            f.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<size_t>(f.gcount()));
        }
        if (bytes.empty())
        {
            return {};
        }
        if (bytes.size() > kReadCapBytes)
        {
            AppendStateLog(L"hooks.log", L"[handover] document exceeds the 4 MiB sanity cap - pointer fallback (never a silent partial): " + mdPath + L"\n");
            return {};
        }
        // UTF-8 BOM strip (the Write tool never emits one, but a user-edited md may carry it).
        if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
        {
            bytes.erase(0, 3);
        }
        const int need = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (need <= 0)
        {
            return {};
        }
        std::wstring wide(static_cast<size_t>(need), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), need);
        // Normalize for a single PS argv + a clean transcript: CRLF -> LF (drop \r), and drop the
        // remaining C0 control chars except \n and \t — never legitimate markdown, and stray
        // controls in an argv gunk the PS string and the recorded user message.
        std::wstring norm;
        norm.reserve(wide.size());
        for (const wchar_t c : wide)
        {
            if (c == L'\r' || (c < 0x20 && c != L'\n' && c != L'\t'))
            {
                continue;
            }
            norm.push_back(c);
        }
        size_t b = 0;
        size_t e = norm.size();
        while (b < e && (norm[b] == L' ' || norm[b] == L'\t' || norm[b] == L'\n'))
        {
            ++b;
        }
        while (e > b && (norm[e - 1] == L' ' || norm[e - 1] == L'\t' || norm[e - 1] == L'\n'))
        {
            --e;
        }
        norm = norm.substr(b, e - b);
        if (norm.empty())
        {
            return {}; // whitespace-only document — pointer fallback beats an empty first message
        }
        return norm; // FULL content — the caller tiers the delivery channel (never truncates)
    }
    catch (...)
    {
        LogSwallowedException(L"ReadHandoverDocumentPrompt");
        return {}; // the caller falls back to the pointer-style prompt
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

    std::wstring NpmClaudeLauncherInPath(const std::vector<std::wstring>& pathDirs, std::wstring_view excludeDir)
    {
        // The legacy npm/Node launcher: claude.cmd (cmd/PowerShell) or claude.bat, in PATH order,
        // returned by FULL PATH so `<launcher> install` runs the REAL npm claude (never our shim, never
        // a PATHEXT surprise). Skip `excludeDir` (OUR prepended shim dir) so the shim's own claude.cmd,
        // which sits first on PATH, is never mistaken for a real npm install. [Agentmaster]
        for (const auto& raw : pathDirs)
        {
            const std::wstring dir = NormalizeDir(raw);
            if (dir.empty())
            {
                continue;
            }
            if (!excludeDir.empty() &&
                ::CompareStringOrdinal(dir.c_str(), static_cast<int>(dir.size()), excludeDir.data(), static_cast<int>(excludeDir.size()), TRUE) == CSTR_EQUAL)
            {
                continue; // our shim dir — not a real npm claude (both are NormalizeDir-shaped)
            }
            if (FileExistsNotDir(dir + L"claude.cmd"))
            {
                return dir + L"claude.cmd";
            }
            if (FileExistsNotDir(dir + L"claude.bat"))
            {
                return dir + L"claude.bat";
            }
        }
        return {};
    }

    std::wstring NpmClaudeLauncherOnPath()
    {
        // Gather PATH dirs from the environment (same split as ResolveClaudeExe) and exclude our own
        // shim dir (<stateDir>\shim, NormalizeDir-shaped so the case-insensitive compare in the core
        // matches), then delegate to the testable core.
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
        const std::wstring shimDir = NormalizeDir(AgentmasterStateDir() + L"\\shim");
        return NpmClaudeLauncherInPath(dirs, shimDir);
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
            // Forensics: returning {} silently disables the transparent `claude` PATH shim for the
            // whole run — a hand-typed claude then fires ZERO hooks and only the Fleet Observer finds
            // it. That "why is my + tab not wiring up" mystery should never be un-diagnosable.
            LogSwallowedException(L"MaterializeClaudeShim (create shim dir)");
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

    std::vector<std::pair<std::wstring, std::wstring>> ParseLaunchModels(std::wstring_view spec)
    {
        // The same lenient splitting as ParseEnvAssignments (newline / ';' separated, '\r' a
        // separator too so a TextBox's \r-normalized text parses, blanks + '#' comments skipped) —
        // but the entry shape is "Display name | model-id" instead of NAME=VALUE, and a bare
        // token with no '|' is BOTH (a raw model id still lists, labeled as itself).
        const auto isSep = [](wchar_t c) { return c == L';' || c == L'\n' || c == L'\r'; };

        std::vector<std::pair<std::wstring, std::wstring>> out;
        size_t i = 0;
        while (i <= spec.size() && out.size() < kMaxLaunchModels)
        {
            size_t sep = i;
            while (sep < spec.size() && !isSep(spec[sep]))
            {
                ++sep;
            }
            const std::wstring_view entry = EnvTrim(spec.substr(i, sep - i));
            if (!entry.empty() && entry.front() != L'#')
            {
                const size_t bar = entry.find(L'|');
                const std::wstring_view name = EnvTrim(bar == std::wstring_view::npos ? entry : entry.substr(0, bar));
                const std::wstring_view id = EnvTrim(bar == std::wstring_view::npos ? entry : entry.substr(bar + 1));
                if (!name.empty() && !id.empty())
                {
                    out.emplace_back(std::wstring{ name }, std::wstring{ id });
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

    bool LaunchModelsAreSupersededDefault(std::wstring_view spec)
    {
        const auto entries = ParseLaunchModels(spec);
        if (entries.empty())
        {
            // "" (or an all-comment/blank spec) is the deliberate "no models — the submenus offer
            // just Default". Never ours to replace, and it must not match a superseded list that
            // happened to parse to nothing either.
            return false;
        }
        for (const auto& superseded : kSupersededLaunchModels)
        {
            if (entries == ParseLaunchModels(superseded))
            {
                return true;
            }
        }
        return false;
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

    EnvLexResult LexLaunchModelsText(std::wstring_view text)
    {
        // The launch-models twin of LexEnvText (see the header). Verdicts MUST mirror
        // ParseLaunchModels: what it accepts is Ok/Warn-listed here, what it skips is Error, what
        // its kMaxLaunchModels cap drops is Warn-dropped — the border/status the UI paints from
        // this must never disagree with what the submenus actually offer.
        EnvLexResult r;
        std::unordered_map<std::wstring, uint32_t> seenNames; // fold(display name) of prior listed entries -> line no
        size_t listed = 0; // entries the parser ACCEPTS (Ok + listed Warns) — its cap counter
        uint32_t lineNo = 0;
        size_t i = 0;

        const auto fold = [&](std::wstring_view v) {
            const auto up = [](wchar_t c) { return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c; };
            std::wstring f{ v };
            for (auto& c : f)
            {
                c = up(c);
            }
            return f;
        };
        const auto note = [&](EnvLineKind k, const std::wstring& msg, uint32_t at, bool offered) {
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
            // Unlike the env lexer, `ok` = the OFFERED model count (Ok + listed Warns), so the
            // status line's "N models" is the true size of every picker submenu.
            if (offered)
            {
                ++r.ok;
            }
        };

        // Walk '\n' lines for the line NUMBERS, then split each line on the parser's remaining
        // intra-line separators (';' / a stray '\r' from a paste) so every entry the parser sees
        // is lexed as its own diagnostic — carrying its line's number.
        while (i <= text.size())
        {
            const size_t nl = text.find(L'\n', i);
            const size_t end = (nl == std::wstring_view::npos) ? text.size() : nl;
            ++lineNo;
            const std::wstring_view line = text.substr(i, end - i);

            size_t j = 0;
            while (j <= line.size())
            {
                size_t sep = j;
                while (sep < line.size() && line[sep] != L';' && line[sep] != L'\r')
                {
                    ++sep;
                }
                const std::wstring_view entry = EnvTrim(line.substr(j, sep - j));
                EnvLineDiag d;
                d.line = lineNo;
                bool offered = false;
                if (entry.empty() || entry.front() == L'#')
                {
                    d.kind = EnvLineKind::Ignored;
                }
                else
                {
                    const size_t bar = entry.find(L'|');
                    const std::wstring name{ EnvTrim(bar == std::wstring_view::npos ? entry : entry.substr(0, bar)) };
                    const std::wstring id{ EnvTrim(bar == std::wstring_view::npos ? entry : entry.substr(bar + 1)) };
                    d.name = name;
                    if (name.empty() && id.empty())
                    {
                        d.kind = EnvLineKind::Error;
                        d.message = L"empty entry (expected Display name | model-id)";
                    }
                    else if (name.empty())
                    {
                        d.kind = EnvLineKind::Error;
                        d.message = L"missing display name (text before '|')";
                    }
                    else if (id.empty())
                    {
                        d.kind = EnvLineKind::Error;
                        d.message = L"missing model id (text after '|')";
                    }
                    else if (listed >= kMaxLaunchModels)
                    {
                        // The parser's cap: entry #33+ is never consumed — valid or not, it is DROPPED.
                        d.kind = EnvLineKind::Warn;
                        d.message = L"past the " + std::to_wstring(kMaxLaunchModels) + L"-model cap (not offered)";
                    }
                    else
                    {
                        ++listed;
                        offered = true;
                        if (const auto it = seenNames.find(fold(name)); it != seenNames.end())
                        {
                            d.kind = EnvLineKind::Warn;
                            d.message = L"duplicate name \"" + name + L"\" (also line " + std::to_wstring(it->second) + L" \x2014 both will be listed)";
                        }
                        else if (id.find_first_of(L" \t") != std::wstring::npos)
                        {
                            d.kind = EnvLineKind::Warn;
                            d.message = L"model id \"" + id + L"\" contains spaces \x2014 check it (it launches quoted)";
                            seenNames.emplace(fold(name), lineNo);
                        }
                        else
                        {
                            d.kind = EnvLineKind::Ok;
                            seenNames.emplace(fold(name), lineNo);
                        }
                    }
                }
                note(d.kind, d.message, d.line, offered);
                r.lines.push_back(std::move(d));
                if (sep >= line.size())
                {
                    break;
                }
                j = sep + 1;
            }

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
    // Shared spawn PRELUDE: everything that must be true of the world before a managed claude is
    // started in `workingDir`. Today that is only the workspace-trust seed — the one thing that,
    // left undone, parks the new tab on a modal nobody is there to answer (ClaudeSpawn.h).
    // Best-effort by construction: a failure here costs one manual click, never the launch.
    //
    // ⚠ This is a WRITE to ~/.claude.json — one of only two mutations we make outside the profile —
    // so it belongs to the LAUNCH SEAM, never to a spec BUILDER. It used to be called from inside
    // BuildClaudeSpawn/BuildClaudeRestartSpec, which silently made "describe a launch" mean "mutate
    // the user's global config": the standalone test harness builds specs for FAKE dirs with a
    // default-constructed AppSettings (trustWorkspaceOnLaunch defaults ON), so every run seeded a
    // phantom trusted project into the developer's REAL ~/.claude.json and took Claude's config lock
    // while a live claude might be writing. Keep the builders PURE — the three real launch seams in
    // TerminalPage.AgentSessions.cpp call this explicitly. [Agentmaster]
    void PrepareManagedClaudeWorkspace(std::wstring_view workingDir, const AppSettings& settings)
    {
        if (settings.trustWorkspaceOnLaunch)
        {
            EnsureClaudeWorkspaceTrusted(workingDir);
        }
    }

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

    ClaudeSpawnSpec BuildClaudeSpawn(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName, std::wstring_view resumeSessionId, const AppSettings& settings, std::wstring_view forkFromSessionId, std::wstring_view claudeLauncher, std::wstring_view forkIntoSessionId, std::wstring_view modelOverride, std::wstring_view initialPrompt)
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
        spec.commandline = BuildClaudeCommandline(settingsFwd, spec.sessionId, resume, settings.skipPermissions, forkFromSessionId, claudeLauncher, modelOverride, initialPrompt);

        // NOTE: no workspace-trust seed here — building a spec must stay PURE (no writes to
        // ~/.claude.json). The launch seam calls PrepareManagedClaudeWorkspace before spawning.
        // The cog's global env + the hook-correlation vars, applied to every session (CCMGR_* always win).
        AppendManagedClaudeEnv(spec, settings);
        return spec;
    }

    ClaudeSpawnSpec BuildClaudeRestartSpec(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName, std::wstring_view sessionId, const AppSettings& settings, std::wstring_view claudeLauncher, std::wstring_view forkParentId)
    {
        // Relaunch an EXISTING managed conversation IN PLACE (the tab's connection died and the user hit
        // "Restart session"). Unlike BuildClaudeSpawn this NEVER mints a new id. The relaunch form is
        // derived from the CURRENT on-disk state:
        //   * a transcript exists -> RESUME it (claude --resume <id>), continuing the conversation;
        //   * no transcript, but the session is a fork whose SOURCE transcript still exists -> RE-FORK
        //     from the source INTO the same id (claude --resume <parent> --fork-session --session-id
        //     <id> — the [restore->refork] recipe). A never-messaged fork writes NO transcript until its
        //     first turn, so a plain fresh relaunch would silently swap the forked branch for an EMPTY
        //     conversation (the reported "restart a fork -> a new claude session, not the current one").
        //   * neither             -> a FRESH launch that REUSES <id> (claude --session-id <id>). A
        //     never-prompted / early-crashed non-fork wrote no transcript, so reusing the id is a legit
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
        // Own transcript wins over the fork link (a fork that produced content is resumed, never
        // re-forked off its now-divergent source); a vanished source falls through to fresh.
        std::wstring forkFrom;
        if (!resume && !forkParentId.empty() && ClaudeConversationExists(forkParentId))
        {
            forkFrom = std::wstring{ forkParentId };
        }
        const auto settingsFwd = ToForwardSlashes(settingsPath);
        spec.commandline = BuildClaudeCommandline(settingsFwd, spec.sessionId, resume, settings.skipPermissions, forkFrom, claudeLauncher);

        // NOTE: no workspace-trust seed here — see BuildClaudeSpawn. The restart seam calls
        // PrepareManagedClaudeWorkspace before rebuilding the connection.
        AppendManagedClaudeEnv(spec, settings);
        return spec;
    }
}
