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

    // Read the PE "Subsystem" of a process's main image (IMAGE_SUBSYSTEM_*: 2 = GUI, 3 = console)
    // from its loaded base (PEB.ImageBaseAddress). 0 if undeterminable (denied / WOW64 / exited).
    // This is how the Claude Code CLI (a console app) is told apart from the Claude DESKTOP app (an
    // Electron GUI binary ALSO named Claude.exe) — see IsClaudeDesktopGuiApp.
    uint16_t ReadProcessImageSubsystem(uint32_t pid);

    // True iff the pid names a process that is still running (OpenProcess + GetExitCodeProcess !=
    // STILL_ACTIVE is false). A cheap liveness gate for the cached-PID fast path.
    bool ProcessAlive(uint32_t pid);

    // ===== PURE: tree helpers over a snapshot (no syscalls) ================================

    // Case-insensitive compare of two leaf image names ("Claude.exe" == "claude.exe"). Pure.
    bool ImageNameEq(std::wstring_view a, std::wstring_view b);

    // BFS the process tree rooted at `root` — descendant-OR-SELF — and return the pid of the
    // SHALLOWEST process whose leaf image matches `imageLeaf` (e.g. L"claude.exe"): the root
    // itself (a Manager-launched claude IS the ConPTY root — no shell in between), else
    // root -> shell -> [cmd-shim ->] claude. 0 if none (the root must exist in the snapshot to
    // self-match). Pure over the snapshot — no extra Toolhelp/PEB calls. Replaces
    // FindClaudeDescendantPid (which, children-only, never correlated a Launched claude).
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

    // The DIRECT children of `shellPid` that are real commands — i.e. NOT console infrastructure
    // (conhost.exe / OpenConsole.exe). In snapshot order. Pure. These are the processes a shell
    // launched to run a command (git / claude / a build / even a nested cmd); each inherits the
    // shell's LIVE working dir at spawn time, which is how a pwsh tab's cwd is recovered out-of-band
    // (pwsh freezes its OWN process cwd but passes $PWD to children — see ResolveShellCwd). (PERSISTENCE.md)
    std::vector<uint32_t> CommandChildrenOf(const std::vector<ProcEntry>& snap, uint32_t shellPid);

    // A shell tab's resolved working directory + whether the reading is trustworthy. (OBSERVER.md)
    struct ShellCwd
    {
        std::wstring cwd;
        bool reliable{}; // true when child-derived OR the shell is cmd.exe (its own PEB tracks `cd`)
    };

    // Resolve a shell's REAL cwd out-of-band (read-only; no shell cooperation). cmd.exe syncs its
    // process cwd on `cd`, so its own PEB is accurate. pwsh/powershell keep their OWN process cwd
    // frozen at the launch dir (Set-Location updates only $PWD) but pass the live $PWD as the cwd of
    // any native child — so we read the NEWEST CommandChildrenOf child's PEB cwd, falling back to the
    // shell's own PEB when there is no child (accurate for cmd; the stale launch dir for an idle pwsh,
    // flagged reliable=false so callers can prefer a cached earlier reading). (OBSERVER.md / PERSISTENCE.md)
    ShellCwd ResolveShellCwd(const std::vector<ProcEntry>& snap, uint32_t shellPid, std::wstring_view shellImage);

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

    // Read another process's MSIX package family name (e.g. "Agentmaster_56k4f06dsfp9r",
    // "AgentmasterDev_56k4f06dsfp9r", "Microsoft.WindowsTerminal_8wekyb3d8bbwe"). Empty for an
    // unpackaged process or on denial. The authoritative way to tell an external claude's host
    // terminal apart: real Windows Terminal vs an Agentmaster (release) vs an Agentmaster Dev instance.
    std::wstring ReadProcessPackageFamily(uint32_t pid);

    // Read a process's full image path (QueryFullProcessImageNameW). Empty on denial/exit. The
    // unpackaged fallback for host classification (a loose Release/Debug build path).
    std::wstring ReadProcessImagePath(uint32_t pid);

    // BFS UP the ancestor chain of `pid` (NOT self) for the nearest WindowsTerminal.exe / wt.exe — the
    // hosting terminal of a claude (directly for a Manager-launched ConPTY-root claude, or past its
    // shell for a hand-typed one). 0 if none (a bare cmd/console host, or the host already exited).
    // Pure over the snapshot. (OBSERVER.md §11c)
    uint32_t FindTerminalHostPid(const std::vector<ProcEntry>& snap, uint32_t pid);

    // The display label for an EXTERNAL claude's HOST: walk to the hosting terminal
    // (FindTerminalHostPid) and name it by package family / image path — "Windows Terminal" (real WT),
    // "Agentmaster" / "Agentmaster Dev" (another of OUR instances). With no live terminal ancestor:
    // "Agentmaster" when the claude still carries our AM_SESSION stamp (an orphan whose host exited),
    // else the nearest shell leaf ("cmd" / "pwsh") for a console-hosted claude, else empty. This is
    // what fixes "Agentmaster shows as WindowsTerminal" and tells release/dev apart. (OBSERVER.md §11c)
    std::wstring ResolveExternalHostLabel(const std::vector<ProcEntry>& snap, uint32_t claudePid, bool amSessionPresent);

    // True iff this `claude.exe` is actually the Claude DESKTOP app — an Electron GUI binary that
    // shares the leaf name "Claude.exe" — or one of its renderer/gpu/utility/crashpad children, NOT a
    // Claude Code CLI session. The desktop app + its helpers all run with cwd C:\WINDOWS\system32, so
    // without this they pollute the External census as bogus "system32 sessions". Discriminated purely
    // by PE subsystem (facts.subsystem, filled by ReadClaudeFacts): the CLI is a console app, the
    // desktop app a GUI app. An UNDETERMINABLE subsystem (0 — denied/elevated/WOW64) is treated as NOT
    // the desktop app, so a real elevated CLI session is never hidden (and a denied PEB can't read a
    // system32 cwd anyway, so it never reintroduces the bug). Pure.
    bool IsClaudeDesktopGuiApp(const ClaudeProcessFacts& facts);

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

    // Agentmaster (subagent/Task activity): the newest last-write time (Unix ms) among a session's
    // SUBAGENT + tool-result SIDE files — Claude Code's layout writes a Task/Agent subagent's own
    // conversation to projects/<proj>/<id>/subagents/agent-<agentId>.jsonl (sharing the parent's
    // sessionId, isSidechain:true) and externalizes big tool outputs to .../tool-results/*, BOTH
    // while the MAIN transcript (<id>.jsonl) stays QUIESCENT. The state engine + the timing adornment
    // fold this in so a tab whose turn delegated to a subagent reads ACTIVE, not stale/idle.
    // `transcriptPath` is the resolved <...>/<id>.jsonl (the side dirs are its sibling <id>/ folder;
    // the ".jsonl" is stripped to find them). 0 when there is no such activity / no side dirs.
    // Filesystem only — one shallow FindFirstFile per side dir, an instant miss when absent (the
    // common case), so it is cheap enough to call each scan tick / survey. (Direct children only —
    // a rare nested subagent is covered by the presence-"busy" floor instead.)
    int64_t SubagentActivityUnixMs(std::wstring_view transcriptPath);

    // A transcript's user-facing metadata, read out-of-band for the Manager's external rows (a real
    // title instead of a bare "claude") and the read-only Flight Plan of a selected external session.
    struct TranscriptInfo
    {
        bool found{};
        int64_t createdUnixMs{}; // file ctime (≈ conversation start)
        int64_t lastActivityUnixMs{}; // file mtime (≈ last activity)
        std::wstring title; // a display title: the FIRST REAL human prompt (noise-filtered — IsNoiseUserPrompt), collapsed to one trimmed line
        std::wstring customTitle; // the user-SET conversation title, when any: the LAST {"type":"custom-title","customTitle":...} line (newer retitles win). Preferred over `title` for display + tab matching.
        std::wstring aiTitle; // the async-generated picker title ({"type":"ai-title"}), when any: the LAST one. Second in display precedence.
        std::wstring summary; // a LEGACY {"type":"summary"} line's text (v2.0.75-2.1.25 stratum only; zero in recent files). Third in precedence.
        std::wstring gitBranch; // the gitBranch recorded on the user lines (first seen), if any
        std::vector<std::wstring> userPrompts; // the REAL human prompts in order (noise/meta/sidechain filtered; capped at maxPrompts)
    };

    // Read <projectsDir>/<encode(cwd)>/<sessionId>.jsonl out-of-band. Always stats (created/last);
    // then reads up to `maxBytes` from the START of the file (0 == the whole file) and parses the
    // HUMAN prompts (role==user, text content, skipping isMeta / tool_result turns — the same rule
    // ParseTranscriptDelta uses), keeping at most `maxPrompts`. `title` is the first prompt collapsed
    // to one line; `gitBranch` is the first seen. (Recent transcripts carry NO "summary" line —
    // verified 0/1842 over 90 days — so the first prompt is the title source.) Filesystem only.
    TranscriptInfo ReadTranscriptInfoIn(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, size_t maxBytes, size_t maxPrompts);
    TranscriptInfo ReadTranscriptInfo(std::wstring_view cwd, std::wstring_view sessionId, size_t maxBytes, size_t maxPrompts);

    // Agentmaster (TAB_OVERLAY.md row 3 "Transcript"): read a transcript into a plain-text
    // conversation — ONLY the human + assistant TEXT messages, in order. Tool calls, tool results,
    // thinking, meta/summary/sidechain lines are all dropped (the user asked for "the whole
    // conversation where only the text messages of user and assistant are present"). Each message
    // becomes "User:\n<text>" / "Assistant:\n<text>" separated by a blank line. `codex` selects the
    // rollout format (event_msg user_message / agent_message); otherwise the Claude projects .jsonl
    // (type user/assistant -> message.content text blocks, skipping tool_result/tool_use/thinking +
    // IsNoiseUserPrompt). `maxBytes` 0 == the whole file. Filesystem only; empty on any read failure.
    std::wstring ReadConversationText(std::wstring_view transcriptPath, bool codex, size_t maxBytes);

    // ===== Session summary (TAB_OVERLAY.md summary panel) — the session-end.js analyzer, ported =====
    // A faithful C++ port of ~/.claude/hooks/session-end.js parseTranscript + its field mapping, so the
    // per-tab summary panel renders the same box (Session / Parent / Plan / Dir / Folder / Resume /
    // Duration / Branch / Tasks / Messages / Files Read / Files Edited). One forward pass over the
    // Claude transcript .jsonl collects every field; the overlay composes the box + decides the live
    // type label.
    struct SessionSummary
    {
        bool found{ false };
        std::vector<std::wstring> userMsgs; // type=user, userType=external, text content; deduped; command/bash/Caveat/Overview/interrupt-skipped
        std::vector<std::wstring> filesRead; // Read tool file_path basenames, sorted + unique
        std::vector<std::wstring> filesCreated; // Write tool file_path basenames whose result was "File created successfully at:" (NEW files), sorted + unique
        std::vector<std::wstring> filesEdited; // Edit / overwriting-Write tool file_path basenames (existing files), sorted + unique
        std::wstring branch; // first gitBranch seen
        std::wstring firstTs; // first entry.timestamp (ISO) — conversation start (age)
        std::wstring lastTs; // last entry.timestamp (ISO) — last activity
        std::wstring lastUserTs; // last real user INTERACTION's entry.timestamp (ISO) — "last user msg" ago: a typed external-user prompt OR the user's answer to an AskUserQuestion (a tool_result on an interactive tool_use), so answering a question advances the clock too
        int tasksCompleted{ 0 };
        int tasksPending{ 0 }; // pending + in_progress, from the LAST TodoWrite
        bool hasExitPlanMode{ false }; // an ExitPlanMode tool_use (plan-end signal)
        bool hasPlanContent{ false }; // planContent on the first external-user msg (plan-start signal)
        std::wstring parentSessionId; // from "read the full transcript at: <...>.jsonl" in the first msg
        std::wstring planFilePath; // a Write into a /plans/ dir (plan-end's plan file)
        std::vector<std::wstring> planFilesRead; // Reads from a /plans/ dir (full paths)
    };

    // Port of session-end.js parseTranscript: one forward pass over a Claude transcript .jsonl.
    // `maxBytes` 0 == the whole file. Filesystem only; `found` is false if the file can't be read.
    SessionSummary AnalyzeSessionTranscript(std::wstring_view transcriptPath, size_t maxBytes);

    // Agentmaster: collapse-mode TABLE de-noiser. When a summary message is flattened to ONE line
    // (wrap-off — the panel escapes newlines to a literal "\n", or the Sessions-page detail box which
    // is always one-line), an embedded table buries the one-liner in a wall of ─/┼/│/| noise. This does
    // TWO things, so a table reads as clean content:
    //   (1) DROP the horizontal RULE rows — box-drawing "├────┼────┤" / "┌──┬──┐" / "└──┴──┘" and
    //       markdown "|----|----|" carry no data, so the whole line goes.
    //   (2) DE-FRAME the DATA rows — strip the │/| cell bars + cell padding and rejoin the cell TEXT
    //       with " · " (U+00B7), dropping empty cells. So "│ Name │ Age │" -> "Name · Age".
    //
    // Pure + total. A line (split on '\n'; '\r' dropped) is a droppable RULE row iff, after trimming
    // surrounding spaces/tabs, it is: non-empty; built ENTIRELY of table-structure chars — box-drawing
    // U+2500..U+257F plus the markdown set [-+=~:|#*_ ] (so a data cell carrying other content, e.g.
    // "│ :) │" or "│ 30 │", is kept); AND actually rule-SHAPED — it has a box-drawing char OR a run of
    // >=3 of the FILL chars -/=/~/#/*/_ (so a lone "│ │" skeleton, a stray ":", or a "* item" bullet is
    // not mistaken for a rule). The fill set covers markdown thematic breaks too — "------", "======",
    // "######", "***", "___" all collapse away, while "### Title" / "=== first GET ===" keep their text
    // (the all-structural guard fails on the letters).
    //
    // A surviving line is DE-FRAMED iff it is a table DATA row: it contains a box-drawing vertical
    // (│ ┃ ║ — these never occur in prose, so any │-bearing content row is safe to split), OR it is a
    // bar-FRAMED markdown row (trimmed, first AND last char '|', >=2 pipes) — the frame requirement
    // keeps a stray prose/code pipe ("foo | grep", "| head") verbatim. Every other line (prose, a bar
    // chart "1K ████ 1.96×", a titled rule "=== X ===") passes through verbatim. If the message has NO
    // rule rows AND nothing to de-frame it is returned unchanged (cheap no-op); if EVERY line is a rule
    // it is ALSO returned unchanged, so a numbered bullet never renders empty. Grounded in a 1.4 GB /
    // 686-session corpus scan. Used by both SummaryEscapeMsg collapse paths.
    std::wstring StripSummaryTableRules(const std::wstring& msg);

    // Agentmaster: the summary-only "is this NOT a real human prompt?" filter — keeps system-injected
    // pseudo-"user" messages (command/bash echoes, task/bash notifications, status+summary blocks,
    // subagent telemetry, system reminders) AND verbatim-pasted Claude-Code TUI output (a message
    // starting with the ●/⏺/⎿ marker glyphs) out of the summary's numbered Messages list. Distinct from
    // IsNoiseUserPrompt (titles / Flight Plan). Exposed for tests. Expects leading whitespace trimmed.
    bool SeIsCommandNoise(const std::wstring& c);

    // Agentmaster: the session-end.js summary BOX rendered to PLAIN TEXT — the SINGLE source of
    // truth shared by the per-tab overlay's summary panel (AgentTabOverlay) AND the Sessions page's
    // detail pane (TerminalPage.AgentSessionsPage), so the two renderings can never drift. Section
    // dividers are emitted as a lone `kSummarySepMark` line: a display turns each into a full-width
    // rule, the clipboard/plain path into a `─` run. `full` picks the audience:
    //  - full=false: the value-add only (Parent/Plan, Tasks, Messages, Files) — for the overlay,
    //    whose badge already shows the header / id / Dir / Folder / Resume / Branch.
    //  - full=true: the COMPLETE box (also id, Dir, Folder, Resume, Branch). The header line is
    //    emitted when `full && !liveLabel.empty()` OR the transcript is plan-start/plan-end — so a
    //    caller with no live state (the Sessions page) can pass an empty label to suppress the
    //    otherwise-redundant header while STILL surfacing the plan signal.
    constexpr wchar_t kSummarySepMark = L'\x1F'; // ASCII Unit Separator — never occurs in transcript content
    std::wstring RenderSessionSummaryBox(const SessionSummary& a, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, const std::wstring& planFile, bool full);

    // Port of session-end.js formatDuration: "2h 12m (20:21 -> 22:33)" from two ISO timestamps, the
    // HH:MM shown in LOCAL time (like the hook). Empty if either timestamp is missing/unparseable.
    std::wstring FormatSessionDuration(std::wstring_view startIso, std::wstring_view endIso);

    // session-end.js getPlanFileFromParent: the LAST `Write` into a /plans/ dir in a transcript
    // (a plan-start session's plan lives in its PARENT). Empty if none. Filesystem only.
    std::wstring FindPlanFileInTranscript(std::wstring_view transcriptPath);

    // The ONE display-title precedence over a TranscriptInfo (SESSIONS.md §6.3): customTitle >
    // aiTitle > legacy summary > title (the first REAL prompt). Pure.
    std::wstring TranscriptDisplayTitle(const TranscriptInfo& info);

    // Agentmaster: the CURRENT git branch of `dir`, read LIVE from .git/HEAD (handles a worktree/
    // submodule .git FILE + a detached HEAD -> short SHA). Empty when `dir` is not under a git repo.
    // The authoritative current branch for the per-tab overlay's "<folder>/<branch>" row: unlike a
    // transcript's recorded gitBranch (a per-line historical SNAPSHOT, first-seen — it reads stale,
    // or "HEAD", when the repo was momentarily detached as the line was written), this is what the
    // working dir is on RIGHT NOW. Filesystem only (a tiny read) — safe off the UI thread.
    std::wstring ReadGitBranchForDir(const std::wstring& dir);

    // ===== Codex (OpenAI Codex CLI) — observe-only enrichment (OBSERVER.md §19-Q3, Phase C1) ====
    // Codex is the Claude analog with three divergences: its config home is CODEX_HOME (else
    // ~/.codex); it can't pin a session id at launch (the id is auto-minted, embedded in the
    // date-sharded rollout filename `sessions/YYYY/MM/DD/rollout-<ISO-ts>-<uuid>.jsonl`); and
    // model/effort/sandbox/approval usually come from config.toml, so they're read from the rollout
    // (turn_context), not the command line. All reads are out-of-band; Codex is never adopted/driven
    // in C1.

    // The Codex config home: CODEX_HOME (of our process) if set, else %USERPROFILE%\.codex. Trailing
    // slashes trimmed. Empty only if USERPROFILE is unset. (A codex PROCESS may carry its OWN
    // CODEX_HOME in its env — CodexProcessFacts.codexHome — which callers prefer when known.)
    std::wstring CodexDefaultHome();

    // Pure: fill the parsed fields of `facts` (model/sandbox/approvalMode/resumeTarget + wtSession/
    // amSession/codexHome) from a command line + env map. Codex flags: model (--model/-m), sandbox
    // (--sandbox/-s), approval (--ask-for-approval/-a); a `resume <guid>` token yields resumeTarget.
    // Does NOT touch pid/start/cwd/commandline/runningApp (the OS layer + caller own those). Total.
    void ParseCodexFacts(std::wstring_view commandline, const std::unordered_map<std::wstring, std::wstring>& env, CodexProcessFacts& facts);

    // Read one codex.exe's facts out-of-band: cwd + command line + env (PEB) + start time, then
    // ParseCodexFacts. `parentPid` left 0 (caller fills from the snapshot), `runningApp` Unknown
    // (caller classifies with its own AM_SESSION). Empty PEB fields => observe-only. (Phase C1)
    CodexProcessFacts ReadCodexFacts(uint32_t pid);

    // Pure: extract the session uuid from a rollout stem ("rollout-<ISO-ts>-<uuid>"): the trailing
    // 36 chars iff they form a hyphenated UUID (Codex uses time-ordered UUIDv7). Empty if not.
    std::wstring CodexRolloutUuid(std::wstring_view rolloutStem);

    // The resolved Codex rollout for a correlated codex: its conversation id (== the rollout uuid),
    // the full .jsonl path (date-sharded — NOT derivable from cwd+id, so it is carried), and timing.
    struct CodexSession
    {
        std::wstring sessionId; // the rollout uuid ("" if none found yet — a never-prompted codex)
        std::wstring rolloutPath; // full path to the rollout .jsonl ("" if none)
        int64_t createdUnixMs{}; // rollout ctime (≈ conversation start)
        int64_t lastActivityUnixMs{}; // rollout mtime (≈ last activity)
    };

    // Resolve the active rollout for a codex at `cwd` started ~`startUnixMs`, searching under an
    // EXPLICIT codex home (so the harness can point it at a temp dir). Scans the date-sharded
    // sessions dir for the local day around the start (± 1 day for midnight/skew), confirms each
    // candidate's cwd via a cheap head-scan of its session_meta line (`ExtractCwdFromTranscriptHead`
    // — cwd precedes the bulky base_instructions, so a 4 KB head suffices), and picks by ctime ≈
    // start IDENTITY (PickNewestTranscript), falling back to newest-mtime-in-cwd for a resumed
    // session (whose rollout predates the process). Empty session when the cwd has no rollout yet.
    CodexSession ResolveCodexSessionIn(std::wstring_view codexHome, std::wstring_view cwd, int64_t startUnixMs);
    CodexSession ResolveCodexSession(std::wstring_view cwd, int64_t startUnixMs); // CodexDefaultHome()

    // Resolve a rollout's full path by its known uuid (an explicit `codex resume <guid>`): globs
    // <home>/sessions/ recursively for `rollout-*<uuid>.jsonl`. Empty if not found. (Phase C1)
    std::wstring ResolveCodexRolloutPathIn(std::wstring_view codexHome, std::wstring_view sessionId);

    // A Codex rollout's user-facing metadata, read out-of-band for the External row + read-only plan.
    struct CodexRolloutInfo
    {
        bool found{};
        int64_t createdUnixMs{}; // file ctime (≈ conversation start)
        int64_t lastActivityUnixMs{}; // file mtime (≈ last activity)
        std::wstring cwd; // session_meta.cwd
        std::wstring title; // first REAL human prompt (a `user_message` event), one line — the display title
        std::wstring model; // first turn_context.model (e.g. "gpt-5.5")
        std::wstring effort; // first turn_context collaboration_mode.settings.reasoning_effort (else top-level)
        std::wstring sandbox; // first turn_context.sandbox_policy.type (read-only / workspace-write / danger-full-access)
        std::wstring approvalMode; // first turn_context.approval_policy (untrusted / on-request / never)
        std::wstring gitBranch; // best-effort: session_meta.git.branch, if recorded
        std::vector<std::wstring> userPrompts; // the human prompts in order (user_message events; noise-filtered; capped)
    };

    // PURE + total: parse Codex rollout JSONL text into a CodexRolloutInfo (sans timing). Each line is
    // a RolloutLine {timestamp,type,payload}: `session_meta` carries cwd; the FIRST `turn_context`
    // carries model/effort/sandbox/approval; `event_msg`/`user_message` payloads are the human prompts
    // (the FIRST is the title — these events are the clean human input; the AGENTS.md / context blobs
    // live in `response_item` user messages and are skipped). `truncated` (a head read may end
    // mid-line) drops the trailing partial segment. (Phase C1; the testable core of ReadCodexRolloutInfo.)
    void ParseCodexRolloutText(std::wstring_view text, bool truncated, size_t maxPrompts, CodexRolloutInfo& out);

    // Read <rolloutPath> out-of-band: stat (created/last), then ReadFileHead(maxBytes; 0 == whole
    // file) -> ParseCodexRolloutText. Filesystem only. (Phase C1)
    CodexRolloutInfo ReadCodexRolloutInfo(std::wstring_view rolloutPath, size_t maxBytes, size_t maxPrompts);

    // Agentmaster: the Codex analog of RenderSessionSummaryBox (shared by the overlay's summary
    // panel; the Sessions page is Claude-only so it never calls this). full=false = prompts only;
    // full=true = the complete box (header + id + Dir + Folder + Resume + Model + Branch + prompts).
    std::wstring RenderCodexSummaryBox(const CodexRolloutInfo& info, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, bool full);

    // ===== Codex turn-state (Phase C2): rollout-tail -> Running / Waiting / Idle ===============
    // Codex's turn lifecycle is EXPLICIT in the rollout (no stop_reason guessing): the event_msg
    // payloads `task_started` (open a turn) and `task_complete` / `turn_aborted` / `thread_rolled_back`
    // (close it). Rollout timestamps are MONOTONIC (verified across the live corpus), so the LAST
    // boundary in file order is the current state — a caller need only feed forward deltas and keep
    // the last non-None result. (No approval/error event exists in the rollout, so this is a 3-state
    // floor; NeedsApproval/Error are not PULL-derivable — that is C3.)

    // One classified rollout line's turn-boundary verdict.
    struct CodexBoundary
    {
        bool isBoundary{}; // true iff this line is a task_started / task_complete / turn_aborted / thread_rolled_back
        CodexState state{ CodexState::Unknown }; // Running for task_started; Waiting for the three closers
        std::wstring lastAgentMessage; // task_complete.last_agent_message (else empty)
    };

    // PURE + total: classify ONE rollout JSONL line. A RolloutLine {type,payload}; only an
    // `event_msg` whose payload.type is a turn-boundary returns isBoundary=true. Trailing CR/LF are
    // tolerated; a non-event_msg / non-JSON / non-boundary line returns {false, Unknown, ""}. (The
    // testable heart of ReadCodexStateDelta.)
    CodexBoundary ClassifyCodexLine(std::wstring_view jsonLine);

    // OS-touching: derive a codex rollout's CURRENT turn state by reading forward from `offsetInOut`
    // (advancing the byte cursor past complete lines) — or, on FIRST sight (offsetInOut <= 0), a
    // rotated/truncated file (cursor past EOF), or a survey that fell too far behind, by SEEKING to
    // the tail window and scanning only the last bytes (so first-sight state is instant on a multi-MB
    // rollout instead of catching up a chunk per survey — the last boundary is always near EOF, since
    // task_complete ends every at-rest session). Returns the LAST boundary's state seen this read,
    // else `prior` (sticky — a quiet read keeps the known state). The caller (the S-lane, per codex
    // pid) persists `offsetInOut` + the returned state in its CodexInfo cache and gates the call on a
    // changed mtime, so steady-state cost is a few KB per active codex per survey, zero when idle.
    // `lastAgentMessageOut` (optional) receives the latest task_complete message seen. Pure-IO; never
    // writes. (Phase C2.)
    CodexState ReadCodexStateDelta(std::wstring_view rolloutPath, int64_t& offsetInOut, CodexState prior, std::wstring* lastAgentMessageOut = nullptr);

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
