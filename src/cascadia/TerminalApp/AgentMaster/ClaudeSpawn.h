// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
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

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "SessionModels.h" // AppSettings (spawn reads the Claude-session settings)

namespace Agentmaster
{
    struct ClaudeSpawnSpec
    {
        std::wstring sessionId; // lowercase hyphenated UUID (== --session-id == CCMGR_SESSION_ID)
        std::wstring commandline; // e.g. claude --dangerously-skip-permissions --settings "<file>" --session-id <uuid>
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

    // The PowerShell forwarder script content. Static across sessions of ONE profile: it reads
    // CCMGR_SESSION_ID + CCMGR_HOOK_PIPE from the environment and the hook JSON from stdin,
    // then writes one wire line to the pipe. `-Event <Name>` selects the event. `stateDir` (the
    // ACTIVE profile dir) is baked into the bridge-discovery fallback (<stateDir>\bridge.json) —
    // each profile runs its own engine + pipe, so a fixed ~/.agentmaster path would route a
    // dev-profile session's hooks to the release instance's bridge.
    std::wstring BuildForwarderScript(const std::wstring& stateDir);

    // The Claude settings JSON: always wires each consumed hook event to the forwarder, and
    // additionally carries the optional Claude-session settings the user sets via the cog:
    //   * model              -> emits "model": "<v>"            (only when non-empty)
    //   * includeCoAuthoredBy -> emits "includeCoAuthoredBy": false (only when false)
    //   * skipPermissions    -> when FALSE, emits "permissions": { "defaultMode": "default" }
    //                           (the "other variation"; when TRUE the CLI flag handles bypass)
    // `forwarderPath` should already be in a JSON-friendly (forward-slash) form.
    std::wstring BuildHooksSettingsJson(std::wstring_view forwarderPath, std::wstring_view model, bool includeCoAuthoredBy, bool skipPermissions);

    // Assemble the claude command line. `settingsPath` should be forward-slash form.
    // `skipPermissions` ON => prepend --dangerously-skip-permissions (the app gates risk via
    // its own Approval Policy, and `bypassPermissions` mode also skips the startup "trust this
    // folder" dialog that would otherwise wedge an unattended ConPTY session). OFF => no flag
    // (BuildHooksSettingsJson then carries permissions.defaultMode:"default" instead).
    // resume=false: claude [--dangerously-skip-permissions] --settings "<f>" --session-id <id>
    // resume=true : claude [--dangerously-skip-permissions] --resume <id> --settings "<f>"
    // forkFromSessionId set (Agentmaster): claude [..] --resume <forkFrom> --fork-session --session-id
    //   <id> --settings "<f>" — branch an existing conversation into a NEW id (`id`), leaving the
    //   SOURCE transcript untouched (no two-writers-on-one-.jsonl). Overrides the resume/fresh forms.
    // claudeLauncher (Agentmaster): the REAL claude launcher as a FULL PATH (from ResolveRealClaude at
    //   engine init, BEFORE the shim dir is on PATH so it is never our shim). The programmatic spawn
    //   runs through ConPTY's CreateProcessW, which appends only ".exe" and IGNORES PATHEXT — so a bare
    //   `claude` token misses the npm `claude.cmd` and fails with 0x80070002 (ERROR_FILE_NOT_FOUND).
    //   Given a launcher we emit its full path: a .exe as the (quoted) leading token; a .cmd/.bat
    //   wrapped in `cmd /c` (CreateProcessW cannot execute a batch file directly). Empty => the bare
    //   `claude` token (back-compat, and the not-found path that surfaces the error to the user).
    std::wstring BuildClaudeCommandline(std::wstring_view settingsPath, std::wstring_view sessionId, bool resume, bool skipPermissions, std::wstring_view forkFromSessionId = {}, std::wstring_view claudeLauncher = {});

