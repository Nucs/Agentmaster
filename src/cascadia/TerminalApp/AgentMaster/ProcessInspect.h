// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — ProcessInspect: the reusable, unit-testable process- + transcript-inspection
// primitives the Fleet Observer is built on (doc/agentmaster/OBSERVER.md §6 / §8b).
//
// These promote the PEB/Toolhelp helpers that used to live anonymously inside ClaudeSpawn.cpp
// (ReadProcessCwd, FindClaudeDescendantPid, NameIsClaude) into one place and extend them with a
// full out-of-band read of any process's PEB (cwd / command line / environment / start time) and
// the cwd -> newest-transcript resolution. The PARSE + tree + encode helpers are PURE (string ->
// value), so the standalone harness can exercise them with canned inputs; only the Snapshot* /
// Read* / Resolve* functions touch the OS.
//
// Plain C++ + Win32, no WinRT (the .cpp is <PrecompiledHeader>NotUsing and links into the test
// harness), like the rest of AgentMaster/.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Activity.h" // ClaudeProcessFacts, RunningApp

namespace Agentmaster
{
    // One row of a Toolhelp process snapshot: pid, parent pid, and the leaf image name.
    struct ProcEntry
    {
        uint32_t pid{};
        uint32_t ppid{};
        std::wstring image; // leaf exe name ("claude.exe", "pwsh.exe", ...) as Toolhelp reports it
    };

    // ===== OS-touching: process enumeration + PEB reads =====================================

    // ONE CreateToolhelp32Snapshot of every process -> {pid, ppid, image}. This is the only
    // expensive primitive (~10-12 ms); reuse a single snapshot for the whole survey (census +
    // every tab's shell tree). Empty on failure.
    std::vector<ProcEntry> SnapshotProcesses();

    // Read a process's REAL current directory from its PEB (x64). Tracks `cd` across a claude
    // relaunch (PowerShell never syncs ITS process cwd, but spawns claude with the right one).
    // Trailing slashes are trimmed. Empty on any failure (denied / WOW64 / exited).
    std::wstring ReadProcessCwd(uint32_t pid);

    // Read a process's full command line from its PEB (x64). Verbatim (no trim). Empty on failure.
    std::wstring ReadProcessCommandLine(uint32_t pid);

    // Read a process's environment block from its PEB (x64) as NAME -> VALUE. Names are returned
    // with their original case (Windows env names are case-INsensitive — use EnvLookup to read).
    // Empty on failure. (OBSERVER.md §6: env pointer @ 0x80, size @ 0x3F0, split on NUL.)
    std::unordered_map<std::wstring, std::wstring> ReadProcessEnv(uint32_t pid);

    // Process creation time as Unix epoch milliseconds (GetProcessTimes). 0 on failure. Used to
    // tie-break which transcript belongs to which claude when several share an encoded cwd dir.
    int64_t ProcessStartUnixMs(uint32_t pid);

    // True iff the pid names a process that is still running (OpenProcess + GetExitCodeProcess !=
    // STILL_ACTIVE is false). A cheap liveness gate for the cached-PID fast path.
    bool ProcessAlive(uint32_t pid);

    // ===== PURE: tree helpers over a snapshot (no syscalls) ================================

    // Case-insensitive compare of two leaf image names ("Claude.exe" == "claude.exe"). Pure.
    bool ImageNameEq(std::wstring_view a, std::wstring_view b);

    // BFS the process tree under `root` and return the pid of the SHALLOWEST descendant whose leaf
    // image matches `imageLeaf` (e.g. L"claude.exe"): root -> shell -> [cmd-shim ->] claude. 0 if
    // none. Pure over the snapshot — no extra Toolhelp/PEB calls. Replaces FindClaudeDescendantPid.
    uint32_t FindDescendantByImage(const std::vector<ProcEntry>& snap, uint32_t root, std::wstring_view imageLeaf);

    // The pids whose parent is `parent`, in snapshot order. Pure.
    std::vector<uint32_t> ChildrenOf(const std::vector<ProcEntry>& snap, uint32_t parent);

    // ===== PURE: command-line + env parsing (the testable core of ReadClaudeFacts) =========

    // Case-insensitive environment lookup over a ReadProcessEnv map (Windows env names ignore
    // case). Returns the value or empty. Pure.
    std::wstring EnvLookup(const std::unordered_map<std::wstring, std::wstring>& env, std::wstring_view name);

    // Extract the value following a `--flag` token in a command line: ExtractCmdlineArg(cl,
    // L"--model") on `claude --model opus ...` -> L"opus". Handles `--flag value` and
    // `--flag=value`; respects simple double-quoting around the value. nullopt if the flag is
    // absent or has no value. Pure.
    std::optional<std::wstring> ExtractCmdlineArg(std::wstring_view commandline, std::wstring_view flag);

