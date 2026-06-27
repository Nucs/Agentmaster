// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
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

    // Which coding agent a row/session describes. The engine grew up Claude-only; Codex support
    // (OBSERVER.md §19-Q3, Phase C1) is OBSERVE-ONLY — a Codex row is enriched out-of-band from its
    // rollout transcript and surfaced in the External group, but never adopted/driven (that is a
    // later phase). `Claude` is the default so every pre-existing row/struct is unchanged.
    enum class AgentKind
    {
        Claude,
        Codex
    };

    // The turn state of an OBSERVED Codex session, derived PULL-only from its rollout tail (Phase C2,
    // OBSERVER.md §19-Q3 follow-up). The rollout's turn lifecycle is explicit + unambiguous (unlike
    // Claude's stop_reason heuristics): `task_started` opens a turn -> Running; `task_complete` /
    // `turn_aborted` / `thread_rolled_back` close it -> Waiting; no turn yet -> Idle. Codex records NO
    // approval/permission/error event in the rollout (verified across the live corpus), so
    // NeedsApproval / Error are deliberately NOT here — they are not PULL-derivable (that is C3's
    // PUSH hooks). A 3-state observe-only floor; `Unknown` = the rollout has not been read yet.
    enum class CodexState
    {
        Unknown,
        Idle,
        Running,
        Waiting
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
        // PE subsystem of the main image (IMAGE_SUBSYSTEM_*): 3 = console (the Claude Code CLI),
        // 2 = GUI (the Claude DESKTOP Electron app — same "Claude.exe" leaf name, NOT a CLI session),
        // 0 = undeterminable (denied / elevated / WOW64). Lets the census drop the desktop app + its
        // renderer/gpu/utility children, which run with cwd C:\WINDOWS\system32 (OBSERVER.md §5a).
        uint16_t subsystem{};
        bool alive{ true };
        RunningApp runningApp{ RunningApp::Unknown };
    };

    // Raw facts read out-of-band from one codex.exe (OpenAI Codex CLI). The Codex analog of
    // ClaudeProcessFacts (OBSERVER.md §19-Q3, Phase C1). Codex diverges from Claude in three ways
    // the comments below flag: (a) its config home is CODEX_HOME (else ~/.codex), not ~/.claude;
    // (b) it canNOT pin a session id at launch (no --session-id) — the id is auto-minted and lives
    // in the date-sharded rollout file; (c) model/effort/sandbox/approval usually come from
    // ~/.codex/config.toml, so the COMMAND LINE is often bare (the authoritative values are read
    // from the rollout's turn_context instead). All reads are out-of-band; Codex is observe-only.
    struct CodexProcessFacts
    {
        uint32_t pid{};
        uint32_t parentPid{};
        int64_t startUnixMs{}; // GetProcessTimes(creation) — the rollout ctime tie-break anchor
        std::wstring wtSession; // env WT_SESSION   (exact tab / ConPTY id — same correlation key as Claude)
        std::wstring amSession; // env AM_SESSION   (our ownership stamp; empty for a hand-typed/external codex)
        std::wstring codexHome; // env CODEX_HOME   (the rollout root; empty => default ~/.codex)
        std::wstring cwd; // PEB CurrentDirectory
        std::wstring commandline; // PEB CommandLine
        // parsed from the command line (often bare — config.toml carries the real values, read from
        // the rollout turn_context instead): model (--model/-m), sandbox (--sandbox/-s), approval
        // (--ask-for-approval/-a); resumeTarget = an explicit `codex resume <guid>` id (authoritative).
        std::wstring model;
        std::wstring sandbox;
        std::wstring approvalMode;
        std::wstring resumeTarget;
        uint16_t subsystem{}; // PE subsystem (3 = console CLI) — codex.exe is a console app
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
        std::wstring ownerWindowId; // the window whose roster hosts this tab (§19-Q1 attribution)
        RunningApp runningApp{ RunningApp::Unknown };
        bool alive{ true };
        int64_t observedUnixMs{};
        // Transcript timing (the conversation's age + last activity; for the Manager's per-row
        // timing adornment). 0 until the transcript exists. Stat-only — cheap.
        int64_t createdUnixMs{}; // transcript ctime (≈ conversation start)
        int64_t lastActivityUnixMs{}; // transcript mtime (≈ last activity)
    };

    // Per-tab activity for EVERY tab, claude or not (the TabActivityTable; published to the UI
    // lane). (OBSERVER.md §5a)
    struct TabActivityRow
    {
        std::wstring wtSession; // key
        uint32_t shellPid{};
        TabActivity activity{ TabActivity::Unknown };
        std::wstring image; // foreground / shell image leaf ("pwsh.exe", "claude.exe", ...)
        std::wstring cwd; // for Powershell / Cmd / ClaudeCode / Codex
        bool busy{}; // shell has a running child (a command in progress)
        std::wstring sessionId; // when activity == ClaudeCode (mirror of the CorrelationRow)
        std::wstring model; // when activity == Codex: the model (from the rollout turn_context), to enrich the observe badge "○ codex · <model>"
        CodexState codexState{ CodexState::Unknown }; // when activity == Codex (Phase C2): rollout-tail-derived turn state, to enrich the badge ("○ codex · <model> · running")
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
        std::wstring ownerWindowId; // the owning window (roster key / AM_SESSION suffix; §19-Q1)
        std::wstring cwd;
        uint32_t pid{};
        RunningApp runningApp{ RunningApp::Unknown };
        bool background{};
        std::wstring model, effort, permissionMode, sessionName;
        // Agentmaster: the git branch recorded in the transcript (ReadTranscriptInfo, cached per
        // sid). The live writer for SessionInfo::branch — drives the per-tab overlay's row-2
        // "<workdir folder>/<branch>". Empty when the transcript records none / isn't read yet.
        std::wstring gitBranch;
        // Claude's self-reported presence heartbeat (busy/idle/waiting/shell) from
        // ~/.claude/sessions/<pid>.json, pid-liveness-validated by the S-lane. A display FACT,
        // never SessionState (Rule #13). Empty when no live presence file matches this claude.
        std::wstring presenceStatus;
        int64_t observedUnixMs{};
        int64_t createdUnixMs{}; // transcript ctime (≈ conversation start) — per-session timing
        int64_t lastActivityUnixMs{}; // transcript mtime (≈ last activity) — per-session timing
    };

    // An EXTERNAL agent — one we do NOT manage: a real Windows Terminal claude (has a WT_SESSION
    // but not our AM_SESSION) OR a bare-console / cmd-hosted claude (neither WT_SESSION nor ours,
    // i.e. RunningApp::Other) OR ANY Codex session (observe-only in Phase C1, regardless of host —
    // even one in our own tab, since Codex is never adopted/driven yet). The census counts these; O6
    // PUBLISHES them (the External() table) so the Manager surfaces an observe-only group (no registry
    // session, never bound — Rule #9/#13). Enriched (out-of-band) with the conversation's id + title +
    // branch + timing so a row shows real data instead of a bare "claude". Facts only, all runtime.
    // (OBSERVER.md §11c / §19-Q2 / §19-Q3). [Name kept `ExternalClaudeRow` for a minimal, rebase-cheap
    // diff; `kind` discriminates Claude vs Codex.]
    struct ExternalClaudeRow
    {
        uint32_t pid{};
        std::wstring wtSession; // its WT_SESSION (the foreign tab id; NOT in our roster); empty if cmd-hosted
        std::wstring cwd;
        std::wstring model;
        std::wstring effort; // Claude effort, or Codex model_reasoning_effort (from the rollout turn_context)
        bool background{};
        int64_t startUnixMs{};
        int64_t observedUnixMs{};
        // Enrichment (transcript-resolved + host classification):
        std::wstring sessionId; // resolved conversation id (EMPTY until the first prompt); enables Adopt + the read-only plan
        std::wstring title; // first human prompt, one line (the display title; recent transcripts carry no summary)
        std::wstring gitBranch; // gitBranch recorded in the transcript, if any
        // Agentmaster: the Claude Code idle RECAP (away_summary) — the >5-min "what we did / what's
        // next" synthesis, normalized. The OBSERVER is the recap provider here: a MANAGED session gets
        // its recap mirrored onto SessionInfo.recap by the SessionScanner's byte-cursor delta, but an
        // external has NO scanner cursor — so the observer reads it out-of-band from the transcript
        // TAIL (ProcessInspect::ReadTranscriptRecapTail), the SAME region the scanner pulls from. This
        // is the external analog of SessionInfo.recap. Re-read only when the transcript mtime advances
        // (a new recap can only appear on growth), so an idle external costs zero content reads. ""
        // until/unless an away_summary is in the tail; "empty never clears" a captured recap. Surfaced
        // read-only on the External card (hover), the EXTERNAL tree row (hover), and the read-only
        // Flight Plan — mirroring where a managed session shows its recap.
        std::wstring recap;
        RunningApp host{ RunningApp::WindowsTerminal }; // WindowsTerminal == WT-hosted; Other == cmd / bare console
        std::wstring hostImage; // the host shell leaf for an Other host ("cmd.exe", "pwsh.exe", ...); empty for WT
        std::wstring hostLabel; // resolved host DISPLAY name: "Windows Terminal" / "Agentmaster" / "Agentmaster Dev" / a shell leaf — distinguishes real WT from OUR instances (ResolveExternalHostLabel)
        uint32_t hostPid{}; // the host shell pid (the agent's parent) — the "same window/tab" grouping key: agents under one terminal window/tab share it. Drives the color-coded pid underline in the EXTERNAL tree.
        int64_t createdUnixMs{}; // transcript ctime (≈ conversation start)
        int64_t lastActivityUnixMs{}; // transcript mtime (≈ last activity)
        // --- Agentmaster (Phase C1): agent kind + Codex-only enrichment ---
        AgentKind kind{ AgentKind::Claude }; // Claude (default — every existing producer/consumer unchanged) or Codex
        std::wstring sandbox; // Codex sandbox mode (read-only / workspace-write / danger-full-access); empty for Claude
        std::wstring approvalMode; // Codex approval policy (untrusted / on-request / never); empty for Claude
        std::wstring rolloutPath; // Codex: the resolved rollout .jsonl path (date-sharded — not derivable from cwd+id); drives the read-only plan + "open rollout". Empty for Claude (its path derives from cwd+id).
        CodexState codexState{ CodexState::Unknown }; // Codex (Phase C2): rollout-tail-derived turn state (Running/Waiting/Idle), drives the row's state dot. Unknown for Claude (external claudes carry no PULL-derived state).
    };
}