    // Assemble the codex (OpenAI Codex CLI) command line (Agentmaster — Codex managed-session
    // support). Codex CANNOT pin a session id and takes NO --settings (unlike claude), so a FRESH
    // launch is just the launcher — model / sandbox / approval come from ~/.codex/config.toml, the
    // cwd is set by the ConPTY, and ownership is stamped via AM_SESSION in the child env by the
    // caller. A RESUME continues an existing rollout by its uuid: `<codex> resume <uuid>` (model /
    // sandbox / approval are INHERITED from the original run — not overridable on resume).
    // codexLauncher (Agentmaster): the resolved codex launcher as a FULL PATH (from
    // ResolveCodexLauncher at engine init). The programmatic spawn runs through ConPTY's
    // CreateProcessW, which appends only ".exe" and IGNORES PATHEXT — so a bare `codex` token misses
    // the npm `codex.cmd` and fails with 0x80070002 (ERROR_FILE_NOT_FOUND). Given a launcher we emit
    // its full path: a .exe as the (quoted) leading token; a .cmd/.bat wrapped in `cmd /c`
    // (CreateProcessW cannot execute a batch file directly). Empty => the bare `codex` token
    // (back-compat, and the not-found path that surfaces the error to the user). Pure + unit-tested.
    // forkCodexUuid (Agentmaster): FORK an existing rollout into a NEW, independent session/rollout —
    // `<codex> fork <uuid>` (Codex's first-class `fork` subcommand, the analog of claude's
    // --fork-session). The source rollout is NOT written to, so a fork is safe even while the source
    // codex is still running (the two-writers hazard of adopting a LIVE external). Fork WINS over
    // resume (mutually exclusive); codex mints the new rollout's uuid and the Fleet Observer resolves
    // it onto SessionInfo.codexSessionId (the two-id model). Neither set => a fresh launch.
    std::wstring BuildCodexCommandline(std::wstring_view resumeCodexUuid, std::wstring_view forkCodexUuid = {}, std::wstring_view codexLauncher = {});

    // Agentmaster (a managed session is "the agent run from a pwsh terminal"). Wrap an inner Windows
    // command line (BuildClaudeCommandline / BuildCodexCommandline output) so it runs INSIDE an
    // interactive PowerShell host that STAYS OPEN after the inner process exits:
    //   "<pwsh>" -NoLogo -NoExit -EncodedCommand <base64(UTF-16LE of "& <inner>")>
    // So when the agent (claude/codex) exits — Ctrl+C, /exit, a crash — the ConPTY ROOT (pwsh) lives
    // on and the user drops to a live `PS <cwd>>` prompt at the session's working dir (the ConPTY cwd),
    // instead of the connection dying into a "[process exited] press Enter to restart" dead pane that
    // then mis-replays the launch commandline at the wrong cwd. The `& ` call operator INVOKES the
    // (quoted) inner exe rather than echoing it; -EncodedCommand carries the script as a base64
    // UTF-16LE blob, side-stepping ALL nested-quote escaping (the inner keeps its own double quotes
    // verbatim — and a base64 token has no spaces, so it is one CreateProcessW arg). The observer still
    // binds the agent: FindDescendantByImage is descendant-OR-self, so claude.exe as a CHILD of pwsh
    // correlates exactly like a hand-typed one; our env (CCMGR_* / AM_SESSION) rides pwsh and is
    // inherited by the child; --settings is in the inner command line. `pwshLauncher` is the resolved
    // pwsh/powershell full path (empty => the bare `pwsh.exe` token). Pure + unit-tested.
    std::wstring BuildPwshHostedCommandline(std::wstring_view pwshLauncher, std::wstring_view innerCommandline);

    // Convert backslashes to forward slashes (safe inside double-quoted args + JSON).
    std::wstring ToForwardSlashes(std::wstring_view path);

    // Quote a string as a PowerShell single-quoted literal: 'text', with every embedded
    // single quote doubled (the ONLY escape that exists inside PS single quotes — backslashes
    // and $ are inert there). Used to embed the per-profile bridge.json path into the
    // generated forwarder script. Pure + unit-tested.
    std::wstring PsSingleQuote(std::wstring_view s);

    // Parse a list of NAME=VALUE assignments into pairs, for AppSettings.env (extra environment
    // applied to every spawned session) and the per-directory env (dir-env.json). Entries are
    // separated by EITHER a newline (the multi-line editor format) OR a ';' (the legacy single-line
    // format), so an old ';'-delimited settings.json still parses. Entries without '=' / with an
    // empty NAME / starting with '#' (a comment) are skipped; whitespace around an entry and around
    // NAME is trimmed; VALUE is taken verbatim (may itself contain '='). Pure + unit-tested.
    std::vector<std::pair<std::wstring, std::wstring>> ParseEnvAssignments(std::wstring_view spec);