    // Fill the PARSED fields of `facts` (model/effort/permissionMode/resumeTarget/sessionIdArg/
    // background/sessionName + wtSession/amSession) from a command line + env map, per the rules in
    // OBSERVER.md §6. Does NOT touch pid/parentPid/startUnixMs/cwd/commandline/runningApp (the OS
    // layer + caller own those). Pure + total — the unit-testable heart of ReadClaudeFacts.
    void ParseClaudeFacts(std::wstring_view commandline, const std::unordered_map<std::wstring, std::wstring>& env, ClaudeProcessFacts& facts);

    // Classify a claude by ownership (OBSERVER.md §7): our AM_SESSION -> Agentmaster; a WT_SESSION
    // but not ours -> WindowsTerminal (external); neither -> Other. Matches on the GUID PREFIX of
    // AM_SESSION so both "<processGuid>" (hand-typed) and "<processGuid>:<windowId>" (Launched)
    // forms are recognized as ours (§19-Q1). Pure.
    RunningApp ClassifyRunningApp(std::wstring_view amSession, std::wstring_view wtSession, std::wstring_view ourAmSession);

    // The owning-window id stamped into a Launched claude's AM_SESSION ("<processGuid>:<windowId>"),
    // or empty for a bare "<processGuid>" (a hand-typed `+`-tab claude, whose window is instead known
    // from the publishing roster). Pure. (§19-Q1)
    std::wstring WindowIdFromAmSession(std::wstring_view amSession);

    // ===== OS-touching: full facts read for one claude pid =================================

    // Read one claude.exe's facts out-of-band: cwd + command line + env (PEB) and start time
    // (GetProcessTimes), then ParseClaudeFacts. `parentPid` is left 0 (the caller fills it from the
    // snapshot) and `runningApp` Unknown (the caller classifies with its own AM_SESSION). On a
    // denied/WOW64 read the PEB-derived fields stay empty and the session falls back to observe-only
    // (never mis-bound). (OBSERVER.md §6)
    ClaudeProcessFacts ReadClaudeFacts(uint32_t pid);

    // ===== transcript resolution: cwd -> the active conversation's sessionId =================

    // Encode a working directory to Claude's project-dir leaf: every non-[A-Za-z0-9] character ->
    // '-' (e.g. C:\Users\ELI\.claude -> C--Users-ELI--claude). No case folding. The cwd should be
    // the PEB form (trailing slash already trimmed). Pure. (OBSERVER.md §8b)
    std::wstring EncodeCwdToProjectDir(std::wstring_view cwd);

    // Extract the first `"cwd":"..."` value from a transcript's head text (the first lines of a
    // .jsonl; the cwd appears on the user/assistant lines, not the leading meta lines). Used by the
    // encoding-drift fallback to confirm a candidate transcript really belongs to a cwd. Empty if
    // none found. Pure (a tolerant string scan, not a full JSON parse). (OBSERVER.md §8b)
    std::wstring ExtractCwdFromTranscriptHead(std::wstring_view headText);

    // One transcript file considered for "which conversation is this claude running": its <id>
    // stem and the timestamps used to disambiguate. mtime = last write (the active session is the
    // most-recently-written); ctime = creation (tie-break against the claude's start time).
    struct TranscriptCandidate
    {
        std::wstring stem; // <session-id> (the .jsonl filename without extension)
        int64_t mtimeMs{}; // last-write Unix ms
        int64_t ctimeMs{}; // creation Unix ms
    };

    // Pick the conversation a claude is running from a cwd's candidate transcripts, by CREATION-time
    // IDENTITY (NOT newest-mtime). When the claude's start is known (startUnixMs > 0): REJECT any
    // transcript created well before it started (those belong to other claudes / are stale) and pick
    // the one whose creation time is closest to that start — so two claudes sharing a cwd bind to
    // their OWN conversation, and a never-written-yet claude resolves to "" (§11d / Rule #14). With no
    // start hint (startUnixMs == 0) it falls back to newest-mtime (tie-broken by ctime). `tieWindowMs`
    // is retained for API compatibility but unused on the identity path. Pure + total. (OBSERVER.md §8b)
    std::wstring PickNewestTranscript(const std::vector<TranscriptCandidate>& candidates, int64_t startUnixMs, int64_t tieWindowMs = 2000);

    // Resolve the active conversation's sessionId for a claude at `cwd` started ~`startUnixMs`,
    // searching transcripts under an EXPLICIT projects root (so the harness can point it at a temp
    // dir). Globs <projectsDir>/<encode(cwd)>/*.jsonl and PickNewestTranscript. Empty if the encoded
    // dir holds no transcript yet (a never-prompted claude — see §11d). (OBSERVER.md §8b)
    std::wstring ResolveSessionIdIn(std::wstring_view projectsDir, std::wstring_view cwd, int64_t startUnixMs);

    // ResolveSessionIdIn against the live Claude projects root (ClaudeProjectsDir()). This is what
    // the S-lane calls each survey; it re-runs until the transcript appears (first prompt), then the
    // id fills and the registry record is created. Empty until then.
    std::wstring ResolveSessionId(std::wstring_view cwd, int64_t startUnixMs);
}
