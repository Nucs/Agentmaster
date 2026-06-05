// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the Fleet Observer's data models (doc/agentmaster/OBSERVER.md §4–§5b).
//
// The observer is the PULL half of the state engine: a process- and transcript-driven survey
// that detects + correlates + enriches every Claude session out-of-band (PEB / Toolhelp /
// filesystem — no hooks, no shim, nothing the user can feel), feeding the one SessionRegistry
// beneath the lossy hook PUSH (HooksBridge). These are the plain-C++ (no-WinRT) structs the two
// engine lanes (ProcessInspect + ProcessObserver) and the per-window UI probe exchange.
//
// Header-only + standalone (no SessionModels.h dependency) so it compiles into the test harness.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Agentmaster
{
    // What a tab is DOING right now, derived from the deepest meaningful descendant of the tab's
    // shell: claude.exe -> ClaudeCode, codex.exe -> Codex (acknowledged only), else the shell
    // image -> Powershell / Cmd, else Other. (OBSERVER.md §4)
    enum class TabActivity
    {
        Unknown,
        Powershell,
        Cmd,
        ClaudeCode,
        Codex,
        Other
    };

    // Who HOSTS a claude: Agentmaster (carries our AM_SESSION stamp; correlate + bind),
    // WindowsTerminal (a WT_SESSION but not our AM_SESSION; external — acknowledge, never bind),
    // Other (neither; a bare console). (OBSERVER.md §4 / §7 classification)
    enum class RunningApp
    {
        Unknown,
        Agentmaster,
        WindowsTerminal,
        Other
    };

    // Raw facts read out-of-band from one claude.exe (S-lane, ~15 µs/process). The PEB reads
    // (cwd/cmdline/env) plus the parsed flags Claude's command line + CLAUDE_* env expose.
    // (OBSERVER.md §5a)
    struct ClaudeProcessFacts
    {
        uint32_t pid{};
        uint32_t parentPid{};
        int64_t startUnixMs{}; // GetProcessTimes(creation)
        std::wstring wtSession; // env WT_SESSION   (exact tab / ConPTY id)
        std::wstring amSession; // env AM_SESSION   (our ownership stamp; empty if external)
        std::wstring cwd; // PEB CurrentDirectory (tracks cd across relaunch)
        std::wstring commandline; // PEB CommandLine
        // parsed from commandline + CLAUDE_* env:
        std::wstring model; // --model / ANTHROPIC_MODEL / CLAUDE_CODE_*MODEL*
        std::wstring effort; // --effort / CLAUDE_CODE_EFFORT_LEVEL
        std::wstring permissionMode; // --permission-mode
        std::wstring resumeTarget; // --resume <path/id>
        std::wstring sessionIdArg; // --session-id <id>  (when explicitly passed)
        bool background{}; // CLAUDE_CODE_SESSION_KIND=bg / CLAUDE_BG_* / "daemon run"
        std::wstring sessionName; // CLAUDE_CODE_SESSION_NAME (bg jobs)
        bool alive{ true };
        RunningApp runningApp{ RunningApp::Unknown };
    };

    // One correlated tab -> claude -> session row (the CorrelationTable; published to the UI lane).
    // (OBSERVER.md §5a)
    struct CorrelationRow
    {
        std::wstring wtSession; // key
        uint32_t claudePid{};
        std::wstring cwd;
        std::wstring sessionId; // resolved from transcript; EMPTY until the first prompt (§11d)
        RunningApp runningApp{ RunningApp::Unknown };
        bool alive{ true };
        int64_t observedUnixMs{};
    };

    // Per-tab activity for EVERY tab, claude or not (the TabActivityTable; published to the UI
    // lane). (OBSERVER.md §5a)
    struct TabActivityRow
    {
        std::wstring wtSession; // key
        uint32_t shellPid{};
        TabActivity activity{ TabActivity::Unknown };
        std::wstring image; // foreground / shell image leaf ("pwsh.exe", "claude.exe", ...)
        std::wstring cwd; // for Powershell / Cmd / ClaudeCode
        bool busy{}; // shell has a running child (a command in progress)
        std::wstring sessionId; // when activity == ClaudeCode (mirror of the CorrelationRow)
        int64_t observedUnixMs{};
    };

    // UI -> engine: this window's tabs, published each probe tick. (OBSERVER.md §5a / §10)
    struct TabRosterEntry
    {
        std::wstring wtSession; // ITerminalConnection::SessionId() (GuidToPlainString, lower)
        uint32_t shellPid{}; // GetProcessId((HANDLE)ConptyConnection::RootProcessHandle())
        bool bound{}; // already has an injector in this window
    };

    // What the S-lane upserts into the registry for an OUR claude (mirrors HookMessage's role).
    // (OBSERVER.md §5b)
    struct ObservedClaude
    {
        std::wstring sessionId; // key (may be empty until the transcript exists — see §11d)
        std::wstring tabToken; // == wtSession (SessionInfo::tabToken)
        std::wstring amSession;
        std::wstring cwd;
        uint32_t pid{};
        RunningApp runningApp{ RunningApp::Unknown };
        bool background{};
        std::wstring model, effort, permissionMode, sessionName;
        int64_t observedUnixMs{};
    };
}