    // Merge a GLOBAL env block (AppSettings.env) with a working-directory's PER-DIR overrides
    // (dir-env.json) into the final ordered NAME=VALUE pairs. Per-dir entries OVERRIDE global ones
    // with the same name (Windows env names are case-INsensitive, so the match is too); within one
    // block the last assignment of a name wins. Reserved CCMGR_* names are dropped (the spawn injects
    // them and a user entry must never clobber the hook correlation). First-seen order is preserved.
    // PURE (no disk) + unit-tested — the testable core of ResolveSessionEnv.
    std::vector<std::pair<std::wstring, std::wstring>> MergeSessionEnv(std::wstring_view globalEnv, std::wstring_view perDirEnv);

    // ResolveSessionEnv = MergeSessionEnv(settings.env, GetDirEnv(workingDir)) — the env applied to a
    // session spawned in `workingDir`. Disk-touching (reads dir-env.json via Persistence::GetDirEnv);
    // the merge itself is the pure MergeSessionEnv above.
    std::vector<std::pair<std::wstring, std::wstring>> ResolveSessionEnv(const AppSettings& settings, std::wstring_view workingDir);

    // --- env-text lexer (drives the Settings cog's live border color + bottom status line) ---
    // A per-line verdict over an env editor's text. Ignored = blank / '#' comment (no var); Ok = a
    // valid NAME=VALUE; Warn = parsed but won't apply as typed (reserved CCMGR_*, an Agentmaster-owned
    // name like AM_SESSION/WT_SESSION, or a duplicate that a later line overrides); Error = unparseable
    // (no '=', empty name, or an invalid name — env names must be [A-Za-z_][A-Za-z0-9_]*).
    enum class EnvLineKind
    {
        Ignored,
        Ok,
        Warn,
        Error
    };
    struct EnvLineDiag
    {
        uint32_t line{}; // 1-based line number
        EnvLineKind kind{ EnvLineKind::Ignored };
        std::wstring name; // the parsed NAME ("" when none)
        std::wstring message; // human note ("" for Ignored/Ok)
    };
    struct EnvLexResult
    {
        std::vector<EnvLineDiag> lines;
        uint32_t ok{}; // count of valid variables
        uint32_t warn{};
        uint32_t error{};
        EnvLineKind worst{ EnvLineKind::Ok }; // worst level present (drives the border color)
        uint32_t firstIssueLine{}; // 1-based line of the first Warn/Error (0 if none)
        std::wstring firstIssue; // its message ("" if none)
    };
    // Lex an env editor's text into per-line diagnostics + counts + the worst level + the first issue.
    // PURE + unit-tested; the UI composes the glyph/color from `worst` and the bottom status from the
    // counts + firstIssue. Uses the SAME accept/skip rules as ParseEnvAssignments, so a line the lexer
    // calls Ok/Warn is exactly one the spawn applies (Warn => applied-but-noteworthy / dropped-if-reserved).
    EnvLexResult LexEnvText(std::wstring_view text);

    // --- shipped global env defaults (ENV_VARS.md §8) ----------------------------------------------
    // Agentmaster ships a tiny set of GLOBAL env defaults, seeded ONCE into AppSettings.env so a NEW
    // install AND an UPDATER both get them — and a user who then EDITS or DELETES one keeps it gone (the
    // seed is gated on a stored version, never re-applied). Each default carries the version it was
    // introduced in; ApplyEnvDefaults appends only those with introVersion > seededVersion whose NAME is
    // not already present (case-insensitive, Windows env semantics), so adding a future default seeds just
    // the new one. Returns {new env text, new seeded version}. PURE + unit-tested (the disk wrapper is
    // Persistence::SeedSessionEnvDefaults).
    //   v1: CLAUDE_CODE_MAX_RETRIES=50000 — the API-retry ceiling. (Claude clamps the EFFECTIVE value to 15
    //       since CLI v2.1.186; the literal 50000 is intentional + harmless, and the user may change it.)
    inline constexpr uint32_t kEnvDefaultsVersion = 1;
    std::pair<std::wstring, uint32_t> ApplyEnvDefaults(std::wstring_view envText, uint32_t seededVersion);

