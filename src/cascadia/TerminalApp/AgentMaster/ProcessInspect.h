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
#include <unordered_set>
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

    // True iff `image` is a known interactive shell leaf (pwsh / powershell / cmd / bash / sh / wsl
    // / zsh). Pure. Used by the activity `busy` heuristic (O6): a shell with a non-shell child is
    // running a command. (claude.exe / codex.exe are NOT shells — they take their own activity
    // branch, not the shell branch.)
    bool IsShellImage(std::wstring_view image);

    // True iff `pid` has a DIRECT child that is a real process — ignoring console infrastructure
    // (conhost.exe / OpenConsole.exe), which is not "a command in progress". Pure. The `busy`
    // heuristic for a CLAUDE: claude at rest has no children; running a tool (a Bash shell child,
    // ripgrep, a helper) gives it one. (OBSERVER.md §6 — the O6 refinement of the O4 placeholder.)
    bool HasActiveChild(const std::vector<ProcEntry>& snap, uint32_t pid);

    // True iff `pid` has a DIRECT child that is neither a shell (IsShellImage) nor console
    // infrastructure — i.e. the SHELL is running a foreground command (busy). Pure. (A claude/codex
    // child counts as non-shell, but those tabs take the ClaudeCode/Codex branch, not the shell one.)
    bool HasNonShellChild(const std::vector<ProcEntry>& snap, uint32_t pid);

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

    // ===== transcript content: timing + title + human prompts ================================

    // Cheap stat of <projectsDir>/<encode(cwd)>/<sessionId>.jsonl: creation time (≈ the conversation
    // start) and last-write time (≈ last activity), as Unix ms. Returns false (outs left 0) when the
    // file is absent. GetFileAttributesEx only — no read, no parse — so it is cheap enough to call
    // each survey for every correlated session. (Feature: per-session age / activity timing.)
    bool TranscriptTimesIn(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, int64_t& createdUnixMs, int64_t& lastActivityUnixMs);
    bool TranscriptTimes(std::wstring_view cwd, std::wstring_view sessionId, int64_t& createdUnixMs, int64_t& lastActivityUnixMs);

    // A transcript's user-facing metadata, read out-of-band for the Manager's external rows (a real
    // title instead of a bare "claude") and the read-only Flight Plan of a selected external session.
    struct TranscriptInfo
    {
        bool found{};
        int64_t createdUnixMs{}; // file ctime (≈ conversation start)
        int64_t lastActivityUnixMs{}; // file mtime (≈ last activity)
        std::wstring title; // a display title: the FIRST human prompt, collapsed to one trimmed line
        std::wstring customTitle; // the user-SET conversation title, when any: the LAST {"type":"custom-title","customTitle":...} line (newer retitles win). Preferred over `title` for display + tab matching.
        std::wstring gitBranch; // the gitBranch recorded on the user lines (first seen), if any
        std::vector<std::wstring> userPrompts; // the human prompts in order (capped at maxPrompts)
    };

    // Read <projectsDir>/<encode(cwd)>/<sessionId>.jsonl out-of-band. Always stats (created/last);
    // then reads up to `maxBytes` from the START of the file (0 == the whole file) and parses the
    // HUMAN prompts (role==user, text content, skipping isMeta / tool_result turns — the same rule
    // ParseTranscriptDelta uses), keeping at most `maxPrompts`. `title` is the first prompt collapsed
    // to one line; `gitBranch` is the first seen. (Recent transcripts carry NO "summary" line —
    // verified 0/1842 over 90 days — so the first prompt is the title source.) Filesystem only.
    TranscriptInfo ReadTranscriptInfoIn(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, size_t maxBytes, size_t maxPrompts);
    TranscriptInfo ReadTranscriptInfo(std::wstring_view cwd, std::wstring_view sessionId, size_t maxBytes, size_t maxPrompts);

    // ===== window activation: surface an external claude's hosting window (Manager UI) ======

    // The last path segment of a Windows/POSIX path ("K:\src\Agentmaster\" -> "Agentmaster";
    // trailing separators ignored). Pure. (The cwd-leaf hint for ScoreClaudeTabName.)
    std::wstring PathLeaf(std::wstring_view path);

    // Score how strongly a terminal TAB NAME looks like the tab hosting a given claude, for the
    // Bring-Window-To-Front tab pick: 100 = EXACTLY equals the title hint (trimmed — a tab named
    // by the conversation's title); 90 = contains "claude" (claude's own OSC title / a user
    // rename); 80 = leads with one of claude's OSC status glyphs (U+2733 / U+2736 / U+273D /
    // U+2738); 70 = full containment either way (name in hint or hint in name, the contained
    // side >= 6 chars); 60 = contains the head (16 chars, min 8) of the title hint; 40 = contains
    // the cwd leaf (shells commonly title tabs by cwd); 0 = no signal. Case-insensitive; hints
    // may be empty. Pure. (Best-effort BY DESIGN — a tab can be hand-renamed to anything; callers
    // must treat 0 as "don't guess a tab". The token-overlap scorer below covers renamed tabs.)
    int ScoreClaudeTabName(std::wstring_view tabName, std::wstring_view titleHint, std::wstring_view cwdLeaf);

    // Tokenize free text for the tab-vs-conversation overlap match: lowercased [a-z0-9]+ runs of
    // length >= 3 (shorter ones — "am", "gh", "of" — are pure noise). Pure.
    std::unordered_set<std::wstring> TokenizeTextLower(std::wstring_view text);

    // The token-overlap tier of the tab pick, for HAND-RENAMED tabs (the common power-user real-WT
    // setup: tabs carry short task labels like "am resume" that appear nowhere in any title): 50
    // when EVERY (>= 1) token of the tab name occurs in the conversation corpus (title + human
    // prompts) — as a whole word, or as a PREFIX of a corpus word (labels abbreviate: "act" ~
    // "actions", "perf" ~ "performance"; the reverse direction is NOT a match, so "resumes" never
    // hits "resume"). Deliberately all-or-nothing — a partial overlap on one generic word
    // ("changes") is noise, and the caller's unique-best guard drops ties. Pure.
    int ScoreTabNameTokens(std::wstring_view tabName, const std::unordered_set<std::wstring>& corpusTokens);

    // The full tab pick over one window's tab NAMES (the unit-testable core of the UIA pick).
    // Per-tab score = max of the title tiers over every hint (ScoreClaudeTabName, incl. the cwd
    // leaf) and the token tier — 50 when the tab's tokens all hit the HEAD corpus (title + custom
    // title + FIRST prompt: a tab is usually labeled for the conversation's PURPOSE), else 45
    // when they all hit the FULL corpus (every prompt; an incidental later-prompt word like
    // "changes" must not tie out a purpose match). The token tier is DISQUALIFIED for a tab whose
    // token set is a STRICT subset of a sibling tab's ("npyiter pr" == {npyiter} beside "npyiter
    // perf" == {npyiter, perf}): such a generic-prefix label would "uniquely" match ANY
    // conversation that mentions the shared word, which is exactly the wrong-tab flip the pick
    // must never make; only the title tiers may speak for it. Returns the best tab's index (-1
    // when nothing scores); `outScore` its score; `outUnique` = strictly ahead of every other
    // scoring tab — callers must SELECT only a unique best (a tie means "don't guess"). Pure.
    int PickClaudeTab(const std::vector<std::wstring>& tabNames, const std::vector<std::wstring>& titleHints, std::wstring_view cwdLeaf, const std::unordered_set<std::wstring>& headCorpusTokens, const std::unordered_set<std::wstring>& fullCorpusTokens, int& outScore, bool& outUnique);

    // Bring the top-level window HOSTING a claude to the foreground (the Manager's EXTERNAL
    // right-click "Bring Window To Front"): restore it when minimized, foreground it, and — when
    // the host is a Windows Terminal-class window (real WT or this fork; the island class
    // CASCADIA_HOSTING_WINDOW_CLASS) — best-effort select the claude's TAB via UI Automation.
    // The tab is picked by scoring every tab name against several hints — the caller's display
    // title, plus (when `sessionId` is given) the transcript's custom title / first prompt and a
    // token-overlap match against the conversation corpus (ScoreClaudeTabName /
    // ScoreTabNameTokens) — and is selected ONLY on a UNIQUE strict-best score (a tie or no
    // signal keeps the window's current tab; never guess). The window is found by walking the
    // claude's ancestor chain (claude -> host shell -> the terminal/editor that owns a visible
    // window; `hostShellPid` roots the walk when the claude itself already exited), checking
    // conhost children for a classic console (the visible console window belongs to a conhost.exe
    // CHILD of the shell), and — when the process tree owns no visible window at all (a Win11
    // default-terminal HANDOFF console, whose visible window is a Windows Terminal in an
    // unrelated process) — scanning foreign WT-class windows for a CONFIDENT tab match. Strictly
    // window activation — it never writes to the foreign session (the observer invariant, Rule
    // #13). Returns false when no host window was found (elevated targets deny UIA/ShowWindow and
    // degrade to whatever the shell permits). Call OFF the UI thread: it takes a Toolhelp
    // snapshot, reads the transcript, and does cross-process UI Automation reads (tens of ms).
    bool BringClaudeWindowToFront(uint32_t claudePid, uint32_t hostShellPid, std::wstring_view sessionId, std::wstring_view titleHint, std::wstring_view cwd);
}
