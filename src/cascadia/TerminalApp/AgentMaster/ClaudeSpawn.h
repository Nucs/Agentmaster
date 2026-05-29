// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the recipe for launching a Claude Code session wired for hooks.
//
// A session is a real `claude.exe` on a ConPTY (DESIGN §5). To make it observable without
// screen-scraping, every spawn:
//   * gets a fresh UUID used as BOTH Claude's --session-id AND our CCMGR_SESSION_ID env,
//     so hook payloads correlate back to the SessionRegistry record (HOOKS.md);
//   * points `claude --settings <file>` at a shared hooks config that forwards the events
//     we consume to a PowerShell forwarder, which posts a flat wire line (HookWire.h) to
//     our named pipe (CCMGR_HOOK_PIPE).
//
// The hooks config + forwarder are identical for every session (session id and pipe come
// from the environment at runtime), so they are materialized once into the app state dir.
//
// The string-building helpers are pure and unit-tested; only Materialize*/BuildClaudeSpawn
// touch the filesystem / OS.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Agentmaster
{
    struct ClaudeSpawnSpec
    {
        std::wstring sessionId; // lowercase hyphenated UUID (== --session-id == CCMGR_SESSION_ID)
        std::wstring commandline; // e.g. claude --settings "<file>" --session-id <uuid>
        std::wstring workingDir; // the M axis
        std::wstring title; // display title
        std::wstring pipeName; // \\.\pipe\agentmaster.<pid>
        std::wstring settingsPath; // materialized hooks settings file (backslash form)
        std::wstring forwarderPath; // materialized forwarder script (backslash form)
        std::vector<std::pair<std::wstring, std::wstring>> env; // child env overrides
    };

    // --- pure helpers (no filesystem / OS state) ---

    // Escape a string for embedding inside a JSON string literal.
    std::wstring JsonEscape(std::wstring_view s);

    // The PowerShell forwarder script content. Static across sessions: it reads
    // CCMGR_SESSION_ID + CCMGR_HOOK_PIPE from the environment and the hook JSON from stdin,
    // then writes one wire line to the pipe. `-Event <Name>` selects the event.
    std::wstring BuildForwarderScript();

    // The Claude hooks settings JSON wiring each consumed event to the forwarder.
    // `forwarderPath` should already be in a JSON-friendly (forward-slash) form.
    std::wstring BuildHooksSettingsJson(std::wstring_view forwarderPath);

    // Assemble the claude command line. `settingsPath` should be forward-slash form.
    // resume=false: a fresh session  -> claude --settings "<f>" --session-id <id>
    // resume=true : resume an existing conversation -> claude --resume <id> --settings "<f>"
    std::wstring BuildClaudeCommandline(std::wstring_view settingsPath, std::wstring_view sessionId, bool resume);

    // Convert backslashes to forward slashes (safe inside double-quoted args + JSON).
    std::wstring ToForwardSlashes(std::wstring_view path);

    // --- OS-touching ---

    // %LOCALAPPDATA%\Agentmaster (falls back to %TEMP%\Agentmaster). Created if absent.
    std::wstring AgentmasterStateDir();

    // Append a line to <AgentmasterStateDir>\<fileLeaf> as UTF-8. Thread-safe, best-effort
    // (swallows all I/O errors). Used as M5's live verification surface for hook -> state.
    void AppendStateLog(std::wstring_view fileLeaf, std::wstring_view line);

    // A fresh lowercase hyphenated UUID (CoCreateGuid).
    std::wstring NewSessionId();

    // True iff Claude has a resumable conversation transcript for `sessionId` — i.e. a file
    // <claude-config>/projects/<encoded-cwd>/<sessionId>.jsonl exists. Session ids are unique
    // UUIDs, so we search across all project dirs instead of reproducing Claude's cwd
    // encoding. Restore uses this to choose `--resume` vs. a fresh start: a session that was
    // opened but never received a prompt has NO transcript, and `claude --resume <id>` on it
    // fails with "No conversation found" (the tab would die with exit code 1).
    bool ClaudeConversationExists(std::wstring_view sessionId);

    // Write the shared forwarder + hooks settings into `stateDir`. Idempotent (overwrites
    // so they always match the running build). Returns {settingsPath, forwarderPath} in
    // backslash form. Throws nothing meaningful for the caller; returns empty paths on I/O
    // failure.
    std::pair<std::wstring, std::wstring> MaterializeSharedHookFiles(const std::wstring& stateDir);

    // Build a complete spawn spec and ensure the shared hook files exist. `pipeName` is the
    // live HooksBridge pipe (HookPipeName(pid)). If `resumeSessionId` is non-empty, the spec
    // RESUMES that conversation (claude --resume <id>) and reuses the id; otherwise a fresh
    // id is generated. The id is always exported as CCMGR_SESSION_ID for hook correlation.
    ClaudeSpawnSpec BuildClaudeSpawn(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName, std::wstring_view resumeSessionId = L"");
}