    // --- OS-touching ---

    // The ACTIVE PROFILE dir — where this install persists everything (sessions.json,
    // windows/<id>.json, hooks files, the shim, settings.json, ...). Resolved ONCE per process
    // via ProfileBootstrap.h: env AGENTMASTER_PROFILE (exported by the WindowEmperor's
    // first-launch bootstrap/picker) > the portable marker > the per-install saved choice >
    // the per-identity default (~/.agentmaster for the release package and unpackaged runs —
    // the historical location — or ~/.agentmaster-dev for AgentmasterDev). Created if absent.
    std::wstring AgentmasterStateDir();

    // Append a line to <AgentmasterStateDir>\<fileLeaf> as UTF-8. Thread-safe, best-effort
    // (swallows all I/O errors). Used as M5's live verification surface for hook -> state.
    void AppendStateLog(std::wstring_view fileLeaf, std::wstring_view line);

    // Agentmaster: the USER-NAVIGATION audit trail. Writes "[nav] <msg>\n" to hooks.log so the
    // whole "what did the user click / select / fork / resume / close" journey is reconstructable
    // with `grep '\[nav\]' hooks.log` — the user-INTENT layer that sits above the engine-mechanism
    // tags ([fork] / [resume] / [rehome] / …), which carry the resulting ids it references. Every
    // UI action/navigation funnel emits one. Thread-safe + best-effort like AppendStateLog.
    void LogNav(std::wstring_view msg);

    // First 8 chars of an id (the established log convention for a short session/conversation id;
    // substr clamps, so a shorter id is returned whole). "" -> "(none)".
    std::wstring ShortId(const std::wstring& id);

    // A fresh lowercase hyphenated UUID (CoCreateGuid).
    std::wstring NewSessionId();

    // True iff Claude has a resumable conversation transcript for `sessionId` — i.e. a file
    // <claude-config>/projects/<encoded-cwd>/<sessionId>.jsonl exists. Session ids are unique
    // UUIDs, so we search across all project dirs instead of reproducing Claude's cwd
    // encoding. Restore uses this to choose `--resume` vs. a fresh start: a session that was
    // opened but never received a prompt has NO transcript, and `claude --resume <id>` on it
    // fails with "No conversation found" (the tab would die with exit code 1).
    bool ClaudeConversationExists(std::wstring_view sessionId);

    // Resolve the on-disk transcript for `sessionId`: <claude-config>/projects/*/<id>.jsonl, or
    // empty if none exists yet. The interval reconciler (SessionScanner) tails this file to
    // recover what a dropped hook missed. Ids are unique UUIDs, so we glob across all project
    // dirs rather than reproduce Claude's cwd encoding. ClaudeConversationExists is now exactly
    // `!ResolveClaudeTranscriptPath(id).empty()`.
    std::wstring ResolveClaudeTranscriptPath(std::wstring_view sessionId);

    // The Claude transcript root: <claude-config>/projects (config dir == CLAUDE_CONFIG_DIR, else
    // ~/.claude). Empty if neither resolves. The SessionScanner enumerates this to DISCOVER claude
    // sessions launched outside the Manager (a hand-typed `claude` in any tab) — claude always
    // writes a per-session <id>.jsonl here regardless of how it was started, so this is the one
    // detection signal that no shell function / alias / PATH quirk can shadow. [Agentmaster]
    std::wstring ClaudeProjectsDir();

    // Given a tab's shell process id, find a `claude.exe` running under it (direct child, or deeper:
    // shell -> cmd-shim -> claude) and return that claude's REAL current directory, read from its PEB.
    // Empty if there is no claude descendant or the read fails. This is how the manager correlates a
    // hand-typed `claude` to its transcript WITHOUT shell integration: PowerShell does NOT sync its
    // own process cwd with Set-Location, but it DOES spawn claude with the right cwd — so we read the
    // cwd from the claude process, not the shell. Doubles as scoping: a tab with no claude descendant
    // returns empty and is skipped (so non-claude tabs never bind to a transcript). [Agentmaster]
    std::wstring ClaudeCwdForShell(uint32_t shellPid);

    // Write the shared forwarder + hooks settings into `stateDir`. Idempotent (overwrites
    // so they always match the running build). Returns {settingsPath, forwarderPath} in
    // backslash form. Throws nothing meaningful for the caller; returns empty paths on I/O
    // failure.
    std::pair<std::wstring, std::wstring> MaterializeSharedHookFiles(const std::wstring& stateDir, const AppSettings& settings);

    // Resolve the real `claude` launcher on PATH (searched as claude.exe/.cmd/.bat, in that
    // order). MUST be called BEFORE the shim dir is prepended to PATH so it never resolves to
    // our own shim. Returns the full path, or empty if claude is not found on PATH. Used to author
    // the hand-typed-adoption shim (where a .cmd launcher is fine — the shim re-invokes it).
    std::wstring ResolveRealClaude();

    // Agentmaster (native-exe-only policy). Resolve the NATIVE claude.exe to LAUNCH — the only
    // supported runtime (the whole Fleet Observer + PEB enrichment are claude.exe-keyed; a pure-Node
    // `claude` is unsupported). Resolution order:
    //   1. `overridePath` — if non-empty it MUST exist AND end ".exe" (else => empty: an invalid
    //      override is "not detected", and the Settings UI flags it).
    //   2. `claude.exe` on PATH (EXACT leaf — never a .cmd/.bat).
    //   3. `<home>\.local\bin\claude.exe` (the native installer's default location).
    //   4. FOLLOW a `claude.cmd`/`.bat` on PATH to its npm native binary — `<launcherDir>\node_modules\
    //      @anthropic-ai\...\claude.exe` (this is "a .cmd is OK *iff* it points to the exe"; the .cmd is
    //      never launched, only used as a breadcrumb to the real exe).
    // Empty => no native claude.exe anywhere ⇒ the app GATES all claude interactions. NEVER returns a
    // .cmd/.bat. `ResolveClaudeExeIn` is the OS-light, unit-testable core: it takes the PATH dirs +
    // home explicitly (so the harness points it at temp dirs) and only does file-existence checks;
    // `ResolveClaudeExe` gathers PATH + USERPROFILE from the environment and delegates to it.
    std::wstring ResolveClaudeExeIn(std::wstring_view overridePath, const std::vector<std::wstring>& pathDirs, std::wstring_view homeDir);
    std::wstring ResolveClaudeExe(std::wstring_view overridePath = {});

    // Agentmaster (Codex managed-session support). Resolve the `codex` launcher to spawn a managed
    // Codex session, as a FULL PATH. Same ConPTY/CreateProcessW hazard as claude — a bare `codex`
    // token appends only ".exe", ignores PATHEXT, and so misses an npm `codex.cmd`, dying with
    // 0x80070002 (ERROR_FILE_NOT_FOUND) — so we resolve the full path and BuildCodexCommandline runs
    // it by path (a .exe directly; a .cmd/.bat via `cmd /c`). Unlike ResolveClaudeExe this is NOT
    // native-exe-only: the Fleet Observer finds codex.exe as a DESCENDANT of the tab shell, so a
    // .cmd/.bat that re-execs the native binary is fine. Resolution: codex.exe / codex.cmd /
    // codex.bat on PATH (per-dir, .exe preferred), then <home>\.local\bin\codex.exe. Empty if codex
    // is not found anywhere (the spawn then falls back to the bare `codex` token + the error).
    std::wstring ResolveCodexLauncher();

    // Agentmaster. Resolve the PowerShell host that wraps a managed agent session (see
    // BuildPwshHostedCommandline), as a FULL PATH (ConPTY's CreateProcessW appends only ".exe" and
    // ignores PATHEXT, so the full path is mandatory). Prefers `pwsh.exe` (PowerShell 7) on PATH;
    // falls back to Windows PowerShell (<System32>\WindowsPowerShell\v1.0\powershell.exe — always
    // present). Empty only if neither is found (the launch then falls back to the bare `pwsh.exe`
    // token). Resolved ONCE at engine init into Engine::pwshExePath.
    std::wstring ResolvePwshLauncher();

    // Write a transparent `claude` PATH shim (claude.cmd for cmd/PowerShell + an
    // extensionless POSIX `claude` for git-bash) into <stateDir>\shim. Each forwards all args
    // to the real claude, injecting `--settings <settingsPath>` UNLESS the caller already
    // passed --settings. Prepending the returned dir to PATH makes a HAND-TYPED `claude` in
    // any shell self-wire for hooks, so a `+`-tab session is adopted into observe+control.
    // Returns the shim dir, or empty if no real claude was found (caller then skips the PATH
    // prepend). `settingsPath` should be in backslash form.
    std::wstring MaterializeClaudeShim(const std::wstring& stateDir, const std::wstring& settingsPath);

    // Publish the live bridge pipe to <stateDir>\bridge.json ({pid,pipe}) so a forwarder
    // whose process did not inherit CCMGR_HOOK_PIPE can still discover the bridge.
    void WriteBridgeDiscovery(std::wstring_view pipeName);

    // Build a complete spawn spec and ensure the shared hook files exist. `pipeName` is the
    // live HooksBridge pipe (HookPipeName(pid)). If `resumeSessionId` is non-empty, the spec
    // RESUMES that conversation (claude --resume <id>) and reuses the id; otherwise a fresh
    // id is generated. The id is always exported as CCMGR_SESSION_ID for hook correlation.
    // If `forkFromSessionId` is non-empty (Agentmaster), the spec FORKS that conversation: a fresh
    // id is minted as the spec id (the fork TARGET) and the commandline resumes <forkFromSessionId>
    // with --fork-session, so the source transcript is never written to. Mutually exclusive with
    // resumeSessionId; if both are set, fork wins.
    // `claudeLauncher` (the real claude full path, resolved by the engine before the shim is on PATH)
    // is threaded to BuildClaudeCommandline so the spawn launches claude BY FULL PATH — the npm
    // `claude.cmd` via `cmd /c`, a native claude.exe directly — instead of a bare `claude` token that
    // ConPTY's CreateProcessW can only resolve as claude.exe (no PATHEXT). Empty => bare-token fallback.
    // `forkIntoSessionId` (Agentmaster, fork only): the id the fork should target INSTEAD of a freshly
    // minted one. A GENUINE fork leaves this empty (mint a new id). A RESTORE re-fork — re-materializing
    // a NEVER-MESSAGED fork whose own `<id>.jsonl` was never written (its first turn never ran) — passes
    // the fork's EXISTING id here so it forks back into the same id, preserving its identity (and the
    // WindowRecord tab ref) across the restart rather than churning a new id every reopen. Collision-free
    // because a transcript-less fork's id is unused on disk. Ignored unless forkFromSessionId is set.
    ClaudeSpawnSpec BuildClaudeSpawn(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName, std::wstring_view resumeSessionId, const AppSettings& settings, std::wstring_view forkFromSessionId = {}, std::wstring_view claudeLauncher = {}, std::wstring_view forkIntoSessionId = {});

    // Build a spec to RELAUNCH an existing managed conversation IN PLACE — the tab's connection died and
    // the user hit "Restart session" (WT's restartConnection). Unlike BuildClaudeSpawn it NEVER mints a
    // new id and NEVER forks: it RESUMES <sessionId> (claude --resume <id>) when a transcript exists, else
    // starts FRESH reusing <sessionId> (claude --session-id <id> — a never-prompted/early-crashed session
    // wrote no transcript, so reusing the id is collision-free and keeps the registry/tab/injector binding
    // stable). It must be used INSTEAD of replaying the original launch commandline, which is `--session-id
    // <id>` for a fresh launch (collides with the now-existing transcript) or a `--fork-session` form for a
    // fork (would re-fork). `claudeLauncher` is the resolved full path (BuildClaudeSpawn semantics).
    ClaudeSpawnSpec BuildClaudeRestartSpec(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName, std::wstring_view sessionId, const AppSettings& settings, std::wstring_view claudeLauncher = {});
}
